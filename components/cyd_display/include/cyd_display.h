#pragma once

/* Display, text and touch support for the ESP32 Cheap Yellow Display,
   shared by the launcher and the boot images in this repository. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define CYD_LCD_WIDTH 320
#define CYD_LCD_HEIGHT 240

/* 5x7 glyphs in 6x8 cells, scaled up: 17 columns x 10 rows at 3x. */
#define CYD_FONT_SCALE 3
#define CYD_FONT_WIDTH (6 * CYD_FONT_SCALE)
#define CYD_FONT_HEIGHT (8 * CYD_FONT_SCALE)
#define CYD_TEXT_COLUMNS (CYD_LCD_WIDTH / CYD_FONT_WIDTH)
#define CYD_TEXT_ROWS (CYD_LCD_HEIGHT / CYD_FONT_HEIGHT)

#define CYD_COLOR_BLACK 0x0000
#define CYD_COLOR_WHITE 0xFFFF
#define CYD_COLOR_GRAY 0x8410
#define CYD_COLOR_YELLOW 0xFFE0
#define CYD_COLOR_CYAN 0x07FF
#define CYD_COLOR_RED 0xF800
#define CYD_COLOR_BUTTON 0x2945
#define CYD_COLOR_BUTTON_DISABLED 0x4A69

/* Extra glyphs past the ASCII range. */
#define CYD_GLYPH_UP '\x01'
#define CYD_GLYPH_DOWN '\x02'

typedef struct {
    int16_t x;
    int16_t y;
} cyd_touch_point_t;

/* Initializes the LCD, clears it to black and turns on the backlight. */
esp_err_t cyd_display_init(void);

esp_err_t cyd_fill_rect(int x, int y, int width, int height, uint16_t color);

/* Draws one full-width text row of CYD_TEXT_COLUMNS cells, each with its own
   colors. The bottom pixel row is always black so stacked buttons stay apart. */
esp_err_t cyd_draw_cells(int row, const char *text,
                         const uint16_t *foreground, const uint16_t *background);

esp_err_t cyd_text_row(int row, uint16_t foreground, uint16_t background, const char *text);

/* Starts the touchscreen and a task that posts one cyd_touch_point_t per tap,
   in display coordinates, to the returned queue. */
esp_err_t cyd_touch_start(QueueHandle_t *queue);
