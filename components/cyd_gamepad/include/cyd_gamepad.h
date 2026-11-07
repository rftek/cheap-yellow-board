#pragma once

/* Shared Xbox / Bluetooth gamepad input for the ESP32 Cheap Yellow Display.
   Wraps the Bluepad32 library (on the BTstack host) behind a small polling API
   so an application can gain controller input without touching Bluepad32 or
   BTstack directly. Add this component's folder to the app's
   EXTRA_COMPONENT_DIRS beside cyd_display, add the Bluepad32 and BTstack
   wiring to the app's CMakeLists.txt (as apps/xbox_controller does), and list
   cyd_gamepad in the app's PRIV_REQUIRES. */

#include <stdbool.h>
#include <stdint.h>

/* A snapshot of a connected controller. Analog sticks run -512..511, triggers
   0..1023. For an Xbox pad the names match the physical controls. */
typedef struct {
    bool connected;

    bool a, b, x, y;              /* face buttons */
    bool lb, rb;                  /* shoulder buttons */
    bool l3, r3;                  /* stick clicks */
    bool up, down, left, right;   /* d-pad */
    bool view, menu, xbox, share; /* View, Menu, Xbox guide, Share/Capture */

    int16_t lx, ly;               /* left stick, -512..511 */
    int16_t rx, ry;               /* right stick, -512..511 */
    uint16_t lt, rt;              /* triggers, 0..1023 */

    uint8_t battery;              /* 0 unknown, 1 empty, 255 full */
} cyd_gamepad_state_t;

/* Starts Bluepad32 on BTstack on its own task and begins accepting a
   controller in pairing mode (and reconnecting a known one). Call once at
   start-up; it initialises NVS if the application has not already. */
void cyd_gamepad_start(void);

/* Copies the latest controller state into *out and returns out->connected.
   Never blocks; safe to call every frame from any task. */
bool cyd_gamepad_read(cyd_gamepad_state_t *out);

/* Forgets the stored pairing and reboots, so a different controller can pair.
   The stored keys are cleared on the next boot, on the BTstack task. */
void cyd_gamepad_forget(void);
