#pragma once

/* Per-board constants. Each chip target is built separately, so these are
   fixed at compile time; code meant to run on more than one board should
   call cyd_display_width() and the like in cyd_display.h instead. */

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32
/* ESP32 Cheap Yellow Display (ESP32-2432S028R): ILI9341 and XPT2046. */
#define CYD_BOARD_ID "esp32-cyd"
#define CYD_LCD_WIDTH 320
#define CYD_LCD_HEIGHT 240
/* 5x7 glyphs in 6x8 cells, scaled up: 17 columns x 10 rows at 3x. */
#define CYD_FONT_SCALE 3
#define CYD_FONT_SMOOTH 0
#elif CONFIG_IDF_TARGET_ESP32S3
/* Waveshare ESP32-S3-Touch-LCD-7: 800x480 RGB panel and GT911. */
#define CYD_BOARD_ID "esp32s3-lcd7"
#define CYD_LCD_WIDTH 800
#define CYD_LCD_HEIGHT 480
/* 26 columns x 12 rows at 5x: 40-pixel rows, about 7.5 mm, are easy to tap. */
#define CYD_FONT_SCALE 5
/* Text comes from anti-aliased DejaVu Sans Mono glyphs drawn for these
   30x40 cells (smooth_font.inc), not from the 5x7 font blown up. */
#define CYD_FONT_SMOOTH 1
#else
#error "cyd_display has no board for this chip target"
#endif

#define CYD_FONT_WIDTH (6 * CYD_FONT_SCALE)
#define CYD_FONT_HEIGHT (8 * CYD_FONT_SCALE)
#define CYD_TEXT_COLUMNS (CYD_LCD_WIDTH / CYD_FONT_WIDTH)
#define CYD_TEXT_ROWS (CYD_LCD_HEIGHT / CYD_FONT_HEIGHT)
