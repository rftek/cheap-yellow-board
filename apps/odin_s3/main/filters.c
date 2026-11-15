/* Odin 2's voice filters, ported to float.

   Odin 2 Synthesizer Plugin, Copyright (C) 2020-2021 TheWaveWarden.
   This port is free software under the GNU General Public License,
   version 3 or later. The filters are based on the diagrams and
   implementations in Will Pirkle's book "Designing Software Synthesizer
   Plug-Ins in C++". */

#include "filters.h"

#include <string.h>

#include "patch.h"

void filter_reset(odin_filter_t *filter)
{
    for (int i = 0; i < 4; ++i) {
        filter->pole[i].z1 = 0.0f;
        filter->pole[i].feedback = 0.0f;
    }
    filter->sem_z1 = filter->sem_z2 = 0.0f;
}

void filter_set_type(odin_filter_t *filter, int type)
{
    if (filter->type == type) {
        return;
    }
    filter->type = (uint8_t)type;
    for (int i = 0; i < 4; ++i) {
        va_one_pole_init(&filter->pole[i]);
    }
    filter_reset(filter);
    static const float ladder_mix[6][5] = {
        {0, 0, 0, 0, 1},   /* LP4 */
        {0, 0, 1, 0, 0},   /* LP2 */
        {0, 0, 4, -8, 4},  /* BP4 */
        {0, 2, -2, 0, 0},  /* BP2 */
        {1, -4, 6, -4, 1}, /* HP4 */
        {1, -2, 1, 0, 0},  /* HP2 */
    };
    if (type >= ODIN_FILTER_LP24 && type <= ODIN_FILTER_HP12) {
        memcpy(filter->mix, ladder_mix[type - ODIN_FILTER_LP24], sizeof(filter->mix));
    }
}

/* Odin's FastMathApproximations::tan is used only below Nyquist, where
   tanf is exact and, at control rate, cheap enough. */
static inline float prewarp(float freq)
{
    return tanf(DSP_PI * freq / DSP_RATE);
}

static void update_ladder(odin_filter_t *f, float g)
{
    float k = f->res * 3.88f + 4.0f * f->res_mod;
    f->k = dsp_clamp(k, 0.0f, 3.88f);
    float G = g / (1.0f + g);
    float one_over = 1.0f / (1.0f + g);
    for (int i = 0; i < 4; ++i) {
        f->pole[i].alpha = G;
    }
    f->pole[0].beta = G * G * G * one_over;
    f->pole[1].beta = G * G * one_over;
    f->pole[2].beta = G * one_over;
    f->pole[3].beta = one_over;
    f->gamma = G * G * G * G;
    f->alpha0 = 1.0f / (1.0f + f->k * f->gamma);
}

static void update_sem(odin_filter_t *f, float g)
{
    float resonance = 24.5f * f->res * f->res * f->res * f->res + 0.5f;
    resonance = dsp_clamp(resonance + f->res_mod * 24.5f, 0.5f, 25.0f);
    float r = 1.0f / (2.0f * resonance);
    f->sem_r = r;
    f->alpha0 = 1.0f / (1.0f + 2.0f * r * g + g * g);
    f->sem_alpha = g;
    f->sem_rho = 2.0f * r + g;
}

static void update_korg(odin_filter_t *f, float g)
{
    float G = g / (1.0f + g);
    for (int i = 0; i < 4; ++i) {
        f->pole[i].alpha = G;
    }
    /* pole 0: LPF1, 1: LPF2, 2: HPF1, 3: HPF2 */
    float k = f->res * 1.95f + 0.01f + f->res_mod * 2.0f;
    k = dsp_clamp(k, 0.01f, 1.96f);
    f->korg_k = k;
    f->alpha0 = 1.0f / (1.0f - k * G + k * G * G);
    if (f->type == ODIN_FILTER_KORG_LP) {
        f->pole[1].beta = (k - k * G) / (1.0f + g);
        f->pole[2].beta = -1.0f / (1.0f + g);
    } else {
        f->pole[3].beta = -1.0f * G / (1.0f + g);
        f->pole[0].beta = 1.0f / (1.0f + g);
    }
}

