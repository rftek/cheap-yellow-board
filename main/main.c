#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyd_display.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gap_bt_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define DEVICE_NAME "CYD BT DIAG"
#define MAX_DEVICES 40
#define MAX_SERVICES 8
#define LOCAL_MTU 517

/* Set by the Bluetooth callbacks for the task waiting on them. */
#define EV_CLASSIC_DONE BIT0
#define EV_BLE_PARAMS BIT1
#define EV_BLE_DONE BIT2
#define EV_GATTC_REG BIT3
#define EV_OPEN BIT4
#define EV_MTU BIT5
#define EV_SEARCH BIT6
#define EV_READ BIT7
#define EV_CLOSE BIT8
#define EV_CANCEL BIT9
#define EV_SDP BIT10
#define EV_NAME BIT11
#define EV_TX_LEVEL BIT12
#define EV_PAGE_TIMEOUT BIT13

/* List screen: header, eight entries, footer. */
#define LIST_FIRST_ROW 1
#define LIST_ROWS (CYD_TEXT_ROWS - 2)
#define FOOTER_ROW (CYD_TEXT_ROWS - 1)
#define HEADER_TITLE_COLUMNS 5
#define ARROW_UP_COLUMN 10
#define ARROW_DOWN_COLUMN 14
#define ARROW_COLUMNS 3

/* Setting rows: label, value, then - and + buttons. */
#define SETTING_LABEL_COLUMNS 9
#define SETTING_VALUE_COLUMNS 4
#define SETTING_MINUS_COLUMN 13
#define SETTING_PLUS_COLUMN 15
#define CONFIG_ROWS (CYD_TEXT_ROWS - 2)

#define COLOR_GREEN 0x07E0

#define SETTINGS_NAMESPACE "bt_diag"
#define SETTINGS_KEY "settings"

static const char *TAG = "bt_diag";
static QueueHandle_t touch_queue;
static EventGroupHandle_t bt_events;
static esp_gatt_if_t gattc_if = ESP_GATT_IF_NONE;
static esp_bt_controller_config_t controller_config;
static esp_bluedroid_config_t host_config;
/* Set when a tap ends a connection test early. */
static bool probe_aborted;

/* ---- Settings ---- */

enum {
    SET_INQUIRY,
    SET_BLE_SCAN,
    SET_ACTIVE,
    SET_INTERVAL,
    SET_DUTY,
    SET_CONNECT,
    SET_PAGE_TIMEOUT,
    SET_AUTO_TEST,
    SET_BLE_ADV_POWER,
    SET_BLE_SCAN_POWER,
    SET_BLE_CONN_POWER,
    SET_BREDR_MIN_POWER,
    SET_BREDR_MAX_POWER,
    SET_VISIBLE,
    SET_COUNT
};

typedef struct {
    const char *label;
    const char *const *options;
    uint8_t count;
    uint8_t initial;
} setting_info_t;

/* Indexed by esp_power_level_t. */
static const char *const power_options[] = {"-12", "-9", "-6", "-3", "0", "+3", "+6", "+9"};
static const char *const off_on_options[] = {"OFF", "ON"};
static const char *const inquiry_options[] = {"1.3", "2.6", "3.8", "5.1", "6.4", "7.7", "10", "13"};
static const uint8_t inquiry_units[] = {1, 2, 3, 4, 5, 6, 8, 10}; /* 1.28 s */
static const char *const scan_options[] = {"2", "4", "6", "10", "15", "20", "30"};
static const uint8_t scan_seconds[] = {2, 4, 6, 10, 15, 20, 30};
static const char *const interval_options[] = {"10", "20", "50", "100", "200", "500", "1000"};
static const uint16_t interval_ms[] = {10, 20, 50, 100, 200, 500, 1000};
static const char *const duty_options[] = {"10", "25", "50", "100"};
static const uint8_t duty_percent[] = {10, 25, 50, 100};
static const char *const connect_options[] = {"2", "3", "5", "8", "10", "15"};
static const uint8_t connect_seconds[] = {2, 3, 5, 8, 10, 15};
static const char *const page_options[] = {"1.3", "2.6", "5.1", "10", "20", "41"};
static const uint16_t page_units[] = {2048, 4096, 8192, 16384, 32768, 65535}; /* 0.625 ms */

#define OPTIONS(list) list, sizeof(list) / sizeof(list[0])

static const setting_info_t setting_info[SET_COUNT] = {
    [SET_INQUIRY] = {"INQUIRY s", OPTIONS(inquiry_options), 4},
    [SET_BLE_SCAN] = {"LE SCAN s", OPTIONS(scan_options), 2},
    [SET_ACTIVE] = {"LE ACTIVE", OPTIONS(off_on_options), 1},
    [SET_INTERVAL] = {"LE INT ms", OPTIONS(interval_options), 3},
    [SET_DUTY] = {"LE DUTY %", OPTIONS(duty_options), 3},
    [SET_CONNECT] = {"LE CONN s", OPTIONS(connect_options), 2},
    [SET_PAGE_TIMEOUT] = {"PAGE TO s", OPTIONS(page_options), 2},
    [SET_AUTO_TEST] = {"AUTO TEST", OPTIONS(off_on_options), 1},
    [SET_BLE_ADV_POWER] = {"LE ADV", OPTIONS(power_options), ESP_PWR_LVL_P3},
    [SET_BLE_SCAN_POWER] = {"LE SCAN", OPTIONS(power_options), ESP_PWR_LVL_P3},
    [SET_BLE_CONN_POWER] = {"LE CONN", OPTIONS(power_options), ESP_PWR_LVL_P3},
    [SET_BREDR_MIN_POWER] = {"BR/EDR LO", OPTIONS(power_options), ESP_PWR_LVL_N0},
    [SET_BREDR_MAX_POWER] = {"BR/EDR HI", OPTIONS(power_options), ESP_PWR_LVL_P9},
    [SET_VISIBLE] = {"VISIBLE", OPTIONS(off_on_options), 0},
};

/* Option indexes, saved in NVS as one blob. */
static uint8_t settings[SET_COUNT];
static bool settings_loaded;

/* ---- Devices ---- */

enum { PROBE_UNTESTED, PROBE_NOT_CONNECTABLE, PROBE_OK, PROBE_FAILED };

typedef struct {
    uint8_t len;    /* UUID length in bytes: 2, 4 or 16 */
    uint32_t value; /* the UUID, or the first 32 bits of a 128-bit one */
} service_t;

typedef struct {
    esp_bd_addr_t addr; /* most significant byte first */
    bool classic;       /* found by BR/EDR inquiry, not LE scanning */
    uint8_t dev_type;   /* LE: esp_bt_dev_type_t from the advertising */
    uint8_t addr_type;
    uint8_t adv_type; /* LE advertising PDU type, excluding scan responses */
    int8_t rssi;
    bool has_rssi;
    int8_t tx_power;
    bool has_tx_power;
    bool has_company;
    uint16_t company_id;
    bool has_service;
    uint16_t service_uuid16; /* first one advertised */
    uint32_t cod;            /* Classic: class of device */
    char name[32];

    /* Connection test */
    uint8_t probe;
    const char *probe_reason; /* failure text, or NULL to show probe_code */
    uint16_t probe_code;
    uint16_t connect_ms;
    uint16_t mtu;
    uint16_t interval; /* 1.25 ms units */
    uint16_t latency;
    uint16_t timeout; /* 10 ms units */
    uint8_t service_count; /* all found; up to MAX_SERVICES kept */
    service_t services[MAX_SERVICES];
    char manufacturer[24];
    char model[24];
} bt_device_t;

static bt_device_t devices[MAX_DEVICES];
static volatile int device_count;
static bool devices_full;
static int list_offset;

/* The connection test in progress, filled by the callbacks. */
static struct {
    esp_bd_addr_t addr;
    volatile bool active;
    uint16_t conn_id;
    esp_gatt_status_t open_status;
    bool connected;
    volatile bool disconnected;
    uint16_t close_reason;
    uint16_t mtu;
    esp_gatt_conn_params_t params;
    uint8_t service_count;
    service_t services[MAX_SERVICES];
    esp_gatt_status_t read_status;
    char read_text[32];
    bool acl_seen;
    esp_bt_status_t acl_status;
    int64_t acl_us;
    esp_bt_status_t sdp_status;
    esp_bt_status_t name_status;
} probe;

/* Read back from the controller for the configuration pages. */
static int8_t gap_tx_level[ESP_BT_GAP_TX_PWR_LVL_TYPE_MAX];
static bool gap_tx_level_valid[ESP_BT_GAP_TX_PWR_LVL_TYPE_MAX];
static uint16_t page_timeout_read;
static bool page_timeout_valid;

/* ---- Settings storage ---- */

/* Returns false when nothing was saved and the defaults are in use. */
static bool load_settings(void)
{
    for (int i = 0; i < SET_COUNT; ++i) {
        settings[i] = setting_info[i].initial;
    }
    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    uint8_t saved[SET_COUNT];
    size_t size = sizeof(saved);
    bool loaded = nvs_get_blob(nvs, SETTINGS_KEY, saved, &size) == ESP_OK && size == sizeof(saved);
    if (loaded) {
        for (int i = 0; i < SET_COUNT; ++i) {
            if (saved[i] < setting_info[i].count) {
                settings[i] = saved[i];
            }
        }
    }
    nvs_close(nvs);
    return loaded;
}

