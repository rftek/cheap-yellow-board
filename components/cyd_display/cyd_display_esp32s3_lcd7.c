/* The Waveshare ESP32-S3-Touch-LCD-7 backend of cyd_display: an 800x480
   RGB panel with its frame buffer in PSRAM, GT911 capacitive touch, and a
   CH422G I2C expander that switches the backlight and resets the touch
   controller. Pins, panel timings and the expander and touch reset
   sequence follow Waveshare's ESP-IDF demo (examples/ESP-IDF/09_lvgl_v9_demo
   in waveshareteam/ESP32-S3-Touch-LCD-7). */

#include "cyd_display.h"

#include "cyd_display_backend.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define LCD_PIXEL_CLOCK_HZ (16 * 1000 * 1000)
#define LCD_BOUNCE_LINES 10
#define LCD_PIN_VSYNC GPIO_NUM_3
#define LCD_PIN_HSYNC GPIO_NUM_46
#define LCD_PIN_DE GPIO_NUM_5
#define LCD_PIN_PCLK GPIO_NUM_7

/* The I2C bus is shared by the CH422G, the GT911 and the I2C connector. */
#define I2C_PIN_SDA GPIO_NUM_8
#define I2C_PIN_SCL GPIO_NUM_9
#define I2C_SPEED_HZ 400000
#define I2C_TIMEOUT_MS 100

/* CH422G: a byte written to the mode address makes EXIO0-7 outputs; the
   levels of all eight are then written as one byte to the output address. */
#define CH422G_MODE_ADDRESS 0x24
#define CH422G_MODE_OUTPUT 0x01
#define CH422G_OUTPUT_ADDRESS 0x38
#define EXIO_TOUCH_RESET (1 << 1)   /* low holds the GT911 in reset */
#define EXIO_BACKLIGHT (1 << 2)
#define EXIO3 (1 << 3)              /* undocumented; high, as in the demo */
#define EXIO_SD_CS (1 << 4)         /* high leaves the TF card deselected */
#define EXIO_CAN_SELECT (1 << 5)    /* high: the native USB port is wired to CAN */
/* Levels kept on every write. EXIO5 stays high, its pulled-up default, so
   driving the expander doesn't move the native USB port over from CAN. */
#define EXIO_KEEP (EXIO3 | EXIO_SD_CS | EXIO_CAN_SELECT)

/* GT911 at 0x5D: its INT pin is held low while it leaves reset. */
#define TOUCH_PIN_INT GPIO_NUM_4
#define GT911_ADDRESS 0x5D
#define GT911_STATUS 0x814E /* bit 7: new report; bits 0-3: touch count */
#define GT911_POINT1 0x814F /* track id, x, y (little-endian), size */
#define GT911_POINT_SIZE 8  /* bytes per finger in the report */
#define GT911_MAX_POINTS 5
#define TOUCH_POLL_MS 25
#define TOUCH_FAST_POLL_MS 5 /* the GT911 itself reports about every 10 ms */
#define TOUCH_DEBOUNCE_MS 250

/* Rows per esp_lcd_panel_draw_bitmap call in cyd_backend_fill_rect. */
#define FILL_ROWS 8

static const char *TAG = "cyd_display";
static esp_lcd_panel_handle_t panel;
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t expander_mode;
static i2c_master_dev_handle_t expander_output;
static i2c_master_dev_handle_t gt911;
static uint8_t expander_levels = EXIO_KEEP;
static QueueHandle_t touch_queue;
static int brightness = CYD_BRIGHTNESS_MAX;
static bool touch_down;
static cyd_touch_point_t touch_last;
static uint16_t fill_line[CYD_LCD_WIDTH * FILL_ROWS];

const char *cyd_board_id(void)
{
    return CYD_BOARD_ID;
}

bool cyd_touch_is_capacitive(void)
{
    return true;
}

int cyd_touch_max_points(void)
{
    return GT911_MAX_POINTS;
}

