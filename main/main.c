#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyd_display.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wifi_credentials.h"

/* Each branch of this repository is one application. GitHub Actions on the
   branch publishes its build as IMAGE_ASSET_NAME on a release tagged with
   the branch name; branches without one are not offered. */
#ifndef IMAGE_REPO
#define IMAGE_REPO "rftek/cheap-yellow-board"
#endif
#define BRANCHES_URL "https://api.github.com/repos/" IMAGE_REPO "/branches?per_page=100"
#define RELEASES_URL "https://api.github.com/repos/" IMAGE_REPO "/releases?per_page=100"
#define IMAGE_ASSET_NAME "firmware.bin"

/* Settings this launcher stores in NVS for the boot images, which are
   published publicly and so carry no credentials of their own. The
   brightness lives in the same namespace. */
#define CONFIG_NAMESPACE CYD_CONFIG_NAMESPACE

/* Countdown before the default image boots; each tap off an entry adds as much again. */
#define MENU_TIMEOUT_US (20LL * 1000 * 1000)
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define GITHUB_JSON_LIMIT (64 * 1024)
#define DOWNLOAD_CHUNK_SIZE 4096
#define MAX_REDIRECTS 5

#define SLOT_COUNT 2
#define MAX_REMOTE_IMAGES 8
#define MENU_FIRST_ROW 1
#define MENU_ROWS (CYD_TEXT_ROWS - 2)
#define STATUS_ROW (CYD_TEXT_ROWS - 1)

/* Status row layout: status text, then brightness down and up buttons. */
#define STATUS_TEXT_COLUMNS 10
#define BRIGHTNESS_DOWN_COLUMN 10
#define BRIGHTNESS_UP_COLUMN 14
#define BRIGHTNESS_BUTTON_COLUMNS 3
/* How long the new level replaces the status text after a press. */
#define BRIGHTNESS_SHOWN_US (1500LL * 1000)

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "cyd_launcher";

/* An asset's updated_at time, which changes with every upload. */
#define VERSION_SIZE 24

typedef struct {
    const esp_partition_t *partition;
    bool valid;
    char name[48];                 /* branch name */
    char version[VERSION_SIZE];    /* empty when flashed some other way */
} slot_t;

typedef struct {
    char name[48]; /* branch name */
    char url[256];
    char version[VERSION_SIZE];
    uint32_t size;
} remote_image_t;

typedef enum { ENTRY_SLOT, ENTRY_REMOTE } entry_kind_t;

typedef struct {
    entry_kind_t kind;
    int index;
} entry_t;

typedef enum {
    NET_CONNECTING,
    NET_FETCHING,
    NET_READY,
    NET_GITHUB_ERROR,
    NET_OFFLINE,
} net_state_t;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} http_buffer_t;

static QueueHandle_t touch_queue;
static EventGroupHandle_t wifi_events;
static volatile bool wifi_give_up;
static slot_t slots[SLOT_COUNT];
static remote_image_t remote_images[MAX_REMOTE_IMAGES];
static int remote_count;
/* Written by the network task; remote_images is complete once NET_READY is set. */
static volatile net_state_t net_state = NET_CONNECTING;
static entry_t entries[MENU_ROWS];
static int entry_count;
static int64_t brightness_shown_until;

/* ---- Stored images ---- */

static void slot_key(char *key, size_t size, const char *field, int slot)
{
    snprintf(key, size, "%s%d", field, slot);
}

static void load_slots(void)
{
    nvs_handle_t nvs;
    bool have_nvs = nvs_open("launcher", NVS_READONLY, &nvs) == ESP_OK;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        slot_t *slot = &slots[i];
        memset(slot, 0, sizeof(*slot));
        slot->partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0 + i, NULL);
        esp_app_desc_t description;
        slot->valid = slot->partition != NULL &&
                      esp_ota_get_partition_description(slot->partition, &description) ==
                          ESP_OK;
        if (!slot->valid) {
            continue;
        }
        char key[16];
        size_t name_size = sizeof(slot->name);
        slot_key(key, sizeof(key), "name", i);
        if (!have_nvs || nvs_get_str(nvs, key, slot->name, &name_size) != ESP_OK) {
            /* Flashed over USB rather than downloaded: use the project name. */
            snprintf(slot->name, sizeof(slot->name), "%s", description.project_name);
        }
        size_t version_size = sizeof(slot->version);
        slot_key(key, sizeof(key), "ver", i);
        if (!have_nvs || nvs_get_str(nvs, key, slot->version, &version_size) != ESP_OK) {
            slot->version[0] = '\0';
        }
        /* Images are named after their branch, so a different project name
           means the slot was reflashed over USB since the download. */
        if (strcmp(slot->name, description.project_name) != 0) {
            snprintf(slot->name, sizeof(slot->name), "%s", description.project_name);
            slot->version[0] = '\0';
        }
        ESP_LOGI(TAG, "Slot %c: %s (%s)", 'A' + i, slot->name,
                 slot->version[0] != '\0' ? slot->version : "flashed over USB");
    }
    if (have_nvs) {
        nvs_close(nvs);
    }
}