/* Starts the TX power settings at the controller's own levels. */
static void use_controller_power(void)
{
    static const struct {
        int setting;
        esp_ble_power_type_t type;
    } ble_powers[] = {
        {SET_BLE_ADV_POWER, ESP_BLE_PWR_TYPE_ADV},
        {SET_BLE_SCAN_POWER, ESP_BLE_PWR_TYPE_SCAN},
        {SET_BLE_CONN_POWER, ESP_BLE_PWR_TYPE_DEFAULT},
    };
    for (size_t i = 0; i < sizeof(ble_powers) / sizeof(ble_powers[0]); ++i) {
        esp_power_level_t level = esp_ble_tx_power_get(ble_powers[i].type);
        if (level >= 0 && level < setting_info[ble_powers[i].setting].count) {
            settings[ble_powers[i].setting] = (uint8_t)level;
        }
    }
    esp_power_level_t bredr_min;
    esp_power_level_t bredr_max;
    if (esp_bredr_tx_power_get(&bredr_min, &bredr_max) == ESP_OK &&
        bredr_min <= bredr_max && bredr_max < setting_info[SET_BREDR_MAX_POWER].count) {
        settings[SET_BREDR_MIN_POWER] = (uint8_t)bredr_min;
        settings[SET_BREDR_MAX_POWER] = (uint8_t)bredr_max;
    }
}

static void save_settings(void)
{
    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_blob(nvs, SETTINGS_KEY, settings, sizeof(settings));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static int connect_timeout_ms(void)
{
    return connect_seconds[settings[SET_CONNECT]] * 1000;
}

static int page_timeout_ms(void)
{
    return page_units[settings[SET_PAGE_TIMEOUT]] * 5 / 8;
}

/* Applies the radio settings that can change while Bluetooth runs. */
static void apply_settings(void)
{
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, settings[SET_BLE_ADV_POWER]);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, settings[SET_BLE_SCAN_POWER]);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, settings[SET_BLE_CONN_POWER]);
    if (settings[SET_BREDR_MIN_POWER] > settings[SET_BREDR_MAX_POWER]) {
        settings[SET_BREDR_MIN_POWER] = settings[SET_BREDR_MAX_POWER];
    }
    esp_err_t err = esp_bredr_tx_power_set(settings[SET_BREDR_MIN_POWER],
                                           settings[SET_BREDR_MAX_POWER]);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BR/EDR TX power not set: %s", esp_err_to_name(err));
    }
    esp_bt_gap_set_page_timeout(page_units[settings[SET_PAGE_TIMEOUT]]);
    bool visible = settings[SET_VISIBLE] != 0;
    esp_bt_gap_set_scan_mode(visible ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
                             visible ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
}

/* ---- Names and formatting ---- */

static const char *power_text(int level)
{
    return level >= 0 && level < (int)(sizeof(power_options) / sizeof(power_options[0]))
               ? power_options[level]
               : "?";
}

static const char *cod_major_name(uint32_t cod)
{
    switch (esp_bt_gap_get_cod_major_dev(cod)) {
    case ESP_BT_COD_MAJOR_DEV_MISC:
        return "MISC";
    case ESP_BT_COD_MAJOR_DEV_COMPUTER:
        return "COMPUTER";
    case ESP_BT_COD_MAJOR_DEV_PHONE:
        return "PHONE";
    case ESP_BT_COD_MAJOR_DEV_LAN_NAP:
        return "NETWORK";
    case ESP_BT_COD_MAJOR_DEV_AV:
        return "AUDIO/VIDEO";
    case ESP_BT_COD_MAJOR_DEV_PERIPHERAL:
        return "PERIPHERAL";
    case ESP_BT_COD_MAJOR_DEV_IMAGING:
        return "IMAGING";
    case ESP_BT_COD_MAJOR_DEV_WEARABLE:
        return "WEARABLE";
    case ESP_BT_COD_MAJOR_DEV_TOY:
        return "TOY";
    case ESP_BT_COD_MAJOR_DEV_HEALTH:
        return "HEALTH";
    default:
        return "UNCATEGORIZED";
    }
}

static const char *company_name(uint16_t company_id)
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

/* Names for 16-bit service UUIDs, shared by SDP (Classic) and GATT (LE);
   at most 12 characters. */
static const char *service_name(const service_t *service)
{
    if (service->len == 16) {
        return service->value == 0x6E400001 ? "NORDIC UART" : "128-BIT";
    }
    if (service->len != 2) {
        return "32-BIT";
    }
    switch (service->value) {
    case 0x1101:
        return "SERIAL PORT";
    case 0x1103:
        return "DIAL-UP NET";
    case 0x1105:
        return "OBEX PUSH";
    case 0x1106:
        return "OBEX FILES";
    case 0x1108:
        return "HEADSET";
    case 0x110A:
        return "AUDIO SOURCE";
    case 0x110B:
        return "AUDIO SINK";
    case 0x110C:
        return "AVRCP TARGET";
    case 0x110E:
        return "AVRCP";
    case 0x110F:
        return "AVRCP CTRL";
    case 0x1112:
        return "HEADSET AG";
    case 0x1115:
        return "PAN USER";
    case 0x1116:
        return "NET ACCESS";
    case 0x111E:
        return "HANDS-FREE";
    case 0x111F:
        return "HANDS-FREE AG";
    case 0x1124:
        return "HID";
    case 0x112D:
        return "SIM ACCESS";
    case 0x112F:
        return "PHONEBOOK";
    case 0x1132:
        return "MESSAGES";
    case 0x1200:
        return "PNP INFO";
    case 0x1203:
        return "GENERIC AUDIO";
    case 0x1800:
        return "GENERIC ACC";
    case 0x1801:
        return "GENERIC ATTR";
    case 0x1802:
        return "ALERT";
    case 0x1803:
        return "LINK LOSS";
    case 0x1804:
        return "TX POWER";
    case 0x1805:
        return "CURRENT TIME";
    case 0x180A:
        return "DEVICE INFO";
    case 0x180D:
        return "HEART RATE";
    case 0x180F:
        return "BATTERY";
    case 0x1810:
        return "BLOOD PRESS";
    case 0x1812:
        return "HID";
    case 0x1816:
        return "CYCLING";
    case 0x181A:
        return "ENVIRONMENT";
    case 0x181C:
        return "USER DATA";
    case 0x1826:
        return "FITNESS";
    case 0xFD6F:
        return "EXPOSURE";
    case 0xFE2C:
        return "FAST PAIR";
    case 0xFEAA:
        return "EDDYSTONE";
    default:
        return "";
    }
}

static void service_text(const service_t *service, char *text, size_t size)
{
    if (service->len == 2) {
        snprintf(text, size, "%04X %s", (unsigned)service->value, service_name(service));
    } else {
        snprintf(text, size, "%08X %s", (unsigned)service->value, service_name(service));
    }
}

static void add_service(service_t *services, uint8_t *count, const esp_bt_uuid_t *uuid)
{
    if (*count < MAX_SERVICES) {
        service_t *service = &services[*count];
        service->len = (uint8_t)uuid->len;
        if (uuid->len == ESP_UUID_LEN_16) {
            service->value = uuid->uuid.uuid16;
        } else if (uuid->len == ESP_UUID_LEN_32) {
            service->value = uuid->uuid.uuid32;
        } else {
            /* 128-bit UUIDs are stored least significant byte first. */
            const uint8_t *bytes = uuid->uuid.uuid128;
            service->value = ((uint32_t)bytes[15] << 24) | ((uint32_t)bytes[14] << 16) |
                             ((uint32_t)bytes[13] << 8) | bytes[12];
        }
    }
    if (*count < UINT8_MAX) {
        ++*count;
    }
}

static const char *ble_address_kind(const bt_device_t *device)
{
    if (device->addr_type == BLE_ADDR_TYPE_PUBLIC) {
        return "PUBLIC ADDRESS";
    }
    /* Random address subtype is in the two most significant bits. */
    switch (device->addr[0] >> 6) {
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
    case ESP_BLE_EVT_CONN_ADV:
        return "CONNECTABLE ADV";
    case ESP_BLE_EVT_CONN_DIR_ADV:
        return "DIRECTED ADV";
    case ESP_BLE_EVT_DISC_ADV:
        return "SCANNABLE ONLY";
    case ESP_BLE_EVT_NON_CONN_ADV:
        return "BEACON ONLY";
    default:
        return "ADV TYPE ?";
    }
}

/* Short failure text (12 characters at most), or NULL for an unknown code. */
static const char *bt_status_text(int status)
{
    switch (status) {
    case ESP_BT_STATUS_HCI_PAGE_TIMEOUT:
        return "PAGE TIMEOUT";
    case ESP_BT_STATUS_HCI_CONNECTION_TOUT:
        return "LINK TIMEOUT";
    case ESP_BT_STATUS_HCI_HOST_REJECT_RESOURCES:
    case ESP_BT_STATUS_HCI_HOST_REJECT_SECURITY:
    case ESP_BT_STATUS_HCI_HOST_REJECT_DEVICE:
        return "REJECTED";
    case ESP_BT_STATUS_HCI_AUTH_FAILURE:
    case ESP_BT_STATUS_AUTH_FAILURE:
    case ESP_BT_STATUS_AUTH_REJECTED:
        return "AUTH FAILED";
    case ESP_BT_STATUS_HCI_HOST_TIMEOUT:
    case ESP_BT_STATUS_TIMEOUT:
        return "TIMEOUT";
    case ESP_BT_STATUS_BUSY:
        return "BUSY";
    case ESP_BT_STATUS_RMT_DEV_DOWN:
        return "DEVICE DOWN";
    default:
        return NULL;
    }
}

static const char *gatt_reason_text(int reason)
{
    switch (reason) {
    case ESP_GATT_CONN_TIMEOUT:
        return "LINK TIMEOUT";
    case ESP_GATT_CONN_TERMINATE_PEER_USER:
        return "PEER CLOSED";
    case ESP_GATT_CONN_TERMINATE_LOCAL_HOST:
        return "LOCAL CLOSE";
    case ESP_GATT_CONN_FAIL_ESTABLISH:
        return "CONN FAILED";
    case ESP_GATT_CONN_LMP_TIMEOUT:
        return "LMP TIMEOUT";
    case ESP_GATT_CONN_CONN_CANCEL:
        return "CANCELLED";
    default:
        return NULL;
    }
}

