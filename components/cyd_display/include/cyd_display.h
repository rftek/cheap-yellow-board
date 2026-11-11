#pragma once

/* Display, text and touch support for the ESP32 Cheap Yellow Display,
   shared by the launcher and the boot images in this repository. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* CYD_LCD_WIDTH/HEIGHT, CYD_FONT_SCALE/WIDTH/HEIGHT, CYD_TEXT_COLUMNS/ROWS. */
#include "cyd_board.h"

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
#define CYD_GLYPH_BACKSPACE '\x03'

typedef struct {
    int16_t x; /* display coordinates, calibrated */
    int16_t y;
    uint16_t raw_x; /* XPT2046 readings, 0-4095 */
    uint16_t raw_y;
} cyd_touch_point_t;

/* Affine touch calibration: x = xa*raw_x + xb*raw_y + xc, and likewise for
   y. Unlike a per-axis scale, it also corrects a panel that is rotated or
   skewed against the LCD. The touchscreen_cal image saves one in NVS
   (cyd_config/touch_cal) and every image's driver applies it. */
typedef struct {
    float xa, xb, xc;
    float ya, yb, yc;
} cyd_touch_calibration_t;

#define CYD_CONFIG_TOUCH_CAL "touch_cal"

/* Backlight level in percent. The launcher saves it in NVS so every image
   starts at the same brightness. */
#define CYD_BRIGHTNESS_MIN 20
#define CYD_BRIGHTNESS_MAX 100
#define CYD_BRIGHTNESS_STEP 20
#define CYD_CONFIG_NAMESPACE "cyd_config"
#define CYD_CONFIG_BRIGHTNESS "brightness"

/* The board's size, at run time: the same values as the CYD_* macros, for
   code that should work unchanged on more than one board. */
int cyd_display_width(void);
int cyd_display_height(void);
int cyd_text_columns(void);
int cyd_text_rows(void);
int cyd_font_scale(void);

/* Which board this build is for ("esp32-cyd") and its chip target ("esp32"). */
const char *cyd_board_id(void);
const char *cyd_board_target(void);

/* What the board has, so shared code can adapt without #if. */
bool cyd_touch_is_capacitive(void);
int cyd_touch_max_points(void);
bool cyd_has_psram(void);

/* Initializes the LCD, clears it to black and turns on the backlight at the
   brightness saved in NVS (full brightness if none). */
esp_err_t cyd_display_init(void);

/* Selects (true) or releases the board's microSD card. On the Waveshare S3
   board the card's chip select is pin EXIO4 of the I/O expander, which an
   SPI driver can't toggle: select the card once and mount it over SPI
   (MOSI 11, MISO 13, SCK 12) with no CS pin (SDSPI_SLOT_NO_CS), as
   Waveshare's example does. ESP_ERR_NOT_SUPPORTED on the CYD, whose card
   has an ordinary GPIO chip select. Call after cyd_display_init. */
esp_err_t cyd_sd_select(bool selected);

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

/* Draws row-major RGB565 pixels (native byte order, as LVGL renders them),
   reordered for the panel. Much faster than cyd_draw_pattern for bitmaps. */
esp_err_t cyd_draw_bitmap(int x, int y, int width, int height, const uint16_t *pixels);

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
   in display coordinates, to the returned queue. It loads the calibration
   saved in NVS, if any. */
esp_err_t cyd_touch_start(QueueHandle_t *queue);

/* For instruments, such as drum pads, where a tap must act at once. Fast
   mode polls the panel every few milliseconds instead of every 25, drops
   the 250 ms gap between taps (to 60 ms on the resistive CYD) and, on a
   multi-touch panel, makes each new finger its own tap. Off by default;
   it can be switched at any time, for example off while a menu is open. */
void cyd_touch_set_fast(bool fast);

/* Called by the touch task for each tap before it is queued, so an app can
   react without waiting for its UI task to finish drawing. Return true when
   the tap was handled and should not be queued. Runs in the touch task:
   keep it short and don't draw from it. */
typedef bool (*cyd_tap_fn)(const cyd_touch_point_t *point, void *context);

/* Sets the tap callback; NULL removes it. */
void cyd_touch_set_tap_callback(cyd_tap_fn tap, void *context);

/* For long presses, with cyd_touch_start: copies up to max of the fingers
   down at the touch task's last poll into points and returns how many. The
   resistive CYD reports one, where its press began. */
int cyd_touch_held(cyd_touch_point_t *points, int max);

/* For UI libraries that poll (such as LVGL) instead of taking taps: sets
   up the touchscreen and loads the saved calibration, without the tap
   task. Use either this with cyd_touch_read, or cyd_touch_start. */
esp_err_t cyd_touch_init(void);

/* Returns true while the screen is pressed, with the calibrated position. */
bool cyd_touch_read(cyd_touch_point_t *point);

/* The built-in calibration, from the CYD documentation's raw ranges. */
void cyd_touch_default_calibration(cyd_touch_calibration_t *calibration);

/* Uses calibration for later taps; NULL restores the default. Not saved. */
void cyd_touch_set_calibration(const cyd_touch_calibration_t *calibration);

/* Saves calibration in NVS for every image and uses it now. */
esp_err_t cyd_touch_save_calibration(const cyd_touch_calibration_t *calibration);

/* Maps raw readings to display coordinates with the given calibration
   (NULL: the one in use), clamped to the screen. */
void cyd_touch_map(const cyd_touch_calibration_t *calibration, uint16_t raw_x,
                   uint16_t raw_y, int16_t *x, int16_t *y);
