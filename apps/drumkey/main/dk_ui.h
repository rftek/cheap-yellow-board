#pragma once

/* What the drums and keys screens share: the header row's layout, with the
   button that switches between them, and the gaps drawn between its
   buttons (they sit side by side, with no blank columns to spare).

     PLAY REC - 140 + [BAR|MET|P1|EDIT] DRUM|LIVE|ROLL|SYN|KEYS MENU

   The screen button goes round KEYS -> DRUM -> LIVE -> ROLL -> SYNTH ->
   KEYS. */

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
#define DK_SYNTH_COLOR 0xFAD2 /* pink */
#define DK_LIVE_COLOR 0x0570  /* green */

/* Draws thin black gaps between the header's buttons; call after drawing
   row 0. */
void dk_header_gaps(void);

/* The screens. Each runs until its screen button takes it to the next,
   keys to drums to LIVE to Piano Roll to SYNTH and round again; one that goes elsewhere
   (the keys' loop row opens the roll) sets dk_next_screen first. */
typedef enum {
    DK_DRUMS,
    DK_KEYS,
    DK_ROLL,
    DK_SYNTH,
    DK_LIVE,
} dk_screen_t;

extern dk_screen_t dk_next_screen;

void drums_screen(QueueHandle_t touch_queue);
void keys_screen(QueueHandle_t touch_queue); /* its loop row opens the roll */
void roll_screen(QueueHandle_t touch_queue);
void synth_screen(QueueHandle_t touch_queue);
void live_screen(QueueHandle_t touch_queue); /* LiveDrum's pads (live_ui.c) */

/* The style the keys play, for notes drawn in the Piano Roll, and a way
   to pick it from there (the roll's INST). */
int keys_current_instrument(void);
void keys_select_instrument(int instrument);

/* The keys' settings menu (loop length, quantize, clear, speaker and the
   rest), until BACK; SYNTH's MENU opens it. roll_menu is the same menu
   for the Piano Roll's MENU, named for it: ROLL SETTINGS, CLEAR ROLL and
   BACK TO ROLL. Clearing empties the keys loop, which the roll edits; the
   drums stay. */
void keys_menu(QueueHandle_t touch_queue);
void roll_menu(QueueHandle_t touch_queue);

/* The status line both settings menus show, so the output picked in one
   (a speaker or the wired DAC) shows in the other too. */
extern char dk_status_text[CYD_TEXT_COLUMNS + 1];