/* ---- Bluetooth callbacks (Bluedroid's BTC task) ---- */

static int find_or_add(const uint8_t *addr, bool classic)
{
    for (int i = 0; i < device_count; ++i) {
        if (devices[i].classic == classic && memcmp(devices[i].addr, addr, 6) == 0) {
            return i;
        }
    }
    if (device_count >= MAX_DEVICES) {
        devices_full = true;
        return -1;
    }
    bt_device_t *device = &devices[device_count];
    memset(device, 0, sizeof(*device));
    memcpy(device->addr, addr, 6);
    device->classic = classic;
    device->adv_type = 0xFF;
    return device_count++;
}

static void copy_name(char *name, size_t size, const uint8_t *text, size_t length)
{
    if (length > size - 1) {
        length = size - 1;
    }
    memcpy(name, text, length);
    name[length] = '\0';
}

static void classic_found(const esp_bt_gap_cb_param_t *param)
{
    int index = find_or_add(param->disc_res.bda, true);
    if (index < 0) {
        return;
    }
    bt_device_t *device = &devices[index];
    for (int i = 0; i < param->disc_res.num_prop; ++i) {
        const esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[i];
        switch (prop->type) {
        case ESP_BT_GAP_DEV_PROP_BDNAME:
            copy_name(device->name, sizeof(device->name), prop->val, prop->len);
            break;
        case ESP_BT_GAP_DEV_PROP_COD:
            device->cod = *(const uint32_t *)prop->val;
            break;
        case ESP_BT_GAP_DEV_PROP_RSSI:
            device->rssi = *(const int8_t *)prop->val;
            device->has_rssi = true;
            break;
        case ESP_BT_GAP_DEV_PROP_EIR: {
            uint8_t length = 0;
            uint8_t *data = esp_bt_gap_resolve_eir_data(prop->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME,
                                                        &length);
            if (data == NULL) {
                data = esp_bt_gap_resolve_eir_data(prop->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME,
                                                   &length);
            }
            if (data != NULL && device->name[0] == '\0') {
                copy_name(device->name, sizeof(device->name), data, length);
            }
            data = esp_bt_gap_resolve_eir_data(prop->val, ESP_BT_EIR_TYPE_TX_POWER_LEVEL, &length);
            if (data != NULL && length == 1) {
                device->tx_power = (int8_t)data[0];
                device->has_tx_power = true;
            }
            break;
        }
        default:
            break;
        }
    }
}

static void bt_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        classic_found(param);
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            xEventGroupSetBits(bt_events, EV_CLASSIC_DONE);
        }
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        if (probe.active && memcmp(param->acl_conn_cmpl_stat.bda, probe.addr, 6) == 0) {
            probe.acl_seen = true;
            probe.acl_status = param->acl_conn_cmpl_stat.stat;
            probe.acl_us = esp_timer_get_time();
        }
        break;
    case ESP_BT_GAP_RMT_SRVCS_EVT:
        if (probe.active && memcmp(param->rmt_srvcs.bda, probe.addr, 6) == 0) {
            probe.sdp_status = param->rmt_srvcs.stat;
            for (int i = 0; i < param->rmt_srvcs.num_uuids; ++i) {
                add_service(probe.services, &probe.service_count, &param->rmt_srvcs.uuid_list[i]);
            }
            xEventGroupSetBits(bt_events, EV_SDP);
        }
        break;
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        if (probe.active && memcmp(param->read_rmt_name.bda, probe.addr, 6) == 0) {
            probe.name_status = param->read_rmt_name.stat;
            copy_name(probe.read_text, sizeof(probe.read_text), param->read_rmt_name.rmt_name,
                      strnlen((const char *)param->read_rmt_name.rmt_name, ESP_BT_GAP_MAX_BDNAME_LEN));
            xEventGroupSetBits(bt_events, EV_NAME);
        }
        break;
    case ESP_BT_GAP_READ_TX_PWR_LVL_EVT:
        if (param->read_tx_pwr_lvl.type < ESP_BT_GAP_TX_PWR_LVL_TYPE_MAX) {
            gap_tx_level[param->read_tx_pwr_lvl.type] = param->read_tx_pwr_lvl.tx_power;
            gap_tx_level_valid[param->read_tx_pwr_lvl.type] =
                param->read_tx_pwr_lvl.stat == ESP_BT_STATUS_SUCCESS;
        }
        xEventGroupSetBits(bt_events, EV_TX_LEVEL);
        break;
    case ESP_BT_GAP_GET_PAGE_TO_EVT:
        page_timeout_read = param->get_page_timeout.page_to;
        page_timeout_valid = param->get_page_timeout.stat == ESP_BT_STATUS_SUCCESS;
        xEventGroupSetBits(bt_events, EV_PAGE_TIMEOUT);
        break;
    /* This tool never pairs: refuse every pairing request. */
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {0};
        esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, pin);
        break;
    }
#if CONFIG_BT_SSP_ENABLED
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, false);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        esp_bt_gap_ssp_passkey_reply(param->key_req.bda, false, 0);
        break;
#endif
    default:
        break;
    }
}

static void ble_found(const esp_ble_gap_cb_param_t *param)
{
    int index = find_or_add(param->scan_rst.bda, false);
    if (index < 0) {
        return;
    }
    bt_device_t *device = &devices[index];
    device->rssi = (int8_t)param->scan_rst.rssi;
    device->has_rssi = true;
    device->addr_type = param->scan_rst.ble_addr_type;
    device->dev_type = param->scan_rst.dev_type;
    if (param->scan_rst.ble_evt_type != ESP_BLE_EVT_SCAN_RSP) {
        device->adv_type = param->scan_rst.ble_evt_type;
    }

    /* Advertisements and scan responses carry different fields; keep the
       first value seen for each. */
    uint8_t *data = (uint8_t *)param->scan_rst.ble_adv;
    uint16_t data_length = param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len;
    uint8_t length = 0;
    if (device->name[0] == '\0') {
        uint8_t *name = esp_ble_resolve_adv_data_by_type(data, data_length,
                                                         ESP_BLE_AD_TYPE_NAME_CMPL, &length);
        if (name == NULL) {
            name = esp_ble_resolve_adv_data_by_type(data, data_length, ESP_BLE_AD_TYPE_NAME_SHORT,
                                                    &length);
        }
        if (name != NULL && length > 0) {
            copy_name(device->name, sizeof(device->name), name, length);
        }
    }
    uint8_t *field = esp_ble_resolve_adv_data_by_type(data, data_length, ESP_BLE_AD_TYPE_TX_PWR,
                                                      &length);
    if (!device->has_tx_power && field != NULL && length == 1) {
        device->tx_power = (int8_t)field[0];
        device->has_tx_power = true;
    }
    field = esp_ble_resolve_adv_data_by_type(data, data_length,
                                             ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE, &length);
    if (!device->has_company && field != NULL && length >= 2) {
        device->company_id = (uint16_t)(field[0] | (field[1] << 8));
        device->has_company = true;
    }
    field = esp_ble_resolve_adv_data_by_type(data, data_length, ESP_BLE_AD_TYPE_16SRV_CMPL, &length);
    if (field == NULL) {
        field = esp_ble_resolve_adv_data_by_type(data, data_length, ESP_BLE_AD_TYPE_16SRV_PART,
                                                 &length);
    }
    if (!device->has_service && field != NULL && length >= 2) {
        device->service_uuid16 = (uint16_t)(field[0] | (field[1] << 8));
        device->has_service = true;
    }
}

static void ble_gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        xEventGroupSetBits(bt_events, EV_BLE_PARAMS);
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            ble_found(param);
        } else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            xEventGroupSetBits(bt_events, EV_BLE_DONE);
        }
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "LE scan did not start: %d", param->scan_start_cmpl.status);
            xEventGroupSetBits(bt_events, EV_BLE_DONE);
        }
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        if (probe.active && memcmp(param->update_conn_params.bda, probe.addr, 6) == 0) {
            probe.params.interval = param->update_conn_params.conn_int;
            probe.params.latency = param->update_conn_params.latency;
            probe.params.timeout = param->update_conn_params.timeout;
        }
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        /* This tool never pairs. */
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, false);
        break;
    default:
        break;
    }
}

static void gattc_callback(esp_gattc_cb_event_t event, esp_gatt_if_t interface,
                           esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        if (param->reg.status == ESP_GATT_OK) {
            gattc_if = interface;
        }
        xEventGroupSetBits(bt_events, EV_GATTC_REG);
        break;
    case ESP_GATTC_CONNECT_EVT:
        if (probe.active && memcmp(param->connect.remote_bda, probe.addr, 6) == 0) {
            probe.params = param->connect.conn_params;
        }
        break;
    case ESP_GATTC_OPEN_EVT:
        if (probe.active && memcmp(param->open.remote_bda, probe.addr, 6) == 0) {
            probe.open_status = param->open.status;
            probe.conn_id = param->open.conn_id;
            probe.mtu = param->open.mtu;
            probe.connected = param->open.status == ESP_GATT_OK;
            xEventGroupSetBits(bt_events, EV_OPEN);
        }
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        if (param->cfg_mtu.status == ESP_GATT_OK) {
            probe.mtu = param->cfg_mtu.mtu;
        }
        xEventGroupSetBits(bt_events, EV_MTU);
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        if (probe.active && param->search_res.conn_id == probe.conn_id) {
            add_service(probe.services, &probe.service_count, &param->search_res.srvc_id.uuid);
        }
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        xEventGroupSetBits(bt_events, EV_SEARCH);
        break;
    case ESP_GATTC_READ_CHAR_EVT:
        probe.read_status = param->read.status;
        probe.read_text[0] = '\0';
        if (param->read.status == ESP_GATT_OK) {
            /* Keep printable characters only. */
            size_t length = 0;
            for (int i = 0; i < param->read.value_len && length < sizeof(probe.read_text) - 1; ++i) {
                char character = (char)param->read.value[i];
                if (character >= ' ' && character <= '~') {
                    probe.read_text[length++] = character;
                }
            }
            probe.read_text[length] = '\0';
        }
        xEventGroupSetBits(bt_events, EV_READ);
        break;
    case ESP_GATTC_CANCEL_OPEN_EVT:
        xEventGroupSetBits(bt_events, EV_CANCEL);
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        if (probe.active && memcmp(param->disconnect.remote_bda, probe.addr, 6) == 0) {
            probe.close_reason = param->disconnect.reason;
            probe.disconnected = true;
            xEventGroupSetBits(bt_events, EV_CLOSE);
        }
        break;
    case ESP_GATTC_CLOSE_EVT:
        if (probe.active && memcmp(param->close.remote_bda, probe.addr, 6) == 0) {
            if (probe.close_reason == 0) {
                probe.close_reason = param->close.reason;
            }
            probe.disconnected = true;
            xEventGroupSetBits(bt_events, EV_CLOSE);
        }
        break;
    default:
        break;
    }
}

