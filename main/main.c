#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "rom/tjpgd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "wifi_credentials.h"

#define LCD_HOST SPI2_HOST
#define LCD_PIN_MOSI GPIO_NUM_13
#define LCD_PIN_SCLK GPIO_NUM_14
#define LCD_PIN_CS GPIO_NUM_15
#define LCD_PIN_DC GPIO_NUM_2
#define LCD_PIN_BACKLIGHT GPIO_NUM_21

#define TOUCH_HOST SPI3_HOST
#define TOUCH_PIN_MOSI GPIO_NUM_32
#define TOUCH_PIN_MISO GPIO_NUM_39
#define TOUCH_PIN_SCLK GPIO_NUM_25
#define TOUCH_PIN_CS GPIO_NUM_33
#define TOUCH_PIN_IRQ GPIO_NUM_36

#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define QUERY_INTERVAL_US (120LL * 1000 * 1000)
#ifndef BIRDNET_BASE_URL
#define BIRDNET_BASE_URL "http://birdnet.local"
#endif
#define API_JSON_CAPACITY (16 * 1024)
#define IMAGE_JPEG_CAPACITY (320 * 1024)
#define JPEG_WORKBUF_SIZE 3100
#define IMAGE_TOP 0
#define IMAGE_HEIGHT LCD_HEIGHT
#define TOUCH_TRIGGER_BIT BIT0
#define MIN_BIRD_CONFIDENCE 0.80
#define COLOR_BLACK 0x0000

static const char *TAG = "birdnet_cyd";
static spi_device_handle_t lcd_spi;
static spi_device_handle_t touch_spi;
static volatile bool wifi_connected;
static TaskHandle_t app_task_handle;
static bool displayed_image_ready;
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
    char detected_time[16];
    double confidence;
    bool found;
} bird_detection_t;

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

static esp_err_t lcd_write(uint8_t dc_level, const uint8_t *data, size_t size)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_PIN_DC, dc_level), TAG, "set D/C pin");

    spi_transaction_t transaction = {
        .length = size * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(lcd_spi, &transaction);
}

static esp_err_t lcd_command(uint8_t command)
{
    return lcd_write(0, &command, sizeof(command));
}

static esp_err_t lcd_data(const uint8_t *data, size_t size)
{
    return lcd_write(1, data, size);
}

static esp_err_t lcd_set_window(int x, int y, int width, int height)
{
    uint8_t columns[] = {(uint8_t)(y >> 8), (uint8_t)y,
                         (uint8_t)((y + height - 1) >> 8), (uint8_t)(y + height - 1)};
    uint8_t pages[] = {(uint8_t)(x >> 8), (uint8_t)x,
                       (uint8_t)((x + width - 1) >> 8), (uint8_t)(x + width - 1)};

    ESP_RETURN_ON_ERROR(lcd_command(0x2A), TAG, "set column address");
    ESP_RETURN_ON_ERROR(lcd_data(columns, sizeof(columns)), TAG, "set column range");
    ESP_RETURN_ON_ERROR(lcd_command(0x2B), TAG, "set page address");
    ESP_RETURN_ON_ERROR(lcd_data(pages, sizeof(pages)), TAG, "set page range");
    return lcd_command(0x2C);
}

