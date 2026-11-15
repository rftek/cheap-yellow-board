/* Wi-Fi, the speaker picker and the settings the music apps share. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airplay_sender.h"
#include "cyd_display.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "music_out.h"
#include "music_out_internal.h"
#include "nvs.h"

/* Screen lines are cut to the display's width on purpose. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_JOIN_TIMEOUT_MS 20000
#define DISCOVER_TIMEOUT_MS 3000
#define MAX_DEVICES 32

/* Speaker picker rows. */
#define HEADER_ROW 0
#define FOOTER_ROW (CYD_TEXT_ROWS - 1)
#define BUTTON_ROW (FOOTER_ROW - 1)
#define LIST_FIRST_ROW 1
#define LIST_ROWS (BUTTON_ROW - LIST_FIRST_ROW)
#define ARROW_COLUMNS 3
#define ARROW_DOWN_COLUMN (CYD_TEXT_COLUMNS - ARROW_COLUMNS)
#define ARROW_UP_COLUMN (ARROW_DOWN_COLUMN - ARROW_COLUMNS - 1)
#define BUTTON_COLUMNS (CYD_TEXT_COLUMNS / 2)

static const char *TAG = "music_out";

static const char *app_title = " MUSIC";

static QueueHandle_t touch_queue;
static EventGroupHandle_t wifi_events;
static esp_netif_t *sta_netif;
static char wifi_ssid[33];
static char wifi_password[65];
static bool have_wifi_settings;
static bool wifi_ready; /* driver initialized */
static bool wifi_running;
static uint8_t wifi_last_reason;
static esp_netif_ip_info_t ip_info;

static airplay_device_t devices[MAX_DEVICES];
static size_t device_count;
static int list_offset;

static airplay_sender_handle_t speaker;
static char speaker_name[sizeof(devices[0].name)];
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

/* Sets up the driver the first time Wi-Fi is turned on. */
static void wifi_setup(void)
{
    if (wifi_ready) {
        return;
    }
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
    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, wifi_ssid, strnlen(wifi_ssid, sizeof(wifi_config.sta.ssid)));
    memcpy(wifi_config.sta.password, wifi_password,
           strnlen(wifi_password, sizeof(wifi_config.sta.password) - 1));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    wifi_ready = true;
}

static void clear_rows(int first, int last)
{
    for (int row = first; row <= last; ++row) {
        ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_WHITE, CYD_COLOR_BLACK, ""));
    }
}

