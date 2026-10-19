#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cyd_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

/* LVGL renders into a strip of DRAW_BUFFER_ROWS rows at a time. */
#define DRAW_BUFFER_ROWS 24
#define UPDATE_PERIOD_MS 33

#define DIAL_SIZE 176
#define TICK_RING_SIZE 160
#define BUTTON_WIDTH 96
#define BUTTON_HEIGHT 44
#define BUTTON_GAP 8

static const char *TAG = "stopwatch";

/* ---- LVGL display and touch glue ---- */

static uint16_t draw_buffer[CYD_LCD_WIDTH * DRAW_BUFFER_ROWS];

typedef struct {
    const uint16_t *pixels;
    int left;
    int top;
    int width;
} flush_area_t;

static uint16_t flush_pixel(int x, int y, void *context)
{
    const flush_area_t *area = context;
    return area->pixels[(y - area->top) * area->width + (x - area->left)];
}

/* LVGL renders row-major RGB565; cyd_draw_pattern reorders the pixels for
   the panel, which fills windows column by column. */
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    flush_area_t flush_area = {
        .pixels = (const uint16_t *)pixels,
        .left = area->x1,
        .top = area->y1,
        .width = lv_area_get_width(area),
    };
    esp_err_t result = cyd_draw_pattern(area->x1, area->y1, lv_area_get_width(area),
                                        lv_area_get_height(area), flush_pixel, &flush_area);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Display flush failed: %s", esp_err_to_name(result));
    }
    lv_display_flush_ready(display);
}

