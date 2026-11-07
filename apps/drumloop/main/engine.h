#pragma once

/* Step sequencer and mixer. An engine renders the kit's sounds as 44100 Hz
   mono 16-bit audio: the steps of a pattern while it plays, and live hits
   whenever they come. The audio task runs one engine for the speaker; the
   export renders a loop with another. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kit.h"

#define DRUM_STEPS 16 /* sixteenth notes in a bar */
#define DRUM_MAX_BARS 16
#define DRUM_BPM_MIN 60
#define DRUM_BPM_MAX 200

typedef struct {
    /* Bit s of steps[bar][track] is step s of that bar. */
    uint16_t steps[DRUM_MAX_BARS][DRUM_TRACKS];
    uint8_t bars; /* loop length: 1, 2, 4, 8 or 16 */
    uint16_t bpm;
    uint8_t muted; /* bit per track */
} drum_pattern_t;

/* Called as each step starts, with the frame it starts at (the count of
   frames rendered before it) and its index in the loop, bar * 16 + step. */
typedef void (*drum_step_cb_t)(uint64_t frame, uint16_t step, void *context);

typedef struct {
    uint32_t pos;      /* next frame of the sound; idle once past its end */
    uint32_t fade_pos; /* the previous hit, fading out after a retrigger */
    uint16_t fade_left;
} drum_voice_t;

typedef struct {
    const drum_pattern_t *pattern; /* read as it plays, so edits apply live */
    uint64_t frame;                /* frames rendered so far */
    bool playing;
    uint16_t next_step;     /* index in the loop of the next step to start */
    double next_step_frame; /* the frame it starts at */
    drum_voice_t voices[DRUM_TRACKS];
    drum_step_cb_t on_step;
    void *context;
} drum_engine_t;

void engine_init(drum_engine_t *engine, const drum_pattern_t *pattern,
                 drum_step_cb_t on_step, void *context);

double engine_frames_per_step(uint16_t bpm);

/* Plays the pattern from its first step, starting at the next frame. */
void engine_start(drum_engine_t *engine);

/* Stops the sequencer; sounds already playing ring out. */
void engine_stop(drum_engine_t *engine);

/* Cuts every sound at once. */
void engine_silence(drum_engine_t *engine);

/* Plays one sound now, as a pad does. */
void engine_hit(drum_engine_t *engine, int track);

/* The step nearest to the moment frames_ago frames before now, as an index
   in the loop, or -1 when the pattern isn't playing. Used to record live
   hits at the step the player heard. */
int engine_quantize(const drum_engine_t *engine, uint32_t frames_ago);

/* Renders frames of mono audio. */
void engine_render(drum_engine_t *engine, int16_t *out, size_t frames);
