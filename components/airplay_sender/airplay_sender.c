/* AirPlay 1 (RAOP) sender, written from the public protocol descriptions.

   Session: RTSP over TCP to the speaker
     [POST /auth-setup]  Apple AirPlay 2 speakers only
     OPTIONS             capabilities, a first round trip
     ANNOUNCE            SDP: Apple Lossless, 352 frames, 16-bit, 2 ch, 44100 Hz
     SETUP               our control and timing UDP ports; the speaker's ports
     RECORD              first sequence number and RTP timestamp
     SET_PARAMETER       volume
     FLUSH               drop buffered audio (stop)
     TEARDOWN            end

   Three UDP flows:
     audio    RTP packets of 352 frames as uncompressed ALAC, to server_port
     control  sync packets (RTP time <-> NTP time) every second, from us;
              resend requests from the speaker, answered from the backlog
     timing   NTP-style requests from the speaker, answered with our clock

   A packet with RTP timestamp T is sent when our clock reaches T; the sync
   packets tell the speaker to play it latency_frames later. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "airplay_sender.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "psa/crypto.h"

static const char *TAG = "airplay_sender";

#define FRAMES AIRPLAY_FRAMES_PER_PACKET
#define RTP_HEADER_SIZE 12
/* Uncompressed ALAC frame: 23 header bits, 352 x 2 x 16 sample bits, 3 end bits. */
#define ALAC_FRAME_SIZE ((23 + FRAMES * 2 * 16 + 3 + 7) / 8)
#define PACKET_SIZE (RTP_HEADER_SIZE + ALAC_FRAME_SIZE)
#define RESEND_HEADER_SIZE 4

/* lwIP's sendto and select run deep; 4 KB overflowed on the S3. */
#define NETWORK_TASK_STACK 6144
#define RTSP_TIMEOUT_MS 5000
#define RTSP_HEADER_MAX 2048
#define SYNC_INTERVAL_US 1000000
#define KEEPALIVE_INTERVAL_US (30 * 1000000LL)
/* An arbitrary NTP epoch for our clock: only differences matter. */
#define NTP_EPOCH_SECONDS 0xEC000000u

#define PT_TIMING_REQUEST 0x52
#define PT_TIMING_REPLY 0x53
#define PT_SYNC 0x54
#define PT_RESEND_REQUEST 0x55
#define PT_RESEND_REPLY 0x56
#define PT_AUDIO 0x60

typedef struct {
    int status;
    char headers[RTSP_HEADER_MAX];
} rtsp_response_t;

struct airplay_sender {
    airplay_sender_config_t config;
    uint32_t latency_frames;
    int volume;

    int rtsp;
    int audio_socket;
    int control_socket;
    int timing_socket;
    struct sockaddr_in remote;
    char local_ip[16];
    char remote_ip[16];
    uint16_t server_port;         /* speaker's audio port */
    uint16_t remote_control_port; /* speaker's control port */
    char url[64];
    char session[64];
    uint32_t cseq;
    char client_instance[17];
    char dacp_id[17];
    uint32_t active_remote;

    SemaphoreHandle_t rtsp_lock; /* one RTSP exchange at a time */
    SemaphoreHandle_t lock;      /* stream state and backlog */
    TaskHandle_t task;
    SemaphoreHandle_t task_done;
    volatile bool task_stop;
    volatile bool connected;

    /* Stream state, under lock. */
    bool playing;
    bool marker;       /* next audio packet starts a stream */
    uint16_t seq;
    uint32_t next_ts;  /* RTP timestamp of the next packet */
    uint32_t ssrc;
    int64_t anchor_us; /* our clock read anchor_ts at anchor_us */
    uint32_t anchor_ts;
    int64_t last_sync_us;

    int16_t chunk[FRAMES * 2]; /* written by the app's task only */
    size_t chunk_frames;

    uint8_t *backlog; /* backlog_packets x PACKET_SIZE */
    uint16_t *backlog_size;

    /* Working buffers, kept off the task stacks. */
    char request_head[768];           /* under rtsp_lock */
    rtsp_response_t scratch_response; /* under rtsp_lock */
    uint8_t resend_packet[RESEND_HEADER_SIZE + PACKET_SIZE]; /* under lock */
};