static void read_touch(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    cyd_touch_point_t point;
    if (cyd_touch_read(&point)) {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint32_t milliseconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_init(void)
{
    lv_init();
    lv_tick_set_cb(milliseconds);
    lv_display_t *display = lv_display_create(CYD_LCD_WIDTH, CYD_LCD_HEIGHT);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_touch);
}

/* ---- Stopwatch ---- */

static bool running;
static int64_t started_us;     /* when the current run began */
static int64_t accumulated_us; /* time from earlier runs */

static lv_obj_t *time_label;
static lv_obj_t *state_label;
static lv_obj_t *second_arc;
static lv_obj_t *start_button;
static lv_obj_t *stop_button;

static int64_t elapsed_us(void)
{
    return running ? accumulated_us + esp_timer_get_time() - started_us : accumulated_us;
}

static void show_time(void)
{
    char text[16];
    int64_t centiseconds = elapsed_us() / 10000;
    unsigned minutes = (unsigned)(centiseconds / 6000);
    unsigned seconds = (unsigned)(centiseconds / 100 % 60);
    if (minutes < 100) {
        snprintf(text, sizeof(text), "%02u:%02u.%02u", minutes, seconds,
                 (unsigned)(centiseconds % 100));
    } else {
        snprintf(text, sizeof(text), "%u:%02u:%02u", minutes / 60, minutes % 60, seconds);
    }
    lv_label_set_text(time_label, text);
    /* The arc sweeps once a minute, in tenths of a second. */
    lv_arc_set_value(second_arc, (int32_t)(centiseconds % 6000 / 10));
}

static void set_enabled(lv_obj_t *button, bool enabled)
{
    if (enabled) {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
}

static void show_state(void)
{
    set_enabled(start_button, !running);
    set_enabled(stop_button, running);
    lv_label_set_text(state_label, running ? "RUNNING" : elapsed_us() > 0 ? "STOPPED" : "READY");
    show_time();
}

static void start_clicked(lv_event_t *event)
{
    (void)event;
    if (!running) {
        started_us = esp_timer_get_time();
        running = true;
        show_state();
    }
}

static void stop_clicked(lv_event_t *event)
{
    (void)event;
    if (running) {
        accumulated_us += esp_timer_get_time() - started_us;
        running = false;
        show_state();
    }
}

/* Stops the watch and sets it back to zero. */
static void clear_clicked(lv_event_t *event)
{
    (void)event;
    running = false;
    accumulated_us = 0;
    show_state();
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (running) {
        show_time();
    }
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_palette_t palette,
                             int x_offset, lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, BUTTON_WIDTH, BUTTON_HEIGHT);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, x_offset, -6);
    lv_obj_set_style_bg_color(button, lv_palette_main(palette), 0);
    lv_obj_set_style_bg_color(button, lv_palette_darken(palette, 4), LV_STATE_DISABLED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_color(label, lv_palette_main(LV_PALETTE_GREY), LV_STATE_DISABLED);
    lv_obj_center(label);
    return button;
}

static void build_face(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    /* Seconds arc around the outside of the dial. */
    second_arc = lv_arc_create(screen);
    lv_obj_set_size(second_arc, DIAL_SIZE, DIAL_SIZE);
    lv_obj_align(second_arc, LV_ALIGN_TOP_MID, 0, 4);
    lv_arc_set_rotation(second_arc, 270);
    lv_arc_set_bg_angles(second_arc, 0, 360);
    lv_arc_set_range(second_arc, 0, 599);
    lv_arc_set_value(second_arc, 0);
    lv_obj_remove_style(second_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(second_arc, false);
    lv_obj_set_style_arc_width(second_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(second_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(second_arc, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_arc_color(second_arc, lv_palette_main(LV_PALETTE_ORANGE),
                               LV_PART_INDICATOR);

    /* Minute-face tick marks inside the arc: long every 5 seconds. */
    lv_obj_t *ticks = lv_scale_create(screen);
    lv_obj_set_size(ticks, TICK_RING_SIZE, TICK_RING_SIZE);
    lv_obj_align_to(ticks, second_arc, LV_ALIGN_CENTER, 0, 0);
    lv_scale_set_mode(ticks, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(ticks, 0, 60);
    lv_scale_set_angle_range(ticks, 360);
    lv_scale_set_rotation(ticks, 270);
    lv_scale_set_total_tick_count(ticks, 61);
    lv_scale_set_major_tick_every(ticks, 5);
    lv_scale_set_label_show(ticks, false);
    lv_obj_set_style_arc_width(ticks, 0, LV_PART_MAIN);
    lv_obj_set_style_length(ticks, 10, LV_PART_INDICATOR);
    lv_obj_set_style_length(ticks, 5, LV_PART_ITEMS);
    lv_obj_set_style_line_width(ticks, 3, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(ticks, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_color(ticks, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_line_color(ticks, lv_color_hex(0x808080), LV_PART_ITEMS);

    time_label = lv_label_create(screen);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_obj_align_to(time_label, second_arc, LV_ALIGN_CENTER, 0, -6);

    state_label = lv_label_create(screen);
    lv_obj_set_style_text_font(state_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(state_label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align_to(state_label, second_arc, LV_ALIGN_CENTER, 0, 24);

    start_button = make_button(screen, "START", LV_PALETTE_GREEN,
                               -(BUTTON_WIDTH + BUTTON_GAP), start_clicked);
    stop_button = make_button(screen, "STOP", LV_PALETTE_RED, 0, stop_clicked);
    make_button(screen, "CLEAR", LV_PALETTE_GREY, BUTTON_WIDTH + BUTTON_GAP, clear_clicked);

    /* Keep the time centered as its width changes. */
    lv_obj_set_width(time_label, DIAL_SIZE - 24);
    lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(time_label, second_arc, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_width(state_label, DIAL_SIZE - 24);
    lv_obj_set_style_text_align(state_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(state_label, second_arc, LV_ALIGN_CENTER, 0, 24);

    show_state();
    lv_timer_create(tick, UPDATE_PERIOD_MS, NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(cyd_display_init());
    ESP_ERROR_CHECK(cyd_touch_init());
    lvgl_init();
    build_face();
    ESP_LOGI(TAG, "Stopwatch ready; free heap: %u bytes", (unsigned)esp_get_free_heap_size());
    while (true) {
        uint32_t wait_ms = lv_timer_handler();
        TickType_t wait = pdMS_TO_TICKS(wait_ms < 10 ? wait_ms : 10);
        /* Always yield at least one tick so lower-priority tasks run. */
        vTaskDelay(wait > 0 ? wait : 1);
    }
}
