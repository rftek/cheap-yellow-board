#pragma once

/* AirPlay 1 (RAOP) audio sender: plays an app's PCM audio on an AirPlay
   speaker on the local network.

   Typical use, after Wi-Fi is up:

       airplay_device_t devices[16];
       size_t count;
       airplay_discover(3000, devices, 16, &count);            // 1. search
       airplay_sender_config_t config = AIRPLAY_SENDER_DEFAULT_CONFIG();
       airplay_sender_handle_t speaker;
       airplay_sender_connect(&devices[i], &config, &speaker);  // 2. pick
       airplay_sender_write(speaker, pcm, frames, portMAX_DELAY, NULL); // 3. audio
       airplay_sender_set_volume(speaker, 40);                  // 4. volume
       airplay_sender_stop(speaker);                            // 5. stop audio
       airplay_sender_disconnect(speaker);                      // 6. disconnect

   Audio is 44100 Hz, signed 16-bit, interleaved, mono or stereo, the only
   rate AirPlay 1 receivers accept. It is sent as uncompressed ALAC without
   encryption, so receivers that require a password or encrypted audio are
   reported as not supported. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIRPLAY_SAMPLE_RATE 44100
#define AIRPLAY_FRAMES_PER_PACKET 352

/* ---- 1. Search ---------------------------------------------------------- */

typedef struct {
    char name[64];       /* friendly name, such as "Living Room" */
    char hostname[64];   /* mDNS host name, without ".local" */
    esp_ip4_addr_t ip;
    uint16_t port;       /* RTSP port */
    char model[32];      /* TXT am, such as "AppleTV5,3" or "ShairportSync" */
    bool password;       /* TXT pw: the speaker asks for a password */
    bool plain_audio;    /* TXT et lists 0: accepts unencrypted audio */
    bool alac;           /* TXT cn lists 1: accepts ALAC */
    bool udp;            /* TXT tp lists UDP */
    bool auth_setup;     /* Apple AirPlay 2 speaker: send /auth-setup first */
    bool supported;      /* this sender can play to it */
} airplay_device_t;

/* Browses mDNS for _raop._tcp speakers for timeout_ms and fills up to max
   devices, supported ones first and then by name. Starts mDNS if the app
   has not. Wi-Fi must be connected. */
esp_err_t airplay_discover(uint32_t timeout_ms, airplay_device_t *devices, size_t max,
                           size_t *count);

/* ---- 2. Pick a device (connect) ----------------------------------------- */

typedef struct airplay_sender *airplay_sender_handle_t;

typedef enum {
    AIRPLAY_EVENT_DISCONNECTED, /* the speaker closed the session */
} airplay_event_t;

/* Called from the sender's network task: keep it short, and don't call
   airplay_sender_disconnect from it. */
typedef void (*airplay_event_cb_t)(airplay_sender_handle_t sender, airplay_event_t event,
                                   void *context);

typedef struct {
    uint8_t channels;          /* input channels, 1 or 2 */
    uint32_t latency_ms;       /* how long the speaker buffers before playing */
    uint16_t backlog_packets;  /* sent packets kept for resend requests */
    int volume;                /* initial volume, 0-100 */
    airplay_event_cb_t on_event;
    void *event_context;
} airplay_sender_config_t;

/* 2 s is what AirPlay 1 speakers expect; shorter works on many receivers
   (Shairport Sync) and makes a game's sound effects less late. 128 packets
   of backlog is about 1 s (182 KB, in PSRAM when there is some). */
#define AIRPLAY_SENDER_DEFAULT_CONFIG() \
    {                                   \
        .channels = 2,                  \
        .latency_ms = 2000,             \
        .backlog_packets = 128,         \
        .volume = 50,                   \
        .on_event = NULL,               \
        .event_context = NULL,          \
    }

/* Opens an RTSP session to the device and gets it ready to play. Takes a
   few hundred milliseconds; fails with ESP_ERR_NOT_SUPPORTED for a device
   whose supported flag is false. */
esp_err_t airplay_sender_connect(const airplay_device_t *device,
                                 const airplay_sender_config_t *config,
                                 airplay_sender_handle_t *sender);

/* ---- 3. Send audio ------------------------------------------------------ */

/* Queues PCM frames (interleaved int16, config.channels per frame) and
   sends them in real time: like i2s_channel_write, it blocks while the
   speaker's buffer is full, so a task that writes in a loop is paced by the
   speaker. Playback starts with the first write after connect or stop.
   If the app stops writing for longer than about half the latency, the gap
   plays as silence. written (may be NULL) gets the frames taken, which is
   fewer than frames only on a timeout. Not to be called from two tasks at
   once. */
esp_err_t airplay_sender_write(airplay_sender_handle_t sender, const int16_t *pcm,
                               size_t frames, TickType_t timeout, size_t *written);

/* Delay from a write to the speaker playing it, for syncing sound to the
   screen. */
uint32_t airplay_sender_latency_ms(airplay_sender_handle_t sender);

/* ---- 4. Volume ---------------------------------------------------------- */

/* 0-100, mapped onto the speaker's -30..0 dB range; 0 mutes. */
esp_err_t airplay_sender_set_volume(airplay_sender_handle_t sender, int volume);
int airplay_sender_get_volume(airplay_sender_handle_t sender);

/* ---- 5. Stop audio ------------------------------------------------------ */

/* Drops the audio the speaker has buffered and not played yet, so it goes
   quiet now instead of after the latency. The session stays open; the next
   write starts playing again. */
esp_err_t airplay_sender_stop(airplay_sender_handle_t sender);

/* ---- 6. Disconnect ------------------------------------------------------ */

/* Ends the session and frees the handle. No write may be in progress. */
esp_err_t airplay_sender_disconnect(airplay_sender_handle_t sender);

/* ---- Status ------------------------------------------------------------- */

/* False once the speaker has closed the session (see AIRPLAY_EVENT_DISCONNECTED). */
bool airplay_sender_is_connected(airplay_sender_handle_t sender);

#ifdef __cplusplus
}
#endif