/* A full-screen message: a yellow title and up to two lines under it. */
static void show_message(const char *title, const char *line1, const char *line2)
{
    ESP_ERROR_CHECK(cyd_text_row(HEADER_ROW, CYD_COLOR_BLACK, CYD_COLOR_YELLOW, app_title));
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

bool music_out_wifi_saved(void)
{
    return have_wifi_settings;
}

bool music_out_wifi_on(void)
{
    return wifi_running;
}

bool music_out_wifi_joined(void)
{
    return wifi_running && (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

const char *music_out_wifi_ssid(void)
{
    return wifi_ssid;
}

bool music_out_wifi_start(char *status, size_t status_size)
{
    if (!have_wifi_settings) {
        snprintf(status, status_size, " Set Wi-Fi in the launcher");
        return false;
    }
    if (music_out_wifi_joined()) {
        snprintf(status, status_size, " Joined %s", wifi_ssid);
        return true;
    }
    char line[CYD_TEXT_COLUMNS + 1];
    snprintf(line, sizeof(line), " %s", wifi_ssid);
    show_message(" Joining Wi-Fi", line, NULL);
    wifi_setup();
    if (!wifi_running) {
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_running = true;
    }
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(WIFI_JOIN_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        esp_wifi_disconnect();
        ESP_LOGW(TAG, "Could not join %s (reason %u)", wifi_ssid, wifi_last_reason);
        snprintf(status, status_size, " Could not join: reason %u", wifi_last_reason);
        return false;
    }
    /* No modem sleep: it delays audio packets and timing replies. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_netif_get_ip_info(sta_netif, &ip_info);
    ESP_LOGI(TAG, "Joined %s: IP " IPSTR, wifi_ssid, IP2STR(&ip_info.ip));
    snprintf(status, status_size, " IP " IPSTR, IP2STR(&ip_info.ip));
    return true;
}

static void disconnect_speaker(void)
{
    if (speaker == NULL) {
        return;
    }
    airplay_sender_handle_t old = speaker;
    music_out_stream_set_output(NULL);
    speaker = NULL;
    speaker_name[0] = '\0';
    if (!speaker_lost) {
        airplay_sender_stop(old);
    }
    airplay_sender_disconnect(old);
    ESP_LOGI(TAG, "Speaker disconnected");
}

void music_out_wifi_stop(void)
{
    disconnect_speaker();
    if (wifi_running) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        wifi_running = false;
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi off");
    }
}

/* ---- Speaker ------------------------------------------------------------ */

static void on_speaker_event(airplay_sender_handle_t sender, airplay_event_t event, void *context)
{
    (void)sender;
    (void)context;
    if (event == AIRPLAY_EVENT_DISCONNECTED) {
        speaker_lost = true;
    }
}

const char *music_out_speaker_name(void)
{
    return speaker != NULL ? speaker_name : NULL;
}

bool music_out_check_lost(void)
{
    if (speaker == NULL || !speaker_lost) {
        return false;
    }
    disconnect_speaker();
    return true;
}

static void show_status(const char *status)
{
    ESP_ERROR_CHECK(cyd_text_row(FOOTER_ROW, CYD_COLOR_GRAY, CYD_COLOR_BLACK, status));
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
    memcpy(header, " PICK A SPEAKER", 15);
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

    clear_rows(LIST_FIRST_ROW, BUTTON_ROW - 1);
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

    /* NO SPEAKER and BACK, side by side. */
    char buttons[CYD_TEXT_COLUMNS + 1];
    snprintf(buttons, sizeof(buttons), " %-*s %s", BUTTON_COLUMNS - 2, "NO SPEAKER", "BACK");
    for (int column = 0; column < CYD_TEXT_COLUMNS; ++column) {
        foreground[column] = CYD_COLOR_WHITE;
        background[column] = column == BUTTON_COLUMNS - 1 ? CYD_COLOR_BLACK : CYD_COLOR_BUTTON;
    }
    memset(buttons + strlen(buttons), ' ', sizeof(buttons) - 1 - strlen(buttons));
    buttons[CYD_TEXT_COLUMNS] = '\0';
    ESP_ERROR_CHECK(cyd_draw_cells(BUTTON_ROW, buttons, foreground, background));

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

static void search(void)
{
    show_message(" Searching", " AirPlay speakers", NULL);
    if (airplay_discover(DISCOVER_TIMEOUT_MS, devices, MAX_DEVICES, &device_count) != ESP_OK) {
        device_count = 0;
    }
    list_offset = 0;
}

/* Connects to a device; true on success. */
static bool connect_to(const airplay_device_t *device, char *status, size_t status_size)
{
    char line[CYD_TEXT_COLUMNS + 1];
    snprintf(line, sizeof(line), " %s", device->name);
    show_message(" Connecting", line, NULL);
    disconnect_speaker();

    airplay_sender_config_t config = AIRPLAY_SENDER_DEFAULT_CONFIG();
    config.channels = 1;
    config.latency_ms = music_out_latency_setting_ms();
    config.lead_packets = 1; /* a note is sent 8 ms ahead, not 32 */
    config.on_event = on_speaker_event;
    airplay_sender_handle_t sender;
    speaker_lost = false;
    esp_err_t result = airplay_sender_connect(device, &config, &sender);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not connect to %s: %s", device->name, esp_err_to_name(result));
        snprintf(status, status_size, " Failed: %s", esp_err_to_name(result));
        return false;
    }
    ESP_LOGI(TAG, "Connected to %s, latency %lu ms", device->name,
             (unsigned long)airplay_sender_latency_ms(sender));
    speaker = sender;
    snprintf(speaker_name, sizeof(speaker_name), "%s", device->name);
    music_out_stream_set_output(sender);
    /* The speaker may have asked for a longer delay than the menu's. */
    snprintf(status, status_size, " Playing on %s, %lu ms delay", device->name,
             (unsigned long)airplay_sender_latency_ms(sender));
    return true;
}

void music_out_pick_speaker(char *status, size_t status_size)
{
    if (!music_out_wifi_joined() && !music_out_wifi_start(status, status_size)) {
        return;
    }
    search();
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
                search();
                show_list();
                xQueueReset(touch_queue);
            }
        } else if (row == BUTTON_ROW) {
            if (column < BUTTON_COLUMNS - 1) {
                disconnect_speaker();
                snprintf(status, status_size, " No speaker: silent");
                return;
            }
            if (column >= BUTTON_COLUMNS) {
                snprintf(status, status_size, " %s",
                         speaker != NULL ? speaker_name : "No speaker: silent");
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
            connect_to(device, status, status_size);
            return;
        }
    }
}

esp_err_t music_out_start(const music_out_config_t *config, QueueHandle_t queue)
{
    if (config == NULL || config->render == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->title != NULL) {
        app_title = config->title;
    }
    touch_queue = queue;
    load_wifi_settings();
    if (!have_wifi_settings) {
        ESP_LOGW(TAG, "No Wi-Fi network saved in NVS %s", CYD_CONFIG_NAMESPACE);
    }
    return music_out_stream_start(config);
}

/* ---- Menu rows ---------------------------------------------------------- */

void music_out_draw_wifi_row(int row)
{
    char line[CYD_TEXT_COLUMNS + 1];
    if (!have_wifi_settings) {
        ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_GRAY, CYD_COLOR_BUTTON_DISABLED,
                                     " WI-FI     NONE SAVED"));
        return;
    }
    if (!wifi_running) {
        snprintf(line, sizeof(line), " WI-FI     OFF");
    } else if (music_out_wifi_joined()) {
        snprintf(line, sizeof(line), " WI-FI     ON  %s", wifi_ssid);
    } else {
        snprintf(line, sizeof(line), " WI-FI     NOT JOINED");
    }
    ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_WHITE, CYD_COLOR_BUTTON, line));
}

