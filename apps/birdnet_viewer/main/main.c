#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyd_display.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "rom/tjpgd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define LCD_WIDTH CYD_LCD_WIDTH
#define LCD_HEIGHT CYD_LCD_HEIGHT
#define QUERY_INTERVAL_US (120LL * 1000 * 1000)
/* Settings the launcher (bootloader branch) stores in NVS for boot images,
   so no credentials are compiled into this published binary. */
#define CONFIG_NAMESPACE "cyd_config"
#define DEFAULT_BIRDNET_BASE_URL "http://birdnet.local"
#define API_JSON_CAPACITY (16 * 1024)
#define IMAGE_JPEG_CAPACITY (320 * 1024)
#define JPEG_WORKBUF_SIZE 3100
#define IMAGE_TOP 0
#define IMAGE_HEIGHT LCD_HEIGHT
/* Notification bits for the BirdNET task. */
#define TAP_BIT BIT0
#define WIFI_JOINED_BIT BIT1
#define MIN_BIRD_CONFIDENCE 0.80
/* A tap on the photo shows the bird's details this long, or until the next tap. */
#define INFO_TIMEOUT_US (30LL * 1000 * 1000)
/* The BirdNET server has no taxonomy, so the family comes from GBIF. */
#define GBIF_MATCH_URL "https://api.gbif.org/v1/species/match?name="
#define COLOR_BLACK CYD_COLOR_BLACK

static const char *TAG = "birdnet_cyd";

static char wifi_ssid[33];
static char wifi_password[65];
static char birdnet_base_url[128];
static volatile bool wifi_connected;
static TaskHandle_t app_task_handle;
static char displayed_species[96];

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
    size_t limit;
} http_response_t;

typedef struct {
    char species[64];
    char scientific_name[96];
    char date[16];
    char first_time[16];
    char detected_time[16];
    double confidence;
    int count; /* detections in the visit; 0 if the server did not say */
    bool found;
} bird_detection_t;

/* Details fetched for the info screen; empty strings until they arrive. */
typedef struct {
    char family[40];
    char weather[40];
    char seen[40];
    char since[40];
} bird_info_t;

/* The bird in the photo, for the info screen. */
static bird_detection_t displayed_bird;

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t offset;
    esp_http_client_handle_t client;
    bool read_error;
} jpeg_reader_t;

typedef struct {
    int offset_x;
    int offset_y;
    unsigned int source_width;
    unsigned int source_height;
    uint8_t *rgb565;
    size_t buffer_capacity;
    /* Placement of the resized tile currently in rgb565. */
    int tile_left;
    int tile_top;
    int tile_width;
} jpeg_output_t;

typedef struct {
    jpeg_reader_t reader;
    jpeg_output_t output;
} jpeg_context_t;

typedef struct {
    const char *data;
    size_t length;
    size_t position;
} json_cursor_t;

typedef struct {
    size_t start;
    size_t end;
} json_value_t;

/* ---- Status screen ----

   Until a photo is up, and whenever none is, the screen shows how the
   viewer is getting on: why it last started (a crash or watchdog reset
   boots straight back into this image, so this is how one shows), the
   Wi-Fi network and its state, the BirdNET server and the last request. */

#define STATUS_SIZE (CYD_TEXT_COLUMNS + 1)
#define STATUS_MAGIC 0xB1DD5747u

/* What the viewer was doing, kept across crash and watchdog resets. */
typedef struct {
    uint32_t magic;
    uint32_t crashes; /* in a row */
    char stage[STATUS_SIZE];
} crash_record_t;

static RTC_NOINIT_ATTR crash_record_t crash_record;
static char start_status[STATUS_SIZE];
static char crash_status[STATUS_SIZE];
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static char wifi_status[STATUS_SIZE] = "starting";
static char server_status[STATUS_SIZE] = "waiting for Wi-Fi";
static volatile uint32_t status_changes;
static uint32_t shown_status_changes;
/* False when something else has been drawn over the status screen. */
static bool status_on_screen;

static void set_stage(const char *stage)
{
    crash_record.magic = STATUS_MAGIC;
    snprintf(crash_record.stage, sizeof(crash_record.stage), "%s", stage);
}

/* Records why this start happened, and what a crashed run was doing. */
static void note_start(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    const char *crash = NULL;
    switch (reason) {
    case ESP_RST_PANIC:
        crash = "CRASHED";
        break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        crash = "WATCHDOG";
        break;
    case ESP_RST_BROWNOUT:
        crash = "BROWNOUT";
        break;
    default:
        break;
    }
    bool have_record = crash_record.magic == STATUS_MAGIC;
    if (crash == NULL) {
        snprintf(start_status, sizeof(start_status), "%s",
                 reason == ESP_RST_SW ? "started" : "powered on");
        crash_record.crashes = 0;
    } else {
        crash_record.crashes = have_record ? crash_record.crashes + 1 : 1;
        if (crash_record.crashes > 1) {
            snprintf(start_status, sizeof(start_status), "%.8s x%u", crash,
                     (unsigned)(crash_record.crashes % 100));
        } else {
            snprintf(start_status, sizeof(start_status), "%s", crash);
        }
        if (have_record) {
            crash_record.stage[sizeof(crash_record.stage) - 1] = '\0';
            snprintf(crash_status, sizeof(crash_status), "in %.14s", crash_record.stage);
        }
        ESP_LOGW(TAG, "Restarted after a %s (reason %d) while %s", crash, (int)reason,
                 have_record ? crash_record.stage : "unknown");
    }
    set_stage("starting");
}

static void set_status(char *field, const char *format, va_list arguments)
{
    char text[STATUS_SIZE];
    vsnprintf(text, sizeof(text), format, arguments);
    portENTER_CRITICAL(&status_lock);
    memcpy(field, text, sizeof(text));
    ++status_changes;
    portEXIT_CRITICAL(&status_lock);
}

