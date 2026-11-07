#pragma once

/* Where the sound goes, and the Wi-Fi it needs: both chosen in the menu.
   DrumLoop starts with Wi-Fi off and no speaker (silent). */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Reads the Wi-Fi network the launcher saved; starts nothing. */
void output_init(QueueHandle_t touch_queue);

/* Wi-Fi. */
bool output_wifi_saved(void);  /* the launcher has a network saved */
bool output_wifi_on(void);     /* the radio is on */
bool output_wifi_joined(void); /* on and joined */
const char *output_wifi_ssid(void);

/* Turns the radio on and joins the saved network, showing progress on a
   full screen (the caller redraws afterwards). On failure the radio stays
   on and status describes why. */
bool output_wifi_start(char *status, size_t status_size);

/* Disconnects the speaker, if any, and turns the radio off. */
void output_wifi_stop(void);

/* Speaker. Full-screen picker: joins Wi-Fi if needed, searches for AirPlay
   speakers and connects to the one tapped, or turns the speaker off. The
   caller redraws afterwards; status gets a line about the outcome. */
void output_pick_speaker(char *status, size_t status_size);

/* The speaker's name, or NULL when silent. */
const char *output_speaker_name(void);

/* Call often: when the speaker has dropped the session, disconnects it,
   goes silent and returns true (once). */
bool output_check_lost(void);