/* ---- Byte and clock helpers --------------------------------------------- */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* Our clock as a 64-bit NTP timestamp. */
static void put_ntp(uint8_t *p, int64_t us)
{
    put32(p, NTP_EPOCH_SECONDS + (uint32_t)(us / 1000000));
    put32(p + 4, (uint32_t)(((uint64_t)(us % 1000000) << 32) / 1000000));
}

/* The RTP timestamp our clock reads at now_us. Under lock, while playing. */
static uint32_t clock_ts(const struct airplay_sender *s, int64_t now_us)
{
    return s->anchor_ts + (uint32_t)((now_us - s->anchor_us) * AIRPLAY_SAMPLE_RATE / 1000000);
}

static void random_hex(char *out, size_t digits)
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < digits; ++i) {
        out[i] = hex[esp_random() & 0xF];
    }
    out[digits] = '\0';
}

/* ---- RTSP --------------------------------------------------------------- */

static esp_err_t send_all(int socket, const void *data, size_t length)
{
    const uint8_t *p = data;
    while (length > 0) {
        int sent = send(socket, p, length, 0);
        if (sent <= 0) {
            return ESP_FAIL;
        }
        p += sent;
        length -= (size_t)sent;
    }
    return ESP_OK;
}

/* The value of header name in a response, copied to value; false if absent. */
static bool header_value(const rtsp_response_t *response, const char *name, char *value,
                         size_t size)
{
    size_t length = strlen(name);
    for (const char *line = response->headers; line != NULL && *line != '\0';) {
        if (strncasecmp(line, name, length) == 0 && line[length] == ':') {
            const char *start = line + length + 1;
            while (*start == ' ') {
                ++start;
            }
            size_t n = strcspn(start, "\r\n");
            if (n >= size) {
                n = size - 1;
            }
            memcpy(value, start, n);
            value[n] = '\0';
            return true;
        }
        line = strstr(line, "\r\n");
        if (line != NULL) {
            line += 2;
        }
    }
    return false;
}

/* Sends one request and reads its response; the body of the response is
   read and dropped. Takes rtsp_lock. */
