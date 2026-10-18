#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

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
#define FONT_SCALE 3
#define FONT_WIDTH (6 * FONT_SCALE)
#define FONT_HEIGHT (8 * FONT_SCALE)
#define TEXT_COLUMNS (LCD_WIDTH / FONT_WIDTH)
#define TEXT_ROWS (LCD_HEIGHT / FONT_HEIGHT)

#define MAX_WIFI_RESULTS 24
#define MAX_BLE_RESULTS 32
#define BLE_SCAN_DURATION_MS 6000

#define BLE_SYNCED_BIT BIT0
#define BLE_SCAN_DONE_BIT BIT1

#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF
#define COLOR_GRAY 0x8410
#define COLOR_YELLOW 0xFFE0
#define COLOR_CYAN 0x07FF
#define COLOR_BUTTON 0x2945
#define COLOR_BUTTON_DISABLED 0x4A69

/* XPT2046 calibration for the landscape orientation used here. */
#define TOUCH_RAW_X_MIN 578
#define TOUCH_RAW_X_MAX 3672
#define TOUCH_RAW_Y_MIN 513
#define TOUCH_RAW_Y_MAX 3600
#define TOUCH_DEBOUNCE_MS 250

/* List header layout: title (tap to rescan), then up and down arrow buttons. */
#define HEADER_TITLE_COLUMNS 10
#define ARROW_UP_COLUMN 10
#define ARROW_DOWN_COLUMN 14
#define ARROW_COLUMNS 3

/* Extra glyphs past the ASCII range. */
#define GLYPH_UP '\x01'
#define GLYPH_DOWN '\x02'

static const char *TAG = "cyd_scanner";
static spi_device_handle_t lcd_spi;
static spi_device_handle_t touch_spi;
static QueueHandle_t touch_queue;
static EventGroupHandle_t ble_events;
static uint8_t ble_own_addr_type;
/* A run of full-height display columns of one text line. */
#define TEXT_CHUNK_COLUMNS 40
static uint8_t text_chunk[TEXT_CHUNK_COLUMNS * FONT_HEIGHT * 2];

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

typedef struct {
    int16_t x;
    int16_t y;
} touch_point_t;

enum { LIST_WIFI, LIST_BLE, LIST_COUNT };

typedef struct {
    int header_row;
    int rows;   /* visible entry rows below the header */
    int offset; /* index of the first visible entry */
} list_view_t;

/* A screen of text rows; the last row is reserved for a footer. */
typedef struct {
    char text[TEXT_ROWS][TEXT_COLUMNS + 1];
    uint16_t color[TEXT_ROWS];
    int count;
} page_t;

static wifi_ap_record_t wifi_results[MAX_WIFI_RESULTS];
static uint16_t wifi_result_count;
static uint16_t wifi_total_count;
static ble_device_t ble_results[MAX_BLE_RESULTS];
static int ble_result_count;
static bool ble_results_full;
static list_view_t list_views[LIST_COUNT];

/* Classic 5x7 ASCII font, 0x20-0x7E, then the up and down arrows; one byte
   per column, LSB at the top. */
static const uint8_t font5x7[97][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00},
    {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00},
    {0x08, 0x2A, 0x1C, 0x2A, 0x08}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08},
    {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
    {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3E}, {0x7E, 0x11, 0x11, 0x11, 0x7E},
    {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41},
    {0x7F, 0x09, 0x09, 0x01, 0x01}, {0x3E, 0x41, 0x41, 0x51, 0x32},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x04, 0x02, 0x7F},
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E},
    {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x7F, 0x20, 0x18, 0x20, 0x7F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x03, 0x04, 0x78, 0x04, 0x03},
    {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x00, 0x7F, 0x41, 0x41},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x41, 0x41, 0x7F, 0x00, 0x00},
    {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7F}, {0x38, 0x54, 0x54, 0x54, 0x18},
    {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x08, 0x14, 0x54, 0x54, 0x3C},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00},
    {0x20, 0x40, 0x44, 0x3D, 0x00}, {0x00, 0x7F, 0x10, 0x28, 0x44},
    {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7C, 0x14, 0x14, 0x14, 0x08}, {0x08, 0x14, 0x14, 0x18, 0x7C},
    {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C},
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x7F, 0x00, 0x00}, {0x00, 0x41, 0x36, 0x08, 0x00},
    {0x02, 0x01, 0x02, 0x04, 0x02},
    {0x20, 0x38, 0x3E, 0x38, 0x20}, {0x02, 0x0E, 0x3E, 0x0E, 0x02},
};

