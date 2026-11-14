#pragma once

/* Each drum track's tone: a resonant filter, a three-band EQ and a level.
   The engine runs them on the track as it mixes, so they can be changed
   while the loop plays (a filter sweep), and they are part of the pattern,
   so an export has them too. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TONE_FILTER_OFF,
    TONE_FILTER_LOWPASS,
    TONE_FILTER_HIGHPASS,
    TONE_FILTER_BANDPASS,
    TONE_FILTERS,
} tone_filter_t;

#define TONE_CUTOFF_MAX 100    /* 0-100: 30 Hz to 18 kHz, evenly by pitch */
#define TONE_RESONANCE_MAX 100 /* 0-100: Q from 0.5 to 10 */
#define TONE_EQ_MIN (-12)      /* dB */
#define TONE_EQ_MAX 12
#define TONE_LEVEL_MIN (-24) /* dB */
#define TONE_LEVEL_MAX 6

/* Where a filter starts when it is first switched on. */
#define TONE_DEFAULT_CUTOFF 60    /* about 1.4 kHz */
#define TONE_DEFAULT_RESONANCE 25 /* Q about 1.1 */

/* All zero is flat: no filter, the EQ flat and the level at 0 dB. */
typedef struct {
    uint8_t filter;    /* tone_filter_t */
    uint8_t cutoff;    /* 0 to TONE_CUTOFF_MAX */
    uint8_t resonance; /* 0 to TONE_RESONANCE_MAX */
    int8_t low;        /* dB, a low shelf at 100 Hz */
    int8_t mid;        /* dB, a peak at 1 kHz */
    int8_t high;       /* dB, a high shelf at 6 kHz */
    int8_t level;      /* dB */
} drum_tone_t;

bool tone_is_flat(const drum_tone_t *tone);
float tone_cutoff_hz(int cutoff);
float tone_q(int resonance);

/* ---- Processing, in the audio task --------------------------------------- */

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} tone_biquad_t;

/* One track's filters. All zero is a valid start. */
typedef struct {
    drum_tone_t applied; /* the settings the coefficients are for */
    bool valid;
    bool filter_on, low_on, mid_on, high_on;
    float gain;
    tone_biquad_t filter, low, mid, high;
} tone_state_t;

/* Brings the coefficients up to tone; cheap when it hasn't changed. The
   filters keep their state, so a change while a sound rings doesn't click. */
void tone_update(tone_state_t *state, const drum_tone_t *tone);

/* Runs frames samples through the filters, EQ and level, in place. */
void tone_process(tone_state_t *state, float *samples, size_t frames);

/* Whether the filters still ring, so silence through them isn't silent yet. */
bool tone_ringing(tone_state_t *state);
