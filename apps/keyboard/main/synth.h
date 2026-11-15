#pragma once

/* The synthesizer: eight voices, each playing one note on one of the
   instruments below, plus a metronome click, mixed to 44100 Hz mono
   16-bit audio. Everything is computed as it plays; there are no samples.
   Not thread safe: only the audio task calls it, apart from the name and
   lookup functions, which read constant tables.

   Instruments come in voices (PIANO, ORGAN, ...), each with styles, the
   way DrumLoop's sounds come in kits: PIANO has C7 GRAND, U1 UPRIGHT,
   DX7 EP and more. An instrument is one style of one voice, identified by
   a number; a recorded note keeps the instrument it was played with. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tone.h"

#define SYNTH_RATE 44100
#define SYNTH_VOICES 8

typedef enum {
    VOICE_PIANO,
    VOICE_ORGAN,
    VOICE_STRINGS,
    VOICE_SYNTH,
    VOICE_PLUCK,
    VOICE_BELL,
    VOICE_CHIP,
    VOICE_PATCH, /* an external engine's patches (synth_set_external), if any */
    VOICE_KINDS,
} voice_kind_t;

/* Who played a note, so a key lifted live never stops a note the loop
   plays, and the other way round. */
typedef enum {
    SOURCE_LIVE,
    SOURCE_LOOP,
} note_source_t;

void synth_init(void);

/* ---- Instruments ----------------------------------------------------------- */

/* The voice's name, at most 6 characters ("PIANO", "STRING"). */
const char *synth_voice_name(int voice);

/* How many styles a voice has, and the instrument number of one. */
int synth_style_count(int voice);
int synth_instrument(int voice, int style);

/* All instruments, for bounds checks. */
int synth_instrument_count(void);

/* The voice (voice_kind_t) an instrument is a style of. */
int synth_instrument_voice(int instrument);

/* An instrument's style name, at most 10 characters ("C7 GRAND"), and a
   few words about it ("BRIGHT CONCERT GRAND"). */
const char *synth_style_name(int instrument);
const char *synth_style_about(int instrument);

/* ---- Notes ----------------------------------------------------------------- */

/* Starts a note (MIDI number, 60 is middle C). When all eight voices are
   busy it takes the one that matters least: the quietest released one,
   else the oldest. */
void synth_note_on(int note, int instrument, note_source_t source);

/* Releases the note; with the sustain pedal down, a live note keeps
   sounding until the pedal comes up. */
void synth_note_off(int note, int instrument, note_source_t source);

/* The sustain pedal, for live notes. */
void synth_sustain(bool down);

/* Releases every note from a source (pedal or not). */
void synth_release_all(note_source_t source);

/* Cuts every voice at once. */
void synth_silence(void);

/* A metronome tick; accent for the first beat of a bar. */
void synth_click(bool accent);

/* An instrument's tone: a filter, EQ and level its notes are run through
   as they are mixed. The UI changes it in place; the audio task reads it. */
tone_settings_t *synth_tone(int instrument);

/* ---- An external engine --------------------------------------------------- */

/* Another synthesizer whose patches play as the styles of VOICE_PATCH, so
   the keys, the loop and an editor treat them like the built-in styles.
   Its callbacks run in the audio task, from these functions. A slot is
   one of its count patches. Keyboard registers none; Drum&Key registers
   its patch synth. */
#define SYNTH_EXTERNAL_MAX 8

typedef struct {
    int count; /* patches, up to SYNTH_EXTERNAL_MAX */
    const char *(*name)(int slot);  /* at most 10 characters */
    const char *(*about)(int slot); /* a few words */
    void (*note_on)(int note, int slot, note_source_t source);
    void (*note_off)(int note, int slot, note_source_t source);
    void (*sustain)(bool down);
    void (*release_all)(note_source_t source);
    void (*silence)(void);
    bool (*sounding)(int slot);
    /* Adds the slot's sound into mix, at full scale (1.0 clips). */
    void (*render)(int slot, float *mix, int frames);
} synth_external_t;

/* Sets the external engine, before any note. */
void synth_set_external(const synth_external_t *external);

/* Renders frames of mono audio, adding every voice and the click. */
void synth_render(int16_t *out, size_t frames);
