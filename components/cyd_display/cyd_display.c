#include "cyd_display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define LCD_HOST SPI2_HOST
#define LCD_PIN_MOSI GPIO_NUM_13
#define LCD_PIN_SCLK GPIO_NUM_14
#define LCD_PIN_CS GPIO_NUM_15
#define LCD_PIN_DC GPIO_NUM_2
#define LCD_PIN_BACKLIGHT GPIO_NUM_21

/* The backlight is dimmed with PWM. */
#define BACKLIGHT_SPEED_MODE LEDC_LOW_SPEED_MODE
#define BACKLIGHT_TIMER LEDC_TIMER_0
#define BACKLIGHT_CHANNEL LEDC_CHANNEL_0
#define BACKLIGHT_RESOLUTION LEDC_TIMER_10_BIT
#define BACKLIGHT_MAX_DUTY ((1 << 10) - 1)
#define BACKLIGHT_FREQUENCY_HZ 5000

#define TOUCH_HOST SPI3_HOST
#define TOUCH_PIN_MOSI GPIO_NUM_32
#define TOUCH_PIN_MISO GPIO_NUM_39
#define TOUCH_PIN_SCLK GPIO_NUM_25
#define TOUCH_PIN_CS GPIO_NUM_33
#define TOUCH_PIN_IRQ GPIO_NUM_36

/* XPT2046 calibration for the landscape orientation used here. */
#define TOUCH_RAW_X_MIN 578
#define TOUCH_RAW_X_MAX 3672
#define TOUCH_RAW_Y_MIN 513
#define TOUCH_RAW_Y_MAX 3600
#define TOUCH_DEBOUNCE_MS 250

/* A run of full-height display columns of one text line. */
#define TEXT_CHUNK_COLUMNS 40

static const char *TAG = "cyd_display";
static spi_device_handle_t lcd_spi;
static spi_device_handle_t touch_spi;
static QueueHandle_t touch_queue;
static int brightness = CYD_BRIGHTNESS_MAX;
static uint8_t text_chunk[TEXT_CHUNK_COLUMNS * CYD_FONT_HEIGHT * 2];

/* Classic 5x7 ASCII font, 0x20-0x7E, then the up, down and backspace
   arrows; one byte per column, LSB at the top. */
static const uint8_t font5x7[98][5] = {
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
    {0x08, 0x1C, 0x3E, 0x08, 0x08},
};

