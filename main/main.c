#include <stdarg.h>
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
#include "esp_mac.h"
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
#define MAX_REMOTE_IMAGES 16
#define MAX_ENTRIES (SLOT_COUNT + MAX_REMOTE_IMAGES)
#define BRANCH_NAME_SIZE 48
/* The menu fills the columns left of the brightness slider: the title, the
   visible entries, the scroll row, then the info row. */
#define MENU_COLUMNS (CYD_TEXT_COLUMNS - BRIGHTNESS_COLUMNS)
#define MENU_FIRST_ROW 1
#define MENU_ROWS (CYD_TEXT_ROWS - 3)
#define SCROLL_ROW (MENU_FIRST_ROW + MENU_ROWS)
#define INFO_ROW (CYD_TEXT_ROWS - 1)

/* Scroll row layout: which entries are shown, then up and down buttons. */
#define SCROLL_TEXT_COLUMNS 7
#define SCROLL_UP_COLUMN 7
#define SCROLL_DOWN_COLUMN 11
#define SCROLL_BUTTON_COLUMNS 3

/* The brightness slider down the right edge: a + button, one bar segment
   per level with the brightest at the top, a - button and the level. */
#define BRIGHTNESS_COLUMNS 3
#define BRIGHTNESS_COLUMN (CYD_TEXT_COLUMNS - BRIGHTNESS_COLUMNS)
#define BRIGHTNESS_LEVELS ((CYD_BRIGHTNESS_MAX - CYD_BRIGHTNESS_MIN) / CYD_BRIGHTNESS_STEP + 1)
#define BRIGHTNESS_UP_ROW 1
#define BRIGHTNESS_BAR_ROW (BRIGHTNESS_UP_ROW + 1)
#define BRIGHTNESS_DOWN_ROW (BRIGHTNESS_BAR_ROW + BRIGHTNESS_LEVELS)
#define BRIGHTNESS_LABEL_ROW (BRIGHTNESS_DOWN_ROW + 1)

/* The info row cycles through the network activity, the address, the
   signal and the free RAM; new activity shows at once. */
#define INFO_PAGE_US (2500LL * 1000)
#define ACTIVITY_SIZE (MENU_COLUMNS + 1)

/* The Wi-Fi network chooser, opened by tapping the network activity, lists
   networks like the menu: the title and a SCAN button, then the networks,
   then BACK and the scroll buttons. Scan results live on the heap only
   while it is open. */
#define MAX_WIFI_RESULTS 24
#define NETWORK_TITLE_COLUMNS 8
#define SCAN_BUTTON_COLUMN (NETWORK_TITLE_COLUMNS + 1)
#define BACK_BUTTON_COLUMNS (SCROLL_UP_COLUMN - 1)

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "cyd_launcher";

/* An asset's updated_at time, which changes with every upload. */
#define VERSION_SIZE 24

typedef struct {
    const esp_partition_t *partition;
    bool valid;
    char name[BRANCH_NAME_SIZE];   /* branch name */
    char version[VERSION_SIZE];    /* empty when flashed some other way */
} slot_t;

typedef struct {
    char name[BRANCH_NAME_SIZE]; /* branch name */
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
static TaskHandle_t network_task_handle;
static EventGroupHandle_t wifi_events;
static volatile bool wifi_give_up;
static slot_t slots[SLOT_COUNT];
static remote_image_t remote_images[MAX_REMOTE_IMAGES];
static int remote_count;
/* Written by the network task; remote_images is complete once NET_READY is set. */
static volatile net_state_t net_state = NET_CONNECTING;
static entry_t entries[MAX_ENTRIES];
static int entry_count;
/* Index of the entry shown on the first menu row. */
static int scroll_offset;
static char title_text[MENU_COLUMNS + 1];

typedef enum { INFO_ACTIVITY, INFO_ADDRESS, INFO_SIGNAL, INFO_RAM, INFO_PAGE_COUNT } info_page_t;

/* What the network is doing, in words, from the network task and the
   Wi-Fi event handler. */
static portMUX_TYPE activity_lock = portMUX_INITIALIZER_UNLOCKED;
static char activity[ACTIVITY_SIZE] = "starting";
static volatile uint32_t activity_changes;
static uint32_t shown_activity_changes;
static volatile bool wifi_connected;
static volatile uint16_t wifi_disconnect_reason;
static esp_ip4_addr_t wifi_address;
static info_page_t info_page;
static int64_t info_page_until;

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

#ifndef BIRDNET_BASE_URL
#define BIRDNET_BASE_URL ""
#endif

/* FNV-1a hash of this build's compiled-in settings. */
static uint32_t compiled_config_hash(void)
{
    static const char *const values[] = {WIFI_SSID, WIFI_PASSWORD, BIRDNET_BASE_URL};
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        for (const char *c = values[i]; ; ++c) {
            hash = (hash ^ (uint8_t)*c) * 16777619u;
            if (*c == '\0') {
                break;
            }
        }
    }
    return hash;
}

/* Seeds the boot images' settings (NVS cyd_config) from this launcher's
   private header, but only when that header changed since it last did:
   settings edited on the device, with the launcher's network chooser or
   wifi_checker's CONNECT screen, survive restarts until a launcher with new
   credentials is flashed. */
static void seed_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS for settings");
        return;
    }
    uint32_t hash = compiled_config_hash();
    uint32_t seeded = 0;
    if (nvs_get_u32(nvs, "seed", &seeded) != ESP_OK || seeded != hash) {
        nvs_set_str(nvs, "wifi_ssid", WIFI_SSID);
        nvs_set_str(nvs, "wifi_pass", WIFI_PASSWORD);
        if (BIRDNET_BASE_URL[0] != '\0') {
            nvs_set_str(nvs, "birdnet_url", BIRDNET_BASE_URL);
        }
        nvs_set_u32(nvs, "seed", hash);
        nvs_commit(nvs);
        ESP_LOGI(TAG, "Stored this launcher's settings for the boot images");
    }
    nvs_close(nvs);
}

/* Reads a setting the images share, falling back to this build's value. */
static void load_config_string(const char *key, const char *fallback, char *value,
                               size_t size)
{
    nvs_handle_t nvs;
    snprintf(value, size, "%s", fallback);
    if (nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, key, value, &size) != ESP_OK) {
            snprintf(value, size, "%s", fallback);
        }
        nvs_close(nvs);
    }
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

/* The level a bar segment of the brightness slider stands for. */
static int brightness_bar_level(int row)
{
    return CYD_BRIGHTNESS_MAX - (row - BRIGHTNESS_BAR_ROW) * CYD_BRIGHTNESS_STEP;
}

