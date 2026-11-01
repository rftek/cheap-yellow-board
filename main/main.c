/* XBOX Controller Test
 *
 * A boot image for the ESP32 Cheap Yellow Display (CYD) that pairs with an
 * Xbox wireless controller over Bluetooth with the Bluepad32 library and then
 * shows a live test of every button, D-pad direction, stick and trigger.
 *
 * Bluepad32 runs on the BTstack host (not Bluedroid). BTstack's run loop,
 * started last in app_main(), never returns, so the Bluepad32 callbacks run
 * on that task and the user interface runs on a second task created here. The
 * callback copies the latest controller state into one small struct that the
 * UI task reads, guarded by a spinlock.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cyd_display.h"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

/* Bluepad32 + BTstack */
#include <btstack_port_esp32.h>
#include <btstack_run_loop.h>
#include <uni.h>

#define COLOR_GREEN 0x07E0
#define COLOR_DIM CYD_COLOR_GRAY

/* "forget the paired controller on the next boot" flag. */
#define NVS_NAMESPACE "xbox_test"
#define NVS_FORGET_KEY "forget"

#define FOOTER_ROW (CYD_TEXT_ROWS - 1)

/* Latest controller state. Written by the Bluepad32 callback on the BTstack
   task, read by the UI task. */
typedef struct {
    bool connected;
    char name[20];
    uint8_t battery; /* 0 = unknown, 1 = empty, 255 = full */
    uni_gamepad_t gamepad;
} controller_state_t;

static controller_state_t s_state;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_touch_queue;

/* ------------------------------------------------------------------ */
/* Bluepad32 custom platform                                          */
/* ------------------------------------------------------------------ */

static void platform_init(int argc, const char **argv)
{
    (void)argc;
    (void)argv;
}

static void platform_on_init_complete(void)
{
    /* Accept a controller that is in pairing mode, and reconnect to one we
       have already bonded with. */
    uni_bt_start_scanning_and_autoconnect_unsafe();
    uni_bt_allow_incoming_connections(true);

    /* If the user tapped "Forget & re-pair", the UI task set a flag in NVS and
       rebooted. Clearing the stored link keys here, on the BTstack task, is
       where uni_bt_del_keys_unsafe() is safe to call. */
    nvs_handle_t nvs;
    uint8_t forget = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, NVS_FORGET_KEY, &forget);
        nvs_close(nvs);
    }
    if (forget) {
        uni_bt_del_keys_unsafe();
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_key(nvs, NVS_FORGET_KEY);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    } else {
        uni_bt_list_keys_unsafe();
    }
}

static uni_error_t platform_on_device_discovered(bd_addr_t addr, const char *name,
                                                 uint16_t cod, uint8_t rssi)
{
    (void)addr;
    (void)name;
    (void)cod;
    (void)rssi;
    return UNI_ERROR_SUCCESS;
}

static void platform_on_device_connected(uni_hid_device_t *d)
{
    (void)d;
}

static void platform_on_device_disconnected(uni_hid_device_t *d)
{
    (void)d;
    portENTER_CRITICAL(&s_mux);
    s_state.connected = false;
    s_state.battery = 0;
    memset(&s_state.gamepad, 0, sizeof(s_state.gamepad));
    portEXIT_CRITICAL(&s_mux);
}

static uni_error_t platform_on_device_ready(uni_hid_device_t *d)
{
    portENTER_CRITICAL(&s_mux);
    s_state.connected = true;
    if (uni_hid_device_has_name(d)) {
        strncpy(s_state.name, d->name, sizeof(s_state.name) - 1);
        s_state.name[sizeof(s_state.name) - 1] = '\0';
    } else {
        strcpy(s_state.name, "Controller");
    }
    portEXIT_CRITICAL(&s_mux);
    return UNI_ERROR_SUCCESS;
}

static void platform_on_controller_data(uni_hid_device_t *d, uni_controller_t *ctl)
{
    (void)d;
    if (ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD)
        return;
    portENTER_CRITICAL(&s_mux);
    s_state.gamepad = ctl->gamepad;
    s_state.battery = ctl->battery;
    portEXIT_CRITICAL(&s_mux);
}

static void platform_on_oob_event(uni_platform_oob_event_t event, void *data)
{
    (void)event;
    (void)data;
}

static const uni_property_t *platform_get_property(uni_property_idx_t idx)
{
    (void)idx;
    return NULL;
}