static const uint8_t *glyph_for(char character)
{
    unsigned char code = (unsigned char)character;
    if (character == CYD_GLYPH_UP) {
        return font5x7[95];
    }
    if (character == CYD_GLYPH_DOWN) {
        return font5x7[96];
    }
    if (character == CYD_GLYPH_BACKSPACE) {
        return font5x7[97];
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

esp_err_t cyd_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    ESP_RETURN_ON_FALSE(x >= 0 && y >= 0 && width > 0 && height > 0 &&
                        x + width <= CYD_LCD_WIDTH && y + height <= CYD_LCD_HEIGHT,
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

esp_err_t cyd_draw_pattern(int x, int y, int width, int height,
                           cyd_pixel_fn pixel, void *context)
{
    ESP_RETURN_ON_FALSE(x >= 0 && y >= 0 && width > 0 && height > 0 &&
                        x + width <= CYD_LCD_WIDTH && y + height <= CYD_LCD_HEIGHT,
                        ESP_ERR_INVALID_ARG, TAG, "pattern outside display");
    ESP_RETURN_ON_ERROR(lcd_set_window(x, y, width, height), TAG, "set pattern window");
    /* Column-major, like text: whole columns are packed into text_chunk. */
    size_t columns_per_chunk = sizeof(text_chunk) / ((size_t)height * 2);
    size_t used = 0;
    for (int column = 0; column < width; ++column) {
        for (int row = 0; row < height; ++row) {
            uint16_t color = pixel(x + column, y + row, context);
            text_chunk[used++] = (uint8_t)(color >> 8);
            text_chunk[used++] = (uint8_t)color;
        }
        if ((column + 1) % columns_per_chunk == 0 || column == width - 1) {
            ESP_RETURN_ON_ERROR(lcd_data(text_chunk, used), TAG, "send pattern chunk");
            used = 0;
        }
    }
    return ESP_OK;
}

esp_err_t cyd_draw_cells(int row, const char *text,
                         const uint16_t *foreground, const uint16_t *background)
{
    ESP_RETURN_ON_FALSE(row >= 0 && row < CYD_TEXT_ROWS, ESP_ERR_INVALID_ARG, TAG,
                        "text row outside display");
    ESP_RETURN_ON_ERROR(lcd_set_window(0, row * CYD_FONT_HEIGHT, CYD_LCD_WIDTH,
                                       CYD_FONT_HEIGHT),
                        TAG, "set text window");
    size_t length = strnlen(text, CYD_TEXT_COLUMNS);
    /* The panel fills a window column by column (top to bottom, then left to
       right), so pixels are sent column-major, a chunk of columns at a time. */
    for (int chunk_x = 0; chunk_x < CYD_LCD_WIDTH; chunk_x += TEXT_CHUNK_COLUMNS) {
        for (int dx = 0; dx < TEXT_CHUNK_COLUMNS; ++dx) {
            int x = chunk_x + dx;
            size_t cell = (size_t)x / CYD_FONT_WIDTH;
            int glyph_x = (x % CYD_FONT_WIDTH) / CYD_FONT_SCALE;
            /* The 14 pixels right of the last full cell take its colors. */
            size_t color_cell = cell < CYD_TEXT_COLUMNS ? cell : CYD_TEXT_COLUMNS - 1;
            uint8_t glyph_bits = cell < length && glyph_x < 5
                                     ? glyph_for(text[cell])[glyph_x]
                                     : 0;
            for (int y = 0; y < CYD_FONT_HEIGHT; ++y) {
                uint16_t color = background[color_cell];
                int glyph_y = (y - 1) / CYD_FONT_SCALE;
                if (y == CYD_FONT_HEIGHT - 1) {
                    color = CYD_COLOR_BLACK;
                } else if (y >= 1 && glyph_y < 7 && (glyph_bits & (1U << glyph_y))) {
                    color = foreground[color_cell];
                }
                size_t index = ((size_t)dx * CYD_FONT_HEIGHT + y) * 2;
                text_chunk[index] = (uint8_t)(color >> 8);
                text_chunk[index + 1] = (uint8_t)color;
            }
        }
        ESP_RETURN_ON_ERROR(lcd_data(text_chunk, sizeof(text_chunk)), TAG, "send text chunk");
    }
    return ESP_OK;
}

esp_err_t cyd_text_row(int row, uint16_t foreground, uint16_t background, const char *text)
{
    uint16_t foregrounds[CYD_TEXT_COLUMNS];
    uint16_t backgrounds[CYD_TEXT_COLUMNS];
    for (int i = 0; i < CYD_TEXT_COLUMNS; ++i) {
        foregrounds[i] = foreground;
        backgrounds[i] = background;
    }
    return cyd_draw_cells(row, text, foregrounds, backgrounds);
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

esp_err_t cyd_set_brightness(int percent)
{
    if (percent < CYD_BRIGHTNESS_MIN) {
        percent = CYD_BRIGHTNESS_MIN;
    }
    if (percent > CYD_BRIGHTNESS_MAX) {
        percent = CYD_BRIGHTNESS_MAX;
    }
    brightness = percent;
    ESP_RETURN_ON_ERROR(ledc_set_duty(BACKLIGHT_SPEED_MODE, BACKLIGHT_CHANNEL,
                                      (uint32_t)percent * BACKLIGHT_MAX_DUTY / 100),
                        TAG, "set backlight duty");
    return ledc_update_duty(BACKLIGHT_SPEED_MODE, BACKLIGHT_CHANNEL);
}

int cyd_get_brightness(void)
{
    return brightness;
}

/* The level the launcher saved, or full brightness when there is none. */
static int saved_brightness(void)
{
    nvs_handle_t nvs;
    uint8_t percent = CYD_BRIGHTNESS_MAX;
    /* Safe if the app initializes NVS again later. */
    if (nvs_flash_init() == ESP_OK &&
        nvs_open(CYD_CONFIG_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, CYD_CONFIG_BRIGHTNESS, &percent);
        nvs_close(nvs);
    }
    return percent;
}

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = BACKLIGHT_SPEED_MODE,
        .duty_resolution = BACKLIGHT_RESOLUTION,
        .timer_num = BACKLIGHT_TIMER,
        .freq_hz = BACKLIGHT_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "configure backlight timer");
    ledc_channel_config_t channel = {
        .gpio_num = LCD_PIN_BACKLIGHT,
        .speed_mode = BACKLIGHT_SPEED_MODE,
        .channel = BACKLIGHT_CHANNEL,
        .timer_sel = BACKLIGHT_TIMER,
        .duty = 0,
    };
    return ledc_channel_config(&channel);
}

esp_err_t cyd_display_init(void)
{
    gpio_config_t display_gpio = {
        .pin_bit_mask = 1ULL << LCD_PIN_DC,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&display_gpio), TAG, "configure display GPIO");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "initialize backlight");

    spi_bus_config_t display_bus = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(text_chunk),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &display_bus, SPI_DMA_CH_AUTO),
                        TAG, "initialize display SPI bus");

    spi_device_interface_config_t display_device = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = LCD_PIN_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_HOST, &display_device, &lcd_spi),
                        TAG, "add display SPI device");
    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "initialize LCD");
    ESP_RETURN_ON_ERROR(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK),
                        TAG, "clear display");
    return cyd_set_brightness(saved_brightness());
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

static esp_err_t touch_read_point(cyd_touch_point_t *point)
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
                                       CYD_LCD_WIDTH - 1);
    point->y = (int16_t)map_touch_axis(raw_y_sum / 5, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX,
                                       CYD_LCD_HEIGHT - 1);
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
            cyd_touch_point_t point;
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

esp_err_t cyd_touch_start(QueueHandle_t *queue)
{
    touch_queue = xQueueCreate(4, sizeof(cyd_touch_point_t));
    ESP_RETURN_ON_FALSE(touch_queue != NULL, ESP_ERR_NO_MEM, TAG, "create touch queue");
    ESP_RETURN_ON_ERROR(touch_bus_init(), TAG, "initialize touchscreen");
    ESP_RETURN_ON_FALSE(xTaskCreate(touchscreen_task, "touchscreen", 3072, NULL, 5, NULL) ==
                            pdPASS,
                        ESP_ERR_NO_MEM, TAG, "create touchscreen task");
    *queue = touch_queue;
    return ESP_OK;
}