static esp_err_t rtsp_request(struct airplay_sender *s, const char *method, const char *uri,
                              const char *extra_headers, const char *content_type,
                              const void *body, size_t body_length, rtsp_response_t *response)
{
    /* The buffers live in the sender, not on the stack: the network task
       sends keepalives from a small stack. */
    xSemaphoreTake(s->rtsp_lock, portMAX_DELAY);
    if (response == NULL) {
        response = &s->scratch_response;
    }
    char *head = s->request_head;
    const size_t head_size = sizeof(s->request_head);
    int n = snprintf(head, head_size,
                     "%s %s RTSP/1.0\r\n"
                     "CSeq: %lu\r\n"
                     "User-Agent: iTunes/7.6.2 (Windows; N;)\r\n"
                     "Client-Instance: %s\r\n"
                     "DACP-ID: %s\r\n"
                     "Active-Remote: %lu\r\n",
                     method, uri, (unsigned long)++s->cseq, s->client_instance, s->dacp_id,
                     (unsigned long)s->active_remote);
    if (s->session[0] != '\0') {
        n += snprintf(head + n, head_size - n, "Session: %s\r\n", s->session);
    }
    if (extra_headers != NULL) {
        n += snprintf(head + n, head_size - n, "%s", extra_headers);
    }
    if (body_length > 0) {
        n += snprintf(head + n, head_size - n, "Content-Type: %s\r\nContent-Length: %u\r\n",
                      content_type, (unsigned)body_length);
    }
    n += snprintf(head + n, head_size - n, "\r\n");
    if (n >= (int)head_size) {
        xSemaphoreGive(s->rtsp_lock);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t result = send_all(s->rtsp, head, (size_t)n);
    if (result == ESP_OK && body_length > 0) {
        result = send_all(s->rtsp, body, body_length);
    }

    /* Read up to the blank line that ends the headers. */
    size_t used = 0;
    char *end = NULL;
    while (result == ESP_OK && end == NULL) {
        if (used == sizeof(response->headers) - 1) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        int got = recv(s->rtsp, response->headers + used, sizeof(response->headers) - 1 - used, 0);
        if (got <= 0) {
            result = got == 0 ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
            break;
        }
        used += (size_t)got;
        response->headers[used] = '\0';
        end = strstr(response->headers, "\r\n\r\n");
    }
    if (result == ESP_OK) {
        size_t header_length = (size_t)(end - response->headers) + 4;
        size_t body_have = used - header_length;
        end[2] = '\0'; /* keep the last header's CRLF for header_value */
        response->status = 0;
        sscanf(response->headers, "RTSP/1.0 %d", &response->status);
        char value[16];
        size_t content_length =
            header_value(response, "Content-Length", value, sizeof(value)) ? strtoul(value, NULL, 10) : 0;
        /* Drop the body. */
        char discard[128];
        while (body_have < content_length) {
            size_t want = content_length - body_have;
            int got = recv(s->rtsp, discard, want < sizeof(discard) ? want : sizeof(discard), 0);
            if (got <= 0) {
                result = ESP_ERR_TIMEOUT;
                break;
            }
            body_have += (size_t)got;
        }
    }
    int status = response->status;
    xSemaphoreGive(s->rtsp_lock);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "%s: no response (%s)", method, esp_err_to_name(result));
        return result;
    }
    ESP_LOGD(TAG, "%s %s -> %d", method, uri, status);
    if (status != 200) {
        ESP_LOGW(TAG, "%s %s: RTSP status %d", method, uri, status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Apple's AirPlay 2 speakers take a RAOP session only after /auth-setup:
   0x01 and a fresh Curve25519 public key. The reply is not needed. */
static esp_err_t auth_setup(struct airplay_sender *s)
{
    uint8_t body[33] = {0x01};
    size_t length = 0;
    psa_key_id_t key = 0;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attributes, 255);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
    if (psa_crypto_init() != PSA_SUCCESS || psa_generate_key(&attributes, &key) != PSA_SUCCESS ||
        psa_export_public_key(key, body + 1, 32, &length) != PSA_SUCCESS || length != 32) {
        ESP_LOGW(TAG, "Curve25519 key generation failed; sending a random key");
        esp_fill_random(body + 1, 32);
    }
    psa_destroy_key(key);
    return rtsp_request(s, "POST", "/auth-setup", NULL, "application/octet-stream", body,
                        sizeof(body), NULL);
}

/* ---- Audio packets ------------------------------------------------------ */

typedef struct {
    uint8_t *out;
    uint32_t bits;
    int count;
} bit_writer_t;

static void put_bits(bit_writer_t *w, uint32_t value, int bits)
{
    w->bits = (w->bits << bits) | (value & ((1u << bits) - 1));
    w->count += bits;
    while (w->count >= 8) {
        w->count -= 8;
        *w->out++ = (uint8_t)(w->bits >> w->count);
    }
}

/* Writes an uncompressed ("escape") ALAC frame of one stereo element. */
static void encode_alac(uint8_t *out, const int16_t *stereo)
{
    bit_writer_t w = {.out = out};
    put_bits(&w, 1, 3);  /* element: channel pair */
    put_bits(&w, 0, 4);  /* element instance */
    put_bits(&w, 0, 12); /* unused */
    put_bits(&w, 0, 1);  /* no sample count: a full frame */
    put_bits(&w, 0, 2);  /* no extra bytes */
    put_bits(&w, 1, 1);  /* not compressed */
    for (int i = 0; i < FRAMES * 2; ++i) {
        put_bits(&w, (uint16_t)stereo[i], 16);
    }
    put_bits(&w, 7, 3); /* end of frame */
    if (w.count > 0) {
        *w.out = (uint8_t)(w.bits << (8 - w.count));
    }
}

/* Sync packet: our clock is at RTP time now_ts, and the frame playing now is
   now_ts - latency. Under lock. */
static void send_sync(struct airplay_sender *s, bool first)
{
    int64_t now_us = esp_timer_get_time();
    uint32_t now_ts = clock_ts(s, now_us);
    uint8_t packet[20];
    packet[0] = first ? 0x90 : 0x80;
    packet[1] = 0x80 | PT_SYNC;
    put16(packet + 2, 7);
    put32(packet + 4, now_ts - s->latency_frames);
    put_ntp(packet + 8, now_us);
    put32(packet + 16, now_ts);
    struct sockaddr_in to = s->remote;
    to.sin_port = htons(s->remote_control_port);
    sendto(s->control_socket, packet, sizeof(packet), 0, (struct sockaddr *)&to, sizeof(to));
    s->last_sync_us = now_us;
}

/* Sends the full chunk as the next packet and keeps it for resends. Under lock. */
static void send_chunk(struct airplay_sender *s)
{
    size_t slot = s->seq % s->config.backlog_packets;
    uint8_t *packet = s->backlog + slot * PACKET_SIZE;
    packet[0] = 0x80;
    packet[1] = (uint8_t)((s->marker ? 0x80 : 0) | PT_AUDIO);
    put16(packet + 2, s->seq);
    put32(packet + 4, s->next_ts);
    put32(packet + 8, s->ssrc);
    encode_alac(packet + RTP_HEADER_SIZE, s->chunk);
    s->backlog_size[slot] = PACKET_SIZE;

    struct sockaddr_in to = s->remote;
    to.sin_port = htons(s->server_port);
    sendto(s->audio_socket, packet, PACKET_SIZE, 0, (struct sockaddr *)&to, sizeof(to));

    s->marker = false;
    s->seq++;
    s->next_ts += FRAMES;
    s->chunk_frames = 0;
}

/* Answers a resend request from the backlog. Under lock. */
static void resend(struct airplay_sender *s, uint16_t first, uint16_t count)
{
    uint8_t *reply = s->resend_packet;
    struct sockaddr_in to = s->remote;
    to.sin_port = htons(s->remote_control_port);
    if (count > s->config.backlog_packets) {
        count = s->config.backlog_packets;
    }
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t seq = (uint16_t)(first + i);
        size_t slot = seq % s->config.backlog_packets;
        const uint8_t *packet = s->backlog + slot * PACKET_SIZE;
        if (s->backlog_size[slot] == 0 || get16(packet + 2) != seq) {
            continue; /* too old */
        }
        reply[0] = 0x80;
        reply[1] = 0x80 | PT_RESEND_REPLY;
        put16(reply + 2, seq);
        memcpy(reply + RESEND_HEADER_SIZE, packet, s->backlog_size[slot]);
        sendto(s->control_socket, reply, RESEND_HEADER_SIZE + s->backlog_size[slot], 0,
               (struct sockaddr *)&to, sizeof(to));
    }
}