bool cyd_has_psram(void)
{
    return true;
}

esp_err_t cyd_backend_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    int rows = height < FILL_ROWS ? height : FILL_ROWS;
    for (int i = 0; i < width * rows; ++i) {
        fill_line[i] = color;
    }
    for (int row = 0; row < height; row += rows) {
        int count = height - row < rows ? height - row : rows;
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, x, y + row, x + width,
                                                      y + row + count, fill_line),
                            TAG, "fill rectangle");
    }
    return ESP_OK;
}

esp_err_t cyd_backend_blit(int x, int y, int width, int height, const uint16_t *pixels)
{
    /* Copies into the PSRAM frame buffer; the panel scans it out itself. */
    return esp_lcd_panel_draw_bitmap(panel, x, y, x + width, y + height, pixels);
}

static esp_err_t expander_write(uint8_t levels)
{
    expander_levels = levels;
    return i2c_master_transmit(expander_output, &levels, 1, I2C_TIMEOUT_MS);
}

static esp_err_t i2c_add(uint16_t address, i2c_master_dev_handle_t *device)
{
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = I2C_SPEED_HZ,
    };
    return i2c_master_bus_add_device(i2c_bus, &config, device);
}

/* Brings up the I2C bus, the expander (backlight off) and the GT911, once. */
static esp_err_t board_init(void)
{
    if (i2c_bus != NULL) {
        return ESP_OK;
    }
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_PIN_SDA,
        .scl_io_num = I2C_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &i2c_bus), TAG, "create I2C bus");
    ESP_RETURN_ON_ERROR(i2c_add(CH422G_MODE_ADDRESS, &expander_mode), TAG, "add CH422G");
    ESP_RETURN_ON_ERROR(i2c_add(CH422G_OUTPUT_ADDRESS, &expander_output), TAG, "add CH422G");
    uint8_t mode = CH422G_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(i2c_master_transmit(expander_mode, &mode, 1, I2C_TIMEOUT_MS), TAG,
                        "set CH422G to outputs");

    /* GT911 reset, as in the demo: INT low while reset is released selects
       address 0x5D. */
    gpio_config_t int_config = {
        .pin_bit_mask = 1ULL << TOUCH_PIN_INT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_config), TAG, "configure touch INT");
    ESP_RETURN_ON_ERROR(expander_write(EXIO_KEEP), TAG, "hold touch in reset");
    esp_rom_delay_us(100 * 1000);
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_PIN_INT, 0), TAG, "set touch INT low");
    esp_rom_delay_us(100 * 1000);
    ESP_RETURN_ON_ERROR(expander_write(EXIO_KEEP | EXIO_TOUCH_RESET), TAG,
                        "release touch reset");
    esp_rom_delay_us(200 * 1000);
    /* The GT911 drives INT from here on; it is polled, so leave it an input. */
    ESP_RETURN_ON_ERROR(gpio_set_direction(TOUCH_PIN_INT, GPIO_MODE_INPUT), TAG,
                        "release touch INT");
    return i2c_add(GT911_ADDRESS, &gt911);
}

esp_err_t cyd_sd_select(bool selected)
{
    ESP_RETURN_ON_FALSE(expander_output != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "cyd_display_init first");
    uint8_t levels = selected ? (uint8_t)(expander_levels & ~EXIO_SD_CS)
                              : (uint8_t)(expander_levels | EXIO_SD_CS);
    return expander_write(levels);
}