/* ---- Bluetooth start-up ---- */

static void show_status(const char *message)
{
    ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK));
    ESP_ERROR_CHECK(cyd_text_row(CYD_TEXT_ROWS / 2, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, message));
}

/* Shows a start-up failure on the screen and stops. */
static void bt_check(esp_err_t err, const char *step)
{
    if (err == ESP_OK) {
        return;
    }
    ESP_LOGE(TAG, "%s failed: %s", step, esp_err_to_name(err));
    show_status(" BLUETOOTH FAILED");
    ESP_ERROR_CHECK(cyd_text_row(CYD_TEXT_ROWS / 2 + 1, CYD_COLOR_RED, CYD_COLOR_BLACK, step));
    ESP_ERROR_CHECK(cyd_text_row(CYD_TEXT_ROWS / 2 + 2, CYD_COLOR_RED, CYD_COLOR_BLACK,
                                 esp_err_to_name(err)));
    vTaskDelete(NULL);
}

static void bt_start(void)
{
    controller_config = (esp_bt_controller_config_t)BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_check(esp_bt_controller_init(&controller_config), "CONTROLLER INIT");
    bt_check(esp_bt_controller_enable(ESP_BT_MODE_BTDM), "CONTROLLER ON");
    host_config = (esp_bluedroid_config_t)BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bt_check(esp_bluedroid_init_with_cfg(&host_config), "BLUEDROID INIT");
    bt_check(esp_bluedroid_enable(), "BLUEDROID ON");

    bt_check(esp_bt_gap_register_callback(bt_gap_callback), "CLASSIC GAP");
    bt_check(esp_ble_gap_register_callback(ble_gap_callback), "LE GAP");
    bt_check(esp_ble_gattc_register_callback(gattc_callback), "GATT CLIENT");
    xEventGroupClearBits(bt_events, EV_GATTC_REG);
    bt_check(esp_ble_gattc_app_register(0), "GATT APP");
    xEventGroupWaitBits(bt_events, EV_GATTC_REG, pdTRUE, pdTRUE, pdMS_TO_TICKS(3000));
    if (gattc_if == ESP_GATT_IF_NONE) {
        bt_check(ESP_ERR_TIMEOUT, "GATT APP");
    }
    esp_bt_gap_set_device_name(DEVICE_NAME);
    esp_ble_gap_set_device_name(DEVICE_NAME);
    esp_ble_gatt_set_local_mtu(LOCAL_MTU);
    if (!settings_loaded) {
        use_controller_power();
    }
    apply_settings();

    const uint8_t *addr = esp_bt_dev_get_address();
    ESP_LOGI(TAG, "Bluetooth on: dual mode, address " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(addr));
}

/* ---- Scanning ---- */

static void show_scan_progress(const char *phase, const char *detail)
{
    char found[CYD_TEXT_COLUMNS + 1];
    snprintf(found, sizeof(found), " FOUND %d%s", device_count, devices_full ? "+" : "");
    ESP_ERROR_CHECK(cyd_text_row(3, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, phase));
    ESP_ERROR_CHECK(cyd_text_row(4, CYD_COLOR_GRAY, CYD_COLOR_BLACK, detail));
    ESP_ERROR_CHECK(cyd_text_row(6, CYD_COLOR_WHITE, CYD_COLOR_BLACK, found));
}

/* Waits for bit, refreshing the device count twice a second. */
static bool wait_scan(EventBits_t bit, int timeout_ms, const char *phase, const char *detail)
{
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < end) {
        if (xEventGroupWaitBits(bt_events, bit, pdTRUE, pdTRUE, pdMS_TO_TICKS(500)) & bit) {
            return true;
        }
        show_scan_progress(phase, detail);
    }
    return false;
}

static void classic_scan(void)
{
    char detail[CYD_TEXT_COLUMNS + 1];
    int units = inquiry_units[settings[SET_INQUIRY]];
    snprintf(detail, sizeof(detail), " INQUIRY %s s", inquiry_options[settings[SET_INQUIRY]]);
    show_scan_progress(" CLASSIC BR/EDR", detail);
    xEventGroupClearBits(bt_events, EV_CLASSIC_DONE);
    esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, units, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Classic inquiry did not start: %s", esp_err_to_name(err));
        return;
    }
    /* Discovery ends after the inquiry and the name requests that follow. */
    if (!wait_scan(EV_CLASSIC_DONE, units * 1280 + 30000, " CLASSIC BR/EDR", " NAMES")) {
        esp_bt_gap_cancel_discovery();
        xEventGroupWaitBits(bt_events, EV_CLASSIC_DONE, pdTRUE, pdTRUE, pdMS_TO_TICKS(2000));
    }
}

static void ble_scan(void)
{
    char detail[CYD_TEXT_COLUMNS + 1];
    int seconds = scan_seconds[settings[SET_BLE_SCAN]];
    uint16_t interval = interval_ms[settings[SET_INTERVAL]] * 8 / 5; /* 0.625 ms units */
    uint16_t window = interval * duty_percent[settings[SET_DUTY]] / 100;
    if (window < 4) {
        window = 4;
    }
    /* Without duplicate filtering, so scan responses (which often carry the
       name) still arrive after the advertisement. */
    esp_ble_scan_params_t params = {
        .scan_type = settings[SET_ACTIVE] ? BLE_SCAN_TYPE_ACTIVE : BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = interval,
        .scan_window = window,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
    };
    snprintf(detail, sizeof(detail), " %s %d s", settings[SET_ACTIVE] ? "ACTIVE" : "PASSIVE", seconds);
    show_scan_progress(" BLUETOOTH LE", detail);
    xEventGroupClearBits(bt_events, EV_BLE_PARAMS | EV_BLE_DONE);
    if (esp_ble_gap_set_scan_params(&params) != ESP_OK ||
        !(xEventGroupWaitBits(bt_events, EV_BLE_PARAMS, pdTRUE, pdTRUE, pdMS_TO_TICKS(2000)) &
          EV_BLE_PARAMS)) {
        ESP_LOGE(TAG, "LE scan parameters not set");
        return;
    }
    if (esp_ble_gap_start_scanning(seconds) != ESP_OK) {
        ESP_LOGE(TAG, "LE scan did not start");
        return;
    }
    if (!wait_scan(EV_BLE_DONE, seconds * 1000 + 3000, " BLUETOOTH LE", detail)) {
        esp_ble_gap_stop_scanning();
    }
}

/* Named devices first, then strongest signal first. */
static int device_compare(const void *left, const void *right)
{
    const bt_device_t *a = left;
    const bt_device_t *b = right;
    bool a_named = a->name[0] != '\0';
    bool b_named = b->name[0] != '\0';
    if (a_named != b_named) {
        return a_named ? -1 : 1;
    }
    return (b->has_rssi ? b->rssi : -128) - (a->has_rssi ? a->rssi : -128);
}

static void log_device(const bt_device_t *device)
{
    ESP_LOGI(TAG, "  %s %4d dBm " ESP_BD_ADDR_STR " %s", device->classic ? "BR/EDR" : "LE    ",
             device->rssi, ESP_BD_ADDR_HEX(device->addr), device->name);
}

static void scan_all(void)
{
    device_count = 0;
    devices_full = false;
    list_offset = 0;
    ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK));
    ESP_ERROR_CHECK(cyd_text_row(1, CYD_COLOR_CYAN, CYD_COLOR_BLACK, " SCANNING"));
    classic_scan();
    ble_scan();
    qsort(devices, device_count, sizeof(devices[0]), device_compare);
    ESP_LOGI(TAG, "Scan: %d%s devices", device_count, devices_full ? "+" : "");
    for (int i = 0; i < device_count; ++i) {
        log_device(&devices[i]);
    }
}

/* ---- Connection tests ---- */

/* Waits for any of bits. When abortable, a tap ends the wait early and sets
   probe_aborted; the tap stays queued. */
static EventBits_t wait_bits(EventBits_t bits, int timeout_ms, bool abortable)
{
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (true) {
        int64_t left_ms = (end - esp_timer_get_time()) / 1000;
        if (left_ms <= 0) {
            return 0;
        }
        int slice_ms = abortable && left_ms > 100 ? 100 : (int)left_ms;
        EventBits_t got = xEventGroupWaitBits(bt_events, bits, pdTRUE, pdFALSE,
                                              pdMS_TO_TICKS(slice_ms));
        if (got & bits) {
            return got & bits;
        }
        if (abortable && uxQueueMessagesWaiting(touch_queue) > 0) {
            probe_aborted = true;
            return 0;
        }
    }
}