/* ---- Network task: timing, resends, syncs, keepalive -------------------- */

static void answer_timing(struct airplay_sender *s)
{
    uint8_t request[32];
    struct sockaddr_in from;
    socklen_t from_length = sizeof(from);
    int got = recvfrom(s->timing_socket, request, sizeof(request), MSG_DONTWAIT,
                       (struct sockaddr *)&from, &from_length);
    int64_t received_us = esp_timer_get_time();
    if (got < 32 || (request[1] & 0x7F) != PT_TIMING_REQUEST) {
        return;
    }
    uint8_t reply[32] = {0x80, 0x80 | PT_TIMING_REPLY, 0x00, 0x07};
    memcpy(reply + 8, request + 24, 8); /* their send time */
    put_ntp(reply + 16, received_us);
    put_ntp(reply + 24, esp_timer_get_time());
    sendto(s->timing_socket, reply, sizeof(reply), 0, (struct sockaddr *)&from, from_length);
}

static void handle_control(struct airplay_sender *s)
{
    uint8_t request[16];
    int got = recv(s->control_socket, request, sizeof(request), MSG_DONTWAIT);
    if (got < 8 || (request[1] & 0x7F) != PT_RESEND_REQUEST) {
        return;
    }
    xSemaphoreTake(s->lock, portMAX_DELAY);
    resend(s, get16(request + 4), get16(request + 6));
    xSemaphoreGive(s->lock);
}

/* Notices the speaker closing the RTSP connection between requests. */
static void check_rtsp(struct airplay_sender *s)
{
    if (xSemaphoreTake(s->rtsp_lock, 0) != pdTRUE) {
        vTaskDelay(1); /* a request is reading its response */
        return;
    }
    char byte[64];
    int got = recv(s->rtsp, byte, sizeof(byte), MSG_DONTWAIT);
    bool closed = got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
    xSemaphoreGive(s->rtsp_lock);
    if (closed && s->connected) {
        ESP_LOGW(TAG, "The speaker closed the session");
        s->connected = false;
        if (s->config.on_event != NULL) {
            s->config.on_event(s, AIRPLAY_EVENT_DISCONNECTED, s->config.event_context);
        }
    }
}

