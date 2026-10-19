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
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "lwip/def.h"
#include "lwip/etharp.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"

#define MAX_WIFI_RESULTS 24
#define MAX_BLE_RESULTS 32
#define MAX_LAN_RESULTS 64
#define BLE_SCAN_DURATION_MS 6000

#define BLE_SYNCED_BIT BIT0
#define BLE_SCAN_DONE_BIT BIT1

/* Joining your network uses the Wi-Fi settings the launcher keeps in NVS. */
#define WIFI_JOIN_TIMEOUT_MS 10000
#define WIFI_CONNECTED_BIT BIT0

/* The LAN sweep ARPs every host of the board's /24, a few at a time so the
   small lwIP ARP table can hold the replies until they are read. */
#define ARP_BATCH_SIZE 8
#define LAN_HOSTS 254
/* Modem sleep (required alongside Bluetooth) delays some replies, so hosts
   that miss the first pass are asked again with a longer wait. */
#define ARP_FIRST_PASS_WAIT_MS 250
#define ARP_SECOND_PASS_WAIT_MS 700

/* List header layout: title (tap to rescan), then up and down arrow buttons. */
#define HEADER_TITLE_COLUMNS 10
#define ARROW_UP_COLUMN 10
#define ARROW_DOWN_COLUMN 14
#define ARROW_COLUMNS 3

#define LAN_COLOR 0x07E0 /* green */

static const char *TAG = "cyd_scanner";
static QueueHandle_t touch_queue;
static EventGroupHandle_t ble_events;
static uint8_t ble_own_addr_type;
static EventGroupHandle_t wifi_events;
static esp_netif_t *sta_netif;
static char wifi_ssid[33];
static char wifi_password[65];
static bool have_wifi_settings;
static volatile bool wifi_join_requested;
/* Valid while WIFI_CONNECTED_BIT is set. */
static esp_netif_ip_info_t ip_info;
static esp_netif_dns_info_t dns_info;
static uint8_t joined_bssid[6];

typedef struct {
    uint32_t ip; /* host byte order */
    uint8_t mac[6];
} lan_device_t;

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

enum { LIST_WIFI, LIST_BLE, LIST_LAN, LIST_COUNT };

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
static lan_device_t lan_results[MAX_LAN_RESULTS];
static int lan_result_count;
static bool lan_results_full;
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

/* Reads the Wi-Fi network the launcher saved; without one the board only listens. */
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
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        if (wifi_join_requested) {
            esp_wifi_connect();
        }
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

/* Joins the saved network after the first scan (a scan cannot run while a
   connection attempt is in progress) and waits for an address. */
static bool wifi_join(void)
{
    if (!have_wifi_settings) {
        return false;
    }
    if ((xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) == 0 && !wifi_join_requested) {
        wifi_join_requested = true;
        esp_wifi_connect();
    }
    EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(WIFI_JOIN_TIMEOUT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "Could not join %s", wifi_ssid);
        return false;
    }
    wifi_ap_record_t ap;
    esp_netif_get_ip_info(sta_netif, &ip_info);
    esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        memcpy(joined_bssid, ap.bssid, sizeof(joined_bssid));
    }
    ESP_LOGI(TAG, "Joined %s: IP " IPSTR ", gateway " IPSTR ", DNS " IPSTR, wifi_ssid,
             IP2STR(&ip_info.ip), IP2STR(&ip_info.gw), IP2STR(&dns_info.ip.u_addr.ip4));
    return true;
}