esp_err_t cyd_set_brightness(int percent)
{
    if (percent < CYD_BRIGHTNESS_MIN) {
        percent = CYD_BRIGHTNESS_MIN;
    }
    if (percent > CYD_BRIGHTNESS_MAX) {
        percent = CYD_BRIGHTNESS_MAX;
    }
    /* The backlight here is only switched on or off (EXIO2), so the level is
       kept for the launcher's setting but doesn't dim the panel. */
    brightness = percent;
    return ESP_OK;
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

esp_err_t cyd_display_init(void)
{
    ESP_RETURN_ON_ERROR(board_init(), TAG, "initialize expander and touch");

    esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PIXEL_CLOCK_HZ,
            .h_res = CYD_LCD_WIDTH,
            .v_res = CYD_LCD_HEIGHT,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .bounce_buffer_size_px = CYD_LCD_WIDTH * LCD_BOUNCE_LINES,
        .dma_burst_size = 64,
        .hsync_gpio_num = LCD_PIN_HSYNC,
        .vsync_gpio_num = LCD_PIN_VSYNC,
        .de_gpio_num = LCD_PIN_DE,
        .pclk_gpio_num = LCD_PIN_PCLK,
        .disp_gpio_num = GPIO_NUM_NC,
        /* B3-B7, G2-G7, R3-R7 */
        .data_gpio_nums = {
            GPIO_NUM_14, GPIO_NUM_38, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_10,
            GPIO_NUM_39, GPIO_NUM_0, GPIO_NUM_45, GPIO_NUM_48, GPIO_NUM_47, GPIO_NUM_21,
            GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_42, GPIO_NUM_41, GPIO_NUM_40,
        },
        .flags.fb_in_psram = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&config, &panel), TAG, "create RGB panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "initialize RGB panel");
    ESP_RETURN_ON_ERROR(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK),
                        TAG, "clear display");
    ESP_RETURN_ON_ERROR(expander_write(expander_levels | EXIO_BACKLIGHT), TAG,
                        "turn on backlight");
    return cyd_set_brightness(saved_brightness());
}