static void network_task(void *argument)
{
    struct airplay_sender *s = argument;
    int64_t last_keepalive_us = esp_timer_get_time();
    while (!s->task_stop) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s->timing_socket, &readable);
        FD_SET(s->control_socket, &readable);
        int highest = s->timing_socket > s->control_socket ? s->timing_socket : s->control_socket;
        if (s->connected) {
            FD_SET(s->rtsp, &readable);
            highest = s->rtsp > highest ? s->rtsp : highest;
        }
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
        int ready = select(highest + 1, &readable, NULL, NULL, &timeout);
        if (ready > 0) {
            if (FD_ISSET(s->timing_socket, &readable)) {
                answer_timing(s);
            }
            if (FD_ISSET(s->control_socket, &readable)) {
                handle_control(s);
            }
            if (s->connected && FD_ISSET(s->rtsp, &readable)) {
                check_rtsp(s);
            }
        }

        int64_t now_us = esp_timer_get_time();
        xSemaphoreTake(s->lock, portMAX_DELAY);
        if (s->playing && now_us - s->last_sync_us >= SYNC_INTERVAL_US) {
            send_sync(s, false);
        }
        xSemaphoreGive(s->lock);

        if (s->connected && now_us - last_keepalive_us >= KEEPALIVE_INTERVAL_US) {
            last_keepalive_us = now_us;
            rtsp_request(s, "OPTIONS", "*", NULL, NULL, NULL, 0, NULL);
        }
    }
    ESP_LOGI(TAG, "Network task stack headroom: %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(s->task_done);
    vTaskDelete(NULL);
}

/* ---- Setup and teardown ------------------------------------------------- */

static int open_udp(uint16_t *port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return -1;
    }
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
    socklen_t length = sizeof(local);
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        getsockname(sock, (struct sockaddr *)&local, &length) != 0) {
        close(sock);
        return -1;
    }
    if (port != NULL) {
        *port = ntohs(local.sin_port);
    }
    return sock;
}

static void free_sender(struct airplay_sender *s)
{
    if (s->task != NULL) {
        s->task_stop = true;
        xSemaphoreTake(s->task_done, portMAX_DELAY);
    }
    int sockets[] = {s->rtsp, s->audio_socket, s->control_socket, s->timing_socket};
    for (size_t i = 0; i < sizeof(sockets) / sizeof(sockets[0]); ++i) {
        if (sockets[i] >= 0) {
            close(sockets[i]);
        }
    }
    if (s->rtsp_lock != NULL) {
        vSemaphoreDelete(s->rtsp_lock);
    }
    if (s->lock != NULL) {
        vSemaphoreDelete(s->lock);
    }
    if (s->task_done != NULL) {
        vSemaphoreDelete(s->task_done);
    }
    heap_caps_free(s->backlog);
    free(s->backlog_size);
    free(s);
}

/* The number after key= in an RTSP Transport header, or 0. */
static uint16_t transport_port(const char *transport, const char *key)
{
    const char *p = strstr(transport, key);
    return p != NULL ? (uint16_t)strtoul(p + strlen(key), NULL, 10) : 0;
}

static float volume_db(int volume)
{
    return volume <= 0 ? -144.0f : -30.0f + 30.0f * (float)volume / 100.0f;
}

static esp_err_t send_volume(struct airplay_sender *s, int volume)
{
    char body[32];
    int n = snprintf(body, sizeof(body), "volume: %.6f\r\n", volume_db(volume));
    return rtsp_request(s, "SET_PARAMETER", s->url, NULL, "text/parameters", body, (size_t)n,
                        NULL);
}

/* Raises the latency to frames when that is longer. Before playing. */
static void raise_latency(struct airplay_sender *s, uint32_t frames)
{
    if (frames > s->latency_frames && frames <= 10 * AIRPLAY_SAMPLE_RATE) {
        s->latency_frames = frames;
        s->config.latency_ms =
            (uint32_t)(((uint64_t)frames * 1000 + AIRPLAY_SAMPLE_RATE - 1) / AIRPLAY_SAMPLE_RATE);
    }
}

