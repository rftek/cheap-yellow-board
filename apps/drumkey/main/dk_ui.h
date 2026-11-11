#pragma once

/* What the drums and keys screens share: the header row's layout, with the
   button that switches between them, and the gaps drawn between its
   buttons (they sit side by side, with no blank columns to spare).

     PLAY REC - 140 + [BAR|MET] KEYS|DRUM MENU   26 columns */

#include <stdbool.h>

#include "cyd_display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct {
    int column;
    int width;
} span_t;

#define DK_PLAY_SPAN {0, 4}
#define DK_REC_SPAN {4, 3}
#define DK_BPM_DOWN_SPAN {7, 2}
#define DK_BPM_SPAN {9, 3}
#define DK_BPM_UP_SPAN {12, 2}
#define DK_SCREEN_SPAN {14, 4} /* the drums' bar page, the keys' metronome */
#define DK_SWITCH_SPAN {18, 4}
#define DK_MENU_SPAN {22, 4}

/* The switch button's color: the other screen's. */
#define DK_DRUMS_COLOR 0xFA20 /* orange */
#define DK_KEYS_COLOR 0x4A7F  /* blue */

/* Draws thin black gaps between the header's buttons; call after drawing
   row 0. */
void dk_header_gaps(void);

/* The screens. Each runs until its switch button is tapped. */
void drums_screen(QueueHandle_t touch_queue);
void keys_screen(QueueHandle_t touch_queue);
