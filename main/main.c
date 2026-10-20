#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cyd_display.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

/* LVGL renders into a strip of DRAW_BUFFER_ROWS rows at a time. */
#define DRAW_BUFFER_ROWS 24
#define FRAME_PERIOD_MS 33

#define MIN_TEETH 8
#define MAX_TEETH 36
/* The gears fill the screen between the two label rows. */
#define LABEL_HEIGHT 22
#define GEAR_AREA_TOP LABEL_HEIGHT
#define GEAR_AREA_HEIGHT (CYD_LCD_HEIGHT - 2 * LABEL_HEIGHT)
#define GEAR_MARGIN 6
/* Surface speed along the pitch circles, chosen at random per pair. */
#define MIN_PITCH_SPEED 25.0f
#define MAX_PITCH_SPEED 70.0f

#define TWO_PI 6.28318531f

static const char *TAG = "gear_roll";

/* ---- LVGL display and touch glue ---- */

static uint16_t draw_buffer[CYD_LCD_WIDTH * DRAW_BUFFER_ROWS];
static unsigned frames_drawn;

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
    if (lv_display_flush_is_last(display)) {
        ++frames_drawn;
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

/* ---- Gears ---- */

/* Two spur gears with the same module (tooth size), so they mesh: their
   centers are the sum of the pitch radii apart, and gear 1 turns at
   -teeth0/teeth1 times gear 0's speed, phased so its teeth fall into gear
   0's gaps where the pitch circles touch. */
typedef struct {
    int teeth;
    lv_color_t color;
    float center_x;
    float center_y;
    float pitch_radius;
    float angle; /* radians */
} gear_t;

static gear_t gears[2];
static float module_px;   /* pitch diameter per tooth */
static float pitch_speed; /* signed surface speed, px/s */
static int64_t last_frame_us;

static lv_obj_t *gear_layer;
static lv_obj_t *info_label;

static int random_between(int low, int high)
{
    return low + (int)(esp_random() % (uint32_t)(high - low + 1));
}

static lv_color_t random_color(void)
{
    return lv_color_hsv_to_rgb((uint16_t)random_between(0, 359), 65, 95);
}

/* Sizes and places both gears to fill the screen at a shared module. */
static void layout_gears(void)
{
    int n0 = gears[0].teeth;
    int n1 = gears[1].teeth;
    int largest = n0 > n1 ? n0 : n1;
    /* Width: outer diameter of both = module * (n0 + n1 + 2) when meshed.
       Height: the larger outer diameter, module * (n + 2). */
    float by_width = (float)(CYD_LCD_WIDTH - 2 * GEAR_MARGIN) / (n0 + n1 + 2);
    float by_height = (float)(GEAR_AREA_HEIGHT - 2) / (largest + 2);
    module_px = by_width < by_height ? by_width : by_height;

    float total_width = module_px * (n0 + n1 + 2);
    float center_y = GEAR_AREA_TOP + GEAR_AREA_HEIGHT / 2.0f;
    gears[0].pitch_radius = module_px * n0 / 2;
    gears[1].pitch_radius = module_px * n1 / 2;
    gears[0].center_x = (CYD_LCD_WIDTH - total_width) / 2 + gears[0].pitch_radius + module_px;
    gears[1].center_x = gears[0].center_x + gears[0].pitch_radius + gears[1].pitch_radius;
    gears[0].center_y = center_y;
    gears[1].center_y = center_y;

    char text[48];
    snprintf(text, sizeof(text), "Z1 %d   Z2 %d   ratio 1 : %.2f", n0, n1, (float)n1 / n0);
    lv_label_set_text(info_label, text);
    ESP_LOGI(TAG, "Gears %d and %d teeth, module %.2f px", n0, n1, module_px);
}

/* Gear 1 follows gear 0: rolling contact plus a half-tooth phase offset. */
static void update_follower_angle(void)
{
    float ratio = (float)gears[0].teeth / gears[1].teeth;
    gears[1].angle = (float)M_PI + (float)M_PI / gears[1].teeth - ratio * gears[0].angle;
}

static void new_gear(gear_t *gear)
{
    int teeth;
    do {
        teeth = random_between(MIN_TEETH, MAX_TEETH);
    } while (teeth == gear->teeth);
    gear->teeth = teeth;
    gear->color = random_color();
}

static lv_point_precise_t polar_point(const gear_t *gear, float radius, float angle)
{
    return (lv_point_precise_t){
        .x = (lv_value_precise_t)lroundf(gear->center_x + radius * cosf(angle)),
        .y = (lv_value_precise_t)lroundf(gear->center_y + radius * sinf(angle)),
    };
}

static void draw_circle(lv_layer_t *layer, float x, float y, float radius, lv_color_t color)
{
    lv_draw_rect_dsc_t circle;
    lv_draw_rect_dsc_init(&circle);
    circle.bg_color = color;
    circle.radius = LV_RADIUS_CIRCLE;
    lv_area_t area = {
        .x1 = (int32_t)lroundf(x - radius),
        .y1 = (int32_t)lroundf(y - radius),
        .x2 = (int32_t)lroundf(x + radius),
        .y2 = (int32_t)lroundf(y + radius),
    };
    lv_draw_rect(layer, &circle, &area);
}

/* A body disc at the root circle, a tapered tooth (two triangles) per
   tooth, and a hub with a bore. */
static void draw_gear(lv_layer_t *layer, const gear_t *gear)
{
    float root_radius = gear->pitch_radius - 1.25f * module_px;
    float outer_radius = gear->pitch_radius + module_px;
    float base_radius = root_radius - 1; /* overlap the body so no seam shows */
    float pitch_angle = TWO_PI / gear->teeth;
    float base_half = pitch_angle * 0.27f;
    float tip_half = pitch_angle * 0.15f;

    draw_circle(layer, gear->center_x, gear->center_y, root_radius, gear->color);

    lv_draw_triangle_dsc_t tooth;
    lv_draw_triangle_dsc_init(&tooth);
    tooth.color = gear->color;
    for (int i = 0; i < gear->teeth; ++i) {
        float angle = gear->angle + i * pitch_angle;
        lv_point_precise_t base_left = polar_point(gear, base_radius, angle - base_half);
        lv_point_precise_t base_right = polar_point(gear, base_radius, angle + base_half);
        lv_point_precise_t tip_left = polar_point(gear, outer_radius, angle - tip_half);
        lv_point_precise_t tip_right = polar_point(gear, outer_radius, angle + tip_half);
        tooth.p[0] = base_left;
        tooth.p[1] = base_right;
        tooth.p[2] = tip_right;
        lv_draw_triangle(layer, &tooth);
        tooth.p[1] = tip_right;
        tooth.p[2] = tip_left;
        lv_draw_triangle(layer, &tooth);
    }

    /* A hub marker off-center shows the rotation even on fine gears. */
    float hub_radius = root_radius * 0.38f;
    draw_circle(layer, gear->center_x, gear->center_y, hub_radius,
                lv_color_darken(gear->color, LV_OPA_40));
    lv_point_precise_t marker = polar_point(gear, hub_radius * 0.6f, gear->angle);
    draw_circle(layer, marker.x, marker.y, hub_radius * 0.2f, lv_color_white());
    draw_circle(layer, gear->center_x, gear->center_y, hub_radius * 0.3f, lv_color_black());
}

static void draw_gears(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    draw_gear(layer, &gears[0]);
    draw_gear(layer, &gears[1]);
}

/* Advances gear 0 by real elapsed time, so the speed holds whatever the
   frame rate is. */
static void advance(lv_timer_t *timer)
{
    (void)timer;
    int64_t now = esp_timer_get_time();
    float seconds = (now - last_frame_us) / 1e6f;
    last_frame_us = now;
    gears[0].angle = fmodf(gears[0].angle + pitch_speed / gears[0].pitch_radius * seconds,
                           TWO_PI);
    update_follower_angle();
    lv_obj_invalidate(gear_layer);
}

/* A tap on a gear re-cuts it with a new random number of teeth. */
static void gear_pressed(lv_event_t *event)
{
    (void)event;
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    for (int i = 0; i < 2; ++i) {
        float dx = point.x - gears[i].center_x;
        float dy = point.y - gears[i].center_y;
        if (sqrtf(dx * dx + dy * dy) <= gears[i].pitch_radius + module_px) {
            int old_teeth = gears[i].teeth;
            new_gear(&gears[i]);
            layout_gears();
            update_follower_angle();
            lv_obj_invalidate(gear_layer);
            ESP_LOGI(TAG, "Gear %d re-cut: %d -> %d teeth", i + 1, old_teeth, gears[i].teeth);
            return;
        }
    }
}

#define FRAME_RATE_LOG_MS 5000

static void log_frame_rate(lv_timer_t *timer)
{
    (void)timer;
    static unsigned last_frames;
    ESP_LOGI(TAG, "%.1f frames/s", (frames_drawn - last_frames) * 1000.0f / FRAME_RATE_LOG_MS);
    last_frames = frames_drawn;
}

static void build_scene(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    gear_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(gear_layer);
    lv_obj_set_size(gear_layer, CYD_LCD_WIDTH, CYD_LCD_HEIGHT);
    lv_obj_set_pos(gear_layer, 0, 0);
    lv_obj_add_event_cb(gear_layer, draw_gears, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(gear_layer, gear_pressed, LV_EVENT_PRESSED, NULL);

    info_label = lv_label_create(screen);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(info_label, lv_color_white(), 0);
    lv_obj_align(info_label, LV_ALIGN_TOP_MID, 0, 3);

    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "tap a gear to re-cut its teeth");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -3);

    /* A random pair, direction and speed every launch. */
    new_gear(&gears[0]);
    new_gear(&gears[1]);
    pitch_speed = MIN_PITCH_SPEED +
                  (MAX_PITCH_SPEED - MIN_PITCH_SPEED) * (esp_random() % 1000) / 1000.0f;
    if (esp_random() & 1) {
        pitch_speed = -pitch_speed;
    }
    layout_gears();
    update_follower_angle();

    last_frame_us = esp_timer_get_time();
    lv_timer_create(advance, FRAME_PERIOD_MS, NULL);
    lv_timer_create(log_frame_rate, FRAME_RATE_LOG_MS, NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(cyd_display_init());
    ESP_ERROR_CHECK(cyd_touch_init());
    lvgl_init();
    build_scene();
    ESP_LOGI(TAG, "Gear roll ready; free heap: %u bytes", (unsigned)esp_get_free_heap_size());
    while (true) {
        uint32_t wait_ms = lv_timer_handler();
        TickType_t wait = pdMS_TO_TICKS(wait_ms < 10 ? wait_ms : 10);
        /* Always yield at least one tick so lower-priority tasks run. */
        vTaskDelay(wait > 0 ? wait : 1);
    }
}
