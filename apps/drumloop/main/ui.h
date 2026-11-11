#pragma once

/* The performance screen and its settings menu. */

#include <stdbool.h>
#include <stdint.h>

#include "engine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Runs the performance screen and its menu; never returns. Wi-Fi and the
   sound output are chosen in the menu (components/music_out). */
void ui_run(QueueHandle_t touch_queue, drum_pattern_t *pattern);
