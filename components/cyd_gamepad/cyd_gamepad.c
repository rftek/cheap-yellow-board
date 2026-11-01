/* Xbox / Bluetooth gamepad input for the CYD, built on Bluepad32 + BTstack.

   BTstack's run loop never returns, so cyd_gamepad_start() runs it on its own
   task; the Bluepad32 callbacks run there and copy the latest controller state
   into one struct that cyd_gamepad_read() returns, guarded by a spinlock. */

#include "cyd_gamepad.h"

#include <string.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <btstack_port_esp32.h>
#include <btstack_run_loop.h>
#include <uni.h>

#define NVS_NAMESPACE "cyd_gamepad"
#define NVS_FORGET_KEY "forget"

static cyd_gamepad_state_t s_state;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- Bluepad32 custom platform ---- */

static void platform_init(int argc, const char **argv)
{
    (void)argc;
    (void)argv;
}

static void platform_on_init_complete(void)
{
    uni_bt_start_scanning_and_autoconnect_unsafe();
    uni_bt_allow_incoming_connections(true);

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
    memset(&s_state, 0, sizeof(s_state));
    portEXIT_CRITICAL(&s_mux);
}

static uni_error_t platform_on_device_ready(uni_hid_device_t *d)
{
    (void)d;
    portENTER_CRITICAL(&s_mux);
    s_state.connected = true;
    portEXIT_CRITICAL(&s_mux);
    return UNI_ERROR_SUCCESS;
}

static void platform_on_controller_data(uni_hid_device_t *d, uni_controller_t *ctl)
{
    (void)d;
    if (ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD)
        return;

    const uni_gamepad_t *g = &ctl->gamepad;
    portENTER_CRITICAL(&s_mux);
    s_state.connected = true;
    s_state.a = g->buttons & BUTTON_A;
    s_state.b = g->buttons & BUTTON_B;
    s_state.x = g->buttons & BUTTON_X;
    s_state.y = g->buttons & BUTTON_Y;
    s_state.lb = g->buttons & BUTTON_SHOULDER_L;
    s_state.rb = g->buttons & BUTTON_SHOULDER_R;
    s_state.l3 = g->buttons & BUTTON_THUMB_L;
    s_state.r3 = g->buttons & BUTTON_THUMB_R;
    s_state.up = g->dpad & DPAD_UP;
    s_state.down = g->dpad & DPAD_DOWN;
    s_state.left = g->dpad & DPAD_LEFT;
    s_state.right = g->dpad & DPAD_RIGHT;
    s_state.view = g->misc_buttons & MISC_BUTTON_SELECT;
    s_state.menu = g->misc_buttons & MISC_BUTTON_START;
    s_state.xbox = g->misc_buttons & MISC_BUTTON_SYSTEM;
    s_state.share = g->misc_buttons & MISC_BUTTON_CAPTURE;
    s_state.lx = g->axis_x;
    s_state.ly = g->axis_y;
    s_state.rx = g->axis_rx;
    s_state.ry = g->axis_ry;
    s_state.lt = g->brake;
    s_state.rt = g->throttle;
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

static struct uni_platform *get_platform(void)
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

/* ---- Public API ---- */

static void gamepad_task(void *arg)
{
    (void)arg;
    btstack_init();
    uni_platform_set_custom(get_platform());
    uni_init(0, NULL);
    btstack_run_loop_execute(); /* does not return */
}

void cyd_gamepad_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    xTaskCreate(gamepad_task, "cyd_gamepad", 8192, NULL, 5, NULL);
}

bool cyd_gamepad_read(cyd_gamepad_state_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_state;
    portEXIT_CRITICAL(&s_mux);
    return out->connected;
}

void cyd_gamepad_forget(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_FORGET_KEY, 1);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    esp_restart();
}
