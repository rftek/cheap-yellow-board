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

/* Backlight level in percent. The launcher saves it in NVS so every image
   starts at the same brightness. */
#define CYD_BRIGHTNESS_MIN 20
#define CYD_BRIGHTNESS_MAX 100
#define CYD_BRIGHTNESS_STEP 20
#define CYD_CONFIG_NAMESPACE "cyd_config"
#define CYD_CONFIG_BRIGHTNESS "brightness"

/* Initializes the LCD, clears it to black and turns on the backlight at the
   brightness saved in NVS (full brightness if none). */
esp_err_t cyd_display_init(void);

/* Sets the backlight, clamped to CYD_BRIGHTNESS_MIN..MAX; does not save it. */
esp_err_t cyd_set_brightness(int percent);
int cyd_get_brightness(void);

esp_err_t cyd_fill_rect(int x, int y, int width, int height, uint16_t color);

/* Returns the RGB565 color of display pixel (x, y). */
typedef uint16_t (*cyd_pixel_fn)(int x, int y, void *context);

/* Fills a rectangle pixel by pixel from a function of display coordinates,
   for gradients and patterns without a frame buffer. */
esp_err_t cyd_draw_pattern(int x, int y, int width, int height,
                           cyd_pixel_fn pixel, void *context);

/* Packs 8-bit channels into RGB565. */
static inline uint16_t cyd_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint16_t)(((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3));
}

/* Draws one full-width text row of CYD_TEXT_COLUMNS cells, each with its own
   colors. The bottom pixel row is always black so stacked buttons stay apart. */
esp_err_t cyd_draw_cells(int row, const char *text,
                         const uint16_t *foreground, const uint16_t *background);

esp_err_t cyd_text_row(int row, uint16_t foreground, uint16_t background, const char *text);

/* Starts the touchscreen and a task that posts one cyd_touch_point_t per tap,
   in display coordinates, to the returned queue. */
esp_err_t cyd_touch_start(QueueHandle_t *queue);
