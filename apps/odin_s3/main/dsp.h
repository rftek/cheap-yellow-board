#pragma once

/* Small DSP pieces shared by the engine, filters and effects.

   Ported from Odin 2's audio code, Copyright (C) 2020-2021 TheWaveWarden,
   GPL-3.0-or-later. Odin computes in double; the ESP32-S3 has a
   single-precision FPU only, so everything here is float. */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define DSP_RATE 44100.0f
#define DSP_PI 3.14159265f

/* Modulation, envelopes, LFOs and filter coefficients are worked out every
   DSP_CONTROL samples (0.36 ms) instead of every sample, as Odin does. */
#define DSP_CONTROL 16

#define MINUS_12_DB_GAIN 0.251189f
#define PLUS_12_DB_GAIN 3.981072f

static inline float dsp_clamp(float x, float low, float high)
{
    return x < low ? low : x > high ? high : x;
}

/* 2^(semitones / 12). */
static inline float dsp_semitones(float semitones)
{
    return exp2f(semitones * (1.0f / 12.0f));
}

/* Odin's dB to gain, with -59.99 dB and below silent. */
static inline float dsp_db_to_gain(float db)
{
    return db <= -59.99f ? 0.0f : powf(10.0f, db * 0.05f);
}

/* JUCE's FastMathApproximations::tanh, a Pade approximant, clamped. */
static inline float dsp_tanh(float x)
{
    if (x > 4.97f) {
        return 1.0f;
    }
    if (x < -4.97f) {
        return -1.0f;
    }
    float x2 = x * x;
    float numerator = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
    float denominator = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + 28.0f * x2));
    return numerator / denominator;
}

/* Odin's volume modulation: down scales towards silence, up adds up to
   12 dB. */
static inline float dsp_mod_gain(float gain, float mod)
{
    if (mod == 0.0f) {
        return gain;
    }
    if (mod < 0.0f) {
        float g = gain * (1.0f + mod);
        return g < 0.0f ? 0.0f : g;
    }
    if (gain > MINUS_12_DB_GAIN) {
        gain *= powf(PLUS_12_DB_GAIN, mod);
    } else {
        gain += (1.0f - gain) * mod;
    }
    return gain > PLUS_12_DB_GAIN ? PLUS_12_DB_GAIN : gain;
}

/* ---- Odin's VAOnePoleFilter (Will Pirkle's virtual analog one-pole) ---- */

typedef struct {
    float alpha; /* feed forward */
    float beta;
    float gamma; /* pre-gain */
    float delta; /* feedback-in coefficient */
    float epsilon;
    float a0;
    float feedback;
    float z1;
} va_one_pole_t;

static inline void va_one_pole_init(va_one_pole_t *f)
{
    *f = (va_one_pole_t){.alpha = 1.0f, .gamma = 1.0f, .a0 = 1.0f};
}

static inline float va_one_pole_feedback_output(const va_one_pole_t *f)
{
    return f->beta * (f->z1 + f->feedback * f->delta);
}

/* Returns the low pass output; the high pass is the input minus it. */
static inline float va_one_pole_lp(va_one_pole_t *f, float x)
{
    x = x * f->gamma + f->feedback + f->epsilon * va_one_pole_feedback_output(f);
    float v = (f->a0 * x - f->z1) * f->alpha;
    float lp = v + f->z1;
    f->z1 = v + lp;
    return lp;
}

static inline float va_one_pole_hp(va_one_pole_t *f, float x)
{
    float in = x * f->gamma + f->feedback + f->epsilon * va_one_pole_feedback_output(f);
    return in - va_one_pole_lp(f, x);
}

/* The one-pole as most filters use it (gamma and a0 1, no feedback or
   epsilon): the low pass output. */
static inline float lp_pole_plain(va_one_pole_t *p, float x)
{
    float v = (x - p->z1) * p->alpha;
    float lp = v + p->z1;
    p->z1 = v + lp;
    return lp;
}

/* Alpha of a plain one-pole, for the noise and delay filters. */
static inline float one_pole_alpha(float freq)
{
    float g = tanf(DSP_PI * dsp_clamp(freq, 20.0f, 20000.0f) / DSP_RATE);
    return g / (1.0f + g);
}

/* ---- Odin's DCBlockingFilter ---- */

typedef struct {
    float x1, y1;
} dc_block_t;

static inline float dc_block(dc_block_t *f, float x)
{
    float y = x - f->x1 + 0.995f * f->y1;
    f->x1 = x;
    f->y1 = y;
    return y;
}

/* ---- Odin's BiquadAllpass (direct form 2) ---- */

typedef struct {
    float a1, a2, b0, b1; /* b2 is 1 */
    float z1, z2;
} allpass_t;

static inline void allpass_set(allpass_t *f, float freq, float radius)
{
    float b1 = -2.0f / radius * cosf(freq * (2.0f * DSP_PI / DSP_RATE));
    float b0 = 1.0f / (radius * radius);
    f->b1 = b1;
    f->a1 = b1;
    f->b0 = b0;
    f->a2 = b0;
}

static inline float allpass(allpass_t *f, float x)
{
    float left = x - f->z2 * f->a2 - f->z1 * f->a1;
    float y = f->z2 + f->b1 * f->z1 + f->b0 * left;
    f->z2 = f->z1;
    f->z1 = left;
    return y;
}

/* A fast uniform random number in [-1, 1). */
static inline float dsp_random(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return (float)(int32_t)*state * (1.0f / 2147483648.0f);
}