static const uint8_t *glyph_for(char character)
{
    unsigned char code = (unsigned char)character;
    if (character == GLYPH_UP) {
        return font5x7[95];
    }
    if (character == GLYPH_DOWN) {
        return font5x7[96];
    }
    if (code < 0x20 || code > 0x7E) {
        code = '?';
    }
    return font5x7[code - 0x20];
}

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

/* Draws one full-width row of character cells, each with its own colors.
   The bottom pixel row is always black so stacked buttons stay separate. */
static esp_err_t lcd_draw_cells(int row, const char *text,
                                const uint16_t *foreground, const uint16_t *background)
{
    ESP_RETURN_ON_FALSE(row >= 0 && row < TEXT_ROWS, ESP_ERR_INVALID_ARG, TAG,
                        "text row outside display");
    ESP_RETURN_ON_ERROR(lcd_set_window(0, row * FONT_HEIGHT, LCD_WIDTH, FONT_HEIGHT),
                        TAG, "set text window");
    size_t length = strnlen(text, TEXT_COLUMNS);
    /* The panel fills a window column by column (top to bottom, then left to
       right), so pixels are sent column-major, a chunk of columns at a time. */
    for (int chunk_x = 0; chunk_x < LCD_WIDTH; chunk_x += TEXT_CHUNK_COLUMNS) {
        for (int dx = 0; dx < TEXT_CHUNK_COLUMNS; ++dx) {
            int x = chunk_x + dx;
            size_t cell = (size_t)x / FONT_WIDTH;
            int glyph_x = (x % FONT_WIDTH) / FONT_SCALE;
            /* The 14 pixels right of the last full cell take its colors. */
            size_t color_cell = cell < TEXT_COLUMNS ? cell : TEXT_COLUMNS - 1;
            uint8_t glyph_bits = cell < length && glyph_x < 5
                                     ? glyph_for(text[cell])[glyph_x]
                                     : 0;
            for (int y = 0; y < FONT_HEIGHT; ++y) {
                uint16_t color = background[color_cell];
                int glyph_y = (y - 1) / FONT_SCALE;
                if (y == FONT_HEIGHT - 1) {
                    color = COLOR_BLACK;
                } else if (y >= 1 && glyph_y < 7 && (glyph_bits & (1U << glyph_y))) {
                    color = foreground[color_cell];
                }
                size_t index = ((size_t)dx * FONT_HEIGHT + y) * 2;
                text_chunk[index] = (uint8_t)(color >> 8);
                text_chunk[index + 1] = (uint8_t)color;
            }
        }
        ESP_RETURN_ON_ERROR(lcd_data(text_chunk, sizeof(text_chunk)), TAG, "send text chunk");
    }
    return ESP_OK;
}

static esp_err_t lcd_text_row(int row, uint16_t foreground, uint16_t background,
                              const char *text)
{
    uint16_t foregrounds[TEXT_COLUMNS];
    uint16_t backgrounds[TEXT_COLUMNS];
    for (int i = 0; i < TEXT_COLUMNS; ++i) {
        foregrounds[i] = foreground;
        backgrounds[i] = background;
    }
    return lcd_draw_cells(row, text, foregrounds, backgrounds);
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
        {0x36, {0x20}, 1}, /* MV only; with BGR set, red and blue were swapped */
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

static esp_err_t touch_read_point(touch_point_t *point)
{
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
    point->x = (int16_t)map_touch_axis(raw_x_sum / 5, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX,
                                       LCD_WIDTH - 1);
    point->y = (int16_t)map_touch_axis(raw_y_sum / 5, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX,
                                       LCD_HEIGHT - 1);
    return ESP_OK;
}

static void touchscreen_task(void *argument)
{
    (void)argument;
    bool was_pressed = false;
    TickType_t last_tap = 0;
    while (true) {
        /* Each new press becomes one tap, debounced against panel chatter. */
        bool pressed = gpio_get_level(TOUCH_PIN_IRQ) == 0;
        if (pressed && !was_pressed &&
            xTaskGetTickCount() - last_tap >= pdMS_TO_TICKS(TOUCH_DEBOUNCE_MS)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            touch_point_t point;
            esp_err_t result = touch_read_point(&point);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "Touchscreen read failed: %s", esp_err_to_name(result));
            } else if (gpio_get_level(TOUCH_PIN_IRQ) == 0) {
                ESP_LOGI(TAG, "Tap at %d,%d", point.x, point.y);
                xQueueSend(touch_queue, &point, 0);
                last_tap = xTaskGetTickCount();
            }
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

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
    ESP_ERROR_CHECK(lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_BLACK));
    ESP_ERROR_CHECK(lcd_text_row(TEXT_ROWS / 2, COLOR_YELLOW, COLOR_BLACK, message));
}

