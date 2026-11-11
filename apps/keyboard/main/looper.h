#pragma once

/* The loop recorder and transport, run by the audio task only.

   A loop is 1 to 8 bars of 4/4. Notes are kept in ticks (960 to a beat),
   not frames, so the tempo can change and the loop follows. Each pass of
   REC is a take that UNDO can remove; takes layer on top of each other
   (overdub), each note on the instrument it was played with.

   Notes are recorded where the player heard the loop when they pressed
   the key: the render position minus the speaker's delay. Over AirPlay
   that is up to 2 s, so the take lines up with what the player heard, not
   with what the board was rendering. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "synth.h"

#define LOOP_TICKS_PER_BEAT 960
#define LOOP_BEATS_PER_BAR 4
#define LOOP_TICKS_PER_BAR (LOOP_TICKS_PER_BEAT * LOOP_BEATS_PER_BAR)
#define LOOP_MAX_BARS 8
#define LOOP_MAX_NOTES 1024

typedef enum {
    QUANTIZE_OFF,
    QUANTIZE_8TH,
    QUANTIZE_16TH,
    QUANTIZE_MODES,
} quantize_t;

/* Called for each loop note that starts or ends, with the frame it starts
   at in the audio stream, for the screen to light the key when the
   speaker plays it. */
typedef void (*looper_note_cb_t)(uint32_t frame, int note, bool on);

typedef struct {
    uint16_t bpm;
    bool metronome;  /* click every beat while the transport runs */
    bool count_in;   /* REC from stop clicks one bar first */
    quantize_t quantize; /* where recorded notes start */
} looper_settings_t;

void looper_init(looper_note_cb_t on_note);

/* Live keys. note_on plays the note now and, while recording, starts
   recording it; note_off ends both. */
void looper_live_on(int note, int instrument, uint32_t latency_frames);
void looper_live_off(int note, int instrument);
void looper_sustain(bool down);

/* Lifts every live key and the pedal, ending any note being recorded. */
void looper_live_all_off(void);

/* Transport. play starts the loop from bar 1; record from stop also
   starts it (after the count-in). Stopping ends the take. */
void looper_play(void);
void looper_stop(void);
void looper_record(bool on);

/* Removes the most recent take; false when there is none. */
bool looper_undo(void);
void looper_clear(void);

/* Doubles the loop up to 8 bars, copying the bars it has; from 8 back to
   1 bar, dropping the notes after bar 1. */
void looper_cycle_bars(void);

/* Renders frames: the transport moves on, loop notes and clicks play, and
   the synth's output goes into pcm. first_frame is the stream frame of
   pcm[0], for the note callback. */
void looper_render(int16_t *pcm, size_t frames, uint32_t first_frame,
                   const looper_settings_t *settings);

/* State for the screen. Read from another task, so values may be a block
   old; that is fine for drawing. */
typedef struct {
    bool playing;
    bool recording;
    bool counting_in;
    uint8_t bars;
    uint16_t notes;
    uint16_t takes;
    int32_t position; /* render position in ticks; negative in the count-in */
    int32_t start;    /* where play started: 0, or -1 bar for a count-in */
    uint32_t elapsed; /* ticks rendered since play */
} looper_state_t;

void looper_get_state(looper_state_t *state);
