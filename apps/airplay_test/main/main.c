/* AirPlay sender test for the Waveshare ESP32-S3-Touch-LCD-7.

   Demo of components/airplay_sender: joins the Wi-Fi network the launcher
   saved, lists the AirPlay speakers on the network, and plays a test tone
   on the one you tap, with stop, volume and disconnect buttons. */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airplay_sender.h"
#include "cyd_display.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

/* Screen lines are cut to the display's width on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_JOIN_TIMEOUT_MS 20000
#define DISCOVER_TIMEOUT_MS 3000
#define MAX_DEVICES 32

#define HEADER_ROW 0
#define FOOTER_ROW (CYD_TEXT_ROWS - 1)
#define LIST_FIRST_ROW 1
#define LIST_ROWS (CYD_TEXT_ROWS - 2)
#define ARROW_COLUMNS 3
#define ARROW_DOWN_COLUMN (CYD_TEXT_COLUMNS - ARROW_COLUMNS)
#define ARROW_UP_COLUMN (ARROW_DOWN_COLUMN - ARROW_COLUMNS - 1)

/* Player screen rows. */
#define PLAY_ROW 3
#define STOP_ROW 5
#define VOLUME_ROW 7
#define DISCONNECT_ROW 9
#define VOLUME_BUTTON_COLUMNS 9
#define VOLUME_STEP 10

#define TONE_FRAMES AIRPLAY_FRAMES_PER_PACKET
#define TONE_NOTE_FRAMES (AIRPLAY_SAMPLE_RATE / 4)

static const char *TAG = "airplay_test";

static QueueHandle_t touch_queue;
static EventGroupHandle_t wifi_events;
static esp_netif_t *sta_netif;
static char wifi_ssid[33];
static char wifi_password[65];
static bool have_wifi_settings;
static uint8_t wifi_last_reason;
static esp_netif_ip_info_t ip_info;

static airplay_device_t devices[MAX_DEVICES];
static size_t device_count;
static int list_offset;

/* The speaker in use; sender_lock covers every call on it, so the tone task
   never writes while the UI stops or disconnects. */
static airplay_sender_handle_t speaker;
static SemaphoreHandle_t sender_lock;
static volatile bool tone_on;
static volatile bool speaker_lost;

/* ---- Wi-Fi -------------------------------------------------------------- */

