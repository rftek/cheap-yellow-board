#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyd_display.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#define MAX_WIFI_RESULTS 24
#define MAX_BLE_RESULTS 32
#define BLE_SCAN_DURATION_MS 6000

#define BLE_SYNCED_BIT BIT0
#define BLE_SCAN_DONE_BIT BIT1

/* List header layout: title (tap to rescan), then up and down arrow buttons. */
#define HEADER_TITLE_COLUMNS 10
#define ARROW_UP_COLUMN 10
#define ARROW_DOWN_COLUMN 14
#define ARROW_COLUMNS 3

static const char *TAG = "cyd_scanner";
static QueueHandle_t touch_queue;
static EventGroupHandle_t ble_events;
static uint8_t ble_own_addr_type;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    uint8_t adv_type; /* advertising PDU type, excluding scan responses */
    int8_t rssi;
    int8_t tx_power;
    bool has_tx_power;
    bool has_company;
    bool has_service;
    uint16_t company_id;
    uint16_t service_uuid16;
    char name[32];
} ble_device_t;

enum { LIST_WIFI, LIST_BLE, LIST_COUNT };

typedef struct {
    int header_row;
    int rows;   /* visible entry rows below the header */
    int offset; /* index of the first visible entry */
} list_view_t;

/* A screen of text rows; the last row is reserved for a footer. */
typedef struct {
    char text[CYD_TEXT_ROWS][CYD_TEXT_COLUMNS + 1];
    uint16_t color[CYD_TEXT_ROWS];
    int count;
} page_t;

static wifi_ap_record_t wifi_results[MAX_WIFI_RESULTS];
static uint16_t wifi_result_count;
static uint16_t wifi_total_count;
static ble_device_t ble_results[MAX_BLE_RESULTS];
static int ble_result_count;
static bool ble_results_full;
static list_view_t list_views[LIST_COUNT];

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

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void wifi_scan(void)
{
    wifi_result_count = 0;
    wifi_total_count = 0;
    esp_err_t result = esp_wifi_scan_start(NULL, true);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(result));
        return;
    }
    esp_wifi_scan_get_ap_num(&wifi_total_count);
    wifi_result_count = MAX_WIFI_RESULTS;
    result = esp_wifi_scan_get_ap_records(&wifi_result_count, wifi_results);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not read Wi-Fi scan results: %s", esp_err_to_name(result));
        wifi_result_count = 0;
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi: %u networks", wifi_total_count);
    for (int i = 0; i < wifi_result_count; ++i) {
        const wifi_ap_record_t *ap = &wifi_results[i];
        ESP_LOGI(TAG, "  %4d dBm ch%-2u %-4s " MACSTR " %s", ap->rssi, ap->primary,
                 wifi_auth_name(ap->authmode), MAC2STR(ap->bssid),
                 ap->ssid[0] != '\0' ? (const char *)ap->ssid : "(hidden)");
    }
}

static int ble_find_or_add(const uint8_t *addr)
{
    for (int i = 0; i < ble_result_count; ++i) {
        if (memcmp(ble_results[i].addr, addr, 6) == 0) {
            return i;
        }
    }
    if (ble_result_count >= MAX_BLE_RESULTS) {
        ble_results_full = true;
        return -1;
    }
    ble_device_t *device = &ble_results[ble_result_count];
    memset(device, 0, sizeof(*device));
    memcpy(device->addr, addr, 6);
    device->adv_type = 0xFF;
    return ble_result_count++;
}