/* LE devices that advertise as connectable to anyone. */
static bool is_probe_candidate(const bt_device_t *device)
{
    return device->classic || device->adv_type == ESP_BLE_EVT_CONN_ADV ||
           device->adv_type == 0xFF;
}

static void probe_start(const bt_device_t *device)
{
    memset(&probe, 0, sizeof(probe));
    memcpy(probe.addr, device->addr, 6);
    probe.active = true;
    xEventGroupClearBits(bt_events, EV_OPEN | EV_MTU | EV_SEARCH | EV_READ | EV_CLOSE |
                                        EV_CANCEL | EV_SDP | EV_NAME);
}

static void probe_fail(bt_device_t *device, const char *reason, uint16_t code)
{
    device->probe = PROBE_FAILED;
    device->probe_reason = reason;
    device->probe_code = code;
    probe.active = false;
}

/* Reads a characteristic by UUID into text; leaves text alone on failure. */
static void gatt_read_text(uint16_t uuid16, char *text, size_t size)
{
    esp_bt_uuid_t uuid = {.len = ESP_UUID_LEN_16, .uuid.uuid16 = uuid16};
    xEventGroupClearBits(bt_events, EV_READ);
    if (probe.disconnected ||
        esp_ble_gattc_read_by_type(gattc_if, probe.conn_id, 0x0001, 0xFFFF, &uuid,
                                   ESP_GATT_AUTH_REQ_NONE) != ESP_OK) {
        return;
    }
    if ((wait_bits(EV_READ | EV_CLOSE, 2000, false) & EV_READ) &&
        probe.read_status == ESP_GATT_OK && probe.read_text[0] != '\0') {
        snprintf(text, size, "%s", probe.read_text);
    }
}

/* Connects as a GATT client, then reads what the link and server offer. */
static void probe_ble(bt_device_t *device)
{
    probe_start(device);
    int64_t start = esp_timer_get_time();
    esp_err_t err = esp_ble_gattc_open(gattc_if, device->addr, device->addr_type, true);
    if (err != ESP_OK) {
        probe_fail(device, "START ERROR", (uint16_t)err);
        return;
    }
    if (!(wait_bits(EV_OPEN | EV_CLOSE, connect_timeout_ms(), true) & EV_OPEN)) {
        if (!probe.disconnected) {
            esp_ble_gattc_cancel_open_params_t cancel = {.gattc_if = gattc_if};
            memcpy(cancel.remote_bda, device->addr, 6);
            esp_ble_gattc_cancel_open(&cancel);
            wait_bits(EV_CANCEL | EV_OPEN, 2000, false);
        }
        if (probe.connected) {
            /* It connected while being cancelled. */
            esp_ble_gap_disconnect(device->addr);
            wait_bits(EV_CLOSE, 3000, false);
        }
        if (probe_aborted) {
            probe.active = false;
            return;
        }
        if (probe.disconnected) {
            probe_fail(device, gatt_reason_text(probe.close_reason), probe.close_reason);
        } else {
            probe_fail(device, "NO RESPONSE", 0);
        }
        return;
    }
    if (probe.open_status != ESP_GATT_OK) {
        const char *reason = gatt_reason_text(probe.close_reason);
        probe_fail(device, reason, reason != NULL ? probe.close_reason : probe.open_status);
        return;
    }
    device->connect_ms = (uint16_t)((esp_timer_get_time() - start) / 1000);

    if (!probe.disconnected && esp_ble_gattc_send_mtu_req(gattc_if, probe.conn_id) == ESP_OK) {
        wait_bits(EV_MTU | EV_CLOSE, 3000, false);
    }
    if (!probe.disconnected && esp_ble_gattc_search_service(gattc_if, probe.conn_id, NULL) == ESP_OK) {
        wait_bits(EV_SEARCH | EV_CLOSE, 8000, false);
    }
    if (device->name[0] == '\0') {
        gatt_read_text(ESP_GATT_UUID_GAP_DEVICE_NAME, device->name, sizeof(device->name));
    }
    gatt_read_text(ESP_GATT_UUID_MANU_NAME, device->manufacturer, sizeof(device->manufacturer));
    gatt_read_text(ESP_GATT_UUID_MODEL_NUMBER_STR, device->model, sizeof(device->model));
    if (!probe.disconnected) {
        esp_ble_gap_disconnect(device->addr);
        wait_bits(EV_CLOSE, 3000, false);
    }

    device->probe = PROBE_OK;
    device->mtu = probe.mtu;
    device->interval = probe.params.interval;
    device->latency = probe.params.latency;
    device->timeout = probe.params.timeout;
    device->service_count = probe.service_count;
    memcpy(device->services, probe.services, sizeof(device->services));
    probe.active = false;
}

/* Pages the device and asks for its SDP service list: a completed ACL
   connection proves it accepts connections. */
static void probe_classic(bt_device_t *device)
{
    probe_start(device);
    int64_t start = esp_timer_get_time();
    esp_err_t err = esp_bt_gap_get_remote_services(device->addr);
    if (err != ESP_OK) {
        probe_fail(device, "START ERROR", (uint16_t)err);
        return;
    }
    /* Neither paging nor SDP can be cancelled, so this wait ignores taps. */
    bool answered = wait_bits(EV_SDP, page_timeout_ms() + 15000, false) & EV_SDP;
    bool connected = (probe.acl_seen && probe.acl_status == ESP_BT_STATUS_SUCCESS) ||
                     (answered && probe.sdp_status == ESP_BT_STATUS_SUCCESS);
    if (!connected) {
        esp_bt_status_t status = probe.acl_seen ? probe.acl_status : probe.sdp_status;
        if (!answered && !probe.acl_seen) {
            probe_fail(device, "NO RESPONSE", 0);
        } else {
            probe_fail(device, bt_status_text(status), (uint16_t)status);
        }
        return;
    }
    device->connect_ms = (uint16_t)(((probe.acl_seen ? probe.acl_us : esp_timer_get_time()) - start) / 1000);
    device->service_count = probe.service_count;
    memcpy(device->services, probe.services, sizeof(device->services));
    if (device->name[0] == '\0' && esp_bt_gap_read_remote_name(device->addr) == ESP_OK &&
        (wait_bits(EV_NAME, page_timeout_ms() + 3000, false) & EV_NAME) &&
        probe.name_status == ESP_BT_STATUS_SUCCESS) {
        snprintf(device->name, sizeof(device->name), "%s", probe.read_text);
    }
    device->probe = PROBE_OK;
    probe.active = false;
}

static void log_probe(const bt_device_t *device)
{
    if (device->probe == PROBE_OK) {
        ESP_LOGI(TAG, "Connect " ESP_BD_ADDR_STR " %s: OK in %u ms, %u services, MTU %u",
                 ESP_BD_ADDR_HEX(device->addr), device->name, device->connect_ms,
                 device->service_count, device->mtu);
        for (int i = 0; i < device->service_count && i < MAX_SERVICES; ++i) {
            char text[CYD_TEXT_COLUMNS + 1];
            service_text(&device->services[i], text, sizeof(text));
            ESP_LOGI(TAG, "    %s", text);
        }
    } else if (device->probe == PROBE_FAILED) {
        ESP_LOGI(TAG, "Connect " ESP_BD_ADDR_STR " %s: failed, %s (0x%X)",
                 ESP_BD_ADDR_HEX(device->addr), device->name,
                 device->probe_reason != NULL ? device->probe_reason : "code",
                 device->probe_code);
    }
}

static void probe_device(bt_device_t *device)
{
    probe_aborted = false;
    if (device->classic) {
        probe_classic(device);
    } else {
        probe_ble(device);
    }
    log_probe(device);
}

/* ---- Pages of text ---- */

/* A screen of text rows; the last row is reserved for a footer. */
typedef struct {
    char text[CYD_TEXT_ROWS][CYD_TEXT_COLUMNS + 1];
    uint16_t color[CYD_TEXT_ROWS];
    int count;
} page_t;

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

static void page_show(const page_t *page)
{
    for (int row = 0; row < CYD_TEXT_ROWS - 1; ++row) {
        if (row < page->count) {
            ESP_ERROR_CHECK(cyd_text_row(row, page->color[row], CYD_COLOR_BLACK, page->text[row]));
        } else {
            ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_WHITE, CYD_COLOR_BLACK, ""));
        }
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

static void wait_tap(cyd_touch_point_t *point)
{
    xQueueReset(touch_queue);
    xQueueReceive(touch_queue, point, portMAX_DELAY);
}

/* ---- Device detail ---- */

static const button_t detail_buttons[] = {
    {0, 5, "BACK", CYD_COLOR_WHITE},
    {6, 5, "TEST", CYD_COLOR_YELLOW},
    {12, 5, "SVCS", CYD_COLOR_WHITE},
};
enum { DETAIL_BACK, DETAIL_TEST, DETAIL_SERVICES, DETAIL_BUTTON_COUNT };

static uint16_t probe_color(const bt_device_t *device)
{
    switch (device->probe) {
    case PROBE_OK:
        return COLOR_GREEN;
    case PROBE_FAILED:
        return CYD_COLOR_RED;
    case PROBE_NOT_CONNECTABLE:
        return CYD_COLOR_GRAY;
    default:
        return CYD_COLOR_WHITE;
    }
}