static esp_err_t lcd_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    ESP_RETURN_ON_FALSE(x >= 0 && y >= 0 && width > 0 && height > 0 &&
                        x + width <= LCD_WIDTH && y + height <= LCD_HEIGHT,
                        ESP_ERR_INVALID_ARG, TAG, "rectangle outside display");
    ESP_RETURN_ON_ERROR(lcd_set_window(x, y, width, height), TAG, "set fill window");

    uint8_t *line = heap_caps_malloc(width * 2, MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(line != NULL, ESP_ERR_NO_MEM, TAG, "allocate fill line");
    for (int column = 0; column < width; ++column) {
        line[column * 2] = (uint8_t)(color >> 8);
        line[column * 2 + 1] = (uint8_t)color;
    }

    esp_err_t result = ESP_OK;
    for (int row = 0; row < height && result == ESP_OK; ++row) {
        result = lcd_data(line, width * 2);
    }
    heap_caps_free(line);
    return result;
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
    const char *url = BIRDNET_BASE_URL "/api/v1/detections/visits";
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
    char metadata_url[384];
    if (!url_encode_path(scientific_name, encoded_name, sizeof(encoded_name)) ||
        snprintf(metadata_url, sizeof(metadata_url),
                 BIRDNET_BASE_URL "/api/v1/image/%s", encoded_name) >=
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
    if (lcd_set_window(left, top, output_width, output_height) != ESP_OK ||
        lcd_data(output->rgb565, (size_t)output_width * output_height * 2) != ESP_OK) {
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
    if (lcd_fill_rect(0, IMAGE_TOP, LCD_WIDTH, IMAGE_HEIGHT, COLOR_BLACK) != ESP_OK) {
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

static esp_err_t lcd_init(void)
{
    static const struct {
        uint8_t command;
        uint8_t data[15];
        uint8_t size;
    } initialization[] = {
        {0xCF, {0x00, 0x83, 0x30}, 3},
        {0xED, {0x64, 0x03, 0x12, 0x81}, 4},
        {0xE8, {0x85, 0x01, 0x79}, 3},
        {0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5},
        {0xF7, {0x20}, 1},
        {0xEA, {0x00, 0x00}, 2},
        {0xC0, {0x26}, 1},
        {0xC1, {0x11}, 1},
        {0xC5, {0x35, 0x3E}, 2},
        {0xC7, {0xBE}, 1},
        {0x36, {0x28}, 1},
        {0x3A, {0x55}, 1},
        {0xB1, {0x00, 0x1B}, 2},
        {0xF2, {0x08}, 1},
        {0x26, {0x01}, 1},
        {0xE0, {0x1F, 0x1A, 0x18, 0x0A, 0x0F, 0x06, 0x45, 0x87,
                0x32, 0x0A, 0x07, 0x02, 0x07, 0x05, 0x00}, 15},
        {0xE1, {0x00, 0x25, 0x27, 0x05, 0x10, 0x09, 0x3A, 0x78,
                0x4D, 0x05, 0x18, 0x0D, 0x38, 0x3A, 0x1F}, 15},
    };

    ESP_RETURN_ON_ERROR(lcd_command(0x01), TAG, "software reset");
    esp_rom_delay_us(150000);
    for (size_t i = 0; i < sizeof(initialization) / sizeof(initialization[0]); ++i) {
        ESP_RETURN_ON_ERROR(lcd_command(initialization[i].command), TAG, "send init command");
        ESP_RETURN_ON_ERROR(lcd_data(initialization[i].data, initialization[i].size),
                            TAG, "send init data");
    }
    ESP_RETURN_ON_ERROR(lcd_command(0x11), TAG, "exit sleep mode");
    esp_rom_delay_us(120000);
    ESP_RETURN_ON_ERROR(lcd_command(0x29), TAG, "turn display on");
    esp_rom_delay_us(20000);
    return ESP_OK;
}

static esp_err_t touch_bus_init(void)
{
    gpio_config_t irq_config = {
        .pin_bit_mask = 1ULL << TOUCH_PIN_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&irq_config), TAG, "configure touch IRQ");

    spi_bus_config_t bus_config = {
        .mosi_io_num = TOUCH_PIN_MOSI,
        .miso_io_num = TOUCH_PIN_MISO,
        .sclk_io_num = TOUCH_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(TOUCH_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "initialize touchscreen SPI bus");

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = TOUCH_PIN_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(TOUCH_HOST, &device_config, &touch_spi),
                        TAG, "add touchscreen SPI device");
    return ESP_OK;
}

static esp_err_t touch_read_raw(uint8_t command, uint16_t *value)
{
    uint8_t tx[3] = {command, 0, 0};
    uint8_t rx[3] = {0};
    spi_transaction_t transaction = {
        .length = sizeof(tx) * 8,
        .rxlength = sizeof(rx) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    ESP_RETURN_ON_ERROR(spi_device_polling_transmit(touch_spi, &transaction),
                        TAG, "read touchscreen ADC");
    *value = (uint16_t)((((uint16_t)rx[1] << 8) | rx[2]) >> 3) & 0x0FFF;
    return ESP_OK;
}

static int map_touch_axis(uint16_t value, int raw_start, int raw_end, int pixel_end)
{
    int pixel = ((int)value - raw_start) * pixel_end / (raw_end - raw_start);
    if (pixel < 0) {
        return 0;
    }
    if (pixel > pixel_end) {
        return pixel_end;
    }
    return pixel;
}

static esp_err_t touch_read(int *x, int *y, bool *pressed,
                            uint16_t *raw_x_result, uint16_t *raw_y_result)
{
    *pressed = gpio_get_level(TOUCH_PIN_IRQ) == 0;
    if (!*pressed) {
        return ESP_OK;
    }

    uint32_t raw_x_sum = 0;
    uint32_t raw_y_sum = 0;
    for (int sample = 0; sample < 5; ++sample) {
        uint16_t raw_x;
        uint16_t raw_y;
        ESP_RETURN_ON_ERROR(touch_read_raw(0x90, &raw_x), TAG, "read touch X");
        ESP_RETURN_ON_ERROR(touch_read_raw(0xD0, &raw_y), TAG, "read touch Y");
        raw_x_sum += raw_x;
        raw_y_sum += raw_y;
    }

    uint16_t raw_x = raw_x_sum / 5;
    uint16_t raw_y = raw_y_sum / 5;
    *raw_x_result = raw_x;
    *raw_y_result = raw_y;
    *x = map_touch_axis(raw_x, 578, 3672, LCD_WIDTH - 1);
    *y = map_touch_axis(raw_y, 513, 3600, LCD_HEIGHT - 1);
    return ESP_OK;
}

static void refresh_birdnet(void)
{
    bird_detection_t bird;
    esp_err_t result = fetch_latest_bird(&bird);
    if (result != ESP_OK) {
        return;
    }

    if (!bird.found) {
        displayed_species[0] = '\0';
        displayed_image_ready = false;
        ESP_ERROR_CHECK(lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_BLACK));
        return;
    }

    bool species_changed = strcmp(displayed_species, bird.scientific_name) != 0;
    bool need_image = species_changed || !displayed_image_ready;
    char image_url[768];
    size_t jpeg_length = 0;
    esp_http_client_handle_t image_client = NULL;
    esp_err_t image_result = ESP_OK;
    if (need_image) {
        image_result = fetch_bird_image(bird.scientific_name, &jpeg_length,
                                        image_url, sizeof(image_url), &image_client);
        if (image_result != ESP_OK) {
            ESP_LOGW(TAG, "Bird image unavailable");
        }
    }

    if (species_changed) {
        displayed_image_ready = false;
    }
    if (image_result == ESP_OK && need_image) {
        image_result = draw_jpeg(image_client, jpeg_length);
        esp_http_client_close(image_client);
        esp_http_client_cleanup(image_client);
        if (image_result == ESP_OK) {
            displayed_image_ready = true;
        } else {
            ESP_LOGW(TAG, "Could not render bird image: %s", esp_err_to_name(image_result));
        }
    }
    if (need_image && image_result != ESP_OK) {
        ESP_ERROR_CHECK(lcd_fill_rect(0, IMAGE_TOP, LCD_WIDTH, IMAGE_HEIGHT, COLOR_BLACK));
    }
    if (!displayed_image_ready) {
        ESP_LOGW(TAG, "No bird image is displayed");
    }
    snprintf(displayed_species, sizeof(displayed_species), "%s", bird.scientific_name);
}

static void touchscreen_task(void *argument)
{
    (void)argument;
    bool was_pressed = false;
    while (true) {
        int x = 0;
        int y = 0;
        uint16_t raw_x = 0;
        uint16_t raw_y = 0;
        bool pressed = false;
        esp_err_t result = touch_read(&x, &y, &pressed, &raw_x, &raw_y);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Touchscreen read failed: %s", esp_err_to_name(result));
        } else if (pressed && !was_pressed) {
            ESP_LOGI(TAG, "Touch at %d,%d (raw %u,%u); refreshing BirdNET",
                     x, y, raw_x, raw_y);
            if (app_task_handle != NULL) {
                xTaskNotifyGive(app_task_handle);
            }
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        ESP_LOGW(TAG, "Wi-Fi disconnected; retrying");
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        wifi_connected = true;
        ESP_LOGI(TAG, "Wi-Fi connected; IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        if (app_task_handle != NULL) {
            xTaskNotifyGive(app_task_handle);
        }
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() != NULL ? ESP_OK : ESP_FAIL);

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(strlen(WIFI_SSID) <= sizeof(wifi_config.sta.ssid) &&
                    strlen(WIFI_PASSWORD) < sizeof(wifi_config.sta.password)
                    ? ESP_OK : ESP_ERR_INVALID_SIZE);
    memcpy(wifi_config.sta.ssid, WIFI_SSID, strlen(WIFI_SSID));
    memcpy(wifi_config.sta.password, WIFI_PASSWORD, strlen(WIFI_PASSWORD));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_start());
}

static void birdnet_task(void *argument)
{
    (void)argument;
    app_task_handle = xTaskGetCurrentTaskHandle();
    int64_t next_query_time = 0;
    bool refresh_requested = true;
    while (true) {
        int64_t now = esp_timer_get_time();
        if (wifi_connected &&
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
        if (ulTaskNotifyTake(pdTRUE, wait_ticks) > 0) {
            refresh_requested = true;
        }
    }
}

void app_main(void)
{
    gpio_config_t display_gpio = {
        .pin_bit_mask = (1ULL << LCD_PIN_DC) | (1ULL << LCD_PIN_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&display_gpio));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BACKLIGHT, 0));

    spi_bus_config_t display_bus = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &display_bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t display_device = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = LCD_PIN_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &display_device, &lcd_spi));
    ESP_ERROR_CHECK(lcd_init());
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BACKLIGHT, 1));

    ESP_ERROR_CHECK(lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_BLACK));

    ESP_ERROR_CHECK(touch_bus_init());
    BaseType_t touch_task_result =
        xTaskCreate(touchscreen_task, "touchscreen", 3072, NULL, 5, NULL);
    ESP_ERROR_CHECK(touch_task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t birdnet_task_result =
        xTaskCreate(birdnet_task, "birdnet", 8192, NULL, 5, &app_task_handle);
    ESP_ERROR_CHECK(birdnet_task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_init();
}
