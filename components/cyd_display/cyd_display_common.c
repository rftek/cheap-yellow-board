/* The board-independent half of cyd_display: the font, text rows and
   drawing helpers, built on the two primitives in cyd_display_backend.h. */

#include "cyd_display.h"

#include <string.h>

#include "cyd_display_backend.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

/* A run of full-height display columns of one text line. */
#define TEXT_CHUNK_COLUMNS 40

static const char *TAG = "cyd_display";
static uint16_t chunk[TEXT_CHUNK_COLUMNS * CYD_FONT_HEIGHT];

#if !CYD_FONT_SMOOTH
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
#endif

int cyd_display_width(void)
{
    return CYD_LCD_WIDTH;
}

int cyd_display_height(void)
{
    return CYD_LCD_HEIGHT;
}

int cyd_text_columns(void)
{
    return CYD_TEXT_COLUMNS;
}

int cyd_text_rows(void)
{
    return CYD_TEXT_ROWS;
}

int cyd_font_scale(void)
{
    return CYD_FONT_SCALE;
}

const char *cyd_board_target(void)
{
    return CONFIG_IDF_TARGET;
}

/* The glyph's index in either font: 0x20-0x7E, then the three arrows. */
static int glyph_index(char character)
{
    unsigned char code = (unsigned char)character;
    if (character == CYD_GLYPH_UP) {
        return 95;
    }
    if (character == CYD_GLYPH_DOWN) {
        return 96;
    }
    if (character == CYD_GLYPH_BACKSPACE) {
        return 97;
    }
    if (code < 0x20 || code > 0x7E) {
        code = '?';
    }
    return code - 0x20;
}

#if CYD_FONT_SMOOTH
/* One glyph per text cell, 4-bit coverage, two pixels per byte. */
static const uint8_t smooth_font[98][CYD_FONT_HEIGHT][CYD_FONT_WIDTH / 2] = {
#include "smooth_font.inc"
};

/* Mixes background and foreground by coverage 0-15, channel by channel. */
static uint16_t blend565(uint16_t background, uint16_t foreground, unsigned coverage)
{
    if (coverage == 0) {
        return background;
    }
    if (coverage == 15) {
        return foreground;
    }
    unsigned rest = 15 - coverage;
    unsigned red = ((background >> 11) * rest + (foreground >> 11) * coverage + 7) / 15;
    unsigned green = (((background >> 5) & 0x3F) * rest + ((foreground >> 5) & 0x3F) * coverage + 7) / 15;
    unsigned blue = ((background & 0x1F) * rest + (foreground & 0x1F) * coverage + 7) / 15;
    return (uint16_t)(red << 11 | green << 5 | blue);
}
#endif

static bool inside_display(int x, int y, int width, int height)
{
    return x >= 0 && y >= 0 && width > 0 && height > 0 &&
           x + width <= CYD_LCD_WIDTH && y + height <= CYD_LCD_HEIGHT;
}

esp_err_t cyd_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    ESP_RETURN_ON_FALSE(inside_display(x, y, width, height), ESP_ERR_INVALID_ARG, TAG,
                        "rectangle outside display");
    return cyd_backend_fill_rect(x, y, width, height, color);
}

esp_err_t cyd_draw_pattern(int x, int y, int width, int height,
                           cyd_pixel_fn pixel, void *context)
{
    ESP_RETURN_ON_FALSE(inside_display(x, y, width, height), ESP_ERR_INVALID_ARG, TAG,
                        "pattern outside display");
    /* Column by column, a strip of whole columns at a time. */
    int columns_per_chunk = (int)(sizeof(chunk) / sizeof(chunk[0])) / height;
    for (int column = 0; column < width; column += columns_per_chunk) {
        int columns = width - column < columns_per_chunk ? width - column : columns_per_chunk;
        for (int c = 0; c < columns; ++c) {
            for (int row = 0; row < height; ++row) {
                chunk[row * columns + c] = pixel(x + column + c, y + row, context);
            }
        }
        ESP_RETURN_ON_ERROR(cyd_backend_blit(x + column, y, columns, height, chunk), TAG,
                            "draw pattern chunk");
    }
    return ESP_OK;
}

esp_err_t cyd_draw_bitmap(int x, int y, int width, int height, const uint16_t *pixels)
{
    ESP_RETURN_ON_FALSE(inside_display(x, y, width, height), ESP_ERR_INVALID_ARG, TAG,
                        "bitmap outside display");
    return cyd_backend_blit(x, y, width, height, pixels);
}