static void load_wifi_settings(void)
{
    nvs_handle_t nvs;
    have_wifi_settings = false;
    if (nvs_open(CYD_CONFIG_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t size = sizeof(wifi_ssid);
    have_wifi_settings = nvs_get_str(nvs, "wifi_ssid", wifi_ssid, &size) == ESP_OK &&
                         wifi_ssid[0] != '\0';
    size = sizeof(wifi_password);
    if (nvs_get_str(nvs, "wifi_pass", wifi_password, &size) != ESP_OK) {
        wifi_password[0] = '\0';
    }
    nvs_close(nvs);
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    (void)argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_last_reason = ((const wifi_event_sta_disconnected_t *)event_data)->reason;
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(wifi_events != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(sta_netif != NULL ? ESP_OK : ESP_FAIL);
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    load_wifi_settings();
    if (have_wifi_settings) {
        wifi_config_t wifi_config = {0};
        memcpy(wifi_config.sta.ssid, wifi_ssid, strnlen(wifi_ssid, sizeof(wifi_config.sta.ssid)));
        memcpy(wifi_config.sta.password, wifi_password,
               strnlen(wifi_password, sizeof(wifi_config.sta.password) - 1));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* Joins the saved network if not joined yet and waits for an address. */
static bool wifi_join(void)
{
    if ((xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) != 0) {
        return true;
    }
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(WIFI_JOIN_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        esp_wifi_disconnect();
        ESP_LOGW(TAG, "Could not join %s (reason %u)", wifi_ssid, wifi_last_reason);
        return false;
    }
    /* No modem sleep: it delays audio packets and timing replies. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_netif_get_ip_info(sta_netif, &ip_info);
    ESP_LOGI(TAG, "Joined %s: IP " IPSTR, wifi_ssid, IP2STR(&ip_info.ip));
    return true;
}

/* ---- Test tone ---------------------------------------------------------- */

/* A C major arpeggio, a quarter second a note, so dropouts are easy to hear. */
static void tone_task(void *argument)
{
    (void)argument;
    static const float notes[] = {523.25f, 659.25f, 783.99f, 1046.50f, 783.99f, 659.25f};
    static int16_t pcm[TONE_FRAMES * 2];
    float phase = 0;
    uint32_t frame = 0;
    while (true) {
        if (!tone_on) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (int i = 0; i < TONE_FRAMES; ++i, ++frame) {
            uint32_t note = (frame / TONE_NOTE_FRAMES) % (sizeof(notes) / sizeof(notes[0]));
            uint32_t in_note = frame % TONE_NOTE_FRAMES;
            /* 5 ms fade in and out per note, so the steps don't click. */
            float envelope = 1.0f;
            uint32_t fade = AIRPLAY_SAMPLE_RATE / 200;
            if (in_note < fade) {
                envelope = (float)in_note / fade;
            } else if (in_note > TONE_NOTE_FRAMES - fade) {
                envelope = (float)(TONE_NOTE_FRAMES - in_note) / fade;
            }
            phase += 2.0f * (float)M_PI * notes[note] / AIRPLAY_SAMPLE_RATE;
            if (phase > 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
            int16_t sample = (int16_t)(sinf(phase) * envelope * 12000.0f);
            pcm[2 * i] = sample;
            pcm[2 * i + 1] = sample;
        }
        xSemaphoreTake(sender_lock, portMAX_DELAY);
        if (speaker != NULL && tone_on) {
            esp_err_t result = airplay_sender_write(speaker, pcm, TONE_FRAMES, pdMS_TO_TICKS(500), NULL);
            if (result == ESP_ERR_INVALID_STATE) {
                tone_on = false;
            }
        }
        xSemaphoreGive(sender_lock);
    }
}

/* ---- Screens ------------------------------------------------------------ */

static void clear_rows(int first, int last)
{
    for (int row = first; row <= last; ++row) {
        ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_WHITE, CYD_COLOR_BLACK, ""));
    }
}

/* A full-screen message: a yellow title and up to two lines under it. */
static void show_message(const char *title, const char *line1, const char *line2)
{
    ESP_ERROR_CHECK(cyd_text_row(HEADER_ROW, CYD_COLOR_BLACK, CYD_COLOR_YELLOW, " AIRPLAY TEST"));
    clear_rows(LIST_FIRST_ROW, FOOTER_ROW);
    int row = CYD_TEXT_ROWS / 2 - 2;
    ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, title));
    if (line1 != NULL) {
        ESP_ERROR_CHECK(cyd_text_row(row + 1, CYD_COLOR_WHITE, CYD_COLOR_BLACK, line1));
    }
    if (line2 != NULL) {
        ESP_ERROR_CHECK(cyd_text_row(row + 2, CYD_COLOR_GRAY, CYD_COLOR_BLACK, line2));
    }
}

static void show_list(void)
{
    char line[CYD_TEXT_COLUMNS + 1];
    bool can_up = list_offset > 0;
    bool can_down = list_offset + LIST_ROWS < (int)device_count;

    /* Header: title (tap to search again) and the page arrows. */
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    char header[CYD_TEXT_COLUMNS + 1];
    memset(header, ' ', CYD_TEXT_COLUMNS);
    header[CYD_TEXT_COLUMNS] = '\0';
    char title[CYD_TEXT_COLUMNS + 1];
    int title_length = snprintf(title, sizeof(title), " AIRPLAY %u", (unsigned)device_count);
    memcpy(header, title, (size_t)title_length);
    for (int column = 0; column < CYD_TEXT_COLUMNS; ++column) {
        foreground[column] = CYD_COLOR_BLACK;
        background[column] = CYD_COLOR_YELLOW;
    }
    header[ARROW_UP_COLUMN + 1] = CYD_GLYPH_UP;
    header[ARROW_DOWN_COLUMN + 1] = CYD_GLYPH_DOWN;
    for (int i = 0; i < ARROW_COLUMNS; ++i) {
        foreground[ARROW_UP_COLUMN + i] = CYD_COLOR_WHITE;
        background[ARROW_UP_COLUMN + i] = can_up ? CYD_COLOR_BUTTON : CYD_COLOR_BUTTON_DISABLED;
        foreground[ARROW_DOWN_COLUMN + i] = CYD_COLOR_WHITE;
        background[ARROW_DOWN_COLUMN + i] = can_down ? CYD_COLOR_BUTTON : CYD_COLOR_BUTTON_DISABLED;
    }
    ESP_ERROR_CHECK(cyd_draw_cells(HEADER_ROW, header, foreground, background));

    clear_rows(LIST_FIRST_ROW, FOOTER_ROW - 1);
    if (device_count == 0) {
        ESP_ERROR_CHECK(cyd_text_row(CYD_TEXT_ROWS / 2 - 1, CYD_COLOR_YELLOW, CYD_COLOR_BLACK,
                                     " No AirPlay speakers"));
        ESP_ERROR_CHECK(cyd_text_row(CYD_TEXT_ROWS / 2, CYD_COLOR_GRAY, CYD_COLOR_BLACK,
                                     " Tap the title to retry"));
    }
    for (int i = 0; i < LIST_ROWS && list_offset + i < (int)device_count; ++i) {
        const airplay_device_t *device = &devices[list_offset + i];
        int name_columns = CYD_TEXT_COLUMNS - 4;
        snprintf(line, sizeof(line), " %-*.*s%3s", name_columns - 1, name_columns - 1,
                 device->name, device->supported ? "" : "NO");
        ESP_ERROR_CHECK(cyd_text_row(LIST_FIRST_ROW + i,
                                     device->supported ? CYD_COLOR_WHITE : CYD_COLOR_GRAY,
                                     device->supported ? CYD_COLOR_BUTTON : CYD_COLOR_BLACK, line));
    }

    snprintf(line, sizeof(line), " " IPSTR " %.10s", IP2STR(&ip_info.ip), wifi_ssid);
    ESP_ERROR_CHECK(cyd_text_row(FOOTER_ROW, CYD_COLOR_GRAY, CYD_COLOR_BLACK, line));
}

/* Why a speaker can't be used, in a few words. */
static const char *unsupported_reason(const airplay_device_t *device)
{
    if (device->ip.addr == 0) {
        return " No IPv4 address";
    }
    if (device->password) {
        return " Needs a password";
    }
    if (!device->plain_audio) {
        return " Needs encryption";
    }
    if (!device->alac) {
        return " No ALAC support";
    }
    return " Needs TCP audio";
}

static void show_status(const char *status)
{
    ESP_ERROR_CHECK(cyd_text_row(FOOTER_ROW, CYD_COLOR_GRAY, CYD_COLOR_BLACK, status));
}

static void show_volume_row(void)
{
    char line[CYD_TEXT_COLUMNS + 1];
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    int volume = airplay_sender_get_volume(speaker);
    /* The two buttons, and the volume centered between them. */
    memset(line, ' ', CYD_TEXT_COLUMNS);
    line[CYD_TEXT_COLUMNS] = '\0';
    memcpy(line + 2, "VOL -", 5);
    memcpy(line + CYD_TEXT_COLUMNS - 7, "VOL +", 5);
    char number[8];
    int length = snprintf(number, sizeof(number), "%d%%", volume);
    memcpy(line + (CYD_TEXT_COLUMNS - length) / 2, number, (size_t)length);
    for (int column = 0; column < CYD_TEXT_COLUMNS; ++column) {
        bool button = column < VOLUME_BUTTON_COLUMNS ||
                      column >= CYD_TEXT_COLUMNS - VOLUME_BUTTON_COLUMNS;
        foreground[column] = CYD_COLOR_WHITE;
        background[column] = button ? CYD_COLOR_BUTTON : CYD_COLOR_BLACK;
    }
    ESP_ERROR_CHECK(cyd_draw_cells(VOLUME_ROW, line, foreground, background));
}

static void show_player(const airplay_device_t *device)
{
    char line[CYD_TEXT_COLUMNS + 1];
    snprintf(line, sizeof(line), " %s", device->name);
    ESP_ERROR_CHECK(cyd_text_row(HEADER_ROW, CYD_COLOR_BLACK, CYD_COLOR_YELLOW, line));
    clear_rows(1, FOOTER_ROW);
    snprintf(line, sizeof(line), " " IPSTR " %.14s", IP2STR(&device->ip), device->model);
    ESP_ERROR_CHECK(cyd_text_row(1, CYD_COLOR_GRAY, CYD_COLOR_BLACK, line));
    ESP_ERROR_CHECK(cyd_text_row(PLAY_ROW, CYD_COLOR_WHITE, CYD_COLOR_BUTTON, " PLAY TEST TONE"));
    ESP_ERROR_CHECK(cyd_text_row(STOP_ROW, CYD_COLOR_WHITE, CYD_COLOR_BUTTON, " STOP"));
    show_volume_row();
    ESP_ERROR_CHECK(cyd_text_row(DISCONNECT_ROW, CYD_COLOR_WHITE, CYD_COLOR_BUTTON, " DISCONNECT"));
}

/* ---- Interaction -------------------------------------------------------- */

static void on_speaker_event(airplay_sender_handle_t sender, airplay_event_t event, void *context)
{
    (void)sender;
    (void)context;
    if (event == AIRPLAY_EVENT_DISCONNECTED) {
        speaker_lost = true;
    }
}

/* Waits for a tap; false if the speaker dropped the session first. */
static bool wait_for_tap(cyd_touch_point_t *point)
{
    while (true) {
        if (xQueueReceive(touch_queue, point, pdMS_TO_TICKS(250)) == pdTRUE) {
            return true;
        }
        if (speaker_lost) {
            return false;
        }
    }
}

static void stop_tone(void)
{
    tone_on = false;
    xSemaphoreTake(sender_lock, portMAX_DELAY);
    airplay_sender_stop(speaker);
    xSemaphoreGive(sender_lock);
}

/* Connects to a device and runs the player screen until disconnect. */
static void play_to(const airplay_device_t *device)
{
    char line[CYD_TEXT_COLUMNS + 1];
    snprintf(line, sizeof(line), " %s", device->name);
    show_message(" Connecting", line, NULL);

    airplay_sender_config_t config = AIRPLAY_SENDER_DEFAULT_CONFIG();
    config.on_event = on_speaker_event;
    airplay_sender_handle_t sender;
    speaker_lost = false;
    esp_err_t result = airplay_sender_connect(device, &config, &sender);
    if (result != ESP_OK) {
        char reason[CYD_TEXT_COLUMNS + 1];
        snprintf(reason, sizeof(reason), " %s", esp_err_to_name(result));
        show_message(" Could not connect", line, " Tap to go back");
        show_status(reason);
        xQueueReset(touch_queue);
        cyd_touch_point_t point;
        xQueueReceive(touch_queue, &point, portMAX_DELAY);
        return;
    }
    xSemaphoreTake(sender_lock, portMAX_DELAY);
    speaker = sender;
    xSemaphoreGive(sender_lock);

    show_player(device);
    snprintf(line, sizeof(line), " Ready, latency %lu ms", (unsigned long)airplay_sender_latency_ms(sender));
    show_status(line);
    xQueueReset(touch_queue);
    cyd_touch_point_t point;
    while (wait_for_tap(&point)) {
        int row = point.y / CYD_FONT_HEIGHT;
        int column = point.x / CYD_FONT_WIDTH;
        if (row == PLAY_ROW) {
            tone_on = true;
            show_status(" Playing the arpeggio");
        } else if (row == STOP_ROW) {
            stop_tone();
            show_status(" Stopped");
        } else if (row == VOLUME_ROW) {
            int volume = airplay_sender_get_volume(speaker);
            if (column < VOLUME_BUTTON_COLUMNS) {
                volume -= VOLUME_STEP;
            } else if (column >= CYD_TEXT_COLUMNS - VOLUME_BUTTON_COLUMNS) {
                volume += VOLUME_STEP;
            } else {
                continue;
            }
            xSemaphoreTake(sender_lock, portMAX_DELAY);
            airplay_sender_set_volume(speaker, volume);
            xSemaphoreGive(sender_lock);
            show_volume_row();
        } else if (row == DISCONNECT_ROW) {
            break;
        }
    }

    bool lost = speaker_lost;
    tone_on = false;
    xSemaphoreTake(sender_lock, portMAX_DELAY);
    if (!lost) {
        airplay_sender_stop(speaker);
    }
    airplay_sender_disconnect(speaker);
    speaker = NULL;
    xSemaphoreGive(sender_lock);
    if (lost) {
        show_message(" Speaker disconnected", line, " Tap to go back");
        xQueueReset(touch_queue);
        xQueueReceive(touch_queue, &point, portMAX_DELAY);
    }
}

static void search(void)
{
    show_message(" Searching", " _raop._tcp", NULL);
    if (airplay_discover(DISCOVER_TIMEOUT_MS, devices, MAX_DEVICES, &device_count) != ESP_OK) {
        device_count = 0;
    }
    list_offset = 0;
}

/* Handles taps on the list until one asks for a new search. */
static void browse_list(void)
{
    show_list();
    xQueueReset(touch_queue);
    while (true) {
        cyd_touch_point_t point;
        xQueueReceive(touch_queue, &point, portMAX_DELAY);
        int row = point.y / CYD_FONT_HEIGHT;
        int column = point.x / CYD_FONT_WIDTH;
        if (row == HEADER_ROW) {
            if (column >= ARROW_UP_COLUMN && column < ARROW_UP_COLUMN + ARROW_COLUMNS) {
                if (list_offset > 0) {
                    list_offset = list_offset > LIST_ROWS ? list_offset - LIST_ROWS : 0;
                    show_list();
                }
            } else if (column >= ARROW_DOWN_COLUMN) {
                if (list_offset + LIST_ROWS < (int)device_count) {
                    list_offset += LIST_ROWS;
                    show_list();
                }
            } else if (column < ARROW_UP_COLUMN) {
                return;
            }
        } else if (row >= LIST_FIRST_ROW && row < LIST_FIRST_ROW + LIST_ROWS) {
            int index = list_offset + row - LIST_FIRST_ROW;
            if (index >= (int)device_count) {
                continue;
            }
            const airplay_device_t *device = &devices[index];
            if (!device->supported) {
                show_status(unsupported_reason(device));
                continue;
            }
            play_to(device);
            show_list();
            xQueueReset(touch_queue);
        }
    }
}

static void main_task(void *argument)
{
    (void)argument;
    if (!have_wifi_settings) {
        ESP_LOGW(TAG, "No Wi-Fi network saved in NVS %s", CYD_CONFIG_NAMESPACE);
        show_message(" No Wi-Fi saved", " Set one in the launcher", NULL);
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        char line[CYD_TEXT_COLUMNS + 1];
        snprintf(line, sizeof(line), " %s", wifi_ssid);
        show_message(" Joining Wi-Fi", line, NULL);
        if (!wifi_join()) {
            char reason[CYD_TEXT_COLUMNS + 1];
            snprintf(reason, sizeof(reason), " Reason %u", wifi_last_reason);
            show_message(" Could not join", line, " Tap to retry");
            show_status(reason);
            xQueueReset(touch_queue);
            cyd_touch_point_t point;
            xQueueReceive(touch_queue, &point, portMAX_DELAY);
            continue;
        }
        search();
        browse_list();
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(cyd_display_init());

    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);
    sender_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(sender_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_init();

    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));
    /* The UI outranks the tone task so it gets sender_lock between writes. */
    BaseType_t result = xTaskCreate(main_task, "ui", 8192, NULL, 6, NULL);
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    result = xTaskCreate(tone_task, "tone", 4096, NULL, 5, NULL);
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
