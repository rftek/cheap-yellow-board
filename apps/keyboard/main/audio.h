#pragma once

/* Keyboard's side of the audio task. The touch task and the UI send it
   commands (keys, transport, loop edits) through a queue; it runs them at
   the start of each 8 ms block and renders the looper and synth through
   music_out (components/music_out), to the AirPlay speaker or, with none,
   on the board's clock. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "looper.h"

/* Settings the UI changes and the audio task reads every block. */
extern volatile looper_settings_t audio_settings;

/* Starts music_out (silent). The speaker picker reads taps from
   touch_queue. */
esp_err_t audio_start(QueueHandle_t touch_queue);

/* Keys, from the touch task. */
void audio_note_on(int note, int instrument);
void audio_note_off(int note, int instrument);
void audio_sustain(bool down);
void audio_all_notes_off(void); /* every live note, as when leaving the keys */

/* Transport and loop edits, from the UI. */
void audio_play(void);
void audio_stop(void); /* also drops what the speaker has buffered */
void audio_record(bool on);
void audio_undo(void);
void audio_clear(void);
void audio_cycle_bars(void);

/* A loop note the speaker has now reached, for lighting its key: true and
   the note when one is due. */
bool audio_next_heard_note(int *note, bool *on);
