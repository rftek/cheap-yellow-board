/* Odin 2's distortion and effects, ported to float and mono.

   Odin 2 Synthesizer Plugin, Copyright (C) 2020-2021 TheWaveWarden.
   This port is free software under the GNU General Public License,
   version 3 or later.

   Odin runs every effect on two channels; this port runs the left one.
   The LFOs of the phaser and flanger move at control rate. */

#include "fx.h"

#include <string.h>

#include "esp_heap_caps.h"

/* ---- Distortion ---------------------------------------------------------- */

#define THRESHOLD_MIN 0.05f
#define THRESHOLD_SMOOTHING 0.998f

void distortion_reset(distortion_t *d, const distortion_settings_t *s)
{
    memset(d, 0, sizeof(*d));
    d->threshold_smooth = s->threshold;
}

/* Odin's 3x decimation filter: a 9th-order Butterworth low pass. */
static inline float decimate_step(distortion_t *d, float x)
{
    float *xv = d->xv, *yv = d->yv;
    memmove(xv, xv + 1, 9 * sizeof(float));
    memmove(yv, yv + 1, 9 * sizeof(float));
    xv[9] = x * 0.019966841051093f;
    yv[9] = (xv[0] + xv[9]) + 9 * (xv[1] + xv[8]) + 36 * (xv[2] + xv[7]) + 84 * (xv[3] + xv[6]) +
            126 * (xv[4] + xv[5]) + (-0.0003977153f * yv[0]) + (-0.0064474617f * yv[1]) +
            (-0.0476997403f * yv[2]) + (-0.2185829743f * yv[3]) + (-0.6649234123f * yv[4]) +
            (-1.4773657709f * yv[5]) + (-2.2721421641f * yv[6]) + (-2.6598673212f * yv[7]) +
            (-1.8755960587f * yv[8]);
    return yv[9];
}

float distortion_process(distortion_t *d, const distortion_settings_t *s, float x)
{
    float up[3] = {0.66666666f * d->last_input + 0.33333333f * x,
                   0.33333333f * d->last_input + 0.66666666f * x, x};
    d->last_input = x;
    d->threshold_smooth =
        d->threshold_smooth * THRESHOLD_SMOOTHING + (1.0f - THRESHOLD_SMOOTHING) * s->threshold;
    float threshold = (d->threshold_smooth - s->threshold_mod) * (1.0f - THRESHOLD_MIN) + THRESHOLD_MIN;
    threshold = dsp_clamp(threshold, THRESHOLD_MIN, 1.0f);

    switch (s->algorithm) {
    case 1: /* clamp */
        for (int i = 0; i < 3; ++i) {
            up[i] = dsp_clamp(up[i], -threshold, threshold);
        }
        break;
    case 3: { /* zero */
        float t = 0.5f + threshold * 0.5f;
        for (int i = 0; i < 3; ++i) {
            if (up[i] > t || up[i] < -t) {
                up[i] = 0.0f;
            }
        }
        break;
    }
    case 4: /* sine, then cube: Odin's switch falls through */
        for (int i = 0; i < 3; ++i) {
            up[i] = sinf(up[i]);
        }
        /* fall through */
    case 5: /* cube */
        for (int i = 0; i < 3; ++i) {
            up[i] = up[i] * up[i] * up[i];
        }
        break;
    case 2: /* fold */
        for (int i = 0; i < 3; ++i) {
            for (int guard = 0; guard < 64 && fabsf(up[i]) > threshold; ++guard) {
                up[i] = up[i] > threshold ? 2.0f * threshold - up[i] : -2.0f * threshold - up[i];
            }
        }
        break;
    default:
        return x;
    }
    decimate_step(d, up[0]);
    decimate_step(d, up[1]);
    float y = decimate_step(d, up[2]);

    float drywet = dsp_clamp(s->drywet + s->drywet_mod, 0.0f, 1.0f);
    if (s->algorithm == 4 || s->algorithm == 5) {
        return y * drywet + x * (1.0f - drywet);
    }
    return y * drywet / threshold + x * (1.0f - drywet);
}