void music_out_draw_output_row(int row)
{
    char line[CYD_TEXT_COLUMNS + 1];
    if (speaker != NULL) {
        snprintf(line, sizeof(line), " OUTPUT    %s", speaker_name);
    } else {
        snprintf(line, sizeof(line), " OUTPUT    NONE (SILENT)");
    }
    ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_WHITE, CYD_COLOR_BUTTON, line));
}

void music_out_draw_latency_row(int row)
{
    char line[CYD_TEXT_COLUMNS + 1];
    snprintf(line, sizeof(line), " AIRPLAY DELAY %lu MS",
             (unsigned long)music_out_latency_setting_ms());
    ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_WHITE, CYD_COLOR_BUTTON, line));
}

bool music_out_tap_wifi(char *status, size_t status_size)
{
    if (!have_wifi_settings) {
        snprintf(status, status_size, " Set Wi-Fi in the launcher");
        return false;
    }
    if (wifi_running) {
        music_out_wifi_stop();
        snprintf(status, status_size, " Wi-Fi off");
        return true;
    }
    music_out_wifi_start(status, status_size);
    return true;
}

/* ---- Shared settings ---------------------------------------------------- */

#define SETTINGS_NAMESPACE "music"
#define SETTINGS_LATENCY "latency_ms"
#define SETTINGS_TEMPO "tempo"

static const uint32_t latency_choices[] = {2000, 1000, 500, 250, 150, 100};

uint32_t music_out_latency_setting_ms(void)
{
    uint32_t latency = MUSIC_OUT_DEFAULT_LATENCY_MS;
    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, SETTINGS_LATENCY, &latency);
        nvs_close(nvs);
    }
    return latency;
}

uint32_t music_out_next_latency_setting(void)
{
    uint32_t latency = music_out_latency_setting_ms();
    size_t count = sizeof(latency_choices) / sizeof(latency_choices[0]);
    size_t next = 0;
    for (size_t i = 0; i < count; ++i) {
        if (latency_choices[i] == latency) {
            next = (i + 1) % count;
        }
    }
    latency = latency_choices[next];
    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, SETTINGS_LATENCY, latency);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    return latency;
}

uint16_t music_out_load_tempo(uint16_t fallback)
{
    uint16_t bpm = fallback;
    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u16(nvs, SETTINGS_TEMPO, &bpm);
        nvs_close(nvs);
    }
    if (bpm < MUSIC_TEMPO_MIN || bpm > MUSIC_TEMPO_MAX) {
        bpm = fallback;
    }
    return bpm;
}

void music_out_save_tempo(uint16_t bpm)
{
    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        uint16_t saved = 0;
        if (nvs_get_u16(nvs, SETTINGS_TEMPO, &saved) != ESP_OK || saved != bpm) {
            nvs_set_u16(nvs, SETTINGS_TEMPO, bpm);
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
}