/* Fills in this row's cells of the brightness slider: the + and - buttons
   dimmed at either end of the range, the bar lit up to the current level,
   and the level below them. */
static void brightness_cells(int row, char *text, uint16_t *foreground, uint16_t *background)
{
    int level = cyd_get_brightness();
    for (int i = BRIGHTNESS_COLUMN; i < CYD_TEXT_COLUMNS; ++i) {
        text[i] = ' ';
        foreground[i] = CYD_COLOR_GRAY;
        background[i] = CYD_COLOR_BLACK;
    }
    if (row == BRIGHTNESS_UP_ROW || row == BRIGHTNESS_DOWN_ROW) {
        bool up = row == BRIGHTNESS_UP_ROW;
        bool enabled = up ? level < CYD_BRIGHTNESS_MAX : level > CYD_BRIGHTNESS_MIN;
        for (int i = BRIGHTNESS_COLUMN; i < CYD_TEXT_COLUMNS; ++i) {
            foreground[i] = enabled ? CYD_COLOR_WHITE : CYD_COLOR_BUTTON_DISABLED;
            background[i] = CYD_COLOR_BUTTON;
        }
        text[BRIGHTNESS_COLUMN + 1] = up ? '+' : '-';
    } else if (row >= BRIGHTNESS_BAR_ROW && row < BRIGHTNESS_DOWN_ROW) {
        uint16_t color = brightness_bar_level(row) <= level ? CYD_COLOR_YELLOW : CYD_COLOR_BUTTON;
        for (int i = BRIGHTNESS_COLUMN; i < CYD_TEXT_COLUMNS; ++i) {
            background[i] = color;
        }
    } else if (row == BRIGHTNESS_LABEL_ROW) {
        /* "100%" loses its '%' to fit. */
        char label[16];
        snprintf(label, sizeof(label), "%d%%", level);
        memcpy(text + BRIGHTNESS_COLUMN, label, strnlen(label, BRIGHTNESS_COLUMNS));
    }
}

/* Draws MENU_COLUMNS cells of menu, then the row's part of the slider. */
static void draw_menu_cells(int row, const char *menu_text, const uint16_t *menu_foreground,
                            const uint16_t *menu_background)
{
    char text[CYD_TEXT_COLUMNS + 1];
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    memset(text, ' ', CYD_TEXT_COLUMNS);
    text[CYD_TEXT_COLUMNS] = '\0';
    memcpy(text, menu_text, strnlen(menu_text, MENU_COLUMNS));
    memcpy(foreground, menu_foreground, MENU_COLUMNS * sizeof(foreground[0]));
    memcpy(background, menu_background, MENU_COLUMNS * sizeof(background[0]));
    brightness_cells(row, text, foreground, background);
    ESP_ERROR_CHECK(cyd_draw_cells(row, text, foreground, background));
}

/* Like cyd_text_row, but only across the menu columns. */
static void draw_menu_row(int row, uint16_t foreground, uint16_t background, const char *text)
{
    uint16_t foregrounds[MENU_COLUMNS];
    uint16_t backgrounds[MENU_COLUMNS];
    for (int i = 0; i < MENU_COLUMNS; ++i) {
        foregrounds[i] = foreground;
        backgrounds[i] = background;
    }
    draw_menu_cells(row, text, foregrounds, backgrounds);
}

static void draw_title(const char *text)
{
    if (text != title_text) {
        snprintf(title_text, sizeof(title_text), "%s", text);
    }
    draw_menu_row(0, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, title_text);
}

static void set_activity(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void set_activity(const char *format, ...)
{
    char text[ACTIVITY_SIZE];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    portENTER_CRITICAL(&activity_lock);
    memcpy(activity, text, sizeof(activity));
    ++activity_changes;
    portEXIT_CRITICAL(&activity_lock);
}

/* One page of information: what the network is doing, the address and
   signal once joined, or the free RAM. */
static void draw_info(void)
{
    char text[48] = ""; /* room for a whole SSID, to satisfy -Wformat-truncation */
    switch (info_page) {
    case INFO_ACTIVITY:
        portENTER_CRITICAL(&activity_lock);
        memcpy(text, activity, sizeof(activity));
        portEXIT_CRITICAL(&activity_lock);
        break;
    case INFO_ADDRESS:
        snprintf(text, sizeof(text), IPSTR, IP2STR(&wifi_address));
        break;
    case INFO_SIGNAL: {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(text, sizeof(text), "%ddBm %.*s", ap.rssi, (int)sizeof(ap.ssid),
                     (const char *)ap.ssid);
        }
        break;
    }
    default:
        snprintf(text, sizeof(text), "RAM %uK free", (unsigned)(esp_get_free_heap_size() / 1024));
        break;
    }
    draw_menu_row(INFO_ROW, CYD_COLOR_GRAY, CYD_COLOR_BLACK, text);
}

/* Moves to the next info page, skipping the address and signal until
   Wi-Fi has joined. */
static void next_info_page(void)
{
    do {
        info_page = (info_page + 1) % INFO_PAGE_COUNT;
    } while (!wifi_connected && (info_page == INFO_ADDRESS || info_page == INFO_SIGNAL));
    info_page_until = esp_timer_get_time() + INFO_PAGE_US;
    draw_info();
}

/* New network activity shows at once; otherwise the pages take turns. */
static void update_info(void)
{
    uint32_t changes = activity_changes;
    if (changes != shown_activity_changes) {
        shown_activity_changes = changes;
        info_page = INFO_ACTIVITY;
        info_page_until = esp_timer_get_time() + INFO_PAGE_US;
        draw_info();
    } else if (esp_timer_get_time() >= info_page_until) {
        next_info_page();
    }
}

typedef struct {
    int first_column;
    int columns;
    const char *label;
    uint16_t foreground;
} button_t;

/* Fills a button's cells of a row, with its label centered. */
static void button_cells(const button_t *button, char *text, uint16_t *foreground,
                         uint16_t *background)
{
    int length = (int)strnlen(button->label, button->columns);
    for (int i = 0; i < button->columns; ++i) {
        foreground[button->first_column + i] = button->foreground;
        background[button->first_column + i] = CYD_COLOR_BUTTON;
    }
    memcpy(text + button->first_column + (button->columns - length) / 2, button->label, length);
}

static bool button_hit(const button_t *button, int column)
{
    return column >= button->first_column && column < button->first_column + button->columns;
}

