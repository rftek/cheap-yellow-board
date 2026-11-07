#pragma once

/* The audio task: runs the engine and streams its output to the AirPlay
   speaker, one 8 ms packet at a time. While a speaker is attached it always
   writes, silence included, so a pad hit plays without restarting the
   stream. */

#include <stdbool.h>
#include <stdint.h>

#include "airplay_sender.h"
#include "engine.h"
#include "esp_err.h"

typedef struct {
    uint32_t frame; /* frames written when the step starts (low 32 bits) */
    uint16_t step;  /* index in the loop, bar * 16 + step */
} audio_step_t;

/* Starts the task. pattern stays owned by the caller, who may edit it while
   it plays. */
esp_err_t audio_start(drum_pattern_t *pattern);

/* Attaches a connected speaker, or detaches with NULL. Detaching waits for
   the write in progress, so the caller can then disconnect the handle. */
void audio_set_speaker(airplay_sender_handle_t speaker);

/* Silent mode, for using the pads and sequencer with no speaker: the
   engine runs in real time on the board's clock and its output is thrown
   away. Ignored while a speaker is attached; attaching one ends it. */
void audio_set_silent(bool on);

bool audio_has_speaker(void);

/* Volume 0-100 on the attached speaker. */
esp_err_t audio_set_volume(int volume);
int audio_get_volume(void);

/* Plays a sound now. With record, it is also written into the pattern at
   the step the player heard when they hit it (if the loop is playing). */
void audio_hit(int track, bool record);

/* Starts the loop from its first step, or stops it; stopping also silences
   the speaker at once instead of after its buffer. */
void audio_play(bool play);
bool audio_is_playing(void);

/* The frame the speaker is playing now: frames written minus its latency. */
uint32_t audio_heard_frame(void);

/* The next step start the speaker has reached, for the playhead: true and
   *step when one is due by now. */
bool audio_next_heard_step(audio_step_t *step);