/* Records which branch and version a slot holds; NULL name clears it. */
static void save_slot(int slot, const char *name, const char *version)
{
    nvs_handle_t nvs;
    if (nvs_open("launcher", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    char name_key[16];
    char version_key[16];
    slot_key(name_key, sizeof(name_key), "name", slot);
    slot_key(version_key, sizeof(version_key), "ver", slot);
    if (name != NULL) {
        nvs_set_str(nvs, name_key, name);
        nvs_set_str(nvs, version_key, version);
    } else {
        nvs_erase_key(nvs, name_key);
        nvs_erase_key(nvs, version_key);
    }
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* Writes key only when it differs, so unchanged settings cost no flash wear. */
static void store_config_string(nvs_handle_t nvs, const char *key, const char *value)
{
    char current[128];
    size_t size = sizeof(current);
    if (nvs_get_str(nvs, key, current, &size) == ESP_OK && strcmp(current, value) == 0) {
        return;
    }
    nvs_set_str(nvs, key, value);
}

/* Shares this launcher's private settings with the boot images. */
static void store_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS for settings");
        return;
    }
    store_config_string(nvs, "wifi_ssid", WIFI_SSID);
    store_config_string(nvs, "wifi_pass", WIFI_PASSWORD);
#ifdef BIRDNET_BASE_URL
    store_config_string(nvs, "birdnet_url", BIRDNET_BASE_URL);
#endif
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* The image that boots when nobody chooses: the last one chosen, else the
   first stored one. */
static int default_slot(void)
{
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (slots[i].valid && boot != NULL && boot->address == slots[i].partition->address) {
            return i;
        }
    }
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (slots[i].valid) {
            return i;
        }
    }
    return -1;
}

/* ---- Screen ---- */

static void draw_title(const char *text)
{
    ESP_ERROR_CHECK(cyd_text_row(0, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, text));
}

/* Network status, or the new brightness just after a press, then the
   brightness buttons, dimmed at either end of the range. */
static void draw_status(void)
{
    char status[16];
    if (esp_timer_get_time() < brightness_shown_until) {
        snprintf(status, sizeof(status), "%d%% light", cyd_get_brightness());
    } else {
        switch (net_state) {
        case NET_CONNECTING:
            snprintf(status, sizeof(status), "Wi-Fi...");
            break;
        case NET_FETCHING:
            snprintf(status, sizeof(status), "GitHub...");
            break;
        case NET_READY:
            snprintf(status, sizeof(status), "%d online", remote_count);
            break;
        case NET_GITHUB_ERROR:
            snprintf(status, sizeof(status), "no GitHub");
            break;
        default:
            snprintf(status, sizeof(status), "offline");
            break;
        }
    }

    char text[CYD_TEXT_COLUMNS + 1];
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    memset(text, ' ', CYD_TEXT_COLUMNS);
    text[CYD_TEXT_COLUMNS] = '\0';
    memcpy(text, status, strnlen(status, STATUS_TEXT_COLUMNS));
    for (int i = 0; i < CYD_TEXT_COLUMNS; ++i) {
        foreground[i] = CYD_COLOR_GRAY;
        background[i] = CYD_COLOR_BLACK;
    }
    bool can_dim = cyd_get_brightness() > CYD_BRIGHTNESS_MIN;
    bool can_brighten = cyd_get_brightness() < CYD_BRIGHTNESS_MAX;
    for (int i = 0; i < BRIGHTNESS_BUTTON_COLUMNS; ++i) {
        background[BRIGHTNESS_DOWN_COLUMN + i] = CYD_COLOR_BUTTON;
        foreground[BRIGHTNESS_DOWN_COLUMN + i] =
            can_dim ? CYD_COLOR_WHITE : CYD_COLOR_BUTTON_DISABLED;
        background[BRIGHTNESS_UP_COLUMN + i] = CYD_COLOR_BUTTON;
        foreground[BRIGHTNESS_UP_COLUMN + i] =
            can_brighten ? CYD_COLOR_WHITE : CYD_COLOR_BUTTON_DISABLED;
    }
    text[BRIGHTNESS_DOWN_COLUMN + 1] = CYD_GLYPH_DOWN;
    text[BRIGHTNESS_UP_COLUMN + 1] = CYD_GLYPH_UP;
    ESP_ERROR_CHECK(cyd_draw_cells(STATUS_ROW, text, foreground, background));
}