static esp_err_t gt911_read(uint16_t reg, uint8_t *data, size_t size)
{
    uint8_t address[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return i2c_master_transmit_receive(gt911, address, sizeof(address), data, size,
                                       I2C_TIMEOUT_MS);
}

static esp_err_t gt911_clear_status(void)
{
    uint8_t command[3] = {(uint8_t)(GT911_STATUS >> 8), (uint8_t)GT911_STATUS, 0};
    return i2c_master_transmit(gt911, command, sizeof(command), I2C_TIMEOUT_MS);
}

/* The fingers in one GT911 report, each with the controller's track id,
   which stays the same while that finger is down. */
typedef struct {
    int count;
    uint8_t ids[GT911_MAX_POINTS];
    cyd_touch_point_t points[GT911_MAX_POINTS];
} touch_report_t;

/* Takes the GT911's latest report into *report and returns true, if it has
   a new one. The first finger is kept for cyd_touch_read. */
static bool touch_take_report(touch_report_t *report)
{
    uint8_t status;
    if (gt911_read(GT911_STATUS, &status, 1) != ESP_OK || !(status & 0x80)) {
        return false;
    }
    int count = status & 0x0F;
    uint8_t data[GT911_MAX_POINTS * GT911_POINT_SIZE];
    report->count = 0;
    if (count > 0 && count <= GT911_MAX_POINTS &&
        gt911_read(GT911_POINT1, data, (size_t)count * GT911_POINT_SIZE) == ESP_OK) {
        for (int i = 0; i < count; ++i) {
            const uint8_t *entry = data + i * GT911_POINT_SIZE;
            cyd_touch_point_t *point = &report->points[i];
            report->ids[i] = entry[0];
            point->raw_x = (uint16_t)(entry[1] | (entry[2] << 8));
            point->raw_y = (uint16_t)(entry[3] | (entry[4] << 8));
            cyd_touch_map(NULL, point->raw_x, point->raw_y, &point->x, &point->y);
        }
        report->count = count;
    }
    gt911_clear_status();
    touch_down = report->count > 0;
    if (touch_down) {
        touch_last = report->points[0];
    }
    return true;
}

/* Returns whether a finger is down, with the first touch point in *point. */
static bool touch_poll(cyd_touch_point_t *point)
{
    touch_report_t report;
    touch_take_report(&report);
    *point = touch_last;
    return touch_down;
}

static void touchscreen_task(void *argument)
{
    (void)argument;
    bool was_pressed = false;
    TickType_t last_tap = 0;
    uint32_t held_ids = 0; /* track ids down in the last report, one bit each */
    while (true) {
        touch_report_t report;
        bool fresh = touch_take_report(&report);
        bool fast = cyd_backend_touch_fast();
        if (fresh) {
            /* Fast mode: every finger that wasn't down before is a tap. */
            uint32_t ids = 0;
            for (int i = 0; i < report.count; ++i) {
                uint32_t bit = 1u << (report.ids[i] & 31);
                if (fast && !(held_ids & bit)) {
                    cyd_backend_post_tap(touch_queue, &report.points[i]);
                    last_tap = xTaskGetTickCount();
                }
                ids |= bit;
            }
            held_ids = ids;
            cyd_backend_set_held(report.points, report.count);
        }
        /* Otherwise each new press becomes one tap, debounced like the CYD's. */
        if (!fast && touch_down && !was_pressed &&
            xTaskGetTickCount() - last_tap >= pdMS_TO_TICKS(TOUCH_DEBOUNCE_MS)) {
            cyd_touch_point_t point = touch_last;
            cyd_backend_post_tap(touch_queue, &point);
            ESP_LOGI(TAG, "Tap at %d,%d", point.x, point.y);
            last_tap = xTaskGetTickCount();
        }
        was_pressed = touch_down;
        TickType_t poll = pdMS_TO_TICKS(fast ? TOUCH_FAST_POLL_MS : TOUCH_POLL_MS);
        vTaskDelay(poll > 0 ? poll : 1);
    }
}

esp_err_t cyd_touch_init(void)
{
    return board_init();
}

bool cyd_touch_read(cyd_touch_point_t *point)
{
    return touch_poll(point);
}

esp_err_t cyd_touch_start(QueueHandle_t *queue)
{
    touch_queue = xQueueCreate(16, sizeof(cyd_touch_point_t));
    ESP_RETURN_ON_FALSE(touch_queue != NULL, ESP_ERR_NO_MEM, TAG, "create touch queue");
    ESP_RETURN_ON_ERROR(cyd_touch_init(), TAG, "initialize touchscreen");
    ESP_RETURN_ON_FALSE(xTaskCreate(touchscreen_task, "touchscreen", 3072, NULL, 5, NULL) ==
                            pdPASS,
                        ESP_ERR_NO_MEM, TAG, "create touchscreen task");
    *queue = touch_queue;
    return ESP_OK;
}

/* The GT911 reports display coordinates, so there is nothing to calibrate:
   the calibration is the identity, and setting or saving one does nothing. */
void cyd_touch_default_calibration(cyd_touch_calibration_t *calibration)
{
    *calibration = (cyd_touch_calibration_t){.xa = 1, .yb = 1};
}

void cyd_touch_set_calibration(const cyd_touch_calibration_t *calibration)
{
    (void)calibration;
}

esp_err_t cyd_touch_save_calibration(const cyd_touch_calibration_t *calibration)
{
    (void)calibration;
    return ESP_OK;
}

static int16_t clamp_coordinate(float value, int limit)
{
    if (value < 0) {
        return 0;
    }
    if (value > limit - 1) {
        return (int16_t)(limit - 1);
    }
    return (int16_t)(value + 0.5f);
}

void cyd_touch_map(const cyd_touch_calibration_t *calibration, uint16_t raw_x,
                   uint16_t raw_y, int16_t *x, int16_t *y)
{
    cyd_touch_calibration_t identity;
    if (calibration == NULL) {
        cyd_touch_default_calibration(&identity);
        calibration = &identity;
    }
    const cyd_touch_calibration_t *c = calibration;
    *x = clamp_coordinate(c->xa * raw_x + c->xb * raw_y + c->xc, CYD_LCD_WIDTH);
    *y = clamp_coordinate(c->ya * raw_x + c->yb * raw_y + c->yc, CYD_LCD_HEIGHT);
}
