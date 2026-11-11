#pragma once

/* DrumLoop's side of the audio task: the engine, its transport and the
   pads' hits, rendered through music_out (components/music_out), which
   streams to the AirPlay speaker or, with no speaker, runs on the clock. */

#include <stdbool.h>
#include <stdint.h>

#include "engine.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct {
    uint32_t frame; /* frames written when the step starts (low 32 bits) */
    uint16_t step;  /* index in the loop, bar * 16 + step */
} audio_step_t;

/* Starts music_out (silent) with the engine as its sound. pattern stays
   owned by the caller, who may edit it while it plays. The speaker picker
   reads taps from touch_queue. */
esp_err_t audio_start(drum_pattern_t *pattern, QueueHandle_t touch_queue);

/* Plays a sound now. With record, it is also written into the pattern at
   the step the player heard when they hit it (if the loop is playing). */
void audio_hit(int track, bool record);

/* Starts the loop from its first step, or stops it; stopping also silences
   the speaker at once instead of after its buffer. */
void audio_play(bool play);
bool audio_is_playing(void);

/* The next step start the speaker has reached, for the playhead: true and
   *step when one is due by now. */
bool audio_next_heard_step(audio_step_t *step);