/* Steps the backlight by delta percent and saves it for every image. */
static void change_brightness(int delta)
{
    int before = cyd_get_brightness();
    ESP_ERROR_CHECK(cyd_set_brightness(before + delta));
    int after = cyd_get_brightness();
    if (after != before) {
        nvs_handle_t nvs;
        if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_u8(nvs, CYD_CONFIG_BRIGHTNESS, (uint8_t)after);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        ESP_LOGI(TAG, "Brightness %d%%", after);
    }
    brightness_shown_until = esp_timer_get_time() + BRIGHTNESS_SHOWN_US;
    draw_status();
}

/* True when a slot already holds this exact build of the branch. */
static bool remote_is_stored(const remote_image_t *image)
{
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (slots[i].valid && strcmp(slots[i].name, image->name) == 0 &&
            strcmp(slots[i].version, image->version) == 0) {
            return true;
        }
    }
    return false;
}

/* Stored images first, then branches whose current build is not stored:
   new branches and newer builds of stored ones. */
static void build_entries(void)
{
    entry_count = 0;
    for (int i = 0; i < SLOT_COUNT && entry_count < MENU_ROWS; ++i) {
        if (slots[i].valid) {
            entries[entry_count++] = (entry_t){.kind = ENTRY_SLOT, .index = i};
        }
    }
    if (net_state != NET_READY) {
        return;
    }
    for (int i = 0; i < remote_count && entry_count < MENU_ROWS; ++i) {
        if (!remote_is_stored(&remote_images[i])) {
            entries[entry_count++] = (entry_t){.kind = ENTRY_REMOTE, .index = i};
        }
    }
}

static void draw_entry(int index, bool highlighted)
{
    char line[CYD_TEXT_COLUMNS + 1];
    uint16_t color = CYD_COLOR_WHITE;
    const entry_t *entry = &entries[index];
    if (entry->kind == ENTRY_SLOT) {
        /* "A*name": slot letter, '*' on the image that boots by default. */
        snprintf(line, sizeof(line), "%c%c%.15s", 'A' + entry->index,
                 entry->index == default_slot() ? '*' : ' ', slots[entry->index].name);
    } else {
        snprintf(line, sizeof(line), "+ %.15s", remote_images[entry->index].name);
        color = CYD_COLOR_CYAN;
    }
    if (highlighted) {
        ESP_ERROR_CHECK(cyd_text_row(MENU_FIRST_ROW + index, CYD_COLOR_BLACK, CYD_COLOR_WHITE,
                                     line));
    } else {
        ESP_ERROR_CHECK(cyd_text_row(MENU_FIRST_ROW + index, color, CYD_COLOR_BUTTON, line));
    }
}

static void draw_entries(void)
{
    for (int i = 0; i < MENU_ROWS; ++i) {
        if (i < entry_count) {
            draw_entry(i, false);
        } else {
            ESP_ERROR_CHECK(cyd_text_row(MENU_FIRST_ROW + i, CYD_COLOR_WHITE, CYD_COLOR_BLACK,
                                         ""));
        }
    }
}

static void draw_menu(const char *title)
{
    draw_title(title);
    draw_entries();
    draw_status();
}

