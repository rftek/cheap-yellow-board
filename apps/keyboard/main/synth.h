#pragma once

/* The synthesizer: eight voices, each playing one note on one of the
   instruments below, plus a metronome click, mixed to 44100 Hz mono
   16-bit audio. Everything is computed as it plays; there are no samples.
   Not thread safe: only the audio task calls it. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SYNTH_RATE 44100
#define SYNTH_VOICES 8

typedef enum {
    INSTRUMENT_EPIANO,  /* FM electric piano: bright attack, long decay */
    INSTRUMENT_ORGAN,   /* drawbar organ: sine harmonics, held */
    INSTRUMENT_STRINGS, /* two detuned saws, slow attack and release */
    INSTRUMENT_SYNTH,   /* saw and square through a sweeping low-pass */
    INSTRUMENT_PLUCK,   /* plucked string (Karplus-Strong) */
    INSTRUMENT_BELL,    /* FM bell: inharmonic, rings on */
    INSTRUMENT_CHIP,    /* pulse wave, like an 8-bit console */
    INSTRUMENTS,
} instrument_t;

/* Who played a note, so a key lifted live never stops a note the loop
   plays, and the other way round. */
typedef enum {
    SOURCE_LIVE,
    SOURCE_LOOP,
} note_source_t;

void synth_init(void);

/* The instrument's name, at most 7 characters ("E.PIANO"). */
const char *synth_instrument_name(int instrument);

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

/* Renders frames of mono audio, adding every voice and the click. */
void synth_render(int16_t *out, size_t frames);
