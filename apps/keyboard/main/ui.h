#pragma once

/* The keyboard screen and its settings menu. */

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Runs the keyboard screen and its menu; never returns. Wi-Fi and the
   sound output are chosen in the menu (components/music_out). */
void ui_run(QueueHandle_t touch_queue);