static void set_wifi_status(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void set_wifi_status(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    set_status(wifi_status, format, arguments);
    va_end(arguments);
}

static void set_server_status(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void set_server_status(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    set_status(server_status, format, arguments);
    va_end(arguments);
}

static void draw_status(void)
{
    char wifi[STATUS_SIZE];
    char server[STATUS_SIZE];
    portENTER_CRITICAL(&status_lock);
    memcpy(wifi, wifi_status, sizeof(wifi));
    memcpy(server, server_status, sizeof(server));
    shown_status_changes = status_changes;
    portEXIT_CRITICAL(&status_lock);

    char network[STATUS_SIZE + 16];
    snprintf(network, sizeof(network), "Wi-Fi %.11s", wifi_ssid);
    const char *host = birdnet_base_url;
    if (strncmp(host, "http://", 7) == 0) {
        host += 7;
    } else if (strncmp(host, "https://", 8) == 0) {
        host += 8;
    }
    char memory[32];
    snprintf(memory, sizeof(memory), "RAM %uK free", (unsigned)(esp_get_free_heap_size() / 1024));
    bool crashed = crash_record.crashes > 0;
    const struct {
        uint16_t color;
        const char *text;
    } rows[CYD_TEXT_ROWS] = {
        {CYD_COLOR_YELLOW, "BIRDNET VIEWER"},
        {crashed ? CYD_COLOR_RED : CYD_COLOR_GRAY, start_status},
        {CYD_COLOR_RED, crash_status},
        {CYD_COLOR_CYAN, network},
        {CYD_COLOR_WHITE, wifi},
        {CYD_COLOR_CYAN, host},
        {CYD_COLOR_WHITE, server},
        {CYD_COLOR_GRAY, wifi_connected ? "" : "EN opens launcher"},
        {CYD_COLOR_GRAY, memory},
        {CYD_COLOR_GRAY, "tap to refresh"},
    };
    for (int row = 0; row < CYD_TEXT_ROWS; ++row) {
        ESP_ERROR_CHECK(cyd_text_row(row, rows[row].color, CYD_COLOR_BLACK, rows[row].text));
    }
    status_on_screen = true;
}

static bool displayed_image_ready;
/* True while the bird info screen is up in place of the photo. */
static bool info_on_screen;

/* Redraws the status screen when it changed, unless a photo or the bird
   info is up. */
static void update_status_screen(void)
{
    if (!displayed_image_ready && !info_on_screen &&
        (!status_on_screen || status_changes != shown_status_changes)) {
        draw_status();
    }
}

/* Says what the viewer is doing, on screen and in the crash record. */
static void show_progress(const char *stage)
{
    set_stage(stage);
    set_server_status("%s", stage);
    update_status_screen();
}

/* The status of the last request that got an answer, else 0. */
static int last_http_status;

/* "visits HTTP 404", or the error without its ESP_ERR_ prefix. */
static void report_failure(const char *what, esp_err_t result)
{
    if (result == ESP_FAIL && last_http_status != 0 && last_http_status != 200) {
        set_server_status("%s HTTP %d", what, last_http_status);
    } else {
        const char *name = esp_err_to_name(result);
        set_server_status("%s %s", what, strncmp(name, "ESP_ERR_", 8) == 0 ? name + 8 : name);
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    http_response_t *response = event->user_data;
    size_t chunk_length = (size_t)event->data_len;
    if (response->length > response->limit ||
        chunk_length > response->limit - response->length) {
        ESP_LOGE(TAG, "HTTP response exceeded %u-byte limit",
                 (unsigned)response->limit);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t required = response->length + chunk_length;
    if (required > response->capacity) {
        size_t new_capacity = response->capacity * 2;
        if (new_capacity < required) {
            new_capacity = required;
        }
        if (new_capacity > response->limit) {
            new_capacity = response->limit;
        }
        uint8_t *new_data = heap_caps_realloc(response->data, new_capacity + 1,
                                              MALLOC_CAP_8BIT);
        if (new_data == NULL) {
            ESP_LOGE(TAG, "Could not grow HTTP response buffer to %u bytes",
                     (unsigned)new_capacity);
            return ESP_ERR_NO_MEM;
        }
        response->data = new_data;
        response->capacity = new_capacity;
    }
    memcpy(response->data + response->length, event->data, chunk_length);
    response->length += chunk_length;
    return ESP_OK;
}

static esp_err_t http_get(const char *url, size_t capacity, http_response_t *response)
{
    memset(response, 0, sizeof(*response));
    size_t initial_capacity = capacity < 2048 ? capacity : 2048;
    response->data = heap_caps_malloc(initial_capacity + 1, MALLOC_CAP_8BIT);
    if (response->data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    response->capacity = initial_capacity;
    response->limit = capacity;

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = strncmp(url, "https://", 8) == 0
                                 ? esp_crt_bundle_attach
                                 : NULL,
        .event_handler = http_event_handler,
        .user_data = response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        heap_caps_free(response->data);
        response->data = NULL;
        return ESP_FAIL;
    }

    esp_err_t result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    last_http_status = result == ESP_OK ? status : 0;
    if (result == ESP_OK && status != 200) {
        ESP_LOGE(TAG, "GET %s returned HTTP %d", url, status);
        result = ESP_FAIL;
    }
    esp_http_client_cleanup(client);
    if (result != ESP_OK) {
        heap_caps_free(response->data);
        response->data = NULL;
        return result;
    }
    response->data[response->length] = '\0';
    return ESP_OK;
}

static void http_response_free(http_response_t *response)
{
    heap_caps_free(response->data);
    memset(response, 0, sizeof(*response));
}

static void json_skip_whitespace(json_cursor_t *cursor)
{
    while (cursor->position < cursor->length &&
           (cursor->data[cursor->position] == ' ' ||
            cursor->data[cursor->position] == '\t' ||
            cursor->data[cursor->position] == '\r' ||
            cursor->data[cursor->position] == '\n')) {
        ++cursor->position;
    }
}

static int json_hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static bool json_parse_string(json_cursor_t *cursor, char *output, size_t capacity)
{
    if (cursor->position >= cursor->length || cursor->data[cursor->position++] != '"') {
        return false;
    }
    size_t written = 0;
    while (cursor->position < cursor->length) {
        unsigned char character = (unsigned char)cursor->data[cursor->position++];
        if (character == '"') {
            if (output != NULL && capacity > 0) {
                output[written] = '\0';
            }
            return true;
        }
        if (character < 0x20) {
            return false;
        }
        if (character == '\\') {
            if (cursor->position >= cursor->length) {
                return false;
            }
            character = (unsigned char)cursor->data[cursor->position++];
            if (character == 'u') {
                if (cursor->length - cursor->position < 4) {
                    return false;
                }
                int codepoint = 0;
                for (int index = 0; index < 4; ++index) {
                    int nibble = json_hex_value(cursor->data[cursor->position++]);
                    if (nibble < 0) {
                        return false;
                    }
                    codepoint = (codepoint << 4) | nibble;
                }
                character = codepoint <= 0x7F ? (unsigned char)codepoint : '?';
            } else if (character == '"' || character == '\\' || character == '/') {
                /* Keep the escaped character as-is. */
            } else if (character == 'b') {
                character = '\b';
            } else if (character == 'f') {
                character = '\f';
            } else if (character == 'n') {
                character = '\n';
            } else if (character == 'r') {
                character = '\r';
            } else if (character == 't') {
                character = '\t';
            } else {
                return false;
            }
        }
        if (output != NULL && capacity > 1 && written < capacity - 1) {
            output[written++] = (char)character;
        }
    }
    return false;
}

static bool json_skip_value(json_cursor_t *cursor, unsigned int depth)
{
    if (depth > 16) {
        return false;
    }
    json_skip_whitespace(cursor);
    if (cursor->position >= cursor->length) {
        return false;
    }
    char first = cursor->data[cursor->position];
    if (first == '"') {
        return json_parse_string(cursor, NULL, 0);
    }
    if (first == '{') {
        ++cursor->position;
        json_skip_whitespace(cursor);
        if (cursor->position < cursor->length && cursor->data[cursor->position] == '}') {
            ++cursor->position;
            return true;
        }
        while (cursor->position < cursor->length) {
            if (!json_parse_string(cursor, NULL, 0)) {
                return false;
            }
            json_skip_whitespace(cursor);
            if (cursor->position >= cursor->length || cursor->data[cursor->position++] != ':') {
                return false;
            }
            if (!json_skip_value(cursor, depth + 1)) {
                return false;
            }
            json_skip_whitespace(cursor);
            if (cursor->position >= cursor->length) {
                return false;
            }
            char delimiter = cursor->data[cursor->position++];
            if (delimiter == '}') {
                return true;
            }
            if (delimiter != ',') {
                return false;
            }
            json_skip_whitespace(cursor);
        }
        return false;
    }
    if (first == '[') {
        ++cursor->position;
        json_skip_whitespace(cursor);
        if (cursor->position < cursor->length && cursor->data[cursor->position] == ']') {
            ++cursor->position;
            return true;
        }
        while (cursor->position < cursor->length) {
            if (!json_skip_value(cursor, depth + 1)) {
                return false;
            }
            json_skip_whitespace(cursor);
            if (cursor->position >= cursor->length) {
                return false;
            }
            char delimiter = cursor->data[cursor->position++];
            if (delimiter == ']') {
                return true;
            }
            if (delimiter != ',') {
                return false;
            }
        }
        return false;
    }

    size_t start = cursor->position;
    while (cursor->position < cursor->length) {
        char character = cursor->data[cursor->position];
        if (character == ',' || character == ']' || character == '}' ||
            character == ' ' || character == '\t' ||
            character == '\r' || character == '\n') {
            break;
        }
        ++cursor->position;
    }
    return cursor->position > start;
}

static bool json_object_field(const char *data, size_t start, size_t end,
                              const char *field_name, json_value_t *value)
{
    json_cursor_t cursor = {.data = data, .length = end, .position = start};
    json_skip_whitespace(&cursor);
    if (cursor.position >= end || cursor.data[cursor.position++] != '{') {
        return false;
    }
    json_skip_whitespace(&cursor);
    if (cursor.position < end && cursor.data[cursor.position] == '}') {
        return false;
    }
    while (cursor.position < end) {
        char key[48];
        if (!json_parse_string(&cursor, key, sizeof(key))) {
            return false;
        }
        json_skip_whitespace(&cursor);
        if (cursor.position >= end || cursor.data[cursor.position++] != ':') {
            return false;
        }
        json_skip_whitespace(&cursor);
        size_t value_start = cursor.position;
        if (!json_skip_value(&cursor, 0)) {
            return false;
        }
        size_t value_end = cursor.position;
        if (strcmp(key, field_name) == 0) {
            value->start = value_start;
            value->end = value_end;
            return true;
        }
        json_skip_whitespace(&cursor);
        if (cursor.position >= end) {
            return false;
        }
        char delimiter = cursor.data[cursor.position++];
        if (delimiter == '}') {
            return false;
        }
        if (delimiter != ',') {
            return false;
        }
        json_skip_whitespace(&cursor);
    }
    return false;
}

static bool json_value_string(const char *data, json_value_t value,
                              char *output, size_t capacity)
{
    json_cursor_t cursor = {
        .data = data, .length = value.end, .position = value.start,
    };
    return json_parse_string(&cursor, output, capacity) && cursor.position == value.end;
}

static bool json_value_number(const char *data, json_value_t value, double *number)
{
    char buffer[40];
    size_t length = value.end - value.start;
    if (length == 0 || length >= sizeof(buffer)) {
        return false;
    }
    memcpy(buffer, data + value.start, length);
    buffer[length] = '\0';
    char *end;
    *number = strtod(buffer, &end);
    return end != buffer && *end == '\0';
}

static bool json_object_string(const char *data, size_t start, size_t end,
                               const char *key, char *output, size_t capacity)
{
    json_value_t value;
    output[0] = '\0';
    return json_object_field(data, start, end, key, &value) &&
           json_value_string(data, value, output, capacity);
}

static esp_err_t fetch_latest_bird(bird_detection_t *bird)
{
    char url[sizeof(birdnet_base_url) + 32];
    snprintf(url, sizeof(url), "%s/api/v1/detections/visits", birdnet_base_url);
    http_response_t response;
    esp_err_t result = http_get(url, API_JSON_CAPACITY, &response);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not fetch BirdNET visits: %s", esp_err_to_name(result));
        return result;
    }

    json_value_t visits;
    if (!json_object_field((const char *)response.data, 0, response.length,
                           "visits", &visits) ||
        visits.start >= visits.end || response.data[visits.start] != '[') {
        http_response_free(&response);
        ESP_LOGE(TAG, "BirdNET visits response has no visits array");
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(bird, 0, sizeof(*bird));
    double best_seconds_ago = 0;
    json_cursor_t array = {
        .data = (const char *)response.data,
        .length = visits.end,
        .position = visits.start + 1,
    };
    bool valid_response = true;
    while (array.position < array.length) {
        json_skip_whitespace(&array);
        if (array.position < array.length && array.data[array.position] == ']') {
            ++array.position;
            break;
        }
        json_value_t visit = {.start = array.position};
        if (!json_skip_value(&array, 0)) {
            valid_response = false;
            break;
        }
        visit.end = array.position;

        json_value_t confidence_value;
        json_value_t seconds_ago_value;
        double confidence = 0;
        double seconds_ago = 0;
        if (!json_object_field(array.data, visit.start, visit.end,
                               "best_confidence", &confidence_value) ||
            !json_object_field(array.data, visit.start, visit.end,
                               "seconds_ago", &seconds_ago_value) ||
            !json_value_number(array.data, confidence_value, &confidence) ||
            !json_value_number(array.data, seconds_ago_value, &seconds_ago) ||
            confidence < MIN_BIRD_CONFIDENCE) {
            json_skip_whitespace(&array);
            if (array.position < array.length && array.data[array.position] == ',') {
                ++array.position;
                continue;
            }
            if (array.position < array.length && array.data[array.position] == ']') {
                ++array.position;
                break;
            }
            valid_response = false;
            break;
        }
        if (bird->found && seconds_ago >= best_seconds_ago) {
            json_skip_whitespace(&array);
            if (array.position < array.length && array.data[array.position] == ',') {
                ++array.position;
                continue;
            }
            if (array.position < array.length && array.data[array.position] == ']') {
                ++array.position;
                break;
            }
            valid_response = false;
            break;
        }
        char species[sizeof(bird->species)];
        char scientific_name[sizeof(bird->scientific_name)];
        char date[sizeof(bird->date)];
        char detected_time[sizeof(bird->detected_time)];
        if (!json_object_string(array.data, visit.start, visit.end,
                                "species", species, sizeof(species)) ||
            !json_object_string(array.data, visit.start, visit.end,
                                "sci_name", scientific_name, sizeof(scientific_name)) ||
            !json_object_string(array.data, visit.start, visit.end,
                                "date", date, sizeof(date)) ||
            !json_object_string(array.data, visit.start, visit.end,
                                "last_time", detected_time, sizeof(detected_time)) ||
            species[0] == '\0' || scientific_name[0] == '\0') {
            json_skip_whitespace(&array);
            if (array.position < array.length && array.data[array.position] == ',') {
                ++array.position;
                continue;
            }
            if (array.position < array.length && array.data[array.position] == ']') {
                ++array.position;
                break;
            }
            valid_response = false;
            break;
        }
        snprintf(bird->species, sizeof(bird->species), "%s", species);
        snprintf(bird->scientific_name, sizeof(bird->scientific_name), "%s",
                 scientific_name);
        snprintf(bird->date, sizeof(bird->date), "%s", date);
        snprintf(bird->detected_time, sizeof(bird->detected_time), "%s", detected_time);
        /* Extras for the info screen; a visit without them still counts. */
        if (!json_object_string(array.data, visit.start, visit.end, "first_time",
                                bird->first_time, sizeof(bird->first_time))) {
            snprintf(bird->first_time, sizeof(bird->first_time), "%s", detected_time);
        }
        json_value_t count_value;
        double count = 0;
        bird->count = json_object_field(array.data, visit.start, visit.end, "count",
                                        &count_value) &&
                              json_value_number(array.data, count_value, &count)
                          ? (int)count
                          : 0;
        bird->confidence = confidence;
        bird->found = true;
        best_seconds_ago = seconds_ago;

        json_skip_whitespace(&array);
        if (array.position < array.length && array.data[array.position] == ',') {
            ++array.position;
            continue;
        }
        if (array.position < array.length && array.data[array.position] == ']') {
            ++array.position;
            break;
        }
        valid_response = false;
        break;
    }
    json_skip_whitespace(&array);
    if (array.position != array.length || array.data[array.position - 1] != ']') {
        valid_response = false;
    }
    http_response_free(&response);
    if (!valid_response) {
        ESP_LOGE(TAG, "BirdNET visits response contains malformed JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!bird->found) {
        ESP_LOGW(TAG, "No visit met the %.0f%% confidence threshold",
                 MIN_BIRD_CONFIDENCE * 100);
    } else {
        ESP_LOGI(TAG, "Latest qualifying visit: %s (%.1f%%), %s %s",
                 bird->species, bird->confidence * 100,
                 bird->date, bird->detected_time);
    }
    return ESP_OK;
}

static bool url_encode_path(const char *source, char *destination, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t output = 0;
    for (const unsigned char *cursor = (const unsigned char *)source;
         *cursor != '\0'; ++cursor) {
        bool safe = (*cursor >= 'a' && *cursor <= 'z') ||
                    (*cursor >= 'A' && *cursor <= 'Z') ||
                    (*cursor >= '0' && *cursor <= '9') ||
                    *cursor == '-' || *cursor == '_' || *cursor == '.';
        size_t needed = safe ? 1 : 3;
        if (output + needed >= capacity) {
            return false;
        }
        if (safe) {
            destination[output++] = (char)*cursor;
        } else {
            destination[output++] = '%';
            destination[output++] = hex[*cursor >> 4];
            destination[output++] = hex[*cursor & 0x0F];
        }
    }
    destination[output] = '\0';
    return true;
}

static esp_err_t fetch_bird_image(const char *scientific_name, size_t *jpeg_length,
                                  char *image_url,
                                  size_t image_url_capacity,
                                  esp_http_client_handle_t *image_client)
{
    char encoded_name[288];
    char metadata_url[sizeof(birdnet_base_url) + sizeof(encoded_name) + 16];
    if (!url_encode_path(scientific_name, encoded_name, sizeof(encoded_name)) ||
        snprintf(metadata_url, sizeof(metadata_url),
                 "%s/api/v1/image/%s", birdnet_base_url, encoded_name) >=
            (int)sizeof(metadata_url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    http_response_t metadata;
    esp_err_t result = http_get(metadata_url, API_JSON_CAPACITY, &metadata);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not fetch image metadata: %s", esp_err_to_name(result));
        return result;
    }
    json_value_t data_value;
    bool has_data = json_object_field((const char *)metadata.data, 0,
                                      metadata.length, "data", &data_value);
    bool metadata_valid =
        has_data &&
        json_object_string((const char *)metadata.data, data_value.start,
                           data_value.end, "image_url",
                           image_url, image_url_capacity);
    http_response_free(&metadata);
    if (!metadata_valid) {
        ESP_LOGE(TAG, "Image metadata response is malformed or missing image URL");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strncmp(image_url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "Image metadata did not provide an HTTPS image URL");
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_http_client_config_t config = {
        .url = image_url,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    *image_client = esp_http_client_init(&config);
    if (*image_client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    result = esp_http_client_open(*image_client, 0);
    if (result != ESP_OK) {
        esp_http_client_cleanup(*image_client);
        *image_client = NULL;
        ESP_LOGE(TAG, "Could not open Wikimedia image stream: %s",
                 esp_err_to_name(result));
        return result;
    }
    int64_t content_length = esp_http_client_fetch_headers(*image_client);
    int status = esp_http_client_get_status_code(*image_client);
    last_http_status = status;
    if (status != 200 || content_length <= 0 ||
        content_length > IMAGE_JPEG_CAPACITY) {
        ESP_LOGE(TAG, "Wikimedia image stream returned HTTP %d and %u bytes",
                 status, (unsigned)(content_length < 0 ? 0 : content_length));
        esp_http_client_close(*image_client);
        esp_http_client_cleanup(*image_client);
        *image_client = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }
    *jpeg_length = (size_t)content_length;
    ESP_LOGI(TAG, "Opened %u-byte Wikimedia image stream; free heap: %u bytes",
             (unsigned)*jpeg_length, (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

static UINT jpeg_input(JDEC *decoder, BYTE *buffer, UINT length)
{
    jpeg_context_t *context = decoder->device;
    jpeg_reader_t *reader = &context->reader;
    if (reader->offset >= reader->length) {
        return 0;
    }

    size_t wanted = length;
    size_t remaining = reader->length - reader->offset;
    if (wanted > remaining) {
        wanted = remaining;
    }
    uint8_t discard[64];
    size_t received = 0;
    while (received < wanted) {
        size_t chunk = wanted - received;
        if (buffer == NULL && chunk > sizeof(discard)) {
            chunk = sizeof(discard);
        }
        char *destination = buffer != NULL
                                ? (char *)buffer + received
                                : (char *)discard;
        int bytes = esp_http_client_read(reader->client, destination, (int)chunk);
        if (bytes <= 0) {
            reader->read_error = true;
            ESP_LOGE(TAG, "Wikimedia image stream ended during JPEG decode");
            break;
        }
        received += (size_t)bytes;
    }
    reader->offset += received;
    return (UINT)received;
}

static uint16_t jpeg_tile_pixel(int x, int y, void *context)
{
    const jpeg_output_t *output = context;
    size_t index = ((size_t)(y - output->tile_top) * output->tile_width +
                    (x - output->tile_left)) * 2;
    return (uint16_t)((output->rgb565[index] << 8) | output->rgb565[index + 1]);
}

static UINT jpeg_output(JDEC *decoder, void *pixels, JRECT *rectangle)
{
    jpeg_context_t *context = decoder->device;
    jpeg_output_t *output = &context->output;
    int width = rectangle->right - rectangle->left + 1;
    int height = rectangle->bottom - rectangle->top + 1;
    if (width <= 0 || height <= 0 || width > 16 || height > 16) {
        ESP_LOGE(TAG, "Unexpected JPEG output tile %dx%d", width, height);
        return 0;
    }

    const uint8_t *rgb = pixels;
    int left = ((size_t)rectangle->left * LCD_WIDTH +
                output->source_width - 1) / output->source_width;
    int right = ((size_t)(rectangle->right + 1) * LCD_WIDTH +
                 output->source_width - 1) / output->source_width;
    int top = ((size_t)rectangle->top * LCD_HEIGHT +
               output->source_height - 1) / output->source_height;
    int bottom = ((size_t)(rectangle->bottom + 1) * LCD_HEIGHT +
                  output->source_height - 1) / output->source_height;
    int output_width = right - left;
    int output_height = bottom - top;
    if (output_width <= 0 || output_height <= 0 ||
        (size_t)output_width * output_height * 2 > output->buffer_capacity) {
        ESP_LOGE(TAG, "Resized JPEG tile %dx%d exceeds its output buffer",
                 output_width, output_height);
        return 0;
    }
    for (int row = 0; row < output_height; ++row) {
        int source_y = (top + row) * output->source_height / LCD_HEIGHT -
                       rectangle->top;
        for (int column = 0; column < output_width; ++column) {
            int source_x = (left + column) * output->source_width / LCD_WIDTH -
                           rectangle->left;
            int source_index = (source_y * width + source_x) * 3;
            uint8_t red = rgb[source_index];
            uint8_t green = rgb[source_index + 1];
            uint8_t blue = rgb[source_index + 2];
            uint16_t color = (uint16_t)(((red & 0xF8) << 8) |
                                        ((green & 0xFC) << 3) | (blue >> 3));
            int target_index = (row * output_width + column) * 2;
            output->rgb565[target_index] = (uint8_t)(color >> 8);
            output->rgb565[target_index + 1] = (uint8_t)color;
        }
    }
    /* cyd_draw_pattern sends pixels in the panel's column-major order. */
    output->tile_left = left;
    output->tile_top = top;
    output->tile_width = output_width;
    if (cyd_draw_pattern(left, top, output_width, output_height, jpeg_tile_pixel,
                         output) != ESP_OK) {
        return 0;
    }
    return 1;
}

static esp_err_t draw_jpeg(esp_http_client_handle_t image_client, size_t length)
{
    jpeg_context_t context = {
        .reader = {.length = length, .client = image_client},
    };
    JDEC decoder = {0};
    void *work_buffer = heap_caps_malloc(JPEG_WORKBUF_SIZE, MALLOC_CAP_8BIT);
    if (work_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    JRESULT result = jd_prepare(&decoder, jpeg_input, work_buffer,
                                JPEG_WORKBUF_SIZE, &context);
    if (result != JDR_OK) {
        heap_caps_free(work_buffer);
        ESP_LOGE(TAG, "JPEG prepare failed: %d", result);
        return ESP_ERR_INVALID_RESPONSE;
    }

    BYTE scale = 0;
    while (scale < 3 &&
           ((decoder.width + (1U << scale) - 1) >> scale) > LCD_WIDTH) {
        ++scale;
    }
    int scaled_width = (int)((decoder.width + (1U << scale) - 1) >> scale);
    int scaled_height = (int)((decoder.height + (1U << scale) - 1) >> scale);
    if (scaled_width > LCD_WIDTH) {
        heap_caps_free(work_buffer);
        ESP_LOGE(TAG, "JPEG image is too wide to fit the display");
        return ESP_ERR_INVALID_SIZE;
    }
    size_t tile_width = ((size_t)16 * LCD_WIDTH + scaled_width - 1) / scaled_width + 1;
    size_t tile_height = ((size_t)16 * LCD_HEIGHT + scaled_height - 1) / scaled_height + 1;
    if (tile_width > 64 || tile_height > 64) {
        heap_caps_free(work_buffer);
        ESP_LOGE(TAG, "JPEG dimensions are too small for full-screen scaling");
        return ESP_ERR_INVALID_SIZE;
    }
    size_t output_capacity = tile_width * tile_height * 2;
    uint8_t *rgb565 = heap_caps_malloc(output_capacity, MALLOC_CAP_DMA);
    if (rgb565 == NULL) {
        heap_caps_free(work_buffer);
        return ESP_ERR_NO_MEM;
    }
    context.output = (jpeg_output_t){
        .source_width = (unsigned int)scaled_width,
        .source_height = (unsigned int)scaled_height,
        .rgb565 = rgb565,
        .buffer_capacity = output_capacity,
    };
    status_on_screen = false;
    if (cyd_fill_rect(0, IMAGE_TOP, LCD_WIDTH, IMAGE_HEIGHT, COLOR_BLACK) != ESP_OK) {
        heap_caps_free(rgb565);
        heap_caps_free(work_buffer);
        return ESP_FAIL;
    }
    result = jd_decomp(&decoder, jpeg_output, scale);
    heap_caps_free(rgb565);
    heap_caps_free(work_buffer);
    if (result != JDR_OK || context.reader.read_error) {
        ESP_LOGE(TAG, "JPEG decode failed: %d", result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "Rendered %ux%u bird image at 1:%u scale",
             (unsigned)decoder.width, (unsigned)decoder.height, 1U << scale);
    return ESP_OK;
}

static void refresh_birdnet(void)
{
    bird_detection_t bird;
    show_progress("getting visits");
    esp_err_t result = fetch_latest_bird(&bird);
    if (result != ESP_OK) {
        /* A photo already up stays; otherwise the status screen says why. */
        report_failure("visits", result);
        set_stage("idle");
        return;
    }

    if (!bird.found) {
        displayed_species[0] = '\0';
        displayed_image_ready = false;
        set_server_status("no bird >= %.0f%%", MIN_BIRD_CONFIDENCE * 100);
        set_stage("idle");
        return;
    }

    bool species_changed = strcmp(displayed_species, bird.scientific_name) != 0;
    bool need_image = species_changed || !displayed_image_ready;
    char image_url[768];
    size_t jpeg_length = 0;
    esp_http_client_handle_t image_client = NULL;
    esp_err_t image_result = ESP_OK;
    if (need_image) {
        show_progress("getting photo");
        image_result = fetch_bird_image(bird.scientific_name, &jpeg_length,
                                        image_url, sizeof(image_url), &image_client);
        if (image_result != ESP_OK) {
            report_failure("photo", image_result);
            ESP_LOGW(TAG, "Bird image unavailable");
        }
    }

    if (species_changed) {
        displayed_image_ready = false;
    }
    if (image_result == ESP_OK && need_image) {
        set_stage("drawing photo");
        image_result = draw_jpeg(image_client, jpeg_length);
        esp_http_client_close(image_client);
        esp_http_client_cleanup(image_client);
        if (image_result == ESP_OK) {
            displayed_image_ready = true;
            set_server_status("showing photo");
        } else {
            report_failure("drawing", image_result);
            ESP_LOGW(TAG, "Could not render bird image: %s", esp_err_to_name(image_result));
        }
    }
    /* Without a photo, the status screen comes back and says why. */
    if (!displayed_image_ready) {
        ESP_LOGW(TAG, "No bird image is displayed");
    }
    snprintf(displayed_species, sizeof(displayed_species), "%s", bird.scientific_name);
    displayed_bird = bird;
    set_stage("idle");
}

/* ---- Bird info screen ---- */

/* Fetches a JSON document of up to capacity bytes. */
static esp_err_t get_json(const char *url, size_t capacity, http_response_t *response)
{
    esp_err_t result = http_get(url, capacity, response);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "GET %s failed: %s", url, esp_err_to_name(result));
    }
    return result;
}

static int rounded(double value)
{
    return (int)(value < 0 ? value - 0.5 : value + 0.5);
}

static bool json_top_number(const http_response_t *response, const char *key, double *number)
{
    json_value_t value;
    return json_object_field((const char *)response->data, 0, response->length, key, &value) &&
           json_value_number((const char *)response->data, value, number);
}

static bool json_top_string(const http_response_t *response, const char *key, char *output,
                            size_t capacity)
{
    return json_object_string((const char *)response->data, 0, response->length, key, output,
                              capacity);
}

/* The family from GBIF's name match, such as "Paridae"; kept per species. */
static void fetch_family(const char *scientific_name, char *family, size_t capacity)
{
    static char cached_species[96];
    static char cached_family[40];
    if (strcmp(cached_species, scientific_name) != 0) {
        char encoded[288];
        char url[sizeof(GBIF_MATCH_URL) + sizeof(encoded)];
        http_response_t response;
        cached_family[0] = '\0';
        if (url_encode_path(scientific_name, encoded, sizeof(encoded)) &&
            snprintf(url, sizeof(url), GBIF_MATCH_URL "%s", encoded) < (int)sizeof(url) &&
            get_json(url, API_JSON_CAPACITY, &response) == ESP_OK) {
            json_top_string(&response, "family", cached_family, sizeof(cached_family));
            http_response_free(&response);
        }
        snprintf(cached_species, sizeof(cached_species), "%s", scientific_name);
    }
    snprintf(family, capacity, "%s", cached_family[0] != '\0' ? cached_family : "family ?");
}

/* "now 68F Clear": the server keeps only the current weather, not the
   weather at the time of a detection. */
static void fetch_weather(char *weather, size_t capacity)
{
    char url[sizeof(birdnet_base_url) + 32];
    snprintf(url, sizeof(url), "%s/api/v1/weather/current", birdnet_base_url);
    http_response_t response;
    snprintf(weather, capacity, "weather ?");
    if (get_json(url, API_JSON_CAPACITY, &response) != ESP_OK) {
        return;
    }
    double temperature;
    char unit[8];
    char condition[24];
    if (json_top_number(&response, "temp", &temperature)) {
        /* "°F" arrives as "?F": keep the letter. */
        size_t unit_length = json_top_string(&response, "temp_unit", unit, sizeof(unit))
                                 ? strlen(unit)
                                 : 0;
        if (!json_top_string(&response, "condition", condition, sizeof(condition))) {
            condition[0] = '\0';
        }
        snprintf(weather, capacity, "now %d%c %s", rounded(temperature),
                 unit_length > 0 ? unit[unit_length - 1] : ' ', condition);
    }
    http_response_free(&response);
}

/* How often the station has heard the species, and since when. */
static void fetch_species_totals(const char *scientific_name, bird_info_t *info)
{
    char encoded[288];
    char url[sizeof(birdnet_base_url) + sizeof(encoded) + 40];
    http_response_t response;
    snprintf(info->seen, sizeof(info->seen), "heard ? times");
    if (!url_encode_path(scientific_name, encoded, sizeof(encoded)) ||
        snprintf(url, sizeof(url), "%s/api/v1/species/detail?sci_name=%s", birdnet_base_url,
                 encoded) >= (int)sizeof(url) ||
        get_json(url, 4 * API_JSON_CAPACITY, &response) != ESP_OK) {
        return;
    }
    double total;
    char first_seen[16];
    if (json_top_number(&response, "total_detections", &total)) {
        snprintf(info->seen, sizeof(info->seen), "heard %d times", rounded(total));
    }
    if (json_top_string(&response, "first_seen", first_seen, sizeof(first_seen))) {
        snprintf(info->since, sizeof(info->since), "since %s", first_seen);
    }
    http_response_free(&response);
}

/* Splits a name at the last space that keeps the first line within one row. */
static void wrap_name(const char *name, char *first, char *second, size_t capacity)
{
    size_t length = strlen(name);
    size_t split = length;
    if (length > CYD_TEXT_COLUMNS) {
        split = CYD_TEXT_COLUMNS;
        while (split > 0 && name[split] != ' ') {
            --split;
        }
        if (split == 0) {
            split = CYD_TEXT_COLUMNS;
        }
    }
    snprintf(first, capacity, "%.*s", (int)split, name);
    snprintf(second, capacity, "%s", name + split + (name[split] == ' ' ? 1 : 0));
}

/* "Carolina Chickadee / Poecile caro... / Paridae / 10-20 09:06-09:13 /
   19 calls, 94% / now 68F Clear / heard 1127 times / since 2026-09-12". */
static void draw_bird_info(const bird_detection_t *bird, const bird_info_t *info)
{
    char name[2][CYD_TEXT_COLUMNS * 4];
    char heard[48];
    char calls[48];
    wrap_name(bird->species, name[0], name[1], sizeof(name[0]));
    /* "2026-10-20" shows as "10-20"; times drop their seconds. */
    const char *date = strlen(bird->date) >= 10 ? bird->date + 5 : bird->date;
    if (strcmp(bird->first_time, bird->detected_time) == 0) {
        snprintf(heard, sizeof(heard), "%.5s at %.5s", date, bird->detected_time);
    } else {
        snprintf(heard, sizeof(heard), "%.5s %.5s-%.5s", date, bird->first_time,
                 bird->detected_time);
    }
    if (bird->count > 0) {
        snprintf(calls, sizeof(calls), "%d call%s, %d%%", bird->count,
                 bird->count == 1 ? "" : "s", rounded(bird->confidence * 100));
    } else {
        snprintf(calls, sizeof(calls), "confidence %d%%", rounded(bird->confidence * 100));
    }
    const struct {
        uint16_t color;
        const char *text;
    } rows[CYD_TEXT_ROWS] = {
        {CYD_COLOR_YELLOW, name[0]},
        {CYD_COLOR_YELLOW, name[1]},
        {CYD_COLOR_GRAY, bird->scientific_name},
        {CYD_COLOR_CYAN, info->family[0] != '\0' ? info->family : "..."},
        {CYD_COLOR_WHITE, heard},
        {CYD_COLOR_WHITE, calls},
        {CYD_COLOR_WHITE, info->weather[0] != '\0' ? info->weather : "..."},
        {CYD_COLOR_WHITE, info->seen[0] != '\0' ? info->seen : "..."},
        {CYD_COLOR_WHITE, info->since},
        {CYD_COLOR_GRAY, "tap for photo"},
    };
    for (int row = 0; row < CYD_TEXT_ROWS; ++row) {
        ESP_ERROR_CHECK(cyd_text_row(row, rows[row].color, CYD_COLOR_BLACK, rows[row].text));
    }
}

/* Shows what is known about the bird in the photo at once, then fills in
   its family, the weather and its totals as they arrive. */
static void show_bird_info(void)
{
    bird_info_t info = {0};
    info_on_screen = true;
    status_on_screen = false;
    set_stage("showing info");
    draw_bird_info(&displayed_bird, &info);
    fetch_family(displayed_bird.scientific_name, info.family, sizeof(info.family));
    draw_bird_info(&displayed_bird, &info);
    fetch_weather(info.weather, sizeof(info.weather));
    draw_bird_info(&displayed_bird, &info);
    fetch_species_totals(displayed_bird.scientific_name, &info);
    draw_bird_info(&displayed_bird, &info);
    set_stage("idle");
}

/* The photo was streamed, not kept, so going back fetches it again. The
   info stays up until the photo starts drawing over it. */
static void leave_bird_info(void)
{
    displayed_image_ready = false;
    refresh_birdnet();
    info_on_screen = false;
}

/* Turns taps from the shared touchscreen driver into refresh requests. */
static void touch_task(void *argument)
{
    QueueHandle_t touch_queue = argument;
    while (true) {
        cyd_touch_point_t point;
        if (xQueueReceive(touch_queue, &point, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Touch at %d,%d", point.x, point.y);
            if (app_task_handle != NULL) {
                xTaskNotify(app_task_handle, TAP_BIT, eSetBits);
            }
        }
    }
}

/* Why a join failed, in a few words. */
static const char *wifi_failure_text(uint8_t reason, char *text, size_t size)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "no AP found";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "bad password?";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "signal lost";
    default:
        snprintf(text, size, "error %u", (unsigned)reason);
        return text;
    }
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    static unsigned attempts;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        set_wifi_status("joining");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        char text[16];
        if (wifi_connected) {
            set_server_status("waiting for Wi-Fi");
        }
        wifi_connected = false;
        ++attempts;
        set_wifi_status("#%u %s", attempts,
                        wifi_failure_text(disconnected->reason, text, sizeof(text)));
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %u); retrying",
                 (unsigned)disconnected->reason);
        /* Retrying never gives up, so a failed attempt must not abort. */
        esp_err_t result = esp_wifi_connect();
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi reconnect failed: %s", esp_err_to_name(result));
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        wifi_connected = true;
        attempts = 0;
        set_wifi_status(IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Wi-Fi connected; IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        if (app_task_handle != NULL) {
            xTaskNotify(app_task_handle, WIFI_JOINED_BIT, eSetBits);
        }
    }
}

/* Reads Wi-Fi and BirdNET settings from NVS; returns false when there are no
   Wi-Fi settings yet. */
static bool load_config(void)
{
    snprintf(birdnet_base_url, sizeof(birdnet_base_url), "%s", DEFAULT_BIRDNET_BASE_URL);
    nvs_handle_t nvs;
    if (nvs_flash_init() != ESP_OK || nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t size = sizeof(wifi_ssid);
    bool have_wifi = nvs_get_str(nvs, "wifi_ssid", wifi_ssid, &size) == ESP_OK &&
                     wifi_ssid[0] != '\0';
    size = sizeof(wifi_password);
    if (nvs_get_str(nvs, "wifi_pass", wifi_password, &size) != ESP_OK) {
        wifi_password[0] = '\0';
    }
    size = sizeof(birdnet_base_url);
    if (nvs_get_str(nvs, "birdnet_url", birdnet_base_url, &size) != ESP_OK ||
        birdnet_base_url[0] == '\0') {
        snprintf(birdnet_base_url, sizeof(birdnet_base_url), "%s", DEFAULT_BIRDNET_BASE_URL);
    }
    nvs_close(nvs);
    size_t length = strlen(birdnet_base_url);
    while (length > 0 && birdnet_base_url[length - 1] == '/') {
        birdnet_base_url[--length] = '\0';
    }
    ESP_LOGI(TAG, "BirdNET server: %s", birdnet_base_url);
    return have_wifi;
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() != NULL ? ESP_OK : ESP_FAIL);

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    /* An open network has no password and would fail a WPA2 threshold. */
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = wifi_password[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };
    memcpy(wifi_config.sta.ssid, wifi_ssid, strnlen(wifi_ssid, sizeof(wifi_config.sta.ssid)));
    memcpy(wifi_config.sta.password, wifi_password,
           strnlen(wifi_password, sizeof(wifi_config.sta.password) - 1));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_start());
}

/* Refreshes every QUERY_INTERVAL_US. A tap on the photo shows the bird's
   details, and a tap on those (or INFO_TIMEOUT_US) goes back to the photo;
   a tap with no photo up refreshes at once. */
static void birdnet_task(void *argument)
{
    (void)argument;
    app_task_handle = xTaskGetCurrentTaskHandle();
    int64_t next_query_time = 0;
    int64_t info_until = 0;
    bool refresh_requested = true;
    while (true) {
        update_status_screen();
        int64_t now = esp_timer_get_time();
        if (info_on_screen && now >= info_until) {
            leave_bird_info();
            next_query_time = esp_timer_get_time() + QUERY_INTERVAL_US;
        } else if (!info_on_screen && wifi_connected &&
                   (refresh_requested || next_query_time == 0 || now >= next_query_time)) {
            refresh_requested = false;
            refresh_birdnet();
            next_query_time = esp_timer_get_time() + QUERY_INTERVAL_US;
        }

        now = esp_timer_get_time();
        int64_t remaining_us = next_query_time > now ? next_query_time - now : 1000000;
        TickType_t wait_ticks = pdMS_TO_TICKS((uint32_t)(remaining_us / 1000));
        if (wait_ticks == 0 || wait_ticks > pdMS_TO_TICKS(1000)) {
            wait_ticks = pdMS_TO_TICKS(1000);
        }
        uint32_t events = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &events, wait_ticks) != pdTRUE) {
            continue;
        }
        if (events & WIFI_JOINED_BIT) {
            refresh_requested = true;
        }
        if (events & TAP_BIT) {
            if (info_on_screen) {
                leave_bird_info();
                next_query_time = esp_timer_get_time() + QUERY_INTERVAL_US;
            } else if (displayed_image_ready && wifi_connected) {
                show_bird_info();
                info_until = esp_timer_get_time() + INFO_TIMEOUT_US;
            } else {
                refresh_requested = true;
            }
            /* Taps made while the screen was busy are dropped. */
            ulTaskNotifyValueClear(NULL, TAP_BIT);
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(cyd_display_init());
    note_start();
    if (!load_config()) {
        ESP_LOGE(TAG, "No Wi-Fi settings in NVS; start the launcher once to store them");
        ESP_ERROR_CHECK(cyd_text_row(3, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, "NO WI-FI SETTINGS"));
        ESP_ERROR_CHECK(cyd_text_row(5, CYD_COLOR_WHITE, CYD_COLOR_BLACK, "Press EN to run"));
        ESP_ERROR_CHECK(cyd_text_row(6, CYD_COLOR_WHITE, CYD_COLOR_BLACK, "the launcher once"));
        return;
    }

    QueueHandle_t touch_queue;
    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));
    BaseType_t touch_task_result =
        xTaskCreate(touch_task, "touch", 3072, touch_queue, 5, NULL);
    ESP_ERROR_CHECK(touch_task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t birdnet_task_result =
        xTaskCreate(birdnet_task, "birdnet", 8192, NULL, 5, &app_task_handle);
    ESP_ERROR_CHECK(birdnet_task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_init();
}