static void update_diode(odin_filter_t *f, float g)
{
    float G4 = 0.5f * g / (1.0f + g);
    float G3 = 0.5f * g / (1.0f + g - 0.5f * g * G4);
    float G2 = 0.5f * g / (1.0f + g - 0.5f * g * G3);
    float G1 = g / (1.0f + g - g * G2);
    f->gamma = G4 * G3 * G2 * G1;
    f->sg[0] = G4 * G3 * G2;
    f->sg[1] = G4 * G3;
    f->sg[2] = G4;
    f->sg[3] = 1.0f;
    float G = g / (1.0f + g);
    va_one_pole_t *p = f->pole;
    for (int i = 0; i < 4; ++i) {
        p[i].alpha = G;
    }
    p[0].beta = 1.0f / (1.0f + g - g * G2);
    p[1].beta = 1.0f / (1.0f + g - 0.5f * g * G3);
    p[2].beta = 1.0f / (1.0f + g - 0.5f * g * G4);
    p[3].beta = 1.0f / (1.0f + g);
    p[0].delta = g;
    p[1].delta = 0.5f * g;
    p[2].delta = 0.5f * g;
    p[3].delta = 0.0f;
    p[0].gamma = 1.0f + G1 * G2;
    p[1].gamma = 1.0f + G2 * G3;
    p[2].gamma = 1.0f + G3 * G4;
    p[3].gamma = 1.0f;
    p[0].epsilon = G2;
    p[1].epsilon = G3;
    p[2].epsilon = G4;
    p[3].epsilon = 0.0f;
    p[0].a0 = 1.0f;
    p[1].a0 = 0.5f;
    p[2].a0 = 0.5f;
    p[3].a0 = 0.5f;
    f->k = dsp_clamp(16.0f * f->res + f->res_mod * 16.0f, 0.0f, 16.0f);
}

void filter_update(odin_filter_t *filter, float freq)
{
    float g = prewarp(dsp_clamp(freq, 20.0f, 20000.0f));
    switch (filter->type) {
    case ODIN_FILTER_LP24:
    case ODIN_FILTER_LP12:
    case ODIN_FILTER_BP24:
    case ODIN_FILTER_BP12:
    case ODIN_FILTER_HP24:
    case ODIN_FILTER_HP12:
        update_ladder(filter, g);
        break;
    case ODIN_FILTER_SEM12:
        update_sem(filter, g);
        break;
    case ODIN_FILTER_KORG_LP:
    case ODIN_FILTER_KORG_HP:
        update_korg(filter, g);
        break;
    case ODIN_FILTER_DIODE:
        update_diode(filter, g);
        break;
    default:
        break;
    }
}

/* OdinFilterBase::applyOverdrive. */
static inline float overdrive(const odin_filter_t *f, float x, float tanh_factor)
{
    float od = f->overdrive + 2.0f * f->sat_mod;
    if (od <= 0.01f) {
        return x;
    }
    if (od < 1.0f) {
        return x * (1.0f - od) + od * dsp_tanh(tanh_factor * x);
    }
    return dsp_tanh(tanh_factor * od * x);
}

static float process_ladder(odin_filter_t *f, float x)
{
    va_one_pole_t *p = f->pole;
    float sigma = p[0].beta * p[0].z1 + p[1].beta * p[1].z1 + p[2].beta * p[2].z1 +
                  p[3].beta * p[3].z1;
    float u = (x - f->k * sigma) * f->alpha0;
    float y1 = lp_pole_plain(&p[0], u);
    float y2 = lp_pole_plain(&p[1], y1);
    float y3 = lp_pole_plain(&p[2], y2);
    float y4 = lp_pole_plain(&p[3], y3);
    const float *m = f->mix;
    float out = m[0] * u + m[1] * y1 + m[2] * y2 + m[3] * y3 + m[4] * y4;
    return overdrive(f, out, 3.5f);
}

