#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyd_display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* Targets sit this far in from the corners: the very edge of a resistive
   panel is its least linear part, and a finger cannot center on it. */
#define TARGET_INSET 20
#define TARGET_ARM 10
#define GRID_SPACING 40
#define CORNER_COUNT 4
/* A fit whose worst corner is further off than this means a mistapped
   corner, not a skewed panel. */
#define MAX_FIT_ERROR_PX 25.0f

#define COLOR_GREEN 0x07E0
#define COLOR_GRID 0x4208

static const char *TAG = "touch_cal";
static QueueHandle_t touch_queue;

typedef struct {
    int16_t x;
    int16_t y;
    const char *name;
} target_t;

static const target_t corners[CORNER_COUNT] = {
    {TARGET_INSET, TARGET_INSET, "UPPER LEFT"},
    {CYD_LCD_WIDTH - 1 - TARGET_INSET, TARGET_INSET, "UPPER RIGHT"},
    {CYD_LCD_WIDTH - 1 - TARGET_INSET, CYD_LCD_HEIGHT - 1 - TARGET_INSET, "LOWER RIGHT"},
    {TARGET_INSET, CYD_LCD_HEIGHT - 1 - TARGET_INSET, "LOWER LEFT"},
};
static const target_t center = {CYD_LCD_WIDTH / 2, CYD_LCD_HEIGHT / 2, "CENTER"};

typedef struct {
    int first_column;
    int columns;
    const char *label;
} button_t;

enum { BUTTON_LEFT, BUTTON_RIGHT, BUTTON_COUNT };

/* ---- Drawing ---- */

/* A dark grid with a 3-pixel-thick red cross on the target, if any. */
static uint16_t grid_pixel(int x, int y, void *context)
{
    const target_t *target = context;
    if (target != NULL) {
        int dx = abs(x - target->x);
        int dy = abs(y - target->y);
        if ((dx <= TARGET_ARM && dy <= 1) || (dy <= TARGET_ARM && dx <= 1)) {
            return CYD_COLOR_RED;
        }
    }
    if (x % GRID_SPACING == 0 || y % GRID_SPACING == 0 || x == CYD_LCD_WIDTH - 1 ||
        y == CYD_LCD_HEIGHT - 1) {
        return COLOR_GRID;
    }
    return CYD_COLOR_BLACK;
}

static void draw_grid(const target_t *target)
{
    ESP_ERROR_CHECK(cyd_draw_pattern(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, grid_pixel,
                                     (void *)target));
}

/* Two prompt lines, placed clear of the target. */
static void draw_prompt(const target_t *target, const char *line1, const char *line2)
{
    int row = target->y > CYD_LCD_HEIGHT / 3 && target->y < CYD_LCD_HEIGHT * 2 / 3 ? 1 : 4;
    ESP_ERROR_CHECK(cyd_text_row(row, CYD_COLOR_WHITE, CYD_COLOR_BLACK, line1));
    ESP_ERROR_CHECK(cyd_text_row(row + 1, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, line2));
}

/* A 5x5 dot, clipped to the screen. */
static void draw_marker(int x, int y, uint16_t color)
{
    int left = x - 2 < 0 ? 0 : x - 2;
    int top = y - 2 < 0 ? 0 : y - 2;
    int right = x + 2 >= CYD_LCD_WIDTH ? CYD_LCD_WIDTH - 1 : x + 2;
    int bottom = y + 2 >= CYD_LCD_HEIGHT ? CYD_LCD_HEIGHT - 1 : y + 2;
    ESP_ERROR_CHECK(cyd_fill_rect(left, top, right - left + 1, bottom - top + 1, color));
}

static void draw_buttons(int row, const button_t buttons[BUTTON_COUNT])
{
    char text[CYD_TEXT_COLUMNS + 1];
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    memset(text, ' ', CYD_TEXT_COLUMNS);
    text[CYD_TEXT_COLUMNS] = '\0';
    for (int i = 0; i < CYD_TEXT_COLUMNS; ++i) {
        foreground[i] = CYD_COLOR_WHITE;
        background[i] = CYD_COLOR_BLACK;
    }
    for (int b = 0; b < BUTTON_COUNT; ++b) {
        const button_t *button = &buttons[b];
        int length = (int)strlen(button->label);
        memcpy(text + button->first_column + (button->columns - length) / 2, button->label,
               length);
        for (int i = 0; i < button->columns; ++i) {
            background[button->first_column + i] = CYD_COLOR_BUTTON;
        }
    }
    ESP_ERROR_CHECK(cyd_draw_cells(row, text, foreground, background));
}

