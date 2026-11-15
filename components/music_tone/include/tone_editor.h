#pragma once

/* The tone editor screen the music apps share, for the 800x480 S3 board:

     row 0       <   BD TONE   >   BACK     the item, < > for the others
     row 1       FILTER  OFF  LP  HP  BP
     rows 2-7    sliders: cutoff, resonance, low, mid, high, level
     row 8       FLAT (resets the item's tone)
     rows 9-11   the app's own: DrumLoop's pads, Keyboard's test keys

   A slider follows a finger dragged along it, so a filter can be swept
   while the app plays. The app keeps its sound going and draws rows 9-11
   through the callbacks; the editor polls the touch driver's held fingers
   (cyd_touch_held) for the sliders. */

#include <stdbool.h>
#include <stdint.h>

#include "cyd_display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "tone.h"

#define TONE_EDITOR_BOTTOM_ROW 9 /* the app's rows start here */

typedef struct {
    int count; /* items, paged with < and > */
    /* The tone of an item, which the editor changes in place. */
    tone_settings_t *(*settings)(int item, void *context);
    /* The item's name for the title ("BD", "C7 GRAND") and its color. */
    const char *(*name)(int item, void *context);
    uint16_t (*color)(int item, void *context);
    /* Draws rows 9-11 for item, on entry and when the item changes. */
    void (*draw_bottom)(int item, void *context);
    /* Called about every 10 ms: flash pads, follow the loop, and so on. */
    void (*tick)(int item, void *context);
    /* A tap from the queue in rows 9-11. */
    void (*tap)(const cyd_touch_point_t *point, int item, void *context);
    void *context;
} tone_editor_t;

/* Runs the editor from item until BACK; returns the item it ended on. */
int tone_editor_run(QueueHandle_t touch_queue, const tone_editor_t *editor, int item);