static bool wifi_is_joined(void)
{
    return (xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

/* The scan result for the access point the board is connected to. */
static bool wifi_is_joined_ap(const wifi_ap_record_t *ap)
{
    return wifi_is_joined() && memcmp(ap->bssid, joined_bssid, sizeof(joined_bssid)) == 0;
}

/* Dotted quad of an address in host byte order. */
static void format_ip(uint32_t ip, char *text, size_t size)
{
    snprintf(text, size, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16 & 0xFF),
             (unsigned)(ip >> 8 & 0xFF), (unsigned)(ip & 0xFF));
}

typedef struct {
    uint32_t ips[ARP_BATCH_SIZE]; /* host byte order */
    int count;
} arp_batch_t;

/* Hosts of the /24 (by last octet) that have answered this sweep. */
static bool lan_host_found[256];

/* Runs in the lwIP thread: asks every host of the batch for its MAC. */
static esp_err_t arp_request_batch(void *context)
{
    const arp_batch_t *batch = context;
    struct netif *netif = esp_netif_get_netif_impl(sta_netif);
    for (int i = 0; i < batch->count; ++i) {
        ip4_addr_t address = {.addr = lwip_htonl(batch->ips[i])};
        etharp_request(netif, &address);
    }
    return ESP_OK;
}

/* Runs in the lwIP thread: records the hosts of the batch that replied. */
static esp_err_t arp_collect_batch(void *context)
{
    const arp_batch_t *batch = context;
    struct netif *netif = esp_netif_get_netif_impl(sta_netif);
    for (int i = 0; i < batch->count; ++i) {
        ip4_addr_t address = {.addr = lwip_htonl(batch->ips[i])};
        struct eth_addr *mac = NULL;
        const ip4_addr_t *found = NULL;
        if (etharp_find_addr(netif, &address, &mac, &found) < 0 || mac == NULL) {
            continue;
        }
        lan_host_found[batch->ips[i] & 0xFF] = true;
        if (lan_result_count >= MAX_LAN_RESULTS) {
            lan_results_full = true;
            continue;
        }
        lan_device_t *device = &lan_results[lan_result_count++];
        device->ip = batch->ips[i];
        memcpy(device->mac, mac->addr, sizeof(device->mac));
    }
    return ESP_OK;
}

/* ARPs every host of the /24 at base that has not answered yet. */
static void arp_pass(uint32_t base, int wait_ms)
{
    arp_batch_t batch = {.count = 0};
    for (int host = 1; host <= LAN_HOSTS; ++host) {
        if (!lan_host_found[host]) {
            batch.ips[batch.count++] = base + host;
        }
        if (batch.count == ARP_BATCH_SIZE || (host == LAN_HOSTS && batch.count > 0)) {
            esp_netif_tcpip_exec(arp_request_batch, &batch);
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            esp_netif_tcpip_exec(arp_collect_batch, &batch);
            batch.count = 0;
        }
    }
}

static int lan_compare(const void *left, const void *right)
{
    uint32_t a = ((const lan_device_t *)left)->ip;
    uint32_t b = ((const lan_device_t *)right)->ip;
    return a < b ? -1 : a > b;
}

/* Finds the devices of the joined network's /24 that answer ARP. */
static void lan_sweep(void)
{
    lan_result_count = 0;
    lan_results_full = false;
    if (!wifi_is_joined()) {
        return;
    }
    uint32_t own_ip = lwip_ntohl(ip_info.ip.addr);
    uint32_t base = own_ip & 0xFFFFFF00;
    memset(lan_host_found, 0, sizeof(lan_host_found));
    /* The board never appears in its own ARP table, so add it directly. */
    lan_device_t *self = &lan_results[lan_result_count++];
    self->ip = own_ip;
    esp_wifi_get_mac(WIFI_IF_STA, self->mac);
    lan_host_found[own_ip & 0xFF] = true;
    arp_pass(base, ARP_FIRST_PASS_WAIT_MS);
    arp_pass(base, ARP_SECOND_PASS_WAIT_MS);
    qsort(lan_results, lan_result_count, sizeof(lan_results[0]), lan_compare);
    ESP_LOGI(TAG, "LAN: %d%s devices", lan_result_count, lan_results_full ? "+" : "");
    for (int i = 0; i < lan_result_count; ++i) {
        char ip[16];
        format_ip(lan_results[i].ip, ip, sizeof(ip));
        ESP_LOGI(TAG, "  %-15s " MACSTR, ip, MAC2STR(lan_results[i].mac));
    }
}

static void wifi_scan(void)
{
    wifi_result_count = 0;
    wifi_total_count = 0;
    esp_err_t result = esp_wifi_scan_start(NULL, true);
    /* A reconnect attempt in progress blocks scanning briefly. */
    for (int retry = 0; result == ESP_ERR_WIFI_STATE && retry < 10; ++retry) {
        vTaskDelay(pdMS_TO_TICKS(500));
        result = esp_wifi_scan_start(NULL, true);
    }
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
    page_add(&page, CYD_COLOR_WHITE, "CH %u  %d dBm", ap->primary, ap->rssi);
    /* Addresses exist only on the network the board joined; they come before
       the rest so they always fit on the page. */
    if (wifi_is_joined_ap(ap)) {
        char ip[16];
        page_add(&page, CYD_COLOR_GRAY, "BOARD/GATEWAY/DNS");
        format_ip(lwip_ntohl(ip_info.ip.addr), ip, sizeof(ip));
        page_add(&page, CYD_COLOR_WHITE, "%s", ip);
        format_ip(lwip_ntohl(ip_info.gw.addr), ip, sizeof(ip));
        page_add(&page, CYD_COLOR_WHITE, "%s", ip);
        format_ip(lwip_ntohl(dns_info.ip.u_addr.ip4.addr), ip, sizeof(ip));
        page_add(&page, CYD_COLOR_WHITE, "%s", ip);
    } else {
        page_add(&page, CYD_COLOR_GRAY, "IP: not joined");
    }
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

static const char *lan_role(const lan_device_t *device)
{
    if (device->ip == lwip_ntohl(ip_info.ip.addr)) {
        return "ME";
    }
    if (device->ip == lwip_ntohl(ip_info.gw.addr)) {
        return "GW";
    }
    return "";
}

static void show_lan_detail(const lan_device_t *device)
{
    page_t page = {0};
    char ip[16];
    page_add(&page, LAN_COLOR, "LAN DEVICE");
    format_ip(device->ip, ip, sizeof(ip));
    page_add(&page, CYD_COLOR_WHITE, "%s", ip);
    page_add(&page, CYD_COLOR_WHITE, MACSTR, MAC2STR(device->mac));
    if (device->ip == lwip_ntohl(ip_info.ip.addr)) {
        page_add(&page, CYD_COLOR_WHITE, "THIS BOARD");
    }
    if (device->ip == lwip_ntohl(ip_info.gw.addr)) {
        page_add(&page, CYD_COLOR_WHITE, "GATEWAY");
    }
    if (device->ip == lwip_ntohl(dns_info.ip.u_addr.ip4.addr)) {
        page_add(&page, CYD_COLOR_WHITE, "DNS SERVER");
    }
    page_add(&page, CYD_COLOR_GRAY, "ON %.14s", wifi_ssid);
    page_show(&page, "tap to go back");
}

static int list_count(int list)
{
    switch (list) {
    case LIST_WIFI:
        return wifi_result_count;
    case LIST_BLE:
        return ble_result_count;
    default:
        return lan_result_count;
    }
}

/* Shares the rows below the three headers between the lists one row at a
   time, so each list gets a fair share and rows one cannot fill go to the
   others. */
static void layout_lists(void)
{
    int rows[LIST_COUNT] = {0};
    int free_rows = CYD_TEXT_ROWS - LIST_COUNT;
    bool placed = true;
    while (free_rows > 0 && placed) {
        placed = false;
        for (int list = 0; list < LIST_COUNT && free_rows > 0; ++list) {
            if (rows[list] < list_count(list)) {
                ++rows[list];
                --free_rows;
                placed = true;
            }
        }
    }
    int header_row = 0;
    for (int list = 0; list < LIST_COUNT; ++list) {
        list_views[list] = (list_view_t){.header_row = header_row, .rows = rows[list]};
        header_row += 1 + rows[list];
    }
}

static void draw_list_header(int list)
{
    const list_view_t *view = &list_views[list];
    char text[CYD_TEXT_COLUMNS + 1];
    char title[24];
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    uint16_t title_color = list == LIST_WIFI  ? CYD_COLOR_YELLOW
                           : list == LIST_BLE ? CYD_COLOR_CYAN
                                              : LAN_COLOR;
    bool can_scroll_up = view->offset > 0;
    bool can_scroll_down = view->offset + view->rows < list_count(list);

    if (list == LIST_WIFI) {
        snprintf(title, sizeof(title), "WI-FI %u", wifi_total_count);
    } else if (list == LIST_BLE) {
        snprintf(title, sizeof(title), "BLE %d%s", ble_result_count,
                 ble_results_full ? "+" : "");
    } else if (!have_wifi_settings) {
        snprintf(title, sizeof(title), "LAN: none");
    } else if (!wifi_is_joined()) {
        snprintf(title, sizeof(title), "LAN: off");
    } else {
        snprintf(title, sizeof(title), "LAN %d%s", lan_result_count,
                 lan_results_full ? "+" : "");
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
    } else if (list == LIST_BLE) {
        const ble_device_t *device = &ble_results[index];
        if (device->name[0] != '\0') {
            snprintf(line, sizeof(line), "%4d %.12s", device->rssi, device->name);
        } else {
            snprintf(line, sizeof(line), "%4d %02x%02x%02x%02x%02x%02x", device->rssi,
                     device->addr[5], device->addr[4], device->addr[3],
                     device->addr[2], device->addr[1], device->addr[0]);
            color = CYD_COLOR_GRAY;
        }
    } else {
        /* "192.168.1.1    GW": the address, then ME or GW at the right. */
        char ip[16];
        format_ip(lan_results[index].ip, ip, sizeof(ip));
        snprintf(line, sizeof(line), "%-15s%.2s", ip, lan_role(&lan_results[index]));
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
    for (int list = 0; list < LIST_COUNT; ++list) {
        draw_list(list);
    }
    const list_view_t *last_view = &list_views[LIST_COUNT - 1];
    for (int row = last_view->header_row + 1 + last_view->rows; row < CYD_TEXT_ROWS; ++row) {
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

/* ---- Buttons ---- */

typedef struct {
    int first_column;
    int columns;
    const char *label;
    uint16_t foreground;
} button_t;

/* Draws a row of buttons with centered labels; other cells stay black. */
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
        const button_t *button = &buttons[b];
        int length = (int)strnlen(button->label, button->columns);
        int start = button->first_column + (button->columns - length) / 2;
        for (int i = 0; i < button->columns; ++i) {
            foreground[button->first_column + i] = button->foreground;
            background[button->first_column + i] = CYD_COLOR_BUTTON;
        }
        memcpy(text + start, button->label, length);
    }
    ESP_ERROR_CHECK(cyd_draw_cells(row, text, foreground, background));
}

static bool button_hit(const button_t *button, int column)
{
    return column >= button->first_column &&
           column < button->first_column + button->columns;
}

/* ---- Wi-Fi settings editor ---- */

/* Detail page footer for a Wi-Fi network. */
static const button_t detail_buttons[] = {
    {0, 7, "BACK", CYD_COLOR_WHITE},
    {8, 9, "CONNECT", CYD_COLOR_YELLOW},
};
#define DETAIL_CONNECT_BUTTON 1

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
    controls[CONTROL_DELETE] = (button_t){14, 3, "DEL", CYD_COLOR_WHITE};
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

/* Saves the network for every image (NVS cyd_config, read by the launcher
   and the other images too), then rejoins with it. */
static void save_wifi_settings(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    if (nvs_open(CYD_CONFIG_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "wifi_ssid", ssid);
        nvs_set_str(nvs, "wifi_pass", password);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", ssid);
    snprintf(wifi_password, sizeof(wifi_password), "%s", password);
    have_wifi_settings = true;
    ESP_LOGI(TAG, "Saved Wi-Fi network %s", wifi_ssid);

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, wifi_ssid, strnlen(wifi_ssid, sizeof(wifi_config.sta.ssid)));
    memcpy(wifi_config.sta.password, wifi_password,
           strnlen(wifi_password, sizeof(wifi_config.sta.password) - 1));
    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
    wifi_join_requested = true;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();
}

/* Edits the network name and password; returns true when they were saved. */
static bool edit_wifi_settings(const char *ssid)
{
    editor_t editor = {.active = FIELD_PASSWORD};
    snprintf(editor.text[FIELD_SSID], field_capacity(FIELD_SSID) + 1, "%s", ssid);
    if (have_wifi_settings && strcmp(ssid, wifi_ssid) == 0) {
        snprintf(editor.text[FIELD_PASSWORD], field_capacity(FIELD_PASSWORD) + 1, "%s",
                 wifi_password);
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

/* Shows an entry's details until a tap; returns true when the Wi-Fi
   settings changed and the lists should be rescanned. */
static bool open_detail(int list, int index, int row)
{
    draw_list_entry(list, index, row, true);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (list == LIST_WIFI) {
        show_wifi_detail(&wifi_results[index]);
        draw_buttons(CYD_TEXT_ROWS - 1, detail_buttons, 2);
    } else if (list == LIST_BLE) {
        show_ble_detail(&ble_results[index]);
    } else {
        show_lan_detail(&lan_results[index]);
    }
    cyd_touch_point_t point;
    xQueueReset(touch_queue);
    xQueueReceive(touch_queue, &point, portMAX_DELAY);
    if (list == LIST_WIFI && point.y / CYD_FONT_HEIGHT == CYD_TEXT_ROWS - 1 &&
        button_hit(&detail_buttons[DETAIL_CONNECT_BUTTON], point.x / CYD_FONT_WIDTH) &&
        edit_wifi_settings((const char *)wifi_results[index].ssid)) {
        return true;
    }
    show_results();
    return false;
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
            return open_detail(list, view->offset + row - view->header_row - 1, row);
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
        if (have_wifi_settings) {
            show_status(" Joining Wi-Fi");
            if (wifi_join()) {
                show_status(" Scanning LAN");
                lan_sweep();
            }
        }
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
