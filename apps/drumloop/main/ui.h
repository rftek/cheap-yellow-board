#pragma once

/* The performance screen and its settings menu. */

#include <stdbool.h>
#include <stdint.h>

#include "engine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    UI_DISCONNECT,   /* the player tapped CHANGE SPEAKER */
    UI_SPEAKER_LOST, /* the speaker ended the session */
} ui_result_t;

/* Runs the performance screen until the player disconnects or the speaker
   drops the session (*speaker_lost turns true). The audio task must have a
   speaker attached or be in silent mode. */
ui_result_t ui_perform(QueueHandle_t touch_queue, const char *speaker_name,
                       volatile bool *speaker_lost, drum_pattern_t *pattern);

/* The AirPlay latency the player picked in the menu (saved in NVS), used
   for the next connection. */
uint32_t ui_saved_latency_ms(void);
