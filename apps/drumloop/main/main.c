/* DrumLoop for the Waveshare ESP32-S3-Touch-LCD-7.

   A live drum machine: eight 808-style sounds on touch pads and a step
   sequencer of up to 16 bars, played on an AirPlay speaker through
   components/airplay_sender. This file joins Wi-Fi, lets the player pick a
   speaker (or skip it and play silent) and connects; ui.c is the performance screen, audio.c streams
   the engine (engine.c) to the speaker, kit.c makes the sounds and
   export.c renders the loop for a future export. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airplay_sender.h"
#include "audio.h"
#include "cyd_display.h"
#include "engine.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kit.h"
#include "nvs_flash.h"
#include "ui.h"

/* Screen lines are cut to the display's width on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_JOIN_TIMEOUT_MS 20000
#define DISCOVER_TIMEOUT_MS 3000
#define MAX_DEVICES 32
#define DEFAULT_BPM 140

#define HEADER_ROW 0
#define FOOTER_ROW (CYD_TEXT_ROWS - 1)
#define LIST_FIRST_ROW 1
#define SKIP_ROW (FOOTER_ROW - 1)
#define LIST_ROWS (SKIP_ROW - LIST_FIRST_ROW)
#define ARROW_COLUMNS 3
#define ARROW_DOWN_COLUMN (CYD_TEXT_COLUMNS - ARROW_COLUMNS)
#define ARROW_UP_COLUMN (ARROW_DOWN_COLUMN - ARROW_COLUMNS - 1)

static const char *TAG = "drumloop";

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
static volatile bool speaker_lost;
/* Never set: there is no speaker to lose in silent mode. */
static volatile bool no_speaker_lost;

/* The loop. It outlives speaker connections, so a beat survives a
   reconnect; it is not saved across power cycles. */
static drum_pattern_t pattern = {.bars = 1, .bpm = DEFAULT_BPM};

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

/* ---- Speaker list ------------------------------------------------------- */

static void clear_rows(int first, int last)
{
    for (int row = first; row <= last; ++row) {
        ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_WHITE, CYD_COLOR_BLACK, ""));
    }
}

/* A full-screen message: a yellow title and up to two lines under it. */
static void show_message(const char *title, const char *line1, const char *line2)
{
    ESP_ERROR_CHECK(cyd_text_row(HEADER_ROW, CYD_COLOR_BLACK, CYD_COLOR_YELLOW, " DRUMLOOP"));
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

static void show_status(const char *status)
{
    ESP_ERROR_CHECK(cyd_text_row(FOOTER_ROW, CYD_COLOR_GRAY, CYD_COLOR_BLACK, status));
}

/* The button that skips the speaker and opens the pads in silent mode. */
static void show_skip_button(void)
{
    ESP_ERROR_CHECK(cyd_text_row(SKIP_ROW, CYD_COLOR_WHITE, CYD_COLOR_BUTTON,
                                 " NO SPEAKER: PLAY SILENT"));
}

/* Waits for a tap; true if it was on the skip button. */
static bool wait_for_skip_tap(void)
{
    xQueueReset(touch_queue);
    cyd_touch_point_t point;
    xQueueReceive(touch_queue, &point, portMAX_DELAY);
    return point.y / CYD_FONT_HEIGHT == SKIP_ROW;
}

static void wait_for_tap(void)
{
    xQueueReset(touch_queue);
    cyd_touch_point_t point;
    xQueueReceive(touch_queue, &point, portMAX_DELAY);
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
    int title_length = snprintf(title, sizeof(title), " PICK A SPEAKER");
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

    clear_rows(LIST_FIRST_ROW, SKIP_ROW - 1);
    show_skip_button();
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
    show_status(line);
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

/* ---- Playing ------------------------------------------------------------ */

static void on_speaker_event(airplay_sender_handle_t sender, airplay_event_t event, void *context)
{
    (void)sender;
    (void)context;
    if (event == AIRPLAY_EVENT_DISCONNECTED) {
        speaker_lost = true;
    }
}

/* Connects to a device and runs the performance screen until disconnect. */
static void play_to(const airplay_device_t *device)
{
    char line[CYD_TEXT_COLUMNS + 1];
    snprintf(line, sizeof(line), " %s", device->name);
    show_message(" Connecting", line, NULL);

    airplay_sender_config_t config = AIRPLAY_SENDER_DEFAULT_CONFIG();
    config.channels = 1;
    config.latency_ms = ui_saved_latency_ms();
    config.on_event = on_speaker_event;
    airplay_sender_handle_t sender;
    speaker_lost = false;
    esp_err_t result = airplay_sender_connect(device, &config, &sender);
    if (result != ESP_OK) {
        char reason[CYD_TEXT_COLUMNS + 1];
        snprintf(reason, sizeof(reason), " %s", esp_err_to_name(result));
        show_message(" Could not connect", line, " Tap to go back");
        show_status(reason);
        wait_for_tap();
        return;
    }
    ESP_LOGI(TAG, "Connected to %s, latency %lu ms", device->name,
             (unsigned long)airplay_sender_latency_ms(sender));
    audio_set_speaker(sender);

    ui_result_t ui_result = ui_perform(touch_queue, device->name, &speaker_lost, &pattern);

    audio_set_speaker(NULL);
    if (ui_result != UI_SPEAKER_LOST) {
        airplay_sender_stop(sender);
    }
    airplay_sender_disconnect(sender);
    if (ui_result == UI_SPEAKER_LOST) {
        show_message(" Speaker disconnected", line, " Tap to go back");
        wait_for_tap();
    }
}

/* Runs the performance screen with no speaker, for practising on the
   pads or building a beat; the playhead runs on the board's clock. */
static void play_silent(void)
{
    ESP_LOGI(TAG, "No speaker: silent mode");
    audio_set_silent(true);
    ui_perform(touch_queue, "No speaker (silent)", &no_speaker_lost, &pattern);
    audio_set_silent(false);
}

static void search(void)
{
    show_message(" Searching", " AirPlay speakers", NULL);
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
        } else if (row == SKIP_ROW) {
            play_silent();
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
        while (true) {
            show_message(" No Wi-Fi saved", " Set one in the launcher", NULL);
            show_skip_button();
            if (wait_for_skip_tap()) {
                play_silent();
            }
        }
    }
    while (true) {
        char line[CYD_TEXT_COLUMNS + 1];
        snprintf(line, sizeof(line), " %s", wifi_ssid);
        show_message(" Joining Wi-Fi", line, NULL);
        if (!wifi_join()) {
            char reason[CYD_TEXT_COLUMNS + 1];
            snprintf(reason, sizeof(reason), " Reason %u", wifi_last_reason);
            show_message(" Could not join", line, " Tap to retry");
            show_skip_button();
            show_status(reason);
            if (wait_for_skip_tap()) {
                play_silent();
            }
            continue;
        }
        search();
        browse_list();
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(cyd_display_init());
    show_message(" Building the kit", " 808 sounds", NULL);
    ESP_ERROR_CHECK(kit_init());

    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);
    wifi_init();
    ESP_ERROR_CHECK(audio_start(&pattern));

    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));
    BaseType_t result = xTaskCreate(main_task, "ui", 8192, NULL, 5, NULL);
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