/* Draws a full-width row of buttons; other cells stay black. */
static void draw_buttons(int row, const button_t *buttons, int count)
{
    char text[CYD_TEXT_COLUMNS + 1];
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    memset(text, ' ', CYD_TEXT_COLUMNS);
    text[CYD_TEXT_COLUMNS] = '\0';
    for (int i = 0; i < CYD_TEXT_COLUMNS; ++i) {
        foreground[i] = CYD_COLOR_WHITE;
        background[i] = CYD_COLOR_BLACK;
    }
    for (int b = 0; b < count; ++b) {
        button_cells(&buttons[b], text, foreground, background);
    }
    ESP_ERROR_CHECK(cyd_draw_cells(row, text, foreground, background));
}

/* The scroll row's up and down buttons, dimmed at either end of the list. */
static void scroll_button_cells(char *text, uint16_t *foreground, uint16_t *background,
                                bool can_scroll_up, bool can_scroll_down)
{
    static const char up_label[] = {CYD_GLYPH_UP, '\0'};
    static const char down_label[] = {CYD_GLYPH_DOWN, '\0'};
    const button_t buttons[] = {
        {SCROLL_UP_COLUMN, SCROLL_BUTTON_COLUMNS, up_label,
         can_scroll_up ? CYD_COLOR_WHITE : CYD_COLOR_BUTTON_DISABLED},
        {SCROLL_DOWN_COLUMN, SCROLL_BUTTON_COLUMNS, down_label,
         can_scroll_down ? CYD_COLOR_WHITE : CYD_COLOR_BUTTON_DISABLED},
    };
    button_cells(&buttons[0], text, foreground, background);
    button_cells(&buttons[1], text, foreground, background);
}

/* -1 or 1 for a tap on the scroll row's up or down button, else 0. */
static int scroll_tap_direction(int column)
{
    if (column >= SCROLL_UP_COLUMN && column < SCROLL_UP_COLUMN + SCROLL_BUTTON_COLUMNS) {
        return -1;
    }
    if (column >= SCROLL_DOWN_COLUMN && column < SCROLL_DOWN_COLUMN + SCROLL_BUTTON_COLUMNS) {
        return 1;
    }
    return 0;
}

/* The offset of the first visible row one page up or down a list,
   stopping with the last page full. */
static int scrolled_offset(int offset, int count, int direction)
{
    int max_offset = count - MENU_ROWS;
    offset += direction * MENU_ROWS;
    if (offset > max_offset) {
        offset = max_offset;
    }
    return offset < 0 ? 0 : offset;
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
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (slots[i].valid) {
            entries[entry_count++] = (entry_t){.kind = ENTRY_SLOT, .index = i};
        }
    }
    if (net_state == NET_READY) {
        for (int i = 0; i < remote_count; ++i) {
            if (!remote_is_stored(&remote_images[i])) {
                entries[entry_count++] = (entry_t){.kind = ENTRY_REMOTE, .index = i};
            }
        }
    }
    /* The list may have shrunk under the current page. */
    int max_offset = entry_count > MENU_ROWS ? entry_count - MENU_ROWS : 0;
    if (scroll_offset > max_offset) {
        scroll_offset = max_offset;
    }
}

/* "3-9/9": the entries shown, then the up and down buttons, dimmed at
   either end of the list. */
static void draw_scroll_row(void)
{
    char position[36] = ""; /* room for any int, to satisfy -Wformat-truncation */
    int last_shown = scroll_offset + MENU_ROWS < entry_count ? scroll_offset + MENU_ROWS
                                                             : entry_count;
    if (entry_count > 0) {
        snprintf(position, sizeof(position), "%d-%d/%d", scroll_offset + 1, last_shown,
                 entry_count);
        if (strlen(position) > SCROLL_TEXT_COLUMNS) {
            snprintf(position, sizeof(position), "%d-%d", scroll_offset + 1, last_shown);
        }
    }

    char text[MENU_COLUMNS + 1];
    uint16_t foreground[MENU_COLUMNS];
    uint16_t background[MENU_COLUMNS];
    memset(text, ' ', MENU_COLUMNS);
    text[MENU_COLUMNS] = '\0';
    memcpy(text, position, strnlen(position, SCROLL_TEXT_COLUMNS));
    for (int i = 0; i < MENU_COLUMNS; ++i) {
        foreground[i] = CYD_COLOR_GRAY;
        background[i] = CYD_COLOR_BLACK;
    }
    scroll_button_cells(text, foreground, background, scroll_offset > 0,
                        last_shown < entry_count);
    draw_menu_cells(SCROLL_ROW, text, foreground, background);
}

/* Draws entries[index], which must be on the current page. */
static void draw_entry(int index, bool highlighted)
{
    char line[CYD_TEXT_COLUMNS + 1];
    uint16_t color = CYD_COLOR_WHITE;
    const entry_t *entry = &entries[index];
    if (entry->kind == ENTRY_SLOT) {
        /* "A*name": slot letter, '*' on the image that boots by default. */
        snprintf(line, sizeof(line), "%c%c%.12s", 'A' + entry->index,
                 entry->index == default_slot() ? '*' : ' ', slots[entry->index].name);
    } else {
        snprintf(line, sizeof(line), "+ %.12s", remote_images[entry->index].name);
        color = CYD_COLOR_CYAN;
    }
    int row = MENU_FIRST_ROW + index - scroll_offset;
    if (highlighted) {
        draw_menu_row(row, CYD_COLOR_BLACK, CYD_COLOR_WHITE, line);
    } else {
        draw_menu_row(row, color, CYD_COLOR_BUTTON, line);
    }
}

static void draw_entries(void)
{
    for (int i = 0; i < MENU_ROWS; ++i) {
        if (scroll_offset + i < entry_count) {
            draw_entry(scroll_offset + i, false);
        } else {
            draw_menu_row(MENU_FIRST_ROW + i, CYD_COLOR_WHITE, CYD_COLOR_BLACK, "");
        }
    }
    draw_scroll_row();
}

/* Pages the menu by its visible height. */
static void scroll_menu(int direction)
{
    int offset = scrolled_offset(scroll_offset, entry_count, direction);
    if (offset != scroll_offset) {
        scroll_offset = offset;
        draw_entries();
    }
}

static void draw_menu(const char *title)
{
    draw_title(title);
    draw_entries();
    draw_info();
}

/* Sets the backlight and saves it for every image. Returns true when it
   changed, and the caller redraws the slider, which runs down every row. */
static bool set_brightness(int level)
{
    int before = cyd_get_brightness();
    ESP_ERROR_CHECK(cyd_set_brightness(level));
    int after = cyd_get_brightness();
    if (after == before) {
        return false;
    }
    nvs_handle_t nvs;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, CYD_CONFIG_BRIGHTNESS, (uint8_t)after);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Brightness %d%%", after);
    return true;
}

