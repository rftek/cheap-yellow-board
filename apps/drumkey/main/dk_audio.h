#pragma once

/* Drum&Key's audio task: DrumLoop's drum engine and Keyboard's looper and
   synth, mixed into one stream through music_out (one speaker takes one
   AirPlay stream). The touch task and both screens send it hits, notes and
   commands; it runs them at the start of each 8 ms block.

   The two loops share one transport and one tempo: PLAY starts both from
   their first bar in the same block, so a drum loop and a keyboard loop of
   1, 2, 4, 8 (or 16 drum) bars stay in step. */

#include <stdbool.h>
#include <stdint.h>

#include "engine.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "looper.h"

/* Starts music_out (silent) with both engines as its sound. pattern stays
   owned by the caller, who may edit it while it plays; its bpm is the
   shared tempo. The speaker picker reads taps from touch_queue. */
esp_err_t dk_audio_start(drum_pattern_t *pattern, QueueHandle_t touch_queue);

/* ---- Shared transport and tempo ---------------------------------------- */

/* The drum pattern given to dk_audio_start; its bpm is the shared tempo. */
drum_pattern_t *dk_pattern(void);

/* Starts both loops from bar 1, or stops both; stopping also silences the
   speaker at once instead of after its buffer. */
void dk_play(bool play);
bool dk_is_playing(void);

uint16_t dk_tempo(void);
void dk_set_tempo(int bpm);

/* ---- Drums --------------------------------------------------------------- */

typedef struct {
    uint32_t frame; /* frames written when the step starts (low 32 bits) */
    uint16_t step;  /* index in the loop, bar * 16 + step */
} audio_step_t;

/* Plays a drum now. With record, it is also written into the pattern at
   the step the player heard when they hit it (if the loop is playing). */
void drums_hit(int track, bool record);

/* The next drum step the speaker has reached, for the playhead. */
bool drums_next_heard_step(audio_step_t *step);

/* ---- Keys ---------------------------------------------------------------- */

/* Keyboard settings the keys screen changes and the audio task reads each
   block. Its bpm and count-in are not used: the tempo is the pattern's,
   and there is no count-in, since the drums keep time. */
extern volatile looper_settings_t keys_settings;

void keys_note_on(int note, int instrument);
void keys_note_off(int note, int instrument);
void keys_sustain(bool down);
void keys_all_notes_off(void); /* every live note, as when leaving the keys */
void keys_record(bool on);
void keys_undo(void);
void keys_clear(void);
void keys_cycle_bars(void);

/* A keyboard loop note the speaker has now reached, for lighting its key. */
bool keys_next_heard_note(int *note, bool *on);