static esp_err_t open_session(struct airplay_sender *s, const airplay_device_t *device)
{
    s->remote.sin_family = AF_INET;
    s->remote.sin_addr.s_addr = device->ip.addr;
    snprintf(s->remote_ip, sizeof(s->remote_ip), IPSTR, IP2STR(&device->ip));

    /* RTSP connection. */
    s->rtsp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->rtsp < 0) {
        return ESP_ERR_NO_MEM;
    }
    struct timeval timeout = {.tv_sec = RTSP_TIMEOUT_MS / 1000};
    setsockopt(s->rtsp, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(s->rtsp, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    int one = 1;
    setsockopt(s->rtsp, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct sockaddr_in rtsp_address = s->remote;
    rtsp_address.sin_port = htons(device->port);
    if (connect(s->rtsp, (struct sockaddr *)&rtsp_address, sizeof(rtsp_address)) != 0) {
        ESP_LOGW(TAG, "Could not connect to %s:%u", s->remote_ip, device->port);
        return ESP_ERR_NOT_FOUND;
    }
    struct sockaddr_in local;
    socklen_t local_length = sizeof(local);
    getsockname(s->rtsp, (struct sockaddr *)&local, &local_length);
    inet_ntoa_r(local.sin_addr, s->local_ip, sizeof(s->local_ip));

    /* UDP flows, and the task that answers on them, before SETUP: the
       speaker starts timing requests as soon as it has our ports. */
    uint16_t control_port = 0;
    uint16_t timing_port = 0;
    s->audio_socket = open_udp(NULL);
    s->control_socket = open_udp(&control_port);
    s->timing_socket = open_udp(&timing_port);
    if (s->audio_socket < 0 || s->control_socket < 0 || s->timing_socket < 0) {
        return ESP_ERR_NO_MEM;
    }
    s->connected = true;
    if (xTaskCreate(network_task, "airplay_net", NETWORK_TASK_STACK, s, 6, &s->task) != pdPASS) {
        s->task = NULL;
        return ESP_ERR_NO_MEM;
    }

    uint32_t session_id = esp_random();
    snprintf(s->url, sizeof(s->url), "rtsp://%s/%lu", s->local_ip, (unsigned long)session_id);

    esp_err_t result;
    if (device->auth_setup) {
        /* Not every speaker that wants it says so, and some that don't
           want it refuse it: carry on either way. */
        if (auth_setup(s) != ESP_OK) {
            ESP_LOGW(TAG, "auth-setup refused; trying without");
        }
    }
    result = rtsp_request(s, "OPTIONS", "*", NULL, NULL, NULL, 0, NULL);
    if (result != ESP_OK) {
        return result;
    }

    char sdp[512];
    int sdp_length = snprintf(sdp, sizeof(sdp),
                              "v=0\r\n"
                              "o=iTunes %lu 0 IN IP4 %s\r\n"
                              "s=iTunes\r\n"
                              "c=IN IP4 %s\r\n"
                              "t=0 0\r\n"
                              "m=audio 0 RTP/AVP 96\r\n"
                              "a=rtpmap:96 AppleLossless\r\n"
                              "a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 %d\r\n",
                              (unsigned long)session_id, s->local_ip, s->remote_ip, FRAMES,
                              AIRPLAY_SAMPLE_RATE);
    result = rtsp_request(s, "ANNOUNCE", s->url, NULL, "application/sdp", sdp,
                          (size_t)sdp_length, NULL);
    if (result != ESP_OK) {
        return result;
    }

    char transport_request[160];
    snprintf(transport_request, sizeof(transport_request),
             "Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;"
             "control_port=%u;timing_port=%u\r\n",
             control_port, timing_port);
    rtsp_response_t *response = malloc(sizeof(rtsp_response_t));
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }
    result = rtsp_request(s, "SETUP", s->url, transport_request, NULL, NULL, 0, response);
    if (result == ESP_OK) {
        char transport[256];
        char session[sizeof(s->session)];
        if (!header_value(response, "Transport", transport, sizeof(transport)) ||
            !header_value(response, "Session", session, sizeof(session))) {
            result = ESP_ERR_INVALID_RESPONSE;
        } else {
            session[strcspn(session, ";")] = '\0';
            snprintf(s->session, sizeof(s->session), "%s", session);
            s->server_port = transport_port(transport, "server_port=");
            s->remote_control_port = transport_port(transport, "control_port=");
            if (s->server_port == 0 || s->remote_control_port == 0) {
                result = ESP_ERR_INVALID_RESPONSE;
            }
            ESP_LOGI(TAG, "SETUP: audio port %u, control port %u, session %s", s->server_port,
                     s->remote_control_port, s->session);
        }
    }
    free(response);
    if (result != ESP_OK) {
        return result;
    }

    char record[96];
    snprintf(record, sizeof(record), "Range: npt=0-\r\nRTP-Info: seq=%u;rtptime=%lu\r\n", s->seq,
             (unsigned long)s->next_ts);
    response = malloc(sizeof(rtsp_response_t));
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }
    result = rtsp_request(s, "RECORD", s->url, record, NULL, NULL, 0, response);
    if (result == ESP_OK) {
        /* The speaker may say how far ahead it needs audio; a shorter
           config.latency_ms is raised to it. */
        char value[16];
        if (header_value(response, "Audio-Latency", value, sizeof(value))) {
            uint32_t frames = strtoul(value, NULL, 10);
            ESP_LOGI(TAG, "The speaker asks for %lu frames (%lu ms) of latency",
                     (unsigned long)frames, (unsigned long)((uint64_t)frames * 1000 / AIRPLAY_SAMPLE_RATE));
            raise_latency(s, frames);
        }
    }
    free(response);
    if (result != ESP_OK) {
        return result;
    }
    send_volume(s, s->volume);
    return ESP_OK;
}