/* ---- Delay ------------------------------------------------------------------ */

#define DELAY_LENGTH 141120 /* 3.2 s */
#define DUCKING_LENGTH 441
#define DUCKING_SCALE 5.0f

bool delay_init(delay_t *d)
{
    memset(d, 0, sizeof(*d));
    d->buffer = heap_caps_calloc(DELAY_LENGTH, sizeof(float), MALLOC_CAP_SPIRAM);
    d->time = d->time_smooth = 1.0f;
    d->feedback = 0.45f;
    d->dry = 1.0f;
    d->wet = 0.7f;
    d->hp_freq = 80.0f;
    va_one_pole_init(&d->hp_state);
    return d->buffer != NULL;
}

void delay_reset(delay_t *d)
{
    if (d->buffer != NULL) {
        memset(d->buffer, 0, DELAY_LENGTH * sizeof(float));
    }
    d->write = 0;
    d->hp_state.z1 = 0.0f;
    d->dc = (dc_block_t){0};
    d->time_smooth = d->time;
    d->total_sum = d->last_mean = 0.0f;
}

void delay_control(delay_t *d)
{
    float freq = d->hp_freq;
    if (d->hp_mod != 0.0f) {
        freq *= dsp_semitones(d->hp_mod * 64.0f);
    }
    d->hp_alpha = one_pole_alpha(freq);
}

float delay_process(delay_t *d, float x)
{
    if (d->buffer == NULL) {
        return x;
    }
    d->time_smooth = (d->time_smooth - d->time) * 0.99994f + d->time;
    float time = d->time_smooth;
    if (d->time_mod != 0.0f) {
        time *= powf(3.0f, d->time_mod);
    }
    float back = dsp_clamp(time * DSP_RATE, 1.0f, (float)(DELAY_LENGTH - 2));
    float read = (float)d->write - back;
    if (read < 0.0f) {
        read += DELAY_LENGTH;
    }
    int i0 = (int)read;
    float frac = read - (float)i0;
    int i1 = i0 + 1 >= DELAY_LENGTH ? 0 : i0 + 1;
    float out = d->buffer[i0] + (d->buffer[i1] - d->buffer[i0]) * frac;

    float feedback = dsp_clamp(d->feedback + d->feedback_mod, 0.0f, 1.0f);
    d->buffer[d->write] = x + out * feedback;
    if (++d->write >= DELAY_LENGTH) {
        d->write = 0;
    }

    /* High pass on the echoes. */
    d->hp_state.alpha = d->hp_alpha;
    out = out - lp_pole_plain(&d->hp_state, out);

    /* Ducking: the echoes duck under the input's moving average. */
    float level = fabsf(x);
    d->total_sum += level - d->last_mean;
    d->last_mean = d->total_sum / DUCKING_LENGTH;
    float average = d->last_mean * DUCKING_SCALE;
    out *= 1.0f - (average > 1.0f ? 1.0f : average) * d->ducking;

    float wet = dsp_clamp(d->wet + d->wet_mod, 0.0f, 1.0f);
    float dry = dsp_clamp(d->dry + d->dry_mod, 0.0f, 1.0f);
    return dc_block(&d->dc, out) * wet + x * dry;
}

/* ---- Chorus ----------------------------------------------------------------- */

#define CHORUS_MIN_DISTANCE_1 0.015f
#define CHORUS_MIN_DISTANCE_2 0.011f
#define CHORUS_AMOUNT_RANGE 0.02f

void chorus_init(chorus_t *c)
{
    memset(c, 0, sizeof(*c));
    c->amount = c->amount_control = 0.16f;
    c->drywet = 1.0f;
    allpass_set(&c->ap1, 1000.0f, 1.25f);
    allpass_set(&c->ap2, 5000.0f, 1.3f);
    chorus_set_rate(c, 0.15f);
}

void chorus_reset(chorus_t *c)
{
    memset(c->buffer, 0, sizeof(c->buffer));
    c->ap1.z1 = c->ap1.z2 = c->ap2.z1 = c->ap2.z2 = 0.0f;
    c->lfo_pos = 0.0f;
    c->amount = c->amount_control;
}