esp_err_t cyd_draw_cells(int row, const char *text,
                         const uint16_t *foreground, const uint16_t *background)
{
    ESP_RETURN_ON_FALSE(row >= 0 && row < CYD_TEXT_ROWS, ESP_ERR_INVALID_ARG, TAG,
                        "text row outside display");
    /* NULL is an empty row: apps size their row tables by CYD_TEXT_ROWS, so
       on a board with more rows the extra entries are NULL. */
    size_t length = text != NULL ? strnlen(text, CYD_TEXT_COLUMNS) : 0;
    for (int chunk_x = 0; chunk_x < CYD_LCD_WIDTH; chunk_x += TEXT_CHUNK_COLUMNS) {
        int columns = CYD_LCD_WIDTH - chunk_x < TEXT_CHUNK_COLUMNS ? CYD_LCD_WIDTH - chunk_x
                                                                   : TEXT_CHUNK_COLUMNS;
        for (int dx = 0; dx < columns; ++dx) {
            int x = chunk_x + dx;
            size_t cell = (size_t)x / CYD_FONT_WIDTH;
            /* The pixels right of the last full cell take its colors. */
            size_t color_cell = cell < CYD_TEXT_COLUMNS ? cell : CYD_TEXT_COLUMNS - 1;
#if CYD_FONT_SMOOTH
            int cell_x = x % CYD_FONT_WIDTH;
            int shift = cell_x % 2 == 0 ? 4 : 0;
            for (int y = 0; y < CYD_FONT_HEIGHT; ++y) {
                unsigned coverage = 0;
                if (cell < length && cell < CYD_TEXT_COLUMNS) {
                    coverage = (smooth_font[glyph_index(text[cell])][y][cell_x / 2] >> shift) & 0x0F;
                }
                uint16_t color = y == CYD_FONT_HEIGHT - 1
                                     ? CYD_COLOR_BLACK
                                     : blend565(background[color_cell], foreground[color_cell],
                                                coverage);
                chunk[y * columns + dx] = color;
            }
#else
            int glyph_x = (x % CYD_FONT_WIDTH) / CYD_FONT_SCALE;
            uint8_t glyph_bits = cell < length && glyph_x < 5
                                     ? font5x7[glyph_index(text[cell])][glyph_x]
                                     : 0;
            for (int y = 0; y < CYD_FONT_HEIGHT; ++y) {
                uint16_t color = background[color_cell];
                int glyph_y = (y - 1) / CYD_FONT_SCALE;
                if (y == CYD_FONT_HEIGHT - 1) {
                    color = CYD_COLOR_BLACK;
                } else if (y >= 1 && glyph_y < 7 && (glyph_bits & (1U << glyph_y))) {
                    color = foreground[color_cell];
                }
                chunk[y * columns + dx] = color;
            }
#endif
        }
        ESP_RETURN_ON_ERROR(cyd_backend_blit(chunk_x, row * CYD_FONT_HEIGHT, columns,
                                             CYD_FONT_HEIGHT, chunk),
                            TAG, "send text chunk");
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

/* ---- Touch: fast mode and the tap callback, shared by both backends ---- */

static volatile bool touch_fast;
static cyd_tap_fn tap_callback;
static void *tap_context;

void cyd_touch_set_fast(bool fast)
{
    touch_fast = fast;
}

bool cyd_backend_touch_fast(void)
{
    return touch_fast;
}

void cyd_touch_set_tap_callback(cyd_tap_fn tap, void *context)
{
    tap_callback = NULL;
    tap_context = context;
    tap_callback = tap;
}

void cyd_backend_post_tap(QueueHandle_t queue, const cyd_touch_point_t *point)
{
    cyd_tap_fn tap = tap_callback;
    if (tap != NULL && tap(point, tap_context)) {
        return;
    }
    xQueueSend(queue, point, 0);
}

static cyd_fingers_fn fingers_callback;
static void *fingers_context;

void cyd_touch_set_fingers_callback(cyd_fingers_fn fingers, void *context)
{
    fingers_callback = NULL;
    fingers_context = context;
    fingers_callback = fingers;
}

bool cyd_backend_wants_fingers(void)
{
    return fingers_callback != NULL;
}

void cyd_backend_post_fingers(const cyd_touch_fingers_t *fingers)
{
    cyd_fingers_fn callback = fingers_callback;
    if (callback != NULL) {
        callback(fingers, fingers_context);
    }
}

#define HELD_MAX 5
static portMUX_TYPE held_lock = portMUX_INITIALIZER_UNLOCKED;
static cyd_touch_point_t held_points[HELD_MAX];
static int held_count;

void cyd_backend_set_held(const cyd_touch_point_t *points, int count)
{
    if (count > HELD_MAX) {
        count = HELD_MAX;
    }
    portENTER_CRITICAL(&held_lock);
    if (count > 0) {
        memcpy(held_points, points, (size_t)count * sizeof(*points));
    }
    held_count = count;
    portEXIT_CRITICAL(&held_lock);
}

int cyd_touch_held(cyd_touch_point_t *points, int max)
{
    portENTER_CRITICAL(&held_lock);
    int count = held_count < max ? held_count : max;
    if (count > 0) {
        memcpy(points, held_points, (size_t)count * sizeof(*points));
    }
    portEXIT_CRITICAL(&held_lock);
    return count;
}