static void page_add(page_t *page, uint16_t color, const char *format, ...)
{
    if (page->count >= TEXT_ROWS - 1) {
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
        page_add(page, color, "%.*s", TEXT_COLUMNS, text);
        size_t step = length < TEXT_COLUMNS ? length : TEXT_COLUMNS;
        text += step;
        length -= step;
    }
}

static void page_show(const page_t *page, const char *footer)
{
    for (int row = 0; row < TEXT_ROWS; ++row) {
        if (row < page->count) {
            ESP_ERROR_CHECK(lcd_text_row(row, page->color[row], COLOR_BLACK, page->text[row]));
        } else if (row == TEXT_ROWS - 1) {
            ESP_ERROR_CHECK(lcd_text_row(row, COLOR_GRAY, COLOR_BLACK, footer));
        } else {
            ESP_ERROR_CHECK(lcd_text_row(row, COLOR_WHITE, COLOR_BLACK, ""));
        }
    }
}

static void show_wifi_detail(const wifi_ap_record_t *ap)
{
    page_t page = {0};
    char phy[24];
    wifi_phy_text(ap, phy, sizeof(phy));
    page_add(&page, COLOR_YELLOW, "WI-FI NETWORK");
    page_add_wrapped(&page, COLOR_WHITE,
                     ap->ssid[0] != '\0' ? (const char *)ap->ssid : "(hidden)", 2);
    page_add(&page, COLOR_WHITE, "RSSI %d dBm", ap->rssi);
    page_add(&page, COLOR_WHITE, "CHANNEL %u", ap->primary);
    page_add(&page, COLOR_WHITE, "AUTH %s", wifi_auth_name(ap->authmode));
    page_add(&page, COLOR_WHITE, "CIPHER %s", wifi_cipher_name(ap->pairwise_cipher));
    page_add(&page, COLOR_WHITE, MACSTR, MAC2STR(ap->bssid));
    page_add(&page, COLOR_WHITE, "802.11%s%s", phy, ap->wps ? " WPS" : "");
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
    page_add(&page, COLOR_CYAN, "BLE DEVICE");
    if (device->name[0] != '\0') {
        page_add_wrapped(&page, COLOR_WHITE, device->name, 2);
    } else {
        page_add(&page, COLOR_GRAY, "(no name)");
    }
    page_add(&page, COLOR_WHITE, "RSSI %d dBm", device->rssi);
    page_add(&page, COLOR_WHITE, "%02x:%02x:%02x:%02x:%02x:%02x",
             device->addr[5], device->addr[4], device->addr[3],
             device->addr[2], device->addr[1], device->addr[0]);
    page_add(&page, COLOR_WHITE, "%s", ble_address_kind(device));
    page_add(&page, COLOR_WHITE, "%s", ble_advertising_kind(device->adv_type));
    if (device->has_tx_power) {
        page_add(&page, COLOR_WHITE, "TX POWER %d dBm", device->tx_power);
    }
    if (device->has_company) {
        const char *company = ble_company_name(device->company_id);
        if (company != NULL) {
            page_add(&page, COLOR_WHITE, "MFR %s", company);
        } else {
            page_add(&page, COLOR_WHITE, "MFR 0x%04X", device->company_id);
        }
    }
    if (device->has_service) {
        page_add(&page, COLOR_WHITE, "SERVICE 0x%04X", device->service_uuid16);
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
    int list_rows = TEXT_ROWS - 2;
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
    char text[TEXT_COLUMNS + 1];
    char title[24];
    uint16_t foreground[TEXT_COLUMNS];
    uint16_t background[TEXT_COLUMNS];
    uint16_t title_color = list == LIST_WIFI ? COLOR_YELLOW : COLOR_CYAN;
    bool can_scroll_up = view->offset > 0;
    bool can_scroll_down = view->offset + view->rows < list_count(list);

    if (list == LIST_WIFI) {
        snprintf(title, sizeof(title), "WI-FI %u", wifi_total_count);
    } else {
        snprintf(title, sizeof(title), "BLE %d%s", ble_result_count,
                 ble_results_full ? "+" : "");
    }
    memset(text, ' ', TEXT_COLUMNS);
    text[TEXT_COLUMNS] = '\0';
    memcpy(text, title, strnlen(title, HEADER_TITLE_COLUMNS - 1));
    for (int i = 0; i < TEXT_COLUMNS; ++i) {
        foreground[i] = title_color;
        background[i] = COLOR_BLACK;
    }
    for (int i = 0; i < ARROW_COLUMNS; ++i) {
        background[ARROW_UP_COLUMN + i] = COLOR_BUTTON;
        foreground[ARROW_UP_COLUMN + i] = can_scroll_up ? COLOR_WHITE : COLOR_BUTTON_DISABLED;
        background[ARROW_DOWN_COLUMN + i] = COLOR_BUTTON;
        foreground[ARROW_DOWN_COLUMN + i] =
            can_scroll_down ? COLOR_WHITE : COLOR_BUTTON_DISABLED;
    }
    text[ARROW_UP_COLUMN + 1] = GLYPH_UP;
    text[ARROW_DOWN_COLUMN + 1] = GLYPH_DOWN;
    ESP_ERROR_CHECK(lcd_draw_cells(view->header_row, text, foreground, background));
}

static void draw_list_entry(int list, int index, int row, bool highlighted)
{
    /* 17 columns: "-dBm" plus a 12-character SSID or name. */
    char line[TEXT_COLUMNS + 1];
    uint16_t color = COLOR_WHITE;
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
            color = COLOR_GRAY;
        }
    }
    if (highlighted) {
        ESP_ERROR_CHECK(lcd_text_row(row, COLOR_BLACK, COLOR_WHITE, line));
    } else {
        ESP_ERROR_CHECK(lcd_text_row(row, color, COLOR_BUTTON, line));
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
    for (int row = ble_view->header_row + 1 + ble_view->rows; row < TEXT_ROWS; ++row) {
        ESP_ERROR_CHECK(lcd_text_row(row, COLOR_WHITE, COLOR_BLACK, ""));
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
    touch_point_t point;
    xQueueReset(touch_queue);
    xQueueReceive(touch_queue, &point, portMAX_DELAY);
    show_results();
}

/* Returns true when the tap asks for a new scan. */
static bool handle_list_tap(const touch_point_t *point)
{
    int row = point->y / FONT_HEIGHT;
    int column = point->x / FONT_WIDTH;
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
        touch_point_t point;
        do {
            xQueueReceive(touch_queue, &point, portMAX_DELAY);
        } while (!handle_list_tap(&point));
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
        .max_transfer_sz = sizeof(text_chunk),
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
    ESP_ERROR_CHECK(lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_BLACK));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BACKLIGHT, 1));

    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);
    wifi_init();
    ble_init();

    touch_queue = xQueueCreate(4, sizeof(touch_point_t));
    ESP_ERROR_CHECK(touch_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(touch_bus_init());
    BaseType_t touch_task_result =
        xTaskCreate(touchscreen_task, "touchscreen", 3072, NULL, 5, NULL);
    ESP_ERROR_CHECK(touch_task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t scan_task_result =
        xTaskCreate(scan_task, "scan", 6144, NULL, 5, NULL);
    ESP_ERROR_CHECK(scan_task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