void chorus_set_rate(chorus_t *c, float hz)
{
    c->lfo_inc = 2.0f * hz / DSP_RATE;
}

/* Odin's parabolic sine and its quarter-shifted twin, from a 0-2 phase. */
static inline float parabola(float pos)
{
    int sign = pos < 1.0f ? 1 : -1;
    float p = pos > 1.0f ? pos - 1.0f : pos;
    return 4.0f * p * (1.0f - p) * (float)sign;
}

static inline float chorus_tap(const chorus_t *c, float distance)
{
    float read = (float)c->write - distance * DSP_RATE;
    if (read < 0.0f) {
        read += CHORUS_LENGTH;
    }
    int i0 = (int)read;
    float frac = read - (float)i0;
    int i1 = i0 + 1 >= CHORUS_LENGTH ? 0 : i0 + 1;
    return c->buffer[i0] + (c->buffer[i1] - c->buffer[i0]) * frac;
}

float chorus_process(chorus_t *c, float x)
{
    c->amount = (c->amount - c->amount_control) * 0.995f + c->amount_control;
    c->lfo_pos += c->lfo_inc;
    while (c->lfo_pos > 2.0f) {
        c->lfo_pos -= 2.0f;
    }
    float lfo = parabola(c->lfo_pos) * 0.5f + 0.5f;
    float amount = c->amount + c->amount_mod;
    amount = amount < 0.0f ? 0.0f : amount;

    float out = allpass(&c->ap1, allpass(&c->ap2, chorus_tap(c, CHORUS_MIN_DISTANCE_1 +
                                                                   lfo * CHORUS_AMOUNT_RANGE * amount)));
    out -= chorus_tap(c, CHORUS_MIN_DISTANCE_2 + (1.0f - lfo) * CHORUS_AMOUNT_RANGE * amount);
    out += x * 0.5f;
    out *= 0.4f;

    float feedback = dsp_clamp(c->feedback + c->feedback_mod, -1.0f, 1.0f);
    c->buffer[c->write] = x + out * feedback;
    if (++c->write >= CHORUS_LENGTH) {
        c->write = 0;
    }
    float drywet = dsp_clamp(c->drywet + c->drywet_mod, 0.0f, 1.0f);
    return out * drywet + (1.0f - drywet) * x;
}

/* ---- Phaser ----------------------------------------------------------------- */

#define PHASER_MAX_LFO_AMPLITUDE 4000.0f
#define PHASER_RADIUS 1.32f

void phaser_init(phaser_t *p)
{
    memset(p, 0, sizeof(*p));
    p->lfo_sign = 1;
    p->amount = 0.3f;
    p->base_freq = 4000.0f;
    p->feedback = 0.25f * 0.97f;
    p->drywet = 0.5f;
    phaser_set_rate(p, 0.25f);
    phaser_control(p);
}

void phaser_reset(phaser_t *p)
{
    for (int i = 0; i < 12; ++i) {
        p->ap[i].z1 = p->ap[i].z2 = 0.0f;
    }
    p->lfo_pos = 0.0f;
    p->lfo_sign = 1;
    p->store_output = 0.0f;
}

void phaser_set_rate(phaser_t *p, float hz)
{
    p->lfo_inc = 2.0f * hz / DSP_RATE;
}

void phaser_control(phaser_t *p)
{
    static const float spread[12] = {-0.4f, 0.0f, 0.2f, 0.4f, -0.13f, -0.34f,
                                     -0.375f, -0.23f, 0.13f, 0.36f, -0.09f, -0.29f};
    float inc = p->lfo_inc;
    if (p->rate_mod != 0.0f) {
        inc *= powf(4.0f, p->rate_mod);
    }
    p->lfo_pos += inc * DSP_CONTROL;
    while (p->lfo_pos > 1.0f) {
        p->lfo_pos -= 1.0f;
        p->lfo_sign = -p->lfo_sign;
    }
    float lfo = 4.0f * p->lfo_pos * (1.0f - p->lfo_pos) * (float)p->lfo_sign;
    float amount = dsp_clamp(p->amount + p->amount_mod, 0.0f, 1.5f);
    float base = dsp_clamp(p->base_freq + p->freq_mod * 2000.0f, 400.0f, 8000.0f);
    float freq = base + lfo * amount * PHASER_MAX_LFO_AMPLITUDE;
    for (int i = 0; i < 12; ++i) {
        allpass_set(&p->ap[i], freq * (1.0f + spread[i]), PHASER_RADIUS);
    }
}