/* ---- Public API --------------------------------------------------------- */

esp_err_t airplay_sender_connect(const airplay_device_t *device,
                                 const airplay_sender_config_t *config,
                                 airplay_sender_handle_t *sender)
{
    if (device == NULL || config == NULL || sender == NULL ||
        (config->channels != 1 && config->channels != 2) || config->backlog_packets == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!device->supported) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    struct airplay_sender *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s->rtsp = s->audio_socket = s->control_socket = s->timing_socket = -1;
    s->config = *config;
    s->latency_frames = (uint32_t)((uint64_t)config->latency_ms * AIRPLAY_SAMPLE_RATE / 1000);
    s->volume = config->volume < 0 ? 0 : config->volume > 100 ? 100 : config->volume;
    s->seq = (uint16_t)esp_random();
    s->next_ts = esp_random();
    s->ssrc = esp_random();
    s->marker = true;
    random_hex(s->client_instance, 16);
    random_hex(s->dacp_id, 16);
    s->active_remote = esp_random();
    s->rtsp_lock = xSemaphoreCreateMutex();
    s->lock = xSemaphoreCreateMutex();
    s->task_done = xSemaphoreCreateBinary();
    size_t backlog_bytes = (size_t)config->backlog_packets * PACKET_SIZE;
    s->backlog = heap_caps_malloc(backlog_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s->backlog == NULL) {
        s->backlog = heap_caps_malloc(backlog_bytes, MALLOC_CAP_8BIT);
    }
    s->backlog_size = calloc(config->backlog_packets, sizeof(uint16_t));
    if (s->rtsp_lock == NULL || s->lock == NULL || s->task_done == NULL || s->backlog == NULL ||
        s->backlog_size == NULL) {
        free_sender(s);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Connecting to %s at " IPSTR ":%u", device->name, IP2STR(&device->ip),
             device->port);
    esp_err_t result = open_session(s, device);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not open a session with %s: %s", device->name,
                 esp_err_to_name(result));
        free_sender(s);
        return result;
    }
    ESP_LOGI(TAG, "Connected to %s; latency %lu ms", device->name,
             (unsigned long)s->config.latency_ms);
    *sender = s;
    return ESP_OK;
}

