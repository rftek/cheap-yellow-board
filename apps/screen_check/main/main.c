#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cyd_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* How long a pattern's name stays on screen before it is hidden. */
#define LABEL_DURATION_US (1500LL * 1000)
#define LABEL_ROW (CYD_TEXT_ROWS - 1)
#define GRID_SPACING 40

#define COLOR_MAGENTA 0xF81F
#define COLOR_GREEN 0x07E0
#define COLOR_BLUE 0x001F

static const char *TAG = "screen_check";

typedef struct screen_test screen_test_t;

struct screen_test {
    const char *name;
    cyd_pixel_fn pixel; /* NULL for the instructions page */
    uint16_t color;     /* for solid fills */
};

static uint16_t solid_pixel(int x, int y, void *context)
{
    (void)x;
    (void)y;
    return ((const screen_test_t *)context)->color;
}

/* Eight vertical bars in the classic order, brightest first. */
static uint16_t color_bars_pixel(int x, int y, void *context)
{
    static const uint16_t bars[] = {
        CYD_COLOR_WHITE, CYD_COLOR_YELLOW, CYD_COLOR_CYAN, COLOR_GREEN,
        COLOR_MAGENTA,   CYD_COLOR_RED,    COLOR_BLUE,     CYD_COLOR_BLACK,
    };
    (void)y;
    (void)context;
    return bars[x * 8 / CYD_LCD_WIDTH];
}

/* Four horizontal bands ramping from black on the left: red, green, blue, gray. */
static uint16_t rgb_ramps_pixel(int x, int y, void *context)
{
    (void)context;
    uint8_t level = (uint8_t)(x * 255 / (CYD_LCD_WIDTH - 1));
    switch (y * 4 / CYD_LCD_HEIGHT) {
    case 0:
        return cyd_rgb(level, 0, 0);
    case 1:
        return cyd_rgb(0, level, 0);
    case 2:
        return cyd_rgb(0, 0, level);
    default:
        return cyd_rgb(level, level, level);
    }
}

/* Sixteen gray steps from black to white. */
static uint16_t gray_steps_pixel(int x, int y, void *context)
{
    (void)y;
    (void)context;
    uint8_t level = (uint8_t)(x * 16 / CYD_LCD_WIDTH * 255 / 15);
    return cyd_rgb(level, level, level);
}

/* Alternating single pixels: shows focus, crosstalk and dead pixels. */
static uint16_t checkerboard_pixel(int x, int y, void *context)
{
    (void)context;
    return ((x ^ y) & 1) ? CYD_COLOR_WHITE : CYD_COLOR_BLACK;
}

/* A white grid inside a 1-pixel red border: shows geometry and clipped edges. */
static uint16_t grid_pixel(int x, int y, void *context)
{
    (void)context;
    if (x == 0 || y == 0 || x == CYD_LCD_WIDTH - 1 || y == CYD_LCD_HEIGHT - 1) {
        return CYD_COLOR_RED;
    }
    if (x % GRID_SPACING == 0 || y % GRID_SPACING == 0) {
        return CYD_COLOR_WHITE;
    }
    return CYD_COLOR_BLACK;
}

static screen_test_t tests[] = {
    {"SCREEN CHECK", NULL, 0},
    {"RED", solid_pixel, CYD_COLOR_RED},
    {"GREEN", solid_pixel, COLOR_GREEN},
    {"BLUE", solid_pixel, COLOR_BLUE},
    {"WHITE", solid_pixel, CYD_COLOR_WHITE},
    {"BLACK", solid_pixel, CYD_COLOR_BLACK},
    {"CYAN", solid_pixel, CYD_COLOR_CYAN},
    {"MAGENTA", solid_pixel, COLOR_MAGENTA},
    {"YELLOW", solid_pixel, CYD_COLOR_YELLOW},
    {"GRAY 50%", solid_pixel, 0x8410},
    {"COLOR BARS", color_bars_pixel, 0},
    {"RGB RAMPS", rgb_ramps_pixel, 0},
    {"GRAY STEPS", gray_steps_pixel, 0},
    {"CHECKERBOARD", checkerboard_pixel, 0},
    {"GRID", grid_pixel, 0},
};

#define TEST_COUNT ((int)(sizeof(tests) / sizeof(tests[0])))

static void show_instructions(void)
{
    static const char *const lines[CYD_TEXT_ROWS] = {
        "SCREEN CHECK", "", "Tap right half:", "  next pattern", "Tap left half:",
        "  previous", "", "EN button exits", "to the launcher", "",
    };
    for (int row = 0; row < CYD_TEXT_ROWS; ++row) {
        ESP_ERROR_CHECK(cyd_text_row(row, row == 0 ? CYD_COLOR_YELLOW : CYD_COLOR_WHITE,
                                     CYD_COLOR_BLACK, lines[row]));
    }
}

/* Draws test number index; returns true when a label was drawn over it. */
static bool show_test(int index)
{
    screen_test_t *test = &tests[index];
    ESP_LOGI(TAG, "%d/%d %s", index, TEST_COUNT - 1, test->name);
    if (test->pixel == NULL) {
        show_instructions();
        return false;
    }
    ESP_ERROR_CHECK(cyd_draw_pattern(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, test->pixel, test));
    char label[32];
    snprintf(label, sizeof(label), "%d/%d %s", index, TEST_COUNT - 1, test->name);
    ESP_ERROR_CHECK(cyd_text_row(LABEL_ROW, CYD_COLOR_WHITE, CYD_COLOR_BLACK, label));
    return true;
}

static void hide_label(int index)
{
    screen_test_t *test = &tests[index];
    ESP_ERROR_CHECK(cyd_draw_pattern(0, LABEL_ROW * CYD_FONT_HEIGHT, CYD_LCD_WIDTH,
                                     CYD_FONT_HEIGHT, test->pixel, test));
}

void app_main(void)
{
    QueueHandle_t touch_queue;
    ESP_ERROR_CHECK(cyd_display_init());
    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));

    int current = 0;
    show_test(current);
    int64_t hide_at = 0;
    while (true) {
        TickType_t wait = portMAX_DELAY;
        if (hide_at != 0) {
            int64_t remaining_ms = (hide_at - esp_timer_get_time()) / 1000;
            wait = pdMS_TO_TICKS(remaining_ms > 0 ? remaining_ms : 1);
        }
        cyd_touch_point_t point;
        if (xQueueReceive(touch_queue, &point, wait) == pdTRUE) {
            current = point.x >= CYD_LCD_WIDTH / 2 ? (current + 1) % TEST_COUNT
                                                   : (current + TEST_COUNT - 1) % TEST_COUNT;
            hide_at = show_test(current) ? esp_timer_get_time() + LABEL_DURATION_US : 0;
        } else if (hide_at != 0 && esp_timer_get_time() >= hide_at) {
            hide_label(current);
            hide_at = 0;
        }
    }
}