static void add_probe_rows(page_t *page, const bt_device_t *device)
{
    uint16_t color = probe_color(device);
    switch (device->probe) {
    case PROBE_OK:
        page_add(page, color, "CONNECTS %u ms", device->connect_ms);
        if (device->classic) {
            page_add(page, CYD_COLOR_WHITE, "SDP %u SERVICES", device->service_count);
        } else {
            /* Interval in 1.25 ms units, supervision timeout in 10 ms units. */
            page_add(page, CYD_COLOR_WHITE, "MTU %u INT %u ms", device->mtu,
                     device->interval * 5 / 4);
            page_add(page, CYD_COLOR_WHITE, "LAT %u TO %u ms", device->latency,
                     device->timeout * 10);
        }
        break;
    case PROBE_FAILED:
        if (device->probe_reason != NULL) {
            page_add(page, color, "FAIL %s", device->probe_reason);
        } else {
            page_add(page, color, "FAIL CODE 0x%X", device->probe_code);
        }
        break;
    case PROBE_NOT_CONNECTABLE:
        page_add(page, color, "NOT CONNECTABLE");
        break;
    default:
        page_add(page, color, "NOT TESTED");
        break;
    }
}

static void show_detail(const bt_device_t *device)
{
    page_t page = {0};
    if (device->classic) {
        page_add(&page, CYD_COLOR_YELLOW, "CLASSIC BR/EDR");
    } else {
        page_add(&page, CYD_COLOR_CYAN, device->dev_type == ESP_BT_DEVICE_TYPE_DUMO
                                            ? "LE (DUAL MODE)"
                                            : "BLUETOOTH LE");
    }
    if (device->name[0] != '\0') {
        page_add(&page, CYD_COLOR_WHITE, "%s", device->name);
    } else {
        page_add(&page, CYD_COLOR_GRAY, "(no name)");
    }
    page_add(&page, CYD_COLOR_WHITE, ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(device->addr));
    if (device->has_tx_power) {
        page_add(&page, CYD_COLOR_WHITE, "RSSI %d TX %d", device->rssi, device->tx_power);
    } else if (device->has_rssi) {
        page_add(&page, CYD_COLOR_WHITE, "RSSI %d dBm", device->rssi);
    } else {
        page_add(&page, CYD_COLOR_GRAY, "RSSI unknown");
    }
    if (device->classic) {
        page_add(&page, CYD_COLOR_WHITE, "%s", cod_major_name(device->cod));
        page_add(&page, CYD_COLOR_GRAY, "COD 0x%06X", (unsigned)device->cod);
    } else {
        page_add(&page, CYD_COLOR_WHITE, "%s", ble_address_kind(device));
        page_add(&page, CYD_COLOR_WHITE, "%s", ble_advertising_kind(device->adv_type));
    }
    add_probe_rows(&page, device);
    page_show(&page);
    draw_buttons(FOOTER_ROW, detail_buttons, DETAIL_BUTTON_COUNT);
}

/* What the device advertised and what the connection test read. */
static void show_services(const bt_device_t *device)
{
    page_t page = {0};
    page_add(&page, CYD_COLOR_CYAN, "SERVICES %u", device->service_count);
    if (device->manufacturer[0] != '\0') {
        page_add(&page, CYD_COLOR_WHITE, "MFR %s", device->manufacturer);
    } else if (device->has_company) {
        const char *company = company_name(device->company_id);
        if (company != NULL) {
            page_add(&page, CYD_COLOR_WHITE, "MFR %s", company);
        } else {
            page_add(&page, CYD_COLOR_WHITE, "MFR ID 0x%04X", device->company_id);
        }
    }
    if (device->model[0] != '\0') {
        page_add(&page, CYD_COLOR_WHITE, "MODEL %s", device->model);
    }
    if (device->service_count == 0 && device->has_service) {
        service_t advertised = {.len = 2, .value = device->service_uuid16};
        char text[CYD_TEXT_COLUMNS + 1];
        service_text(&advertised, text, sizeof(text));
        page_add(&page, CYD_COLOR_GRAY, "ADVERTISED:");
        page_add(&page, CYD_COLOR_WHITE, "%s", text);
    }
    for (int i = 0; i < device->service_count && i < MAX_SERVICES; ++i) {
        char text[CYD_TEXT_COLUMNS + 1];
        service_text(&device->services[i], text, sizeof(text));
        page_add(&page, CYD_COLOR_WHITE, "%s", text);
    }
    if (device->probe != PROBE_OK && device->service_count == 0) {
        page_add(&page, CYD_COLOR_GRAY, "TEST TO READ");
    }
    page_show(&page);
    ESP_ERROR_CHECK(cyd_text_row(FOOTER_ROW, CYD_COLOR_GRAY, CYD_COLOR_BLACK, "tap to go back"));
}

static void open_detail(bt_device_t *device)
{
    while (true) {
        show_detail(device);
        cyd_touch_point_t point;
        wait_tap(&point);
        int column = point.x / CYD_FONT_WIDTH;
        if (point.y / CYD_FONT_HEIGHT != FOOTER_ROW || button_hit(&detail_buttons[DETAIL_BACK], column)) {
            return;
        }
        if (button_hit(&detail_buttons[DETAIL_TEST], column)) {
            ESP_ERROR_CHECK(cyd_text_row(FOOTER_ROW, CYD_COLOR_YELLOW, CYD_COLOR_BLACK,
                                         device->classic ? "TESTING..." : "TESTING, TAP=STOP"));
            xQueueReset(touch_queue);
            probe_device(device);
        } else if (button_hit(&detail_buttons[DETAIL_SERVICES], column)) {
            show_services(device);
            wait_tap(&point);
        }
    }
}

/* ---- Configuration pages ---- */

typedef struct {
    char text[CYD_TEXT_COLUMNS + 1];
    uint16_t color;
    int8_t setting; /* index into settings, or -1 for an information row */
} config_row_t;

typedef struct {
    const char *title;
    config_row_t rows[CONFIG_ROWS];
    int count;
} config_page_t;

enum { PAGE_CONTROLLER, PAGE_HOST, PAGE_POWER, PAGE_SCAN, PAGE_BUILD, PAGE_COUNT };

static const button_t config_buttons[] = {
    {0, 5, "BACK", CYD_COLOR_WHITE},
    {6, 5, "<", CYD_COLOR_WHITE},
    {12, 5, ">", CYD_COLOR_WHITE},
};
enum { CONFIG_BACK, CONFIG_PREVIOUS, CONFIG_NEXT, CONFIG_BUTTON_COUNT };

static void config_add(config_page_t *page, uint16_t color, const char *format, ...)
{
    if (page->count >= CONFIG_ROWS) {
        return;
    }
    config_row_t *row = &page->rows[page->count++];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(row->text, sizeof(row->text), format, arguments);
    va_end(arguments);
    row->color = color;
    row->setting = -1;
}

static void config_add_setting(config_page_t *page, int setting)
{
    if (page->count < CONFIG_ROWS) {
        page->rows[page->count++] = (config_row_t){.setting = (int8_t)setting};
    }
}

static const char *on_off(bool on)
{
    return on ? "ON" : "OFF";
}

static const char *controller_status_text(void)
{
    switch (esp_bt_controller_get_status()) {
    case ESP_BT_CONTROLLER_STATUS_IDLE:
        return "IDLE";
    case ESP_BT_CONTROLLER_STATUS_INITED:
        return "INITIALIZED";
    case ESP_BT_CONTROLLER_STATUS_ENABLED:
        return "ENABLED";
    default:
        return "?";
    }
}

static const char *controller_mode_text(uint8_t mode)
{
    switch (mode) {
    case ESP_BT_MODE_BLE:
        return "LE ONLY";
    case ESP_BT_MODE_CLASSIC_BT:
        return "CLASSIC ONLY";
    case ESP_BT_MODE_BTDM:
        return "DUAL";
    default:
        return "IDLE";
    }
}

static const char *host_status_text(void)
{
    switch (esp_bluedroid_get_status()) {
    case ESP_BLUEDROID_STATUS_UNINITIALIZED:
        return "OFF";
    case ESP_BLUEDROID_STATUS_INITIALIZED:
        return "INITIALIZED";
    case ESP_BLUEDROID_STATUS_ENABLED:
        return "ENABLED";
    default:
        return "?";
    }
}

/* Reads values the controller reports only through callbacks. */
static void read_gap_values(void)
{
    for (int type = 0; type < ESP_BT_GAP_TX_PWR_LVL_TYPE_MAX; ++type) {
        gap_tx_level_valid[type] = false;
        xEventGroupClearBits(bt_events, EV_TX_LEVEL);
        if (esp_bt_gap_read_tx_pwr_lvl(type) == ESP_OK) {
            xEventGroupWaitBits(bt_events, EV_TX_LEVEL, pdTRUE, pdTRUE, pdMS_TO_TICKS(500));
        }
    }
    page_timeout_valid = false;
    xEventGroupClearBits(bt_events, EV_PAGE_TIMEOUT);
    if (esp_bt_gap_get_page_timeout() == ESP_OK) {
        xEventGroupWaitBits(bt_events, EV_PAGE_TIMEOUT, pdTRUE, pdTRUE, pdMS_TO_TICKS(500));
    }
}

static void gap_level_text(int type, char *text, size_t size)
{
    if (gap_tx_level_valid[type]) {
        snprintf(text, size, "%+d", gap_tx_level[type]);
    } else {
        snprintf(text, size, "?");
    }
}

#ifdef CONFIG_BT_BLE_42_FEATURES_SUPPORTED
#define HAS_BLE_42 true
#else
#define HAS_BLE_42 false
#endif
#ifdef CONFIG_BT_BLE_50_FEATURES_SUPPORTED
#define HAS_BLE_50 true
#else
#define HAS_BLE_50 false
#endif
#ifdef CONFIG_BT_GATTS_ENABLE
#define HAS_GATTS true
#else
#define HAS_GATTS false
#endif
#ifdef CONFIG_BT_BLE_SMP_ENABLE
#define HAS_SMP true
#else
#define HAS_SMP false
#endif
#ifdef CONFIG_BT_GATTC_CACHE_NVS_FLASH
#define HAS_GATT_CACHE true
#else
#define HAS_GATT_CACHE false
#endif
#ifdef CONFIG_BTDM_CTRL_MODEM_SLEEP
#define SLEEP_TEXT "MODEM"
#else
#define SLEEP_TEXT "OFF"
#endif