/* Sends the full chunk once our clock is near its timestamp. */
static esp_err_t send_chunk_paced(struct airplay_sender *s, TickType_t start, TickType_t timeout)
{
    while (true) {
        if (!s->connected) {
            return ESP_ERR_INVALID_STATE;
        }
        xSemaphoreTake(s->lock, portMAX_DELAY);
        int64_t now_us = esp_timer_get_time();
        if (!s->playing) {
            /* Start (or restart after stop): our clock reads next_ts now. */
            s->playing = true;
            s->marker = true;
            s->anchor_us = now_us;
            s->anchor_ts = s->next_ts;
            send_sync(s, true);
        }
        uint32_t now_ts = clock_ts(s, now_us);
        int32_t ahead = (int32_t)(s->next_ts - now_ts);
        if (ahead < -(int32_t)(s->latency_frames / 2)) {
            /* The app fell behind by too much to catch up: skip to now,
               and the speaker plays the gap as silence. */
            s->next_ts = now_ts;
            ahead = 0;
        }
        /* Audio may be sent config.lead_packets ahead of our clock. */
        int32_t lead = (int32_t)s->config.lead_packets * FRAMES;
        if (ahead <= lead) {
            send_chunk(s);
            xSemaphoreGive(s->lock);
            return ESP_OK;
        }
        xSemaphoreGive(s->lock);

        TickType_t wait = pdMS_TO_TICKS((uint32_t)(ahead - lead) * 1000 / AIRPLAY_SAMPLE_RATE);
        if (wait == 0) {
            wait = 1;
        }
        if (timeout != portMAX_DELAY) {
            TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= timeout) {
                return ESP_ERR_TIMEOUT;
            }
            if (wait > timeout - elapsed) {
                wait = timeout - elapsed;
            }
        }
        vTaskDelay(wait);
    }
}

esp_err_t airplay_sender_write(airplay_sender_handle_t s, const int16_t *pcm, size_t frames,
                               TickType_t timeout, size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    if (s == NULL || (pcm == NULL && frames > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s->connected) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t start = xTaskGetTickCount();
    size_t done = 0;
    while (true) {
        if (s->chunk_frames == FRAMES) {
            esp_err_t result = send_chunk_paced(s, start, timeout);
            if (result != ESP_OK) {
                return result;
            }
        }
        if (done == frames) {
            return ESP_OK;
        }
        size_t take = FRAMES - s->chunk_frames;
        if (take > frames - done) {
            take = frames - done;
        }
        int16_t *dst = s->chunk + s->chunk_frames * 2;
        const int16_t *src = pcm + done * s->config.channels;
        if (s->config.channels == 2) {
            memcpy(dst, src, take * 2 * sizeof(int16_t));
        } else {
            for (size_t i = 0; i < take; ++i) {
                dst[2 * i] = dst[2 * i + 1] = src[i];
            }
        }
        s->chunk_frames += take;
        done += take;
        if (written != NULL) {
            *written = done;
        }
    }
}

uint32_t airplay_sender_latency_ms(airplay_sender_handle_t s)
{
    return s != NULL ? s->config.latency_ms : 0;
}

esp_err_t airplay_sender_set_volume(airplay_sender_handle_t s, int volume)
{
    if (s == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s->connected) {
        return ESP_ERR_INVALID_STATE;
    }
    s->volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    return send_volume(s, s->volume);
}

int airplay_sender_get_volume(airplay_sender_handle_t s)
{
    return s != NULL ? s->volume : 0;
}

esp_err_t airplay_sender_stop(airplay_sender_handle_t s)
{
    if (s == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s->connected) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s->lock, portMAX_DELAY);
    bool was_playing = s->playing;
    s->playing = false;
    s->chunk_frames = 0;
    uint16_t seq = s->seq;
    uint32_t ts = s->next_ts;
    xSemaphoreGive(s->lock);
    if (!was_playing) {
        return ESP_OK;
    }
    char info[64];
    snprintf(info, sizeof(info), "RTP-Info: seq=%u;rtptime=%lu\r\n", seq, (unsigned long)ts);
    return rtsp_request(s, "FLUSH", s->url, info, NULL, NULL, 0, NULL);
}

esp_err_t airplay_sender_disconnect(airplay_sender_handle_t s)
{
    if (s == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s->task_stop = true;
    xSemaphoreTake(s->task_done, portMAX_DELAY);
    s->task = NULL;
    if (s->connected) {
        rtsp_request(s, "TEARDOWN", s->url, NULL, NULL, NULL, 0, NULL);
        s->connected = false;
    }
    free_sender(s);
    ESP_LOGI(TAG, "Disconnected");
    return ESP_OK;
}

bool airplay_sender_is_connected(airplay_sender_handle_t s)
{
    return s != NULL && s->connected;
}