static struct uni_platform *get_xbox_platform(void)
{
    static struct uni_platform plat = {
        .name = "custom",
        .init = platform_init,
        .on_init_complete = platform_on_init_complete,
        .on_device_discovered = platform_on_device_discovered,
        .on_device_connected = platform_on_device_connected,
        .on_device_disconnected = platform_on_device_disconnected,
        .on_device_ready = platform_on_device_ready,
        .on_oob_event = platform_on_oob_event,
        .on_controller_data = platform_on_controller_data,
        .get_property = platform_get_property,
    };
    return &plat;
}

/* ------------------------------------------------------------------ */
/* User interface                                                     */
/* ------------------------------------------------------------------ */

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

static void draw_test_screen(const controller_state_t *state)
{
    const uni_gamepad_t *g = &state->gamepad;
    char line[CYD_TEXT_COLUMNS + 1];

    const token_t face[] = {
        {"A", g->buttons & BUTTON_A},
        {"B", g->buttons & BUTTON_B},
        {"X", g->buttons & BUTTON_X},
        {"Y", g->buttons & BUTTON_Y},
        {"LB", g->buttons & BUTTON_SHOULDER_L},
        {"RB", g->buttons & BUTTON_SHOULDER_R},
    };
    const token_t nav[] = {
        {"Up", g->dpad & DPAD_UP},
        {"Dn", g->dpad & DPAD_DOWN},
        {"Lf", g->dpad & DPAD_LEFT},
        {"Rt", g->dpad & DPAD_RIGHT},
        {"L3", g->buttons & BUTTON_THUMB_L},
        {"R3", g->buttons & BUTTON_THUMB_R},
    };
    const token_t misc[] = {
        {"View", g->misc_buttons & MISC_BUTTON_SELECT},
        {"Menu", g->misc_buttons & MISC_BUTTON_START},
        {"Xbox", g->misc_buttons & MISC_BUTTON_SYSTEM},
        {"Sh", g->misc_buttons & MISC_BUTTON_CAPTURE},
    };

    snprintf(line, sizeof(line), " %.16s", state->name);
    ESP_ERROR_CHECK(cyd_text_row(0, CYD_COLOR_CYAN, CYD_COLOR_BLACK, line));

    draw_tokens(1, face, 6);
    draw_tokens(2, nav, 6);
    draw_tokens(3, misc, 4);
    draw_trigger(4, "LT", g->brake);
    draw_trigger(5, "RT", g->throttle);
    draw_stick(6, "LS", g->axis_x, g->axis_y);
    draw_stick(7, "RS", g->axis_rx, g->axis_ry);

    if (state->battery == 0)
        snprintf(line, sizeof(line), "Battery: n/a");
    else
        snprintf(line, sizeof(line), "Battery: %d%%", state->battery * 100 / 255);
    ESP_ERROR_CHECK(cyd_text_row(8, CYD_COLOR_WHITE, CYD_COLOR_BLACK, line));

    draw_footer();
}

static void forget_and_reboot(void)
{
    nvs_handle_t nvs;

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_FORGET_KEY, 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT, CYD_COLOR_BLACK));
    ESP_ERROR_CHECK(cyd_text_row(CYD_TEXT_ROWS / 2, CYD_COLOR_YELLOW, CYD_COLOR_BLACK,
                                 " Forgetting pad..."));
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
}

static void ui_task(void *arg)
{
    controller_state_t local;
    bool was_connected = false;
    bool first = true;
    int tick = 0;

    (void)arg;
    for (;;) {
        portENTER_CRITICAL(&s_mux);
        local = s_state;
        portEXIT_CRITICAL(&s_mux);

        /* Clear the screen whenever we switch between the two screens. */
        if (first || local.connected != was_connected) {
            ESP_ERROR_CHECK(cyd_fill_rect(0, 0, CYD_LCD_WIDTH, CYD_LCD_HEIGHT,
                                          CYD_COLOR_BLACK));
            was_connected = local.connected;
            first = false;
        }

        if (local.connected)
            draw_test_screen(&local);
        else
            draw_pairing_screen(tick);

        /* A tap on the bottom row forgets the paired controller. */
        cyd_touch_point_t point;
        while (xQueueReceive(s_touch_queue, &point, 0) == pdTRUE) {
            if (point.y / CYD_FONT_HEIGHT == FOOTER_ROW)
                forget_and_reboot();
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_ERROR_CHECK(cyd_display_init());

    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);

    ESP_ERROR_CHECK(cyd_touch_start(&s_touch_queue));
    xTaskCreate(ui_task, "xbox_ui", 4096, NULL, 5, NULL);

    /* Start Bluepad32 on BTstack. btstack_run_loop_execute() does not return,
       so the Bluepad32 callbacks run on this task. */
    btstack_init();
    uni_platform_set_custom(get_xbox_platform());
    uni_init(0, NULL);
    btstack_run_loop_execute();
}