static int ble_gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    if (event->type == BLE_GAP_EVENT_DISC) {
        int index = ble_find_or_add(event->disc.addr.val);
        if (index < 0) {
            return 0;
        }
        ble_device_t *device = &ble_results[index];
        device->rssi = event->disc.rssi;
        device->addr_type = event->disc.addr.type;
        if (event->disc.event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
            device->adv_type = event->disc.event_type;
        }

        /* Advertisements and scan responses carry different fields; keep the
           first value seen for each. */
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                    event->disc.length_data) != 0) {
            return 0;
        }
        if (device->name[0] == '\0' && fields.name != NULL && fields.name_len > 0) {
            size_t length = fields.name_len < sizeof(device->name) - 1
                                ? fields.name_len
                                : sizeof(device->name) - 1;
            memcpy(device->name, fields.name, length);
            device->name[length] = '\0';
        }
        if (!device->has_tx_power && fields.tx_pwr_lvl_is_present) {
            device->tx_power = fields.tx_pwr_lvl;
            device->has_tx_power = true;
        }
        if (!device->has_company && fields.mfg_data != NULL && fields.mfg_data_len >= 2) {
            device->company_id = (uint16_t)(fields.mfg_data[0] | (fields.mfg_data[1] << 8));
            device->has_company = true;
        }
        if (!device->has_service && fields.uuids16 != NULL && fields.num_uuids16 > 0) {
            device->service_uuid16 = fields.uuids16[0].value;
            device->has_service = true;
        }
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        xEventGroupSetBits(ble_events, BLE_SCAN_DONE_BIT);
    }
    return 0;
}

static void ble_on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &ble_own_addr_type) != 0) {
        ESP_LOGE(TAG, "Could not determine BLE address type");
        return;
    }
    xEventGroupSetBits(ble_events, BLE_SYNCED_BIT);
}

static void ble_host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_init(void)
{
    ble_events = xEventGroupCreate();
    ESP_ERROR_CHECK(ble_events != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);
}

/* Named devices first, then strongest signal first. */
static int ble_compare(const void *left, const void *right)
{
    const ble_device_t *a = left;
    const ble_device_t *b = right;
    bool a_named = a->name[0] != '\0';
    bool b_named = b->name[0] != '\0';
    if (a_named != b_named) {
        return a_named ? -1 : 1;
    }
    return b->rssi - a->rssi;
}

static void ble_scan(void)
{
    ble_result_count = 0;
    ble_results_full = false;
    if ((xEventGroupWaitBits(ble_events, BLE_SYNCED_BIT, pdFALSE, pdTRUE,
                             pdMS_TO_TICKS(5000)) & BLE_SYNCED_BIT) == 0) {
        ESP_LOGE(TAG, "BLE host did not start");
        return;
    }
    /* Active scan without controller duplicate filtering so scan responses,
       which usually carry the device name, are not dropped. */
    struct ble_gap_disc_params params = {
        .passive = 0,
        .filter_duplicates = 0,
    };
    xEventGroupClearBits(ble_events, BLE_SCAN_DONE_BIT);
    int rc = ble_gap_disc(ble_own_addr_type, BLE_SCAN_DURATION_MS, &params,
                          ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE scan failed to start: %d", rc);
        return;
    }
    xEventGroupWaitBits(ble_events, BLE_SCAN_DONE_BIT, pdTRUE, pdTRUE,
                        pdMS_TO_TICKS(BLE_SCAN_DURATION_MS + 2000));
    qsort(ble_results, ble_result_count, sizeof(ble_results[0]), ble_compare);

    ESP_LOGI(TAG, "BLE: %d%s devices", ble_result_count, ble_results_full ? "+" : "");
    for (int i = 0; i < ble_result_count; ++i) {
        const ble_device_t *device = &ble_results[i];
        /* NimBLE stores addresses little-endian; print most significant byte first. */
        ESP_LOGI(TAG, "  %4d dBm %02x:%02x:%02x:%02x:%02x:%02x %s", device->rssi,
                 device->addr[5], device->addr[4], device->addr[3],
                 device->addr[2], device->addr[1], device->addr[0], device->name);
    }
}

static void show_status(const char *message)
{
    ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK));
    ESP_ERROR_CHECK(cyd_text_row(CYD_TEXT_ROWS / 2, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, message));
}

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

