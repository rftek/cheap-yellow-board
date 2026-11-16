#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Runs ODIN S3's screens, forever. bpm is the music apps' shared tempo. */
void ui_run(QueueHandle_t touch_queue, int bpm);
