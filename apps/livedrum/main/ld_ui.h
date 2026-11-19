#pragma once

/* LiveDrum's screens: the pads, each pad's sound picker, the banks and the
   settings menu. The standalone app (apps/livedrum) and Drum&Key's LIVE
   screen both run them; the host says how the transport and tempo work
   and what goes on the header's fourth button. */

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct {
    const char *title; /* menu heading, " LIVEDRUM SETTINGS" */
    /* Starts the transport (after count_in bars of clicks, where the host
       has a count-in) or stops it. */
    void (*play)(bool play, int count_in);
    bool (*is_playing)(void);
    uint16_t (*tempo)(void);
    void (*set_tempo)(int bpm);
    /* The header button between EDIT and MENU: NULL for BANK (the banks),
       or a label that leaves the screen (Drum&Key's screen switch). */
    const char *switch_label;
    uint16_t switch_color;
    bool has_count_in; /* REC from stop clicks a bar first (if switched on) */
    /* The settings menus' status line, CYD_TEXT_COLUMNS + 1 chars, shared
       with the host's other menus so the output picked in one shows in
       all; NULL for LiveDrum's own. */
    char *status;
} ld_host_t;

/* Runs the pads screen until the switch button is tapped (never, without
   one). ld_engine_init and ld_sounds_default must have run. */
void ld_screen(QueueHandle_t touch_queue, const ld_host_t *host);