static void page_show(const page_t *page, const char *footer)
{
    for (int row = 0; row < CYD_TEXT_ROWS; ++row) {
        if (row < page->count) {
            ESP_ERROR_CHECK(cyd_text_row(row, page->color[row], CYD_COLOR_BLACK, page->text[row]));
        } else if (row == CYD_TEXT_ROWS - 1) {
            ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_GRAY, CYD_COLOR_BLACK, footer));
        } else {
            ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_WHITE, CYD_COLOR_BLACK, ""));
        }
    }
}

static void show_wifi_detail(const wifi_ap_record_t *ap)
{
    page_t page = {0};
    char phy[24];
    wifi_phy_text(ap, phy, sizeof(phy));
    page_add(&page, CYD_COLOR_YELLOW, "WI-FI NETWORK");
    page_add_wrapped(&page, CYD_COLOR_WHITE,
                     ap->ssid[0] != '\0' ? (const char *)ap->ssid : "(hidden)", 2);
    page_add(&page, CYD_COLOR_WHITE, "RSSI %d dBm", ap->rssi);
    page_add(&page, CYD_COLOR_WHITE, "CHANNEL %u", ap->primary);
    page_add(&page, CYD_COLOR_WHITE, "AUTH %s", wifi_auth_name(ap->authmode));
    page_add(&page, CYD_COLOR_WHITE, "CIPHER %s", wifi_cipher_name(ap->pairwise_cipher));
    page_add(&page, CYD_COLOR_WHITE, MACSTR, MAC2STR(ap->bssid));
    page_add(&page, CYD_COLOR_WHITE, "802.11%s%s", phy, ap->wps ? " WPS" : "");
    page_show(&page, "tap to go back");
}

static const char *ble_company_name(uint16_t company_id)
{
    switch (company_id) {
    case 0x0006:
        return "Microsoft";
    case 0x004C:
        return "Apple";
    case 0x0059:
        return "Nordic";
    case 0x0075:
        return "Samsung";
    case 0x0087:
        return "Garmin";
    case 0x00E0:
        return "Google";
    case 0x012D:
        return "Sony";
    case 0x0171:
        return "Amazon";
    case 0x02E5:
        return "Espressif";
    default:
        return NULL;
    }
}

static const char *ble_address_kind(const ble_device_t *device)
{
    if (device->addr_type == BLE_ADDR_PUBLIC) {
        return "PUBLIC ADDRESS";
    }
    /* Random address subtype is in the two most significant bits. */
    switch (device->addr[5] >> 6) {
    case 3:
        return "STATIC RANDOM";
    case 1:
        return "PRIVATE (RPA)";
    case 0:
        return "PRIVATE (NRPA)";
    default:
        return "RANDOM ADDRESS";
    }
}

static const char *ble_advertising_kind(uint8_t adv_type)
{
    switch (adv_type) {
    case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:
        return "CONNECTABLE";
    case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:
        return "DIRECTED";
    case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:
        return "SCANNABLE";
    case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND:
        return "NON-CONNECTABLE";
    default:
        return "ADV TYPE ?";
    }
}

static void show_ble_detail(const ble_device_t *device)
{
    page_t page = {0};
    page_add(&page, CYD_COLOR_CYAN, "BLE DEVICE");
    if (device->name[0] != '\0') {
        page_add_wrapped(&page, CYD_COLOR_WHITE, device->name, 2);
    } else {
        page_add(&page, CYD_COLOR_GRAY, "(no name)");
    }
    page_add(&page, CYD_COLOR_WHITE, "RSSI %d dBm", device->rssi);
    page_add(&page, CYD_COLOR_WHITE, "%02x:%02x:%02x:%02x:%02x:%02x",
             device->addr[5], device->addr[4], device->addr[3],
             device->addr[2], device->addr[1], device->addr[0]);
    page_add(&page, CYD_COLOR_WHITE, "%s", ble_address_kind(device));
    page_add(&page, CYD_COLOR_WHITE, "%s", ble_advertising_kind(device->adv_type));
    if (device->has_tx_power) {
        page_add(&page, CYD_COLOR_WHITE, "TX POWER %d dBm", device->tx_power);
    }
    if (device->has_company) {
        const char *company = ble_company_name(device->company_id);
        if (company != NULL) {
            page_add(&page, CYD_COLOR_WHITE, "MFR %s", company);
        } else {
            page_add(&page, CYD_COLOR_WHITE, "MFR 0x%04X", device->company_id);
        }
    }
    if (device->has_service) {
        page_add(&page, CYD_COLOR_WHITE, "SERVICE 0x%04X", device->service_uuid16);
    }
    page_show(&page, "tap to go back");
}