float phaser_process(phaser_t *p, float x)
{
    float feedback = dsp_clamp(p->feedback + p->feedback_mod, 0.0f, 0.97f);
    float y = x + p->store_output * feedback;
    /* Odin's order: 3, 2, 1, then 6, 5, 4, and so on. */
    for (int group = 0; group < 12; group += 3) {
        y = allpass(&p->ap[group], allpass(&p->ap[group + 1], allpass(&p->ap[group + 2], y)));
    }
    p->store_output = y;
    float drywet = dsp_clamp(p->drywet + p->drywet_mod * 0.5f, 0.0f, 0.5f);
    return (1.0f - drywet) * x + drywet * y;
}

/* ---- Flanger ---------------------------------------------------------------- */

#define FLANGER_BASE_TIME 0.0105f
#define FLANGER_LFO_MAX_RANGE 0.0095f

void flanger_init(flanger_t *f)
{
    memset(f, 0, sizeof(*f));
    f->lfo_sign = 1;
    f->amount = 0.3f;
    f->feedback = 0.6f;
    f->drywet = 1.0f;
    f->delay_time = f->delay_smooth = FLANGER_BASE_TIME;
    flanger_set_rate(f, 0.2f);
}

void flanger_reset(flanger_t *f)
{
    memset(f->buffer, 0, sizeof(f->buffer));
    f->write = 0;
    f->lfo_pos = 0.0f;
    f->lfo_sign = 1;
    f->delay_smooth = f->delay_time;
}

void flanger_set_rate(flanger_t *f, float hz)
{
    f->lfo_inc = 2.0f * hz / DSP_RATE;
}

void flanger_control(flanger_t *f)
{
    float inc = f->lfo_inc;
    if (f->freq_mod != 0.0f) {
        inc *= powf(4.0f, f->freq_mod * 2.0f);
    }
    f->lfo_pos += inc * DSP_CONTROL;
    while (f->lfo_pos > 1.0f) {
        f->lfo_pos -= 1.0f;
        f->lfo_sign = -f->lfo_sign;
    }
    float lfo = 4.0f * f->lfo_pos * (1.0f - f->lfo_pos) * (float)f->lfo_sign;
    float amount = dsp_clamp(f->amount + f->amount_mod, 0.0f, 1.0f);
    f->delay_time = FLANGER_BASE_TIME + lfo * amount * FLANGER_LFO_MAX_RANGE;
}

float flanger_process(flanger_t *f, float x)
{
    f->delay_smooth = (f->delay_smooth - f->delay_time) * 0.999f + f->delay_time;
    float read = (float)f->write - f->delay_smooth * DSP_RATE;
    if (read < 0.0f) {
        read += FLANGER_LENGTH;
    }
    int i0 = (int)read;
    float frac = read - (float)i0;
    int i1 = i0 + 1 >= FLANGER_LENGTH ? 0 : i0 + 1;
    float out = f->buffer[i0] + (f->buffer[i1] - f->buffer[i0]) * frac;
    float feedback = dsp_clamp(f->feedback + f->feedback_mod, -0.97f, 0.97f);
    f->buffer[f->write] = x + out * feedback;
    if (++f->write >= FLANGER_LENGTH) {
        f->write = 0;
    }
    float wet = (x + out) * 0.5f;
    float drywet = dsp_clamp(f->drywet + f->drywet_mod, 0.0f, 1.0f);
    return drywet * wet + (1.0f - drywet) * x;
}
