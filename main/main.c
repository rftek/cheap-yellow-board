/* XBOX Controller Test
 *
 * A boot image for the ESP32 Cheap Yellow Display that pairs with an Xbox
 * wireless controller and shows a live test of every button, D-pad direction,
 * stick and trigger. All the Bluetooth work lives in the shared cyd_gamepad
 * component; this file is only the user interface, which polls the controller
 * state each frame and draws it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "cyd_display.h"
#include "cyd_gamepad.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define COLOR_GREEN 0x07E0
#define COLOR_DIM CYD_COLOR_GRAY
#define FOOTER_ROW (CYD_TEXT_ROWS - 1)

static QueueHandle_t s_touch_queue;

typedef struct {
    const char *label;
    bool active;
} token_t;

/* Lays out labelled controls left to right across one text row, green when
   active and dim when not, one space between them. */
static void draw_tokens(int row, const token_t *tokens, int count)
{
    char text[CYD_TEXT_COLUMNS + 1];
    uint16_t foreground[CYD_TEXT_COLUMNS];
    uint16_t background[CYD_TEXT_COLUMNS];
    int col = 0;

    for (int i = 0; i < CYD_TEXT_COLUMNS; i++) {
        text[i] = ' ';
        foreground[i] = COLOR_DIM;
        background[i] = CYD_COLOR_BLACK;
    }
    for (int i = 0; i < count && col < CYD_TEXT_COLUMNS; i++) {
        uint16_t color = tokens[i].active ? COLOR_GREEN : COLOR_DIM;
        for (const char *p = tokens[i].label; *p && col < CYD_TEXT_COLUMNS; p++) {
            text[col] = *p;
            foreground[col] = color;
            col++;
        }
        if (col < CYD_TEXT_COLUMNS)
            col++; /* gap */
    }
    text[CYD_TEXT_COLUMNS] = '\0';
    ESP_ERROR_CHECK(cyd_draw_cells(row, text, foreground, background));
}

/* Draws an analog trigger (0-1023) as a ten-segment bar. */
static void draw_trigger(int row, const char *label, int value)
{
    char text[CYD_TEXT_COLUMNS + 1];
    char bar[11];
    int fill = value * 10 / 1023;

    if (fill > 10)
        fill = 10;
    for (int i = 0; i < 10; i++)
        bar[i] = (i < fill) ? '#' : '-';
    bar[10] = '\0';
    snprintf(text, sizeof(text), "%s[%s]", label, bar);
    ESP_ERROR_CHECK(cyd_text_row(row, value > 50 ? COLOR_GREEN : CYD_COLOR_WHITE,
                                 CYD_COLOR_BLACK, text));
}

/* Draws one analog stick's X and Y (each -512..511). */
static void draw_stick(int row, const char *label, int x, int y)
{
    char text[CYD_TEXT_COLUMNS + 1];
    bool active = (x > 80 || x < -80 || y > 80 || y < -80);

    snprintf(text, sizeof(text), "%s x%+04d y%+04d", label, x, y);
    ESP_ERROR_CHECK(cyd_text_row(row, active ? COLOR_GREEN : CYD_COLOR_WHITE,
                                 CYD_COLOR_BLACK, text));
}

static void draw_footer(void)
{
    ESP_ERROR_CHECK(cyd_text_row(FOOTER_ROW, CYD_COLOR_WHITE, CYD_COLOR_BUTTON,
                                 " Forget & re-pair"));
}

static void draw_pairing_screen(int tick)
{
    static const char *const dots[] = {"Searching   ", "Searching.  ",
                                       "Searching.. ", "Searching..."};
    char status[20];

    ESP_ERROR_CHECK(cyd_text_row(0, CYD_COLOR_CYAN, CYD_COLOR_BLACK, " XBOX CONTROLLER"));
    ESP_ERROR_CHECK(cyd_text_row(1, CYD_COLOR_CYAN, CYD_COLOR_BLACK, " TEST"));
    ESP_ERROR_CHECK(cyd_text_row(3, CYD_COLOR_WHITE, CYD_COLOR_BLACK, "Turn on the pad,"));
    ESP_ERROR_CHECK(cyd_text_row(4, CYD_COLOR_WHITE, CYD_COLOR_BLACK, "hold the pair btn"));
    ESP_ERROR_CHECK(cyd_text_row(5, CYD_COLOR_WHITE, CYD_COLOR_BLACK, "on top until the"));
    ESP_ERROR_CHECK(cyd_text_row(6, CYD_COLOR_WHITE, CYD_COLOR_BLACK, "light blinks fast"));
    snprintf(status, sizeof(status), " %s", dots[tick % 4]);
    ESP_ERROR_CHECK(cyd_text_row(8, CYD_COLOR_YELLOW, CYD_COLOR_BLACK, status));
    draw_footer();
}

static void draw_test_screen(const cyd_gamepad_state_t *s)
{
    char line[CYD_TEXT_COLUMNS + 1];

    const token_t face[] = {
        {"A", s->a},   {"B", s->b},   {"X", s->x},
        {"Y", s->y},   {"LB", s->lb}, {"RB", s->rb},
    };
    const token_t nav[] = {
        {"Up", s->up}, {"Dn", s->down}, {"Lf", s->left},
        {"Rt", s->right}, {"L3", s->l3}, {"R3", s->r3},
    };
    const token_t misc[] = {
        {"View", s->view}, {"Menu", s->menu}, {"Xbox", s->xbox}, {"Sh", s->share},
    };

    ESP_ERROR_CHECK(cyd_text_row(0, CYD_COLOR_CYAN, CYD_COLOR_BLACK, " XBOX CONTROLLER"));
    draw_tokens(1, face, 6);
    draw_tokens(2, nav, 6);
    draw_tokens(3, misc, 4);
    draw_trigger(4, "LT", s->lt);
    draw_trigger(5, "RT", s->rt);
    draw_stick(6, "LS", s->lx, s->ly);
    draw_stick(7, "RS", s->rx, s->ry);

    if (s->battery == 0)
        snprintf(line, sizeof(line), "Battery: n/a");
    else
        snprintf(line, sizeof(line), "Battery: %d%%", s->battery * 100 / 255);
    ESP_ERROR_CHECK(cyd_text_row(8, CYD_COLOR_WHITE, CYD_COLOR_BLACK, line));

    draw_footer();
}

void app_main(void)
{
    /* cyd_gamepad_start() initialises NVS and starts Bluetooth on its own
       task, so it runs before the display and touch, which read NVS. */
    cyd_gamepad_start();
    ESP_ERROR_CHECK(cyd_display_init());
    ESP_ERROR_CHECK(cyd_touch_start(&s_touch_queue));

    cyd_gamepad_state_t state;
    bool was_connected = false;
    bool first = true;
    int tick = 0;

    for (;;) {
        cyd_gamepad_read(&state);

        /* Clear the screen whenever we switch between the two screens. */
        if (first || state.connected != was_connected) {
            ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT,
                                          CYD_COLOR_BLACK));
            was_connected = state.connected;
            first = false;
        }

        if (state.connected)
            draw_test_screen(&state);
        else
            draw_pairing_screen(tick);

        /* A tap on the bottom row forgets the paired controller (reboots). */
        cyd_touch_point_t point;
        while (xQueueReceive(s_touch_queue, &point, 0) == pdTRUE) {
            if (point.y / CYD_FONT_HEIGHT == FOOTER_ROW)
                cyd_gamepad_forget();
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}