/* Full-screen message; returns after the next tap. */
static void show_message_and_wait(const char *title, const char *line1, const char *line2)
{
    for (int row = 0; row < CYD_TEXT_ROWS; ++row) {
        const char *text = row == 0 ? title
                           : row == 2 ? line1
                           : row == 3 ? line2
                           : row == STATUS_ROW ? "tap to continue"
                                               : "";
        uint16_t color = row == 0 ? CYD_COLOR_RED
                         : row == STATUS_ROW ? CYD_COLOR_GRAY
                                             : CYD_COLOR_WHITE;
        ESP_ERROR_CHECK(cyd_text_row(row, color, CYD_COLOR_BLACK, text != NULL ? text : ""));
    }
    cyd_touch_point_t point;
    xQueueReset(touch_queue);
    xQueueReceive(touch_queue, &point, portMAX_DELAY);
}

/* ---- Booting ---- */

static void boot_slot(int slot)
{
    const esp_partition_t *partition = slots[slot].partition;
    const esp_partition_t *current = esp_ota_get_boot_partition();
    /* Skip the otadata write when the choice is unchanged. */
    esp_err_t result = current != NULL && current->address == partition->address
                           ? ESP_OK
                           : esp_ota_set_boot_partition(partition);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Slot %c is not bootable: %s", 'A' + slot, esp_err_to_name(result));
        show_message_and_wait("BOOT FAILED", slots[slot].name, esp_err_to_name(result));
        return;
    }
    ESP_LOGI(TAG, "Booting slot %c: %s", 'A' + slot, slots[slot].name);
    esp_restart();
}

/* ---- Network ---- */

static void wifi_event_handler(void *argument, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    (void)argument;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        if (!wifi_give_up) {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(void)
{
    wifi_events = xEventGroupCreate();
    if (wifi_events == NULL || esp_netif_init() != ESP_OK ||
        esp_event_loop_create_default() != ESP_OK ||
        esp_netif_create_default_wifi_sta() == NULL) {
        return false;
    }
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_config) != ESP_OK) {
        return false;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s",
             WIFI_PASSWORD);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        wifi_give_up = true;
        esp_wifi_stop();
        return false;
    }
    return true;
}

static esp_err_t http_buffer_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    http_buffer_t *buffer = event->user_data;
    size_t needed = buffer->length + (size_t)event->data_len + 1;
    if (needed > GITHUB_JSON_LIMIT) {
        ESP_LOGE(TAG, "GitHub response exceeds %d bytes", GITHUB_JSON_LIMIT);
        return ESP_ERR_INVALID_SIZE;
    }
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity * 2 > needed ? buffer->capacity * 2 : needed;
        char *data = realloc(buffer->data, capacity);
        if (data == NULL) {
            return ESP_ERR_NO_MEM;
        }
        buffer->data = data;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, event->data, event->data_len);
    buffer->length += event->data_len;
    buffer->data[buffer->length] = '\0';
    return ESP_OK;
}

static const char *skip_whitespace(const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }
    return text;
}

/* Finds "key": in [from, limit) and returns its value, or NULL. */
static const char *json_value(const char *from, const char *limit, const char *key)
{
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    size_t pattern_length = strlen(pattern);
    for (const char *found = strstr(from, pattern); found != NULL && found < limit;
         found = strstr(found + 1, pattern)) {
        const char *value = skip_whitespace(found + pattern_length);
        if (*value == ':') {
            return skip_whitespace(value + 1);
        }
    }
    return NULL;
}

/* Like json_value, but the last match before limit. */
static const char *json_last_value(const char *from, const char *limit, const char *key)
{
    const char *last = NULL;
    for (const char *value = json_value(from, limit, key); value != NULL;
         value = json_value(value, limit, key)) {
        last = value;
    }
    return last;
}

static bool json_copy_string(const char *value, char *output, size_t size)
{
    if (value == NULL || *value != '"') {
        return false;
    }
    size_t length = 0;
    for (++value; *value != '\0' && *value != '"'; ++value) {
        if (*value == '\\' && value[1] != '\0') {
            ++value;
        }
        if (length + 1 < size) {
            output[length++] = *value;
        }
    }
    output[length] = '\0';
    return *value == '"';
}

