#pragma once

/* The performance screen and its settings menu. */

#include <stdbool.h>
#include <stdint.h>

#include "engine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Runs the performance screen and its menu; never returns. Wi-Fi and the
   sound output are chosen in the menu (output.c). */
void ui_run(QueueHandle_t touch_queue, drum_pattern_t *pattern);

/* The AirPlay latency the player picked in the menu (saved in NVS), used
   for the next connection. */
uint32_t ui_saved_latency_ms(void);