static float process_sem(odin_filter_t *f, float x)
{
    float hpf = f->alpha0 * (x - f->sem_rho * f->sem_z1 - f->sem_z2);
    float bpf = f->sem_alpha * hpf + f->sem_z1;
    float lpf = f->sem_alpha * bpf + f->sem_z2;
    float bsf = x - 2.0f * f->sem_r * bpf;
    f->sem_z1 = f->sem_alpha * hpf + bpf;
    f->sem_z2 = f->sem_alpha * bpf + lpf;
    float t = dsp_clamp(f->transition + f->transition_mod * 2.0f, -1.0f, 1.0f);
    if (t < 0.0f) {
        return (1.0f + t) * bsf - t * lpf;
    }
    return t * hpf + (1.0f - t) * bsf;
}

static float process_korg(odin_filter_t *f, float x)
{
    va_one_pole_t *lpf1 = &f->pole[0], *lpf2 = &f->pole[1], *hpf1 = &f->pole[2],
                  *hpf2 = &f->pole[3];
    float k = f->korg_k;
    float y;
    if (f->type == ODIN_FILTER_KORG_LP) {
        float y1 = lp_pole_plain(lpf1, x);
        float s35 = lpf2->beta * lpf2->z1 + hpf1->beta * hpf1->z1;
        float u = f->alpha0 * (y1 + s35);
        y = k * lp_pole_plain(lpf2, u);
        lp_pole_plain(hpf1, y); /* the high pass only updates its state */
    } else {
        float y1 = x - lp_pole_plain(hpf1, x);
        float s35 = hpf2->beta * hpf2->z1 + lpf1->beta * lpf1->z1;
        float u = f->alpha0 * (y1 + s35);
        y = k * u;
        float h = y - lp_pole_plain(hpf2, y);
        lp_pole_plain(lpf1, h);
    }
    y /= k;
    return overdrive(f, y, 3.0f);
}

static float process_diode(odin_filter_t *f, float x)
{
    va_one_pole_t *p = f->pole;
    p[3].feedback = 0.0f;
    p[2].feedback = va_one_pole_feedback_output(&p[3]);
    p[1].feedback = va_one_pole_feedback_output(&p[2]);
    p[0].feedback = va_one_pole_feedback_output(&p[1]);
    float sigma = f->sg[0] * va_one_pole_feedback_output(&p[0]) +
                  f->sg[1] * va_one_pole_feedback_output(&p[1]) +
                  f->sg[2] * va_one_pole_feedback_output(&p[2]) +
                  f->sg[3] * va_one_pole_feedback_output(&p[3]);
    float k = f->k;
    x *= 1.0f + 0.3f * k;
    float u = (x - k * sigma) / (1.0f + k * f->gamma);
    float out = va_one_pole_lp(&p[3], va_one_pole_lp(&p[2], va_one_pole_lp(&p[1],
                                                                            va_one_pole_lp(&p[0], u))));
    return overdrive(f, out, 3.5f);
}

float filter_process(odin_filter_t *filter, float x)
{
    switch (filter->type) {
    case ODIN_FILTER_LP24:
    case ODIN_FILTER_LP12:
    case ODIN_FILTER_BP24:
    case ODIN_FILTER_BP12:
    case ODIN_FILTER_HP24:
    case ODIN_FILTER_HP12:
        return process_ladder(filter, x);
    case ODIN_FILTER_SEM12:
        return process_sem(filter, x);
    case ODIN_FILTER_KORG_LP:
    case ODIN_FILTER_KORG_HP:
        return process_korg(filter, x);
    case ODIN_FILTER_DIODE:
        return process_diode(filter, x);
    default:
        return x;
    }
}