/* The launcher itself and the legacy main branch are not boot images. */
static bool branch_is_excluded(const char *branch)
{
    static const char *const excluded[] = {"bootloader", "main"};
    for (size_t i = 0; i < sizeof(excluded) / sizeof(excluded[0]); ++i) {
        if (strcmp(branch, excluded[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Finds IMAGE_ASSET_NAME on the release tagged with the branch name. Each
   asset object ends with its browser_download_url; its name, size and
   updated_at come earlier in the same object. */
static bool find_branch_image(const char *json, const char *branch, remote_image_t *image)
{
    const char *end = json + strlen(json);
    for (const char *tag = json_value(json, end, "tag_name"); tag != NULL;
         tag = json_value(tag, end, "tag_name")) {
        char tag_name[48];
        if (!json_copy_string(tag, tag_name, sizeof(tag_name)) || strcmp(tag_name, branch) != 0) {
            continue;
        }
        const char *release_end = json_value(tag, end, "tag_name");
        if (release_end == NULL) {
            release_end = end;
        }
        const char *object_start = json_value(tag, release_end, "assets");
        if (object_start == NULL) {
            return false;
        }
        for (const char *url = json_value(object_start, release_end, "browser_download_url");
             url != NULL; url = json_value(url, release_end, "browser_download_url")) {
            char asset_name[48];
            if (json_copy_string(json_last_value(object_start, url, "name"), asset_name,
                                 sizeof(asset_name)) &&
                strcmp(asset_name, IMAGE_ASSET_NAME) == 0 &&
                json_copy_string(url, image->url, sizeof(image->url)) &&
                json_copy_string(json_last_value(object_start, url, "updated_at"),
                                 image->version, sizeof(image->version))) {
                const char *size = json_last_value(object_start, url, "size");
                image->size = size != NULL ? (uint32_t)strtoul(size, NULL, 10) : 0;
                snprintf(image->name, sizeof(image->name), "%s", branch);
                return true;
            }
            object_start = url;
        }
        return false;
    }
    return false;
}

/* GETs url into buffer, reusing the client's connection. */
static esp_err_t github_get(esp_http_client_handle_t client, http_buffer_t *buffer,
                            const char *url)
{
    buffer->length = 0;
    if (buffer->data != NULL) {
        buffer->data[0] = '\0';
    }
    esp_http_client_set_url(client, url);
    esp_err_t result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "GitHub request failed: %s", esp_err_to_name(result));
        return result;
    }
    if (status != 200 || buffer->data == NULL) {
        ESP_LOGW(TAG, "GitHub returned HTTP %d for %s", status, url);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* Lists the repository's branches and keeps those with a published image. */
static net_state_t fetch_branch_images(void)
{
    http_buffer_t buffer = {0};
    esp_http_client_config_t config = {
        .url = BRANCHES_URL,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_buffer_handler,
        .user_data = &buffer,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return NET_OFFLINE;
    }
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    net_state_t state = NET_GITHUB_ERROR;
    char branches[MAX_REMOTE_IMAGES][48];
    int branch_count = 0;
    if (github_get(client, &buffer, BRANCHES_URL) == ESP_OK) {
        const char *end = buffer.data + buffer.length;
        for (const char *name = json_value(buffer.data, end, "name");
             name != NULL && branch_count < MAX_REMOTE_IMAGES;
             name = json_value(name, end, "name")) {
            if (json_copy_string(name, branches[branch_count], sizeof(branches[0])) &&
                !branch_is_excluded(branches[branch_count])) {
                ++branch_count;
            }
        }
        if (github_get(client, &buffer, RELEASES_URL) == ESP_OK) {
            remote_count = 0;
            for (int i = 0; i < branch_count; ++i) {
                remote_image_t *image = &remote_images[remote_count];
                if (find_branch_image(buffer.data, branches[i], image)) {
                    ESP_LOGI(TAG, "Branch %s: %s, %u bytes", image->name, image->version,
                             (unsigned)image->size);
                    ++remote_count;
                } else {
                    ESP_LOGW(TAG, "Branch %s has no %s release yet", branches[i],
                             IMAGE_ASSET_NAME);
                }
            }
            state = NET_READY;
        }
    }
    esp_http_client_cleanup(client);
    free(buffer.data);
    return state;
}

static void network_task(void *argument)
{
    (void)argument;
    if (!wifi_connect()) {
        ESP_LOGW(TAG, "Wi-Fi did not connect; keeping stored images only");
        net_state = NET_OFFLINE;
    } else {
        net_state = NET_FETCHING;
        net_state = fetch_branch_images();
    }
    vTaskDelete(NULL);
}

/* ---- Downloading ---- */

/* Replaces an older copy of the same image, else fills an empty slot, else
   overwrites the slot that does not boot by default. */
static int target_slot_for(const remote_image_t *image)
{
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (slots[i].valid && strcmp(slots[i].name, image->name) == 0) {
            return i;
        }
    }
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (!slots[i].valid) {
            return i;
        }
    }
    return default_slot() == 0 ? 1 : 0;
}

static void draw_download_progress(const char *phase, size_t received, size_t total)
{
    char line[CYD_TEXT_COLUMNS + 1];
    if (total > 0) {
        snprintf(line, sizeof(line), "%s %u/%uK", phase, (unsigned)(received / 1024),
                 (unsigned)(total / 1024));
    } else {
        snprintf(line, sizeof(line), "%s %uK", phase, (unsigned)(received / 1024));
    }
    ESP_ERROR_CHECK(cyd_text_row(4, CYD_COLOR_WHITE, CYD_COLOR_BLACK, line));
}

/* Opens url, following redirects (GitHub serves assets from another host).
   Returns the content length, or -1 on failure. */
static int64_t http_open_following_redirects(esp_http_client_handle_t client)
{
    for (int redirects = 0; redirects <= MAX_REDIRECTS; ++redirects) {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            return -1;
        }
        int64_t length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            return length;
        }
        if (status < 300 || status >= 400) {
            ESP_LOGE(TAG, "Download returned HTTP %d", status);
            return -1;
        }
        esp_http_client_flush_response(client, NULL);
        if (esp_http_client_set_redirection(client) != ESP_OK) {
            return -1;
        }
    }
    ESP_LOGE(TAG, "Too many redirects");
    return -1;
}

static esp_err_t download_image(const remote_image_t *image, int slot_index)
{
    slot_t *slot = &slots[slot_index];
    char line[CYD_TEXT_COLUMNS + 1];
    for (int row = 0; row < CYD_TEXT_ROWS; ++row) {
        ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_WHITE, CYD_COLOR_BLACK, ""));
    }
    ESP_ERROR_CHECK(cyd_text_row(0, CYD_COLOR_CYAN, CYD_COLOR_BLACK, "DOWNLOADING"));
    snprintf(line, sizeof(line), "%.17s", image->name);
    ESP_ERROR_CHECK(cyd_text_row(2, CYD_COLOR_WHITE, CYD_COLOR_BLACK, line));
    snprintf(line, sizeof(line), "into slot %c", 'A' + slot_index);
    ESP_ERROR_CHECK(cyd_text_row(3, CYD_COLOR_GRAY, CYD_COLOR_BLACK, line));
    draw_download_progress("connect", 0, 0);

    esp_http_client_config_t config = {
        .url = image->url,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char *chunk = NULL;
    esp_ota_handle_t ota = 0;
    bool ota_started = false;
    size_t received = 0;
    esp_err_t result = ESP_FAIL;

    int64_t length = http_open_following_redirects(client);
    if (length < 0) {
        goto cleanup;
    }
    if (length > (int64_t)slot->partition->size) {
        ESP_LOGE(TAG, "Image is %lld bytes; slot holds %u", length,
                 (unsigned)slot->partition->size);
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    chunk = malloc(DOWNLOAD_CHUNK_SIZE);
    if (chunk == NULL) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    /* The slot's old contents are gone from here on. */
    slot->valid = false;
    save_slot(slot_index, NULL, NULL);
    draw_download_progress("erase", 0, (size_t)length);
    result = esp_ota_begin(slot->partition, length > 0 ? (size_t)length : OTA_SIZE_UNKNOWN,
                           &ota);
    if (result != ESP_OK) {
        goto cleanup;
    }
    ota_started = true;

    size_t next_progress = 0;
    while (true) {
        int bytes = esp_http_client_read(client, chunk, DOWNLOAD_CHUNK_SIZE);
        if (bytes < 0) {
            result = ESP_FAIL;
            goto cleanup;
        }
        if (bytes == 0) {
            break;
        }
        result = esp_ota_write(ota, chunk, bytes);
        if (result != ESP_OK) {
            goto cleanup;
        }
        received += (size_t)bytes;
        if (received >= next_progress) {
            draw_download_progress("got", received, (size_t)length);
            next_progress = received + 32 * 1024;
        }
    }
    if (length > 0 && received != (size_t)length) {
        ESP_LOGE(TAG, "Download ended at %u of %lld bytes", (unsigned)received, length);
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    draw_download_progress("verify", received, (size_t)length);
    ota_started = false;
    result = esp_ota_end(ota);
    if (result == ESP_OK) {
        snprintf(slot->name, sizeof(slot->name), "%s", image->name);
        snprintf(slot->version, sizeof(slot->version), "%s", image->version);
        slot->valid = true;
        save_slot(slot_index, image->name, image->version);
        ESP_LOGI(TAG, "Stored %s in slot %c (%u bytes)", image->name, 'A' + slot_index,
                 (unsigned)received);
    }

cleanup:
    if (ota_started) {
        esp_ota_abort(ota);
    }
    free(chunk);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Download of %s failed: %s", image->name, esp_err_to_name(result));
    }
    return result;
}

/* ---- Menu ---- */

static void activate_entry(int index)
{
    entry_t entry = entries[index];
    draw_entry(index, true);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (entry.kind == ENTRY_SLOT) {
        boot_slot(entry.index);
    } else {
        const remote_image_t *image = &remote_images[entry.index];
        int slot = target_slot_for(image);
        esp_err_t result = download_image(image, slot);
        if (result == ESP_OK) {
            boot_slot(slot);
        } else {
            show_message_and_wait("DOWNLOAD FAILED", image->name, esp_err_to_name(result));
        }
    }
    /* Still here: booting or downloading failed. The caller redraws the title. */
    build_entries();
    draw_entries();
    draw_status();
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
    store_config();
    load_slots();
    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));

    int boot_choice = default_slot();
    bool counting = boot_choice >= 0;
    build_entries();
    draw_menu(counting ? "" : "NO IMAGES");
    ESP_ERROR_CHECK(xTaskCreate(network_task, "network", 8192, NULL, 5, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);

    int64_t deadline = esp_timer_get_time() + MENU_TIMEOUT_US;
    int shown_seconds = -1;
    net_state_t shown_state = net_state;
    while (true) {
        cyd_touch_point_t point;
        if (xQueueReceive(touch_queue, &point, pdMS_TO_TICKS(50)) == pdTRUE) {
            int row = point.y / CYD_FONT_HEIGHT - MENU_FIRST_ROW;
            if (row >= 0 && row < entry_count) {
                activate_entry(row);
                /* Still here: booting or downloading failed. Start over. */
                boot_choice = default_slot();
                counting = boot_choice >= 0;
                deadline = esp_timer_get_time() + MENU_TIMEOUT_US;
                shown_seconds = -1;
                if (!counting) {
                    draw_title("NO IMAGES");
                }
            } else {
                /* Any other tap, brightness buttons included, buys more time to choose. */
                int column = point.x / CYD_FONT_WIDTH;
                if (point.y / CYD_FONT_HEIGHT == STATUS_ROW) {
                    if (column >= BRIGHTNESS_DOWN_COLUMN &&
                        column < BRIGHTNESS_DOWN_COLUMN + BRIGHTNESS_BUTTON_COLUMNS) {
                        change_brightness(-CYD_BRIGHTNESS_STEP);
                    } else if (column >= BRIGHTNESS_UP_COLUMN) {
                        change_brightness(CYD_BRIGHTNESS_STEP);
                    }
                }
                deadline += MENU_TIMEOUT_US;
            }
        }

        /* Put the network status back once the brightness has been shown. */
        if (brightness_shown_until != 0 && esp_timer_get_time() >= brightness_shown_until) {
            brightness_shown_until = 0;
            draw_status();
        }

        if (counting) {
            int64_t remaining = deadline - esp_timer_get_time();
            int seconds = (int)((remaining + 999999) / 1000000);
            if (remaining <= 0) {
                boot_slot(boot_choice);
                counting = false;
                draw_title("SELECT IMAGE");
            } else if (seconds != shown_seconds) {
                char title[32];
                shown_seconds = seconds;
                snprintf(title, sizeof(title), "AUTO-BOOT %ds", seconds);
                draw_title(title);
            }
        }

        net_state_t state = net_state;
        if (state != shown_state) {
            shown_state = state;
            if (state == NET_READY) {
                build_entries();
                draw_entries();
                if (!counting && entry_count > 0) {
                    draw_title("SELECT IMAGE");
                }
            }
            draw_status();
        }
    }
}