static int list_count(int list)
{
    return list == LIST_WIFI ? wifi_result_count : ble_result_count;
}

/* Splits the rows below the two headers between the lists: each gets up to
   half, and rows one list cannot fill go to the other. */
static void layout_lists(void)
{
    int list_rows = CYD_TEXT_ROWS - 2;
    int ble_wanted = ble_result_count < list_rows / 2 ? ble_result_count : list_rows / 2;
    int wifi_rows = wifi_result_count < list_rows - ble_wanted
                        ? wifi_result_count
                        : list_rows - ble_wanted;
    int ble_rows = ble_result_count < list_rows - wifi_rows
                       ? ble_result_count
                       : list_rows - wifi_rows;
    list_views[LIST_WIFI] = (list_view_t){.header_row = 0, .rows = wifi_rows};
    list_views[LIST_BLE] = (list_view_t){.header_row = 1 + wifi_rows, .rows = ble_rows};
}

static void draw_list_header(int list)
{
    const list_view_t *view = &list_views[list];
    char text[CYD_TEXT_COLUMNS + 1];
    char title[24];
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    uint16_t title_color = list == LIST_WIFI ? CYD_COLOR_YELLOW : CYD_COLOR_CYAN;
    bool can_scroll_up = view->offset > 0;
    bool can_scroll_down = view->offset + view->rows < list_count(list);

    if (list == LIST_WIFI) {
        snprintf(title, sizeof(title), "WI-FI %u", wifi_total_count);
    } else {
        snprintf(title, sizeof(title), "BLE %d%s", ble_result_count,
                 ble_results_full ? "+" : "");
    }
    memset(text, ' ', CYD_TEXT_COLUMNS);
    text[CYD_TEXT_COLUMNS] = '\0';
    memcpy(text, title, strnlen(title, HEADER_TITLE_COLUMNS - 1));
    for (int i = 0; i < CYD_TEXT_COLUMNS; ++i) {
        foreground[i] = title_color;
        background[i] = CYD_COLOR_BLACK;
    }
    for (int i = 0; i < ARROW_COLUMNS; ++i) {
        background[ARROW_UP_COLUMN + i] = CYD_COLOR_BUTTON;
        foreground[ARROW_UP_COLUMN + i] = can_scroll_up ? CYD_COLOR_WHITE : CYD_COLOR_BUTTON_DISABLED;
        background[ARROW_DOWN_COLUMN + i] = CYD_COLOR_BUTTON;
        foreground[ARROW_DOWN_COLUMN + i] =
            can_scroll_down ? CYD_COLOR_WHITE : CYD_COLOR_BUTTON_DISABLED;
    }
    text[ARROW_UP_COLUMN + 1] = CYD_GLYPH_UP;
    text[ARROW_DOWN_COLUMN + 1] = CYD_GLYPH_DOWN;
    ESP_ERROR_CHECK(cyd_draw_cells(view->header_row, text, foreground, background));
}

