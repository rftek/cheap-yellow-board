#pragma once

/* The audio task's side of ODIN S3: music_out calls the engine for each
   8 ms block, and the UI and touch tasks send it notes and patches through
   a queue, so the engine is only ever touched by the audio task. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "patch.h"

esp_err_t audio_start(QueueHandle_t touch_queue);

void audio_note_on(int note, int velocity);
void audio_note_off(int note);
void audio_all_notes_off(void);
void audio_load_patch(const odin_patch_t *patch);
void audio_set_modwheel(float value);
void audio_set_pitchbend(float value);
void audio_set_xy(float x, float y);
void audio_set_bpm(int bpm);

/* The render time of the last second, in percent of the time a block
   lasts: average and worst. */
void audio_load(int *average, int *worst);
int audio_voices(void);
