#include "tone.h"

#include <math.h>
#include <string.h>


#define TWO_PI (2.0f * (float)M_PI)
#define CUTOFF_LOW_HZ 30.0f
#define CUTOFF_HIGH_HZ 18000.0f
#define Q_LOW 0.5f
#define Q_HIGH 10.0f
#define LOW_SHELF_HZ 100.0f
#define MID_PEAK_HZ 1000.0f
#define MID_PEAK_Q 0.9f
#define HIGH_SHELF_HZ 6000.0f
/* Below this the filters' state is treated as silent. */
#define RING_FLOOR 1e-6f

bool tone_sound_flat(const tone_settings_t *tone)
{
    return tone->filter == TONE_FILTER_OFF && tone->low == 0 && tone->mid == 0 &&
           tone->high == 0 && tone->level == 0;
}

bool tone_is_flat(const tone_settings_t *tone)
{
    return tone_sound_flat(tone) && tone->pitch == 0 && tone->decay == 0;
}

float tone_cutoff_hz(int cutoff)
{
    float t = (float)cutoff / TONE_CUTOFF_MAX;
    return CUTOFF_LOW_HZ * powf(CUTOFF_HIGH_HZ / CUTOFF_LOW_HZ, t);
}

float tone_q(int resonance)
{
    float t = (float)resonance / TONE_RESONANCE_MAX;
    return Q_LOW * powf(Q_HIGH / Q_LOW, t);
}

/* ---- RBJ biquads ---------------------------------------------------------- */

static void set(tone_biquad_t *f, float b0, float b1, float b2, float a0, float a1, float a2)
{
    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;
}

static void set_filter(tone_biquad_t *f, tone_filter_t type, float freq, float q)
{
    float w0 = TWO_PI * freq / TONE_RATE;
    float cosw = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    switch (type) {
    case TONE_FILTER_LOWPASS:
        set(f, (1.0f - cosw) / 2.0f, 1.0f - cosw, (1.0f - cosw) / 2.0f, 1.0f + alpha, -2.0f * cosw,
            1.0f - alpha);
        break;
    case TONE_FILTER_HIGHPASS:
        set(f, (1.0f + cosw) / 2.0f, -(1.0f + cosw), (1.0f + cosw) / 2.0f, 1.0f + alpha,
            -2.0f * cosw, 1.0f - alpha);
        break;
    default: /* band pass, 0 dB at its peak */
        set(f, alpha, 0.0f, -alpha, 1.0f + alpha, -2.0f * cosw, 1.0f - alpha);
        break;
    }
}

static void set_peak(tone_biquad_t *f, float freq, float q, float db)
{
    float a = powf(10.0f, db / 40.0f);
    float w0 = TWO_PI * freq / TONE_RATE;
    float cosw = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    set(f, 1.0f + alpha * a, -2.0f * cosw, 1.0f - alpha * a, 1.0f + alpha / a, -2.0f * cosw,
        1.0f - alpha / a);
}

/* Shelves with a slope of 1. */
static void set_shelf(tone_biquad_t *f, bool high, float freq, float db)
{
    float a = powf(10.0f, db / 40.0f);
    float w0 = TWO_PI * freq / TONE_RATE;
    float cosw = cosf(w0);
    float k = 2.0f * sqrtf(a) * sinf(w0) / 2.0f * sqrtf(2.0f);
    float p = a + 1.0f;
    float m = a - 1.0f;
    if (high) {
        set(f, a * (p + m * cosw + k), -2.0f * a * (m + p * cosw), a * (p + m * cosw - k),
            p - m * cosw + k, 2.0f * (m - p * cosw), p - m * cosw - k);
    } else {
        set(f, a * (p - m * cosw + k), 2.0f * a * (m - p * cosw), a * (p - m * cosw - k),
            p + m * cosw + k, -2.0f * (m + p * cosw), p + m * cosw - k);
    }
}

static inline float run(tone_biquad_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* ---- Tracks ------------------------------------------------------------------ */

void tone_update(tone_state_t *state, const tone_settings_t *tone)
{
    if (state->valid && memcmp(&state->applied, tone, sizeof(*tone)) == 0) {
        return;
    }
    state->applied = *tone;
    state->valid = true;
    state->filter_on = tone->filter != TONE_FILTER_OFF && tone->filter < TONE_FILTERS;
    if (state->filter_on) {
        set_filter(&state->filter, (tone_filter_t)tone->filter, tone_cutoff_hz(tone->cutoff),
                   tone_q(tone->resonance));
    }
    state->low_on = tone->low != 0;
    if (state->low_on) {
        set_shelf(&state->low, false, LOW_SHELF_HZ, tone->low);
    }
    state->mid_on = tone->mid != 0;
    if (state->mid_on) {
        set_peak(&state->mid, MID_PEAK_HZ, MID_PEAK_Q, tone->mid);
    }
    state->high_on = tone->high != 0;
    if (state->high_on) {
        set_shelf(&state->high, true, HIGH_SHELF_HZ, tone->high);
    }
    state->gain = powf(10.0f, tone->level / 20.0f);
}

void tone_process(tone_state_t *state, float *samples, size_t frames)
{
    if (state->filter_on) {
        for (size_t i = 0; i < frames; ++i) {
            samples[i] = run(&state->filter, samples[i]);
        }
    }
    if (state->low_on) {
        for (size_t i = 0; i < frames; ++i) {
            samples[i] = run(&state->low, samples[i]);
        }
    }
    if (state->mid_on) {
        for (size_t i = 0; i < frames; ++i) {
            samples[i] = run(&state->mid, samples[i]);
        }
    }
    if (state->high_on) {
        for (size_t i = 0; i < frames; ++i) {
            samples[i] = run(&state->high, samples[i]);
        }
    }
    if (state->gain != 1.0f) {
        for (size_t i = 0; i < frames; ++i) {
            samples[i] *= state->gain;
        }
    }
}

static bool rings(tone_biquad_t *f, bool on)
{
    if (!on) {
        return false;
    }
    if (fabsf(f->z1) + fabsf(f->z2) > RING_FLOOR) {
        return true;
    }
    f->z1 = f->z2 = 0.0f; /* no denormals while it sits idle */
    return false;
}

bool tone_ringing(tone_state_t *state)
{
    bool ringing = rings(&state->filter, state->filter_on);
    ringing |= rings(&state->low, state->low_on);
    ringing |= rings(&state->mid, state->mid_on);
    ringing |= rings(&state->high, state->high_on);
    return ringing;
}