static void build_config_page(int index, config_page_t *page)
{
    page->count = 0;
    switch (index) {
    case PAGE_CONTROLLER: {
        const esp_bt_controller_config_t *cfg = &controller_config;
        page->title = "CONTROLLER";
        config_add(page, CYD_COLOR_WHITE, "STATUS %s", controller_status_text());
        config_add(page, CYD_COLOR_WHITE, "MODE %s", controller_mode_text(cfg->mode));
        config_add(page, CYD_COLOR_WHITE, "LE LINKS MAX %u", cfg->ble_max_conn);
        config_add(page, CYD_COLOR_WHITE, "ACL MAX %u SCO %u", cfg->bt_max_acl_conn,
                   cfg->bt_max_sync_conn);
        config_add(page, CYD_COLOR_WHITE, "SLEEP %s", SLEEP_TEXT);
        config_add(page, CYD_COLOR_WHITE, "PRIO %u CORE %d", cfg->controller_task_prio,
                   CONFIG_BTDM_CTRL_PINNED_TO_CORE);
        config_add(page, CYD_COLOR_WHITE, "ENC KEY MIN %u", cfg->enc_key_sz_min);
        config_add(page, CYD_COLOR_WHITE, "DUP LIST %u", cfg->normal_adv_size);
        break;
    }
    case PAGE_HOST: {
        const uint8_t *addr = esp_bt_dev_get_address();
        esp_bt_gap_profile_status_t status = {0};
        esp_bt_gap_get_profile_status(&status);
        page->title = "HOST";
        config_add(page, CYD_COLOR_WHITE, "BLUEDROID %s", host_status_text());
        config_add(page, CYD_COLOR_GRAY, "BT ADDRESS");
        config_add(page, CYD_COLOR_WHITE, ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(addr));
        config_add(page, CYD_COLOR_WHITE, "%s", DEVICE_NAME);
        config_add(page, CYD_COLOR_WHITE, "SSP %s SC %s", on_off(host_config.ssp_en),
                   on_off(host_config.sc_en));
        config_add(page, CYD_COLOR_WHITE, "ACL LINKS %u", status.bredr_acl_link_num);
        if (page_timeout_valid) {
            /* 0.625 ms units. */
            config_add(page, CYD_COLOR_WHITE, "PAGE TO %u ms", page_timeout_read * 5 / 8);
        } else {
            config_add(page, CYD_COLOR_GRAY, "PAGE TO ?");
        }
        config_add_setting(page, SET_VISIBLE);
        break;
    }
    case PAGE_POWER: {
        esp_power_level_t bredr_min = 0;
        esp_power_level_t bredr_max = 0;
        char inquiry[6];
        char paging[6];
        esp_bredr_tx_power_get(&bredr_min, &bredr_max);
        gap_level_text(ESP_BT_GAP_TX_PWR_LVL_INQ, inquiry, sizeof(inquiry));
        gap_level_text(ESP_BT_GAP_TX_PWR_LVL_PAGE, paging, sizeof(paging));
        page->title = "TX POWER dBm";
        config_add_setting(page, SET_BLE_ADV_POWER);
        config_add_setting(page, SET_BLE_SCAN_POWER);
        config_add_setting(page, SET_BLE_CONN_POWER);
        config_add_setting(page, SET_BREDR_MIN_POWER);
        config_add_setting(page, SET_BREDR_MAX_POWER);
        /* What the controller reports now. */
        config_add(page, CYD_COLOR_GRAY, "LE NOW %s %s %s",
                   power_text(esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_ADV)),
                   power_text(esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_SCAN)),
                   power_text(esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT)));
        config_add(page, CYD_COLOR_GRAY, "BR NOW %s TO %s", power_text(bredr_min),
                   power_text(bredr_max));
        config_add(page, CYD_COLOR_GRAY, "INQ %s PAGE %s", inquiry, paging);
        break;
    }
    case PAGE_SCAN:
        page->title = "SCAN AND TEST";
        config_add_setting(page, SET_INQUIRY);
        config_add_setting(page, SET_BLE_SCAN);
        config_add_setting(page, SET_ACTIVE);
        config_add_setting(page, SET_INTERVAL);
        config_add_setting(page, SET_DUTY);
        config_add_setting(page, SET_CONNECT);
        config_add_setting(page, SET_PAGE_TIMEOUT);
        config_add_setting(page, SET_AUTO_TEST);
        break;
    default:
        page->title = "BUILD";
        config_add(page, CYD_COLOR_WHITE, "IDF %s", esp_get_idf_version());
        config_add(page, CYD_COLOR_WHITE, "LE 4.2 %s 5.0 %s", on_off(HAS_BLE_42), on_off(HAS_BLE_50));
        config_add(page, CYD_COLOR_WHITE, "GATTC ON GATTS %s", on_off(HAS_GATTS));
        config_add(page, CYD_COLOR_WHITE, "SMP %s CACHE %s", on_off(HAS_SMP), on_off(HAS_GATT_CACHE));
        config_add(page, CYD_COLOR_WHITE, "LOCAL MTU %d", LOCAL_MTU);
        config_add(page, CYD_COLOR_WHITE, "BTC STACK %d", CONFIG_BT_BTC_TASK_STACK_SIZE);
        config_add(page, CYD_COLOR_WHITE, "FREE HEAP %uK", (unsigned)(esp_get_free_heap_size() / 1024));
        config_add(page, CYD_COLOR_WHITE, "MIN HEAP %uK",
                   (unsigned)(esp_get_minimum_free_heap_size() / 1024));
        break;
    }
}

static void draw_setting_row(int row, int setting)
{
    char text[CYD_TEXT_COLUMNS + 1];
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    const setting_info_t *info = &setting_info[setting];
    int value = settings[setting];
    snprintf(text, sizeof(text), "%-*.*s%*s - +", SETTING_LABEL_COLUMNS, SETTING_LABEL_COLUMNS,
             info->label, SETTING_VALUE_COLUMNS, info->options[value]);
    for (int i = 0; i < CYD_TEXT_COLUMNS; ++i) {
        bool minus = i >= SETTING_MINUS_COLUMN && i < SETTING_PLUS_COLUMN;
        bool plus = i >= SETTING_PLUS_COLUMN;
        background[i] = minus || plus ? CYD_COLOR_BUTTON : CYD_COLOR_BLACK;
        foreground[i] = i < SETTING_LABEL_COLUMNS                 ? CYD_COLOR_GRAY
                        : minus && value == 0                     ? CYD_COLOR_BUTTON_DISABLED
                        : plus && value == info->count - 1        ? CYD_COLOR_BUTTON_DISABLED
                        : minus || plus                           ? CYD_COLOR_WHITE
                                                                  : CYD_COLOR_YELLOW;
    }
    ESP_ERROR_CHECK(cyd_draw_cells(row, text, foreground, background));
}

static void draw_config_page(int index, const config_page_t *page)
{
    char title[48];
    snprintf(title, sizeof(title), "%-13.13s%d/%d", page->title, index + 1, PAGE_COUNT);
    ESP_ERROR_CHECK(cyd_text_row(0, CYD_COLOR_CYAN, CYD_COLOR_BLACK, title));
    for (int i = 0; i < CONFIG_ROWS; ++i) {
        int row = 1 + i;
        if (i >= page->count) {
            ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_WHITE, CYD_COLOR_BLACK, ""));
        } else if (page->rows[i].setting >= 0) {
            draw_setting_row(row, page->rows[i].setting);
        } else {
            ESP_ERROR_CHECK(cyd_text_row(row, page->rows[i].color, CYD_COLOR_BLACK,
                                         page->rows[i].text));
        }
    }
    draw_buttons(FOOTER_ROW, config_buttons, CONFIG_BUTTON_COUNT);
}

/* Steps a setting by direction; returns true when it changed. */
static bool step_setting(int setting, int direction)
{
    int value = settings[setting] + direction;
    if (value < 0 || value >= setting_info[setting].count) {
        return false;
    }
    settings[setting] = (uint8_t)value;
    /* Keep the BR/EDR range ordered whichever end moved. */
    if (setting == SET_BREDR_MIN_POWER && settings[SET_BREDR_MAX_POWER] < value) {
        settings[SET_BREDR_MAX_POWER] = (uint8_t)value;
    } else if (setting == SET_BREDR_MAX_POWER && settings[SET_BREDR_MIN_POWER] > value) {
        settings[SET_BREDR_MIN_POWER] = (uint8_t)value;
    }
    save_settings();
    apply_settings();
    ESP_LOGI(TAG, "Setting %s = %s", setting_info[setting].label,
             setting_info[setting].options[settings[setting]]);
    return true;
}

static void open_config(void)
{
    int index = 0;
    config_page_t page;
    read_gap_values();
    build_config_page(index, &page);
    draw_config_page(index, &page);
    while (true) {
        cyd_touch_point_t point;
        wait_tap(&point);
        int row = point.y / CYD_FONT_HEIGHT;
        int column = point.x / CYD_FONT_WIDTH;
        if (row == FOOTER_ROW) {
            if (button_hit(&config_buttons[CONFIG_BACK], column)) {
                return;
            }
            if (button_hit(&config_buttons[CONFIG_PREVIOUS], column)) {
                index = (index + PAGE_COUNT - 1) % PAGE_COUNT;
            } else if (button_hit(&config_buttons[CONFIG_NEXT], column)) {
                index = (index + 1) % PAGE_COUNT;
            } else {
                continue;
            }
        } else if (row >= 1 && row <= page.count && page.rows[row - 1].setting >= 0 &&
                   column >= SETTING_MINUS_COLUMN) {
            int direction = column >= SETTING_PLUS_COLUMN ? 1 : -1;
            if (!step_setting(page.rows[row - 1].setting, direction)) {
                continue;
            }
            /* Other rows show the controller's values, which may have changed too. */
            read_gap_values();
        } else {
            continue;
        }
        build_config_page(index, &page);
        draw_config_page(index, &page);
    }
}