/* + and - step the level; a bar segment sets its own level. Returns true
   when the level changed and the screen needs redrawing. */
static bool brightness_tap(int row)
{
    int level = cyd_get_brightness();
    if (row == BRIGHTNESS_UP_ROW) {
        return set_brightness(level + CYD_BRIGHTNESS_STEP);
    }
    if (row == BRIGHTNESS_DOWN_ROW) {
        return set_brightness(level - CYD_BRIGHTNESS_STEP);
    }
    if (row >= BRIGHTNESS_BAR_ROW && row < BRIGHTNESS_DOWN_ROW) {
        return set_brightness(brightness_bar_level(row));
    }
    return false;
}

/* Full-screen message; returns after the next tap. */
static void show_message_and_wait(const char *title, const char *line1, const char *line2)
{
    for (int row = 0; row < CYD_TEXT_ROWS; ++row) {
        const char *text = row == 0 ? title
                           : row == 2 ? line1
                           : row == 3 ? line2
                           : row == CYD_TEXT_ROWS - 1 ? "tap to continue"
                                               : "";
        uint16_t color = row == 0 ? CYD_COLOR_RED
                         : row == CYD_TEXT_ROWS - 1 ? CYD_COLOR_GRAY
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

/* Says why Wi-Fi did not join, as far as the driver knows. */
static void report_wifi_failure(void)
{
    switch (wifi_disconnect_reason) {
    case 0:
        set_activity("Wi-Fi timeout");
        break;
    case WIFI_REASON_NO_AP_FOUND:
        set_activity("no AP found");
        break;
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        set_activity("bad password?");
        break;
    case WIFI_REASON_BEACON_TIMEOUT:
        set_activity("signal lost");
        break;
    default:
        set_activity("Wi-Fi error %u", (unsigned)wifi_disconnect_reason);
        break;
    }
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    (void)argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        wifi_connected = false;
        if (!wifi_give_up) {
            /* Retrying: show why the last attempt failed. */
            wifi_disconnect_reason = disconnected->reason;
            report_wifi_failure();
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = event_data;
        wifi_address = got_ip->ip_info.ip;
        wifi_connected = true;
        set_activity("Wi-Fi joined");
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void set_wifi_network(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, ssid, strnlen(ssid, sizeof(wifi_config.sta.ssid)));
    memcpy(wifi_config.sta.password, password,
           strnlen(password, sizeof(wifi_config.sta.password) - 1));
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

static bool wifi_wait_connected(void)
{
    EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* Starts Wi-Fi without joining anything, so the network chooser can scan
   whether or not a join worked. */
static bool wifi_init(void)
{
    wifi_events = xEventGroupCreate();
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    if (wifi_events == NULL || esp_netif_init() != ESP_OK ||
        esp_event_loop_create_default() != ESP_OK ||
        esp_netif_create_default_wifi_sta() == NULL || esp_wifi_init(&init_config) != ESP_OK) {
        return false;
    }
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    wifi_give_up = true;
    return esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK && esp_wifi_start() == ESP_OK;
}

/* Leaves any network and joins this one, retrying until
   WIFI_CONNECT_TIMEOUT_MS. */
static bool wifi_try(const char *ssid, const char *password)
{
    wifi_give_up = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
    wifi_connected = false;
    wifi_disconnect_reason = 0;
    set_activity("join %s", ssid);
    set_wifi_network(ssid, password);
    wifi_give_up = false;
    return esp_wifi_connect() == ESP_OK && wifi_wait_connected();
}

/* Joins the shared network, which may have been chosen on the device. On
   failure Wi-Fi stays started but idle, for the chooser's scans. */
static bool wifi_join(void)
{
    char ssid[33];
    char password[65];
    load_config_string("wifi_ssid", WIFI_SSID, ssid, sizeof(ssid));
    load_config_string("wifi_pass", WIFI_PASSWORD, password, sizeof(password));
    bool connected = wifi_try(ssid, password);
    /* A mistyped network chosen on the device must not cut the launcher off
       from GitHub, so fall back to this build's own credentials. */
    if (!connected && (strcmp(ssid, WIFI_SSID) != 0 || strcmp(password, WIFI_PASSWORD) != 0)) {
        ESP_LOGW(TAG, "Saved Wi-Fi network %s did not connect; trying %s", ssid, WIFI_SSID);
        connected = wifi_try(WIFI_SSID, WIFI_PASSWORD);
    }
    if (!connected) {
        wifi_give_up = true;
        esp_wifi_disconnect();
        report_wifi_failure();
    }
    return connected;
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

/* Branches that are not boot images: the launcher itself, the legacy main
   branch and the documentation. */
static bool branch_is_excluded(const char *branch)
{
    static const char *const excluded[] = {"bootloader", "main", "How_to_Build"};
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
        char tag_name[BRANCH_NAME_SIZE];
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
        /* "ESP_ERR_HTTP_CONNECT" shows as "HTTP_CONNECT". */
        const char *name = esp_err_to_name(result);
        set_activity("%s", strncmp(name, "ESP_ERR_", 8) == 0 ? name + 8 : name);
        return result;
    }
    if (status != 200 || buffer->data == NULL) {
        ESP_LOGW(TAG, "GitHub returned HTTP %d for %s", status, url);
        set_activity("GitHub err %d", status);
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
    /* Static: the network task's stack also carries the TLS session. */
    static char branches[MAX_REMOTE_IMAGES][BRANCH_NAME_SIZE];
    int branch_count = 0;
    set_activity("GET branches");
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
        set_activity("GET releases");
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
            set_activity("%d on GitHub", remote_count);
            state = NET_READY;
        }
    }
    esp_http_client_cleanup(client);
    free(buffer.data);
    return state;
}

/* Joins Wi-Fi and lists the branches, then again each time the network
   chooser saves a network. */
static void network_task(void *argument)
{
    (void)argument;
    bool started = wifi_init();
    if (!started) {
        set_activity("Wi-Fi init err");
    }
    while (true) {
        if (started && wifi_join()) {
            net_state = NET_FETCHING;
            net_state = fetch_branch_images();
        } else {
            ESP_LOGW(TAG, "Wi-Fi did not connect; keeping stored images only");
            net_state = NET_OFFLINE;
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        net_state = NET_CONNECTING;
    }
}

/* Has the network task join the saved network and list the branches again. */
static void rejoin_wifi(void)
{
    /* Set here too, so the menu drops the branches before they are refetched. */
    net_state = NET_CONNECTING;
    xTaskNotifyGive(network_task_handle);
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

/* ---- Wi-Fi network chooser ---- */

typedef struct {
    wifi_ap_record_t *networks; /* MAX_WIFI_RESULTS */
    int count;
    int offset;         /* index of the network on the first menu row */
    const char *status; /* shown instead of the count, such as while scanning */
} network_list_t;

static const button_t scan_button = {SCAN_BUTTON_COLUMN, MENU_COLUMNS - SCAN_BUTTON_COLUMN,
                                     "SCAN", CYD_COLOR_WHITE};
static const button_t back_button = {0, BACK_BUTTON_COLUMNS, "BACK", CYD_COLOR_WHITE};

static const char *wifi_auth_name(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3-SAE";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    case WIFI_AUTH_OWE:
        return "OWE";
    default:
        return "OTHER";
    }
}

static const char *wifi_cipher_name(wifi_cipher_type_t cipher)
{
    switch (cipher) {
    case WIFI_CIPHER_TYPE_NONE:
        return "NONE";
    case WIFI_CIPHER_TYPE_WEP40:
    case WIFI_CIPHER_TYPE_WEP104:
        return "WEP";
    case WIFI_CIPHER_TYPE_TKIP:
        return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP:
        return "CCMP";
    case WIFI_CIPHER_TYPE_TKIP_CCMP:
        return "TKIP+CCMP";
    case WIFI_CIPHER_TYPE_GCMP:
        return "GCMP";
    case WIFI_CIPHER_TYPE_GCMP256:
        return "GCMP-256";
    default:
        return "OTHER";
    }
}

static void wifi_phy_text(const wifi_ap_record_t *ap, char *text, size_t size)
{
    snprintf(text, size, "%s%s%s%s%s", ap->phy_11b ? "b" : "", ap->phy_11g ? "g" : "",
             ap->phy_11n ? "n" : "", ap->phy_11ax ? "/ax" : "", ap->phy_lr ? " LR" : "");
}

static const char *network_name(const wifi_ap_record_t *ap)
{
    return ap->ssid[0] != '\0' ? (const char *)ap->ssid : "(hidden)";
}

static bool same_ssid(const wifi_ap_record_t *a, const wifi_ap_record_t *b)
{
    return strncmp((const char *)a->ssid, (const char *)b->ssid, sizeof(a->ssid)) == 0;
}

/* True for the network the launcher is on. */
static bool network_is_joined(const wifi_ap_record_t *ap)
{
    wifi_ap_record_t joined;
    return wifi_connected && esp_wifi_sta_get_ap_info(&joined) == ESP_OK && same_ssid(&joined, ap);
}

static int strongest_first(const void *left, const void *right)
{
    return ((const wifi_ap_record_t *)right)->rssi - ((const wifi_ap_record_t *)left)->rssi;
}

/* Scans for networks, keeping the strongest access point of each name. A
   scan cannot start while a join is in progress, so callers wait for the
   network task to finish joining; a reconnect after a dropped connection
   only delays it. */
static esp_err_t scan_networks(network_list_t *list)
{
    list->count = 0;
    list->offset = 0;
    /* Hidden networks are listed too, so their names can be typed in. */
    const wifi_scan_config_t config = {.show_hidden = true};
    esp_err_t result = esp_wifi_scan_start(&config, true);
    for (int retry = 0; result == ESP_ERR_WIFI_STATE && retry < 10; ++retry) {
        vTaskDelay(pdMS_TO_TICKS(500));
        result = esp_wifi_scan_start(&config, true);
    }
    uint16_t found = MAX_WIFI_RESULTS;
    if (result == ESP_OK) {
        result = esp_wifi_scan_get_ap_records(&found, list->networks);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(result));
        return result;
    }
    qsort(list->networks, found, sizeof(list->networks[0]), strongest_first);
    for (int i = 0; i < found; ++i) {
        const wifi_ap_record_t *ap = &list->networks[i];
        bool duplicate = false;
        for (int kept = 0; kept < list->count && !duplicate; ++kept) {
            duplicate = ap->ssid[0] != '\0' && same_ssid(ap, &list->networks[kept]);
        }
        if (!duplicate) {
            list->networks[list->count++] = *ap;
        }
    }
    ESP_LOGI(TAG, "Wi-Fi scan: %d networks", list->count);
    return ESP_OK;
}

/* "WI-FI 7" or the status, then the SCAN button. */
static void draw_network_title(const network_list_t *list)
{
    char count[24]; /* room for any int, to satisfy -Wformat-truncation */
    const char *title = list->status;
    if (title == NULL) {
        snprintf(count, sizeof(count), "WI-FI %d", list->count);
        title = count;
    }
    char text[MENU_COLUMNS + 1];
    uint16_t foreground[MENU_COLUMNS];
    uint16_t background[MENU_COLUMNS];
    memset(text, ' ', MENU_COLUMNS);
    text[MENU_COLUMNS] = '\0';
    memcpy(text, title, strnlen(title, NETWORK_TITLE_COLUMNS));
    for (int i = 0; i < MENU_COLUMNS; ++i) {
        foreground[i] = CYD_COLOR_YELLOW;
        background[i] = CYD_COLOR_BLACK;
    }
    button_cells(&scan_button, text, foreground, background);
    draw_menu_cells(0, text, foreground, background);
}

/* "-67 name", in yellow for the network the launcher is on. The network
   must be on the current page. */
static void draw_network(const network_list_t *list, int index, bool highlighted)
{
    const wifi_ap_record_t *ap = &list->networks[index];
    char line[24]; /* room for any rssi, to satisfy -Wformat-truncation */
    snprintf(line, sizeof(line), "%d %.10s", ap->rssi, network_name(ap));
    int row = MENU_FIRST_ROW + index - list->offset;
    if (highlighted) {
        draw_menu_row(row, CYD_COLOR_BLACK, CYD_COLOR_WHITE, line);
    } else {
        draw_menu_row(row, network_is_joined(ap) ? CYD_COLOR_YELLOW : CYD_COLOR_WHITE,
                      CYD_COLOR_BUTTON, line);
    }
}

static void draw_network_list(const network_list_t *list)
{
    draw_network_title(list);
    for (int i = 0; i < MENU_ROWS; ++i) {
        if (list->offset + i < list->count) {
            draw_network(list, list->offset + i, false);
        } else {
            draw_menu_row(MENU_FIRST_ROW + i, CYD_COLOR_WHITE, CYD_COLOR_BLACK, "");
        }
    }
    char text[MENU_COLUMNS + 1];
    uint16_t foreground[MENU_COLUMNS];
    uint16_t background[MENU_COLUMNS];
    memset(text, ' ', MENU_COLUMNS);
    text[MENU_COLUMNS] = '\0';
    for (int i = 0; i < MENU_COLUMNS; ++i) {
        foreground[i] = CYD_COLOR_GRAY;
        background[i] = CYD_COLOR_BLACK;
    }
    button_cells(&back_button, text, foreground, background);
    scroll_button_cells(text, foreground, background, list->offset > 0,
                        list->offset + MENU_ROWS < list->count);
    draw_menu_cells(SCROLL_ROW, text, foreground, background);
    draw_info();
}

/* A full-width screen of text rows; the last row is left for buttons. */
typedef struct {
    char text[CYD_TEXT_ROWS][CYD_TEXT_COLUMNS + 1];
    uint16_t color[CYD_TEXT_ROWS];
    int count;
} page_t;

static void page_add(page_t *page, uint16_t color, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void page_add(page_t *page, uint16_t color, const char *format, ...)
{
    if (page->count >= CYD_TEXT_ROWS - 1) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(page->text[page->count], sizeof(page->text[0]), format, arguments);
    va_end(arguments);
    page->color[page->count++] = color;
}

/* Adds text split across up to max_rows full-width rows. */
static void page_add_wrapped(page_t *page, uint16_t color, const char *text, int max_rows)
{
    size_t length = strlen(text);
    for (int row = 0; row < max_rows && (row == 0 || length > 0); ++row) {
        page_add(page, color, "%.*s", CYD_TEXT_COLUMNS, text);
        size_t step = length < CYD_TEXT_COLUMNS ? length : CYD_TEXT_COLUMNS;
        text += step;
        length -= step;
    }
}

static void page_show(const page_t *page)
{
    for (int row = 0; row < CYD_TEXT_ROWS - 1; ++row) {
        ESP_ERROR_CHECK(cyd_text_row(row, row < page->count ? page->color[row] : CYD_COLOR_WHITE,
                                     CYD_COLOR_BLACK, row < page->count ? page->text[row] : ""));
    }
}

/* Detail page buttons for a network. */
static const button_t detail_buttons[] = {
    {0, 7, "BACK", CYD_COLOR_WHITE},
    {8, 9, "CONNECT", CYD_COLOR_YELLOW},
};
#define DETAIL_CONNECT_BUTTON 1

static void show_wifi_detail(const wifi_ap_record_t *ap)
{
    page_t page = {0};
    char phy[24];
    wifi_phy_text(ap, phy, sizeof(phy));
    page_add(&page, CYD_COLOR_YELLOW, "WI-FI NETWORK");
    page_add_wrapped(&page, CYD_COLOR_WHITE, network_name(ap), 2);
    page_add(&page, CYD_COLOR_WHITE, "CH %u  %d dBm", ap->primary, ap->rssi);
    if (network_is_joined(ap)) {
        page_add(&page, CYD_COLOR_GRAY, "JOINED AS");
        page_add(&page, CYD_COLOR_WHITE, IPSTR, IP2STR(&wifi_address));
    }
    page_add(&page, CYD_COLOR_WHITE, "AUTH %s", wifi_auth_name(ap->authmode));
    page_add(&page, CYD_COLOR_WHITE, "CIPHER %s", wifi_cipher_name(ap->pairwise_cipher));
    page_add(&page, CYD_COLOR_WHITE, MACSTR, MAC2STR(ap->bssid));
    page_add(&page, CYD_COLOR_WHITE, "802.11%s%s", phy, ap->wps ? " WPS" : "");
    page_show(&page);
    draw_buttons(CYD_TEXT_ROWS - 1, detail_buttons, 2);
}

enum { FIELD_SSID, FIELD_PASSWORD, FIELD_COUNT };

#define FIELD_LABEL_COLUMNS 2
#define EDITOR_MESSAGE_ROW 2
#define KEY_FIRST_ROW 3
#define KEY_ROWS 5
#define KEY_COLUMNS 2 /* each key is two cells wide, for the resistive panel */
#define KEYS_PER_ROW (CYD_TEXT_COLUMNS / KEY_COLUMNS)
#define CONTROL_ROW (KEY_FIRST_ROW + KEY_ROWS)
#define ACTION_ROW (CONTROL_ROW + 1)
#define KEY_COLOR_ALTERNATE 0x39E7

/* Letters, digits and the symbols most passwords use; the symbol page has
   the rest of printable ASCII. */
static const char *const letter_keys[KEY_ROWS] = {
    "abcdefgh", "ijklmnop", "qrstuvwx", "yz012345", "6789.-_@",
};
static const char *const symbol_keys[KEY_ROWS] = {
    "!\"#$%&'(", ")*+,-./:", ";<=>?@[\\", "]^_`{|}~", "",
};

enum { CONTROL_CAPS, CONTROL_SYMBOLS, CONTROL_SPACE, CONTROL_DELETE, CONTROL_COUNT };
enum { ACTION_CANCEL, ACTION_SAVE, ACTION_COUNT };

typedef struct {
    char text[FIELD_COUNT][65];
    int active;
    bool symbols;
    bool caps;
    const char *message;
} editor_t;

static size_t field_capacity(int field)
{
    /* SSIDs are up to 32 bytes; passphrases up to 63, or 64 hex digits. */
    return field == FIELD_SSID ? 32 : 64;
}

static char editor_key(const editor_t *editor, int row, int key)
{
    const char *keys = (editor->symbols ? symbol_keys : letter_keys)[row];
    if (key >= (int)strlen(keys)) {
        return '\0';
    }
    char character = keys[key];
    if (editor->caps && character >= 'a' && character <= 'z') {
        character = (char)(character - 'a' + 'A');
    }
    return character;
}

/* "N network_" / "P password": the end of the text, so the cursor shows. */
static void draw_editor_field(const editor_t *editor, int field)
{
    char line[CYD_TEXT_COLUMNS + 1];
    bool active = editor->active == field;
    int visible = CYD_TEXT_COLUMNS - FIELD_LABEL_COLUMNS - 1;
    const char *text = editor->text[field];
    size_t length = strlen(text);
    /* 14 = CYD_TEXT_COLUMNS - FIELD_LABEL_COLUMNS - 1 (the cursor). */
    snprintf(line, sizeof(line), "%c %.14s%s", field == FIELD_SSID ? 'N' : 'P',
             length > (size_t)visible ? text + length - visible : text, active ? "_" : "");
    ESP_ERROR_CHECK(cyd_text_row(field, active ? CYD_COLOR_BLACK : CYD_COLOR_WHITE,
                                 active ? CYD_COLOR_WHITE : CYD_COLOR_BUTTON, line));
}

static void draw_editor_message(const editor_t *editor)
{
    const char *message = editor->message;
    if (message == NULL) {
        message = editor->active == FIELD_SSID ? "EDIT NETWORK" : "EDIT PASSWORD";
    }
    ESP_ERROR_CHECK(cyd_text_row(EDITOR_MESSAGE_ROW,
                                 editor->message != NULL ? CYD_COLOR_RED : CYD_COLOR_GRAY,
                                 CYD_COLOR_BLACK, message));
}

static void draw_keyboard(const editor_t *editor)
{
    for (int row = 0; row < KEY_ROWS; ++row) {
        char text[CYD_TEXT_COLUMNS + 1];
        uint16_t foreground[CYD_TEXT_COLUMNS];
        uint16_t background[CYD_TEXT_COLUMNS];
        memset(text, ' ', CYD_TEXT_COLUMNS);
        text[CYD_TEXT_COLUMNS] = '\0';
        for (int column = 0; column < CYD_TEXT_COLUMNS; ++column) {
            int key = column / KEY_COLUMNS;
            bool present = key < KEYS_PER_ROW && editor_key(editor, row, key) != '\0';
            foreground[column] = CYD_COLOR_WHITE;
            /* Alternate shades in a checkerboard so neighboring keys stand apart. */
            background[column] = !present          ? CYD_COLOR_BLACK
                                 : (key + row) % 2 ? KEY_COLOR_ALTERNATE
                                                   : CYD_COLOR_BUTTON;
        }
        for (int key = 0; key < KEYS_PER_ROW; ++key) {
            char character = editor_key(editor, row, key);
            if (character != '\0') {
                text[key * KEY_COLUMNS] = character;
            }
        }
        ESP_ERROR_CHECK(cyd_draw_cells(KEY_FIRST_ROW + row, text, foreground, background));
    }
}

static void editor_controls(const editor_t *editor, button_t controls[CONTROL_COUNT])
{
    controls[CONTROL_CAPS] = (button_t){0, 3, "Aa", editor->caps ? CYD_COLOR_YELLOW : CYD_COLOR_WHITE};
    controls[CONTROL_SYMBOLS] = (button_t){4, 3, editor->symbols ? "abc" : "#+=", CYD_COLOR_WHITE};
    controls[CONTROL_SPACE] = (button_t){8, 5, "SPACE", CYD_COLOR_WHITE};
    static const char backspace_label[] = {CYD_GLYPH_BACKSPACE, '\0'};
    controls[CONTROL_DELETE] = (button_t){14, 3, backspace_label, CYD_COLOR_WHITE};
}

static const button_t editor_actions[ACTION_COUNT] = {
    {0, 8, "CANCEL", CYD_COLOR_WHITE},
    {9, 8, "SAVE", CYD_COLOR_YELLOW},
};

static void draw_editor(const editor_t *editor)
{
    button_t controls[CONTROL_COUNT];
    for (int field = 0; field < FIELD_COUNT; ++field) {
        draw_editor_field(editor, field);
    }
    draw_editor_message(editor);
    draw_keyboard(editor);
    editor_controls(editor, controls);
    draw_buttons(CONTROL_ROW, controls, CONTROL_COUNT);
    draw_buttons(ACTION_ROW, editor_actions, ACTION_COUNT);
}

static void editor_type(editor_t *editor, char character)
{
    char *text = editor->text[editor->active];
    size_t length = strlen(text);
    if (length < field_capacity(editor->active)) {
        text[length] = character;
        text[length + 1] = '\0';
    }
    editor->message = NULL;
    draw_editor_field(editor, editor->active);
    draw_editor_message(editor);
}

/* Saves the network for the launcher and every image (NVS cyd_config),
   then has the network task join it. */
static void save_wifi_settings(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "wifi_ssid", ssid);
        nvs_set_str(nvs, "wifi_pass", password);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Saved Wi-Fi network %s", ssid);
    rejoin_wifi();
}

/* Edits the network name and password; returns true when they were saved. */
static bool edit_wifi_settings(const char *ssid)
{
    /* A hidden network needs its name typed first. */
    editor_t editor = {.active = ssid[0] != '\0' ? FIELD_PASSWORD : FIELD_SSID};
    snprintf(editor.text[FIELD_SSID], field_capacity(FIELD_SSID) + 1, "%s", ssid);
    char saved_ssid[33];
    load_config_string("wifi_ssid", WIFI_SSID, saved_ssid, sizeof(saved_ssid));
    if (strcmp(ssid, saved_ssid) == 0) {
        load_config_string("wifi_pass", WIFI_PASSWORD, editor.text[FIELD_PASSWORD],
                           field_capacity(FIELD_PASSWORD) + 1);
    }
    draw_editor(&editor);
    xQueueReset(touch_queue);
    while (true) {
        cyd_touch_point_t point;
        xQueueReceive(touch_queue, &point, portMAX_DELAY);
        int row = point.y / CYD_FONT_HEIGHT;
        int column = point.x / CYD_FONT_WIDTH;
        if (row < FIELD_COUNT) {
            int previous = editor.active;
            editor.active = row;
            editor.message = NULL;
            draw_editor_field(&editor, previous);
            draw_editor_field(&editor, editor.active);
            draw_editor_message(&editor);
        } else if (row >= KEY_FIRST_ROW && row < KEY_FIRST_ROW + KEY_ROWS) {
            char character = editor_key(&editor, row - KEY_FIRST_ROW, column / KEY_COLUMNS);
            if (column / KEY_COLUMNS < KEYS_PER_ROW && character != '\0') {
                editor_type(&editor, character);
            }
        } else if (row == CONTROL_ROW) {
            button_t controls[CONTROL_COUNT];
            editor_controls(&editor, controls);
            if (button_hit(&controls[CONTROL_CAPS], column)) {
                editor.caps = !editor.caps;
            } else if (button_hit(&controls[CONTROL_SYMBOLS], column)) {
                editor.symbols = !editor.symbols;
            } else if (button_hit(&controls[CONTROL_SPACE], column)) {
                editor_type(&editor, ' ');
                continue;
            } else if (button_hit(&controls[CONTROL_DELETE], column)) {
                char *text = editor.text[editor.active];
                size_t length = strlen(text);
                if (length > 0) {
                    text[length - 1] = '\0';
                }
                editor.message = NULL;
                draw_editor_field(&editor, editor.active);
                draw_editor_message(&editor);
                continue;
            } else {
                continue;
            }
            draw_keyboard(&editor);
            editor_controls(&editor, controls);
            draw_buttons(CONTROL_ROW, controls, CONTROL_COUNT);
        } else if (row == ACTION_ROW) {
            if (button_hit(&editor_actions[ACTION_CANCEL], column)) {
                return false;
            }
            if (button_hit(&editor_actions[ACTION_SAVE], column)) {
                size_t password_length = strlen(editor.text[FIELD_PASSWORD]);
                if (editor.text[FIELD_SSID][0] == '\0') {
                    editor.message = "NETWORK IS EMPTY";
                } else if (password_length > 0 && password_length < 8) {
                    /* Open networks have no password; WPA needs 8 to 63. */
                    editor.message = "PASSWORD: 8 TO 63";
                } else {
                    save_wifi_settings(editor.text[FIELD_SSID], editor.text[FIELD_PASSWORD]);
                    return true;
                }
                draw_editor_message(&editor);
            }
        }
    }
}

/* Shows a network's details until a tap; CONNECT opens the editor. Returns
   true when a network was saved. */
static bool open_network(const wifi_ap_record_t *ap)
{
    show_wifi_detail(ap);
    cyd_touch_point_t point;
    xQueueReset(touch_queue);
    xQueueReceive(touch_queue, &point, portMAX_DELAY);
    return point.y / CYD_FONT_HEIGHT == CYD_TEXT_ROWS - 1 &&
           button_hit(&detail_buttons[DETAIL_CONNECT_BUTTON], point.x / CYD_FONT_WIDTH) &&
           edit_wifi_settings((const char *)ap->ssid);
}

/* Lists the networks in range until BACK, or until one is chosen and saved
   with its password. Returns true when one was saved; the network task is
   then joining it. */
static bool choose_wifi_network(void)
{
    network_list_t list = {.networks = calloc(MAX_WIFI_RESULTS, sizeof(wifi_ap_record_t))};
    if (list.networks == NULL) {
        show_message_and_wait("NO MEMORY", "for a Wi-Fi scan", NULL);
        return false;
    }
    bool saved = false;
    bool scan_wanted = true;
    list.status = net_state == NET_CONNECTING ? "JOINING" : "SCANNING";
    draw_network_list(&list);
    while (true) {
        /* The network task's join must finish first; its progress shows in
           the info row meanwhile. */
        if (scan_wanted && net_state != NET_CONNECTING) {
            scan_wanted = false;
            list.status = "SCANNING";
            draw_network_title(&list);
            list.status = scan_networks(&list) == ESP_OK ? NULL : "SCAN ERR";
            draw_network_list(&list);
            /* Drop taps from during the scan. */
            xQueueReset(touch_queue);
        }

        cyd_touch_point_t point;
        if (xQueueReceive(touch_queue, &point, pdMS_TO_TICKS(50)) == pdTRUE) {
            int row = point.y / CYD_FONT_HEIGHT;
            int column = point.x / CYD_FONT_WIDTH;
            int index = list.offset + row - MENU_FIRST_ROW;
            if (column >= BRIGHTNESS_COLUMN) {
                if (brightness_tap(row)) {
                    draw_network_list(&list);
                }
            } else if (row == 0) {
                if (button_hit(&scan_button, column) && !scan_wanted) {
                    scan_wanted = true;
                    list.status = net_state == NET_CONNECTING ? "JOINING" : "SCANNING";
                    draw_network_title(&list);
                }
            } else if (row >= MENU_FIRST_ROW && row < MENU_FIRST_ROW + MENU_ROWS &&
                       index < list.count) {
                draw_network(&list, index, true);
                vTaskDelay(pdMS_TO_TICKS(120));
                if (open_network(&list.networks[index])) {
                    saved = true;
                    break;
                }
                draw_network_list(&list);
            } else if (row == SCROLL_ROW) {
                if (button_hit(&back_button, column)) {
                    break;
                }
                int offset = scrolled_offset(list.offset, list.count, scroll_tap_direction(column));
                if (offset != list.offset) {
                    list.offset = offset;
                    draw_network_list(&list);
                }
            } else if (row == INFO_ROW) {
                next_info_page();
            }
        }
        update_info();
    }
    free(list.networks);
    return saved;
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
    draw_info();
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
    seed_config();
    load_slots();
    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));

    int boot_choice = default_slot();
    bool counting = boot_choice >= 0;
    build_entries();
    draw_menu(counting ? "" : "NO IMAGES");
    ESP_ERROR_CHECK(xTaskCreate(network_task, "network", 8192, NULL, 5, &network_task_handle) ==
                            pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);

    int64_t deadline = esp_timer_get_time() + MENU_TIMEOUT_US;
    int shown_seconds = -1;
    net_state_t shown_state = net_state;
    info_page_until = esp_timer_get_time() + INFO_PAGE_US;
    while (true) {
        cyd_touch_point_t point;
        if (xQueueReceive(touch_queue, &point, pdMS_TO_TICKS(50)) == pdTRUE) {
            int screen_row = point.y / CYD_FONT_HEIGHT;
            int column = point.x / CYD_FONT_WIDTH;
            int index = scroll_offset + screen_row - MENU_FIRST_ROW;
            if (column < MENU_COLUMNS && screen_row >= MENU_FIRST_ROW &&
                screen_row < MENU_FIRST_ROW + MENU_ROWS && index < entry_count) {
                activate_entry(index);
                /* Still here: booting or downloading failed. Start over. */
                boot_choice = default_slot();
                counting = boot_choice >= 0;
                deadline = esp_timer_get_time() + MENU_TIMEOUT_US;
                shown_seconds = -1;
                if (!counting) {
                    draw_title("NO IMAGES");
                }
            } else if (column < MENU_COLUMNS && screen_row == INFO_ROW &&
                       info_page == INFO_ACTIVITY) {
                /* The countdown waits while the network chooser is open,
                   then starts over. */
                choose_wifi_network();
                build_entries();
                draw_menu(title_text);
                deadline = esp_timer_get_time() + MENU_TIMEOUT_US;
                shown_seconds = -1;
            } else {
                /* Any other tap, the scroll buttons, brightness slider and info row
                   included, buys more time to choose. */
                if (column >= BRIGHTNESS_COLUMN) {
                    if (brightness_tap(screen_row)) {
                        draw_menu(title_text);
                    }
                } else if (screen_row == SCROLL_ROW) {
                    scroll_menu(scroll_tap_direction(column));
                } else if (screen_row == INFO_ROW) {
                    next_info_page();
                }
                deadline += MENU_TIMEOUT_US;
            }
        }

        update_info();

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
            /* The branches show only while the list is complete. */
            shown_state = state;
            build_entries();
            draw_entries();
            if (state == NET_READY && !counting && entry_count > 0) {
                draw_title("SELECT IMAGE");
            }
        }
    }
}