static bool button_hit(const button_t *button, const cyd_touch_point_t *point)
{
    int column = point->x / CYD_FONT_WIDTH;
    return point->y / CYD_FONT_HEIGHT == CYD_TEXT_ROWS - 1 && column >= button->first_column &&
           column < button->first_column + button->columns;
}

static cyd_touch_point_t wait_tap(void)
{
    cyd_touch_point_t point;
    xQueueReset(touch_queue);
    xQueueReceive(touch_queue, &point, portMAX_DELAY);
    return point;
}

static void show_intro(void)
{
    static const char *const lines[CYD_TEXT_ROWS] = {
        "TOUCH CALIBRATION", "", "Tap each red +", "as exactly as you", "can, ideally with",
        "a stylus or nail.", "", "4 corners, then", "a center check.", "tap to start",
    };
    for (int row = 0; row < CYD_TEXT_ROWS; ++row) {
        uint16_t color = row == 0 ? CYD_COLOR_YELLOW
                         : row == CYD_TEXT_ROWS - 1 ? CYD_COLOR_GRAY
                                                    : CYD_COLOR_WHITE;
        ESP_ERROR_CHECK(cyd_text_row(row, color, CYD_COLOR_BLACK, lines[row]));
    }
}

/* ---- Fitting ---- */

static double determinant3(const double m[3][3])
{
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

/* Solves m * solution = rhs by Cramer's rule. */
static bool solve3(const double m[3][3], const double rhs[3], double solution[3])
{
    double det = determinant3(m);
    if (fabs(det) < 1e-9) {
        return false;
    }
    for (int column = 0; column < 3; ++column) {
        double replaced[3][3];
        memcpy(replaced, m, sizeof(replaced));
        for (int row = 0; row < 3; ++row) {
            replaced[row][column] = rhs[row];
        }
        solution[column] = determinant3(replaced) / det;
    }
    return true;
}

/* Least-squares affine fit of the targets to the raw readings, with the
   distance of the worst corner from its target after fitting. */
static bool fit_calibration(const cyd_touch_point_t taps[CORNER_COUNT],
                            cyd_touch_calibration_t *calibration, float *worst_error)
{
    double normal[3][3] = {{0}};
    double rhs_x[3] = {0};
    double rhs_y[3] = {0};
    for (int i = 0; i < CORNER_COUNT; ++i) {
        double row[3] = {taps[i].raw_x, taps[i].raw_y, 1.0};
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                normal[a][b] += row[a] * row[b];
            }
            rhs_x[a] += row[a] * corners[i].x;
            rhs_y[a] += row[a] * corners[i].y;
        }
    }
    double x[3];
    double y[3];
    if (!solve3(normal, rhs_x, x) || !solve3(normal, rhs_y, y)) {
        return false;
    }
    *calibration = (cyd_touch_calibration_t){
        .xa = (float)x[0], .xb = (float)x[1], .xc = (float)x[2],
        .ya = (float)y[0], .yb = (float)y[1], .yc = (float)y[2],
    };
    *worst_error = 0;
    for (int i = 0; i < CORNER_COUNT; ++i) {
        int16_t fx;
        int16_t fy;
        cyd_touch_map(calibration, taps[i].raw_x, taps[i].raw_y, &fx, &fy);
        float error = hypotf(fx - corners[i].x, fy - corners[i].y);
        if (error > *worst_error) {
            *worst_error = error;
        }
    }
    return true;
}

/* ---- Steps ---- */

