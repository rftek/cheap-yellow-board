#pragma once

/* What the drums and keys screens share: the header row's layout, with the
   button that switches between them, and the gaps drawn between its
   buttons (they sit side by side, with no blank columns to spare).

     PLAY REC - 140 + [BAR|MET] DRUM|ROLL|KEYS MENU   26 columns

   The screen button goes round KEYS -> DRUM -> ROLL -> KEYS. */

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
#define DK_ROLL_COLOR 0x8AFF  /* violet */

/* Draws thin black gaps between the header's buttons; call after drawing
   row 0. */
void dk_header_gaps(void);

/* The screens. Each runs until its screen button takes it to the next,
   keys to drums to Piano Roll and round again; one that goes elsewhere
   (the keys' loop row opens the roll) sets dk_next_screen first. */
typedef enum {
    DK_DRUMS,
    DK_KEYS,
    DK_ROLL,
} dk_screen_t;

extern dk_screen_t dk_next_screen;

void drums_screen(QueueHandle_t touch_queue);
void keys_screen(QueueHandle_t touch_queue); /* its loop row opens the roll */
void roll_screen(QueueHandle_t touch_queue);

/* The style the keys play, for notes drawn in the Piano Roll. */
int keys_current_instrument(void);

/* The keys' settings menu (loop length, quantize, clear, speaker and the
   rest), until BACK; the Piano Roll's MENU opens it. */
void keys_menu(QueueHandle_t touch_queue);