static void draw_list_entry(int list, int index, int row, bool highlighted)
{
    /* 17 columns: "-dBm" plus a 12-character SSID or name. */
    char line[CYD_TEXT_COLUMNS + 1];
    uint16_t color = CYD_COLOR_WHITE;
    if (list == LIST_WIFI) {
        const wifi_ap_record_t *ap = &wifi_results[index];
        snprintf(line, sizeof(line), "%4d %.12s", ap->rssi,
                 ap->ssid[0] != '\0' ? (const char *)ap->ssid : "(hidden)");
    } else {
        const ble_device_t *device = &ble_results[index];
        if (device->name[0] != '\0') {
            snprintf(line, sizeof(line), "%4d %.12s", device->rssi, device->name);
        } else {
            snprintf(line, sizeof(line), "%4d %02x%02x%02x%02x%02x%02x", device->rssi,
                     device->addr[5], device->addr[4], device->addr[3],
                     device->addr[2], device->addr[1], device->addr[0]);
            color = CYD_COLOR_GRAY;
        }
    }
    if (highlighted) {
        ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_BLACK, CYD_COLOR_WHITE, line));
    } else {
        ESP_ERROR_CHECK(cyd_text_row(row, color, CYD_COLOR_BUTTON, line));
    }
}

static void draw_list(int list)
{
    const list_view_t *view = &list_views[list];
    draw_list_header(list);
    for (int i = 0; i < view->rows; ++i) {
        draw_list_entry(list, view->offset + i, view->header_row + 1 + i, false);
    }
}

static void show_results(void)
{
    draw_list(LIST_WIFI);
    draw_list(LIST_BLE);
    const list_view_t *ble_view = &list_views[LIST_BLE];
    for (int row = ble_view->header_row + 1 + ble_view->rows; row < CYD_TEXT_ROWS; ++row) {
        ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_WHITE, CYD_COLOR_BLACK, ""));
    }
}

/* Pages a list by its visible height, stopping with the last page full. */
static void scroll_list(int list, int direction)
{
    list_view_t *view = &list_views[list];
    int max_offset = list_count(list) - view->rows;
    int offset = view->offset + direction * view->rows;
    if (offset > max_offset) {
        offset = max_offset;
    }
    if (offset < 0) {
        offset = 0;
    }
    if (offset != view->offset) {
        view->offset = offset;
        draw_list(list);
    }
}

static void open_detail(int list, int index, int row)
{
    draw_list_entry(list, index, row, true);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (list == LIST_WIFI) {
        show_wifi_detail(&wifi_results[index]);
    } else {
        show_ble_detail(&ble_results[index]);
    }
    cyd_touch_point_t point;
    xQueueReset(touch_queue);
    xQueueReceive(touch_queue, &point, portMAX_DELAY);
    show_results();
}

/* Returns true when the tap asks for a new scan. */
static bool handle_list_tap(const cyd_touch_point_t *point)
{
    int row = point->y / CYD_FONT_HEIGHT;
    int column = point->x / CYD_FONT_WIDTH;
    for (int list = 0; list < LIST_COUNT; ++list) {
        const list_view_t *view = &list_views[list];
        if (row == view->header_row) {
            if (column < HEADER_TITLE_COLUMNS) {
                return true;
            }
            if (column >= ARROW_UP_COLUMN && column < ARROW_UP_COLUMN + ARROW_COLUMNS) {
                scroll_list(list, -1);
            } else if (column >= ARROW_DOWN_COLUMN) {
                scroll_list(list, 1);
            }
            return false;
        }
        if (row > view->header_row && row <= view->header_row + view->rows) {
            open_detail(list, view->offset + row - view->header_row - 1, row);
            return false;
        }
    }
    return false;
}

static void scan_task(void *argument)
{
    (void)argument;
    while (true) {
        show_status(" Scanning Wi-Fi");
        wifi_scan();
        show_status(" Scanning BLE");
        ble_scan();
        layout_lists();
        show_results();
        ESP_LOGI(TAG, "Scan complete; free heap: %u bytes",
                 (unsigned)esp_get_free_heap_size());

        /* Drop taps that arrived during the scan, then handle taps until one
           asks for a rescan. */
        xQueueReset(touch_queue);
        cyd_touch_point_t point;
        do {
            xQueueReceive(touch_queue, &point, portMAX_DELAY);
        } while (!handle_list_tap(&point));
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
    wifi_init();
    ble_init();

    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));
    BaseType_t scan_task_result =
        xTaskCreate(scan_task, "scan", 6144, NULL, 5, NULL);
    ESP_ERROR_CHECK(scan_task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