/* Collects the four corner taps; returns false if they do not fit together. */
static bool calibrate(cyd_touch_calibration_t *calibration)
{
    cyd_touch_point_t taps[CORNER_COUNT];
    for (int i = 0; i < CORNER_COUNT; ++i) {
        draw_grid(&corners[i]);
        draw_prompt(&corners[i], "TAP THE + AT", corners[i].name);
        taps[i] = wait_tap();
        /* Yellow: where the calibration in use put the tap. */
        draw_marker(taps[i].x, taps[i].y, CYD_COLOR_YELLOW);
        ESP_LOGI(TAG, "%s: raw %u,%u (mapped %d,%d; target %d,%d)", corners[i].name,
                 taps[i].raw_x, taps[i].raw_y, taps[i].x, taps[i].y, corners[i].x, corners[i].y);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    float worst_error = 0;
    if (!fit_calibration(taps, calibration, &worst_error) || worst_error > MAX_FIT_ERROR_PX) {
        ESP_LOGW(TAG, "Corners do not fit together (worst %.1f px)", worst_error);
        return false;
    }
    ESP_LOGI(TAG, "Fit: x = %.5f*rx + %.5f*ry + %.1f, y = %.5f*rx + %.5f*ry + %.1f "
                  "(worst corner %.1f px)",
             calibration->xa, calibration->xb, calibration->xc, calibration->ya,
             calibration->yb, calibration->yc, worst_error);
    return true;
}

/* Taps the center to compare the calibration in use with the new one;
   returns true to save. */
static bool verify(const cyd_touch_calibration_t *calibration)
{
    static const button_t buttons[BUTTON_COUNT] = {{0, 8, "RETRY"}, {9, 8, "SAVE"}};
    draw_grid(&center);
    draw_prompt(&center, "NOW TAP THE +", "IN THE CENTER");
    cyd_touch_point_t tap = wait_tap();
    int16_t x;
    int16_t y;
    cyd_touch_map(calibration, tap.raw_x, tap.raw_y, &x, &y);
    float before = hypotf(tap.x - center.x, tap.y - center.y);
    float after = hypotf(x - center.x, y - center.y);
    ESP_LOGI(TAG, "Center: before %.1f px, after %.1f px", before, after);

    char line[CYD_TEXT_COLUMNS + 1];
    draw_marker(tap.x, tap.y, CYD_COLOR_YELLOW);
    draw_marker(x, y, COLOR_GREEN);
    snprintf(line, sizeof(line), "BEFORE %3d px", (int)(before + 0.5f));
    ESP_ERROR_CHECK(cyd_text_row(6, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, line));
    snprintf(line, sizeof(line), "AFTER  %3d px", (int)(after + 0.5f));
    ESP_ERROR_CHECK(cyd_text_row(7, COLOR_GREEN, CYD_COLOR_BLACK, line));
    draw_buttons(CYD_TEXT_ROWS - 1, buttons);
    while (true) {
        tap = wait_tap();
        if (button_hit(&buttons[BUTTON_RIGHT], &tap)) {
            return true;
        }
        if (button_hit(&buttons[BUTTON_LEFT], &tap)) {
            return false;
        }
    }
}

/* Draws a dot wherever the screen is tapped, with the new calibration. */
static void test_mode(void)
{
    static const button_t buttons[BUTTON_COUNT] = {{0, 8, "AGAIN"}, {9, 8, "CLEAR"}};
    draw_grid(NULL);
    ESP_ERROR_CHECK(cyd_text_row(0, COLOR_GREEN, CYD_COLOR_BLACK, "SAVED: TAP TO TRY"));
    draw_buttons(CYD_TEXT_ROWS - 1, buttons);
    while (true) {
        cyd_touch_point_t tap = wait_tap();
        if (button_hit(&buttons[BUTTON_LEFT], &tap)) {
            return;
        }
        if (button_hit(&buttons[BUTTON_RIGHT], &tap)) {
            draw_grid(NULL);
            draw_buttons(CYD_TEXT_ROWS - 1, buttons);
            continue;
        }
        draw_marker(tap.x, tap.y, COLOR_GREEN);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(cyd_display_init());
    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));
    while (true) {
        show_intro();
        wait_tap();
        cyd_touch_calibration_t calibration;
        if (!calibrate(&calibration)) {
            ESP_ERROR_CHECK(cyd_text_row(4, CYD_COLOR_RED, CYD_COLOR_BLACK, "CORNERS DON'T"));
            ESP_ERROR_CHECK(cyd_text_row(5, CYD_COLOR_RED, CYD_COLOR_BLACK, "MATCH: TAP, RETRY"));
            wait_tap();
            continue;
        }
        if (!verify(&calibration)) {
            continue;
        }
        if (cyd_touch_save_calibration(&calibration) != ESP_OK) {
            ESP_LOGE(TAG, "Could not save the calibration");
            continue;
        }
        ESP_LOGI(TAG, "Calibration saved for every image");
        test_mode();
    }
}