/* ---- Device list ---- */

static const button_t list_header_buttons[] = {
    {5, 4, "CFG", CYD_COLOR_YELLOW},
};
static const button_t list_footer_buttons[] = {
    {12, 5, "TEST", CYD_COLOR_YELLOW},
};

static void draw_list_header(void)
{
    char text[CYD_TEXT_COLUMNS + 1];
    char title[16];
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    bool can_scroll_up = list_offset > 0;
    bool can_scroll_down = list_offset + LIST_ROWS < device_count;
    const button_t *cfg = &list_header_buttons[0];

    snprintf(title, sizeof(title), "BT%d%s", device_count, devices_full ? "+" : "");
    memset(text, ' ', CYD_TEXT_COLUMNS);
    text[CYD_TEXT_COLUMNS] = '\0';
    memcpy(text, title, strnlen(title, HEADER_TITLE_COLUMNS));
    for (int i = 0; i < CYD_TEXT_COLUMNS; ++i) {
        foreground[i] = CYD_COLOR_CYAN;
        background[i] = CYD_COLOR_BLACK;
    }
    for (int i = 0; i < cfg->columns; ++i) {
        background[cfg->first_column + i] = CYD_COLOR_BUTTON;
        foreground[cfg->first_column + i] = cfg->foreground;
    }
    memcpy(text + cfg->first_column, " CFG", 4);
    for (int i = 0; i < ARROW_COLUMNS; ++i) {
        background[ARROW_UP_COLUMN + i] = CYD_COLOR_BUTTON;
        foreground[ARROW_UP_COLUMN + i] = can_scroll_up ? CYD_COLOR_WHITE : CYD_COLOR_BUTTON_DISABLED;
        background[ARROW_DOWN_COLUMN + i] = CYD_COLOR_BUTTON;
        foreground[ARROW_DOWN_COLUMN + i] =
            can_scroll_down ? CYD_COLOR_WHITE : CYD_COLOR_BUTTON_DISABLED;
    }
    text[ARROW_UP_COLUMN + 1] = CYD_GLYPH_UP;
    text[ARROW_DOWN_COLUMN + 1] = CYD_GLYPH_DOWN;
    ESP_ERROR_CHECK(cyd_draw_cells(0, text, foreground, background));
}

/* "C-72 name": the radio (C Classic, L LE, D dual-mode LE), the signal, then
   the name or address. The text color is the connection test result. */
static void draw_list_entry(int index, bool testing, bool highlighted)
{
    int row = LIST_FIRST_ROW + index - list_offset;
    char text[CYD_TEXT_COLUMNS + 1];
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    const bt_device_t *device = &devices[index];
    char tag = device->classic ? 'C' : device->dev_type == ESP_BT_DEVICE_TYPE_DUMO ? 'D' : 'L';
    int rssi = !device->has_rssi ? 0 : device->rssi < -99 ? -99 : device->rssi;
    char label[13];
    if (device->name[0] != '\0') {
        snprintf(label, sizeof(label), "%.12s", device->name);
    } else {
        snprintf(label, sizeof(label), "%02x%02x%02x%02x%02x%02x", ESP_BD_ADDR_HEX(device->addr));
    }
    snprintf(text, sizeof(text), "%c%3d %-12s", tag, rssi, label);
    uint16_t color = testing ? CYD_COLOR_YELLOW : probe_color(device);
    if (device->name[0] == '\0' && device->probe == PROBE_UNTESTED && !testing) {
        color = CYD_COLOR_GRAY;
    }
    for (int i = 0; i < CYD_TEXT_COLUMNS; ++i) {
        foreground[i] = highlighted ? CYD_COLOR_BLACK : color;
        background[i] = highlighted ? CYD_COLOR_WHITE : CYD_COLOR_BUTTON;
    }
    if (!highlighted) {
        foreground[0] = device->classic ? CYD_COLOR_YELLOW : CYD_COLOR_CYAN;
    }
    ESP_ERROR_CHECK(cyd_draw_cells(row, text, foreground, background));
}

static void draw_list_footer(const char *status, uint16_t color, bool show_test)
{
    if (!show_test) {
        ESP_ERROR_CHECK(cyd_text_row(FOOTER_ROW, color, CYD_COLOR_BLACK, status));
        return;
    }
    char text[CYD_TEXT_COLUMNS + 1];
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    const button_t *test = &list_footer_buttons[0];
    snprintf(text, sizeof(text), "%-12.12s TEST", status);
    for (int i = 0; i < CYD_TEXT_COLUMNS; ++i) {
        bool button = button_hit(test, i);
        foreground[i] = button ? test->foreground : color;
        background[i] = button ? CYD_COLOR_BUTTON : CYD_COLOR_BLACK;
    }
    ESP_ERROR_CHECK(cyd_draw_cells(FOOTER_ROW, text, foreground, background));
}

static void list_summary(char *text, size_t size)
{
    int ok = 0;
    int failed = 0;
    for (int i = 0; i < device_count; ++i) {
        ok += devices[i].probe == PROBE_OK;
        failed += devices[i].probe == PROBE_FAILED;
    }
    if (device_count == 0) {
        snprintf(text, size, "NONE FOUND");
    } else {
        snprintf(text, size, "OK %d FAIL %d", ok, failed);
    }
}

static void show_list(void)
{
    char summary[CYD_TEXT_COLUMNS + 1];
    draw_list_header();
    for (int i = 0; i < LIST_ROWS; ++i) {
        if (list_offset + i < device_count) {
            draw_list_entry(list_offset + i, false, false);
        } else {
            ESP_ERROR_CHECK(cyd_text_row(LIST_FIRST_ROW + i, CYD_COLOR_WHITE, CYD_COLOR_BLACK, ""));
        }
    }
    list_summary(summary, sizeof(summary));
    draw_list_footer(summary, CYD_COLOR_GRAY, device_count > 0);
}

/* Pages the list by its height, stopping with the last page full. */
static bool scroll_to(int offset)
{
    int max_offset = device_count - LIST_ROWS;
    if (offset > max_offset) {
        offset = max_offset;
    }
    if (offset < 0) {
        offset = 0;
    }
    if (offset == list_offset) {
        return false;
    }
    list_offset = offset;
    return true;
}

/* Tests every device that can accept a connection, in list order, until a
   tap stops it. */
static void probe_all(void)
{
    xQueueReset(touch_queue);
    for (int i = 0; i < device_count; ++i) {
        bt_device_t *device = &devices[i];
        if (!is_probe_candidate(device)) {
            device->probe = PROBE_NOT_CONNECTABLE;
        }
    }
    show_list();
    for (int i = 0; i < device_count; ++i) {
        bt_device_t *device = &devices[i];
        if (device->probe == PROBE_NOT_CONNECTABLE) {
            continue;
        }
        if (i < list_offset || i >= list_offset + LIST_ROWS) {
            scroll_to(i / LIST_ROWS * LIST_ROWS);
            show_list();
        }
        char status[32];
        snprintf(status, sizeof(status), "%d/%d TAP TO STOP", i + 1, device_count);
        draw_list_footer(status, CYD_COLOR_YELLOW, false);
        draw_list_entry(i, true, false);
        probe_device(device);
        draw_list_entry(i, false, false);
        if (probe_aborted || uxQueueMessagesWaiting(touch_queue) > 0) {
            ESP_LOGI(TAG, "Connection tests stopped");
            break;
        }
    }
    scroll_to(0);
    show_list();
}

enum { LIST_IDLE, LIST_RESCAN, LIST_TEST };

static int handle_list_tap(const cyd_touch_point_t *point)
{
    int row = point->y / CYD_FONT_HEIGHT;
    int column = point->x / CYD_FONT_WIDTH;
    if (row == 0) {
        if (column < HEADER_TITLE_COLUMNS) {
            return LIST_RESCAN;
        }
        if (button_hit(&list_header_buttons[0], column)) {
            open_config();
            show_list();
        } else if (column >= ARROW_UP_COLUMN && column < ARROW_UP_COLUMN + ARROW_COLUMNS) {
            if (scroll_to(list_offset - LIST_ROWS)) {
                show_list();
            }
        } else if (column >= ARROW_DOWN_COLUMN) {
            if (scroll_to(list_offset + LIST_ROWS)) {
                show_list();
            }
        }
    } else if (row == FOOTER_ROW) {
        if (device_count > 0 && button_hit(&list_footer_buttons[0], column)) {
            return LIST_TEST;
        }
    } else {
        int index = list_offset + row - LIST_FIRST_ROW;
        if (index < device_count) {
            draw_list_entry(index, false, true);
            vTaskDelay(pdMS_TO_TICKS(120));
            open_detail(&devices[index]);
            show_list();
        }
    }
    return LIST_IDLE;
}

static void diag_task(void *argument)
{
    (void)argument;
    show_status(" STARTING BT");
    bt_start();
    while (true) {
        scan_all();
        if (settings[SET_AUTO_TEST]) {
            probe_all();
        } else {
            show_list();
        }
        ESP_LOGI(TAG, "Free heap: %u bytes", (unsigned)esp_get_free_heap_size());

        int action = LIST_IDLE;
        while (action != LIST_RESCAN) {
            cyd_touch_point_t point;
            wait_tap(&point);
            action = handle_list_tap(&point);
            if (action == LIST_TEST) {
                probe_all();
            }
        }
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
    settings_loaded = load_settings();

    bt_events = xEventGroupCreate();
    ESP_ERROR_CHECK(bt_events != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));
    BaseType_t task_result = xTaskCreate(diag_task, "bt_diag", 8192, NULL, 5, NULL);
    ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
