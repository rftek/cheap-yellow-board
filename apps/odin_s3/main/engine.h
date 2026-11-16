#pragma once

/* ODIN S3's sound engine: Odin 2's voice architecture on the ESP32-S3.

   Each voice has three oscillators (analog, wavetable or noise), two
   filters (ladder, SEM, Korg 35 or diode), an amplifier with Odin's
   distortion, three envelopes and three LFOs. After the voices come a
   global envelope and LFO, filter 3, and the delay, phaser, flanger and
   chorus in the patch's order. A 9-row mod matrix connects them, as in
   Odin. Unison stacks up to 4 voices per note; there are 8 voices.

   Everything here runs in the audio task: audio.c queues the UI's notes
   and patch changes and calls these between blocks.

   Ported from Odin 2, Copyright (C) 2020-2021 TheWaveWarden,
   GPL-3.0-or-later. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "patch.h"

#define ENGINE_VOICES 8
#define ENGINE_UNISON_MAX 4

/* Allocates the delay line (PSRAM). */
bool engine_init(void);

/* Switches to a patch: notes stop, and the effects start clean. */
void engine_load_patch(const odin_patch_t *patch);

void engine_note_on(int note, int velocity);
void engine_note_off(int note);
void engine_all_notes_off(void);

void engine_set_modwheel(float value);  /* 0-1 */
void engine_set_pitchbend(float value); /* -1 to 1 */
void engine_set_xy(float x, float y);   /* 0-1 */
void engine_set_bpm(float bpm);         /* for synced LFOs and effects */

/* Renders mono 44.1 kHz audio. */
void engine_render(int16_t *pcm, size_t frames);

/* Voices sounding now (for the screen). */
int engine_active_voices(void);
