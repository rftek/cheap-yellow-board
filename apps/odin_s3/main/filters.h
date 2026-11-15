#pragma once

/* Odin 2's voice filters: the ladder (LP, BP and HP, 24 and 12 dB), SEM 12,
   Korg 35 (LP and HP) and diode ladder. They are Odin's virtual analog
   designs, after Will Pirkle's "Designing Software Synthesizer Plug-Ins in
   C++", ported from Odin 2 (Copyright (C) 2020-2021 TheWaveWarden,
   GPL-3.0-or-later) to float, with coefficients worked out at control
   rate. */

#include <stdint.h>

#include "dsp.h"

typedef struct {
    uint8_t type; /* ODIN_FILTER_* */
    /* State. */
    va_one_pole_t pole[4];
    float sem_z1, sem_z2;
    /* Coefficients from filter_update. */
    float k;      /* ladder, Korg and diode feedback */
    float gamma;  /* ladder G^4, diode gamma */
    float alpha0; /* ladder, SEM, Korg */
    float sem_alpha, sem_rho, sem_r;
    float mix[5]; /* ladder output mix */
    float sg[4];  /* diode */
    float korg_k;
    /* Settings. */
    float res;        /* 0-1 */
    float overdrive;  /* saturation * 2 */
    float transition; /* SEM */
    float res_mod;    /* set before filter_update */
    float sat_mod;
    float transition_mod;
} odin_filter_t;

void filter_set_type(odin_filter_t *filter, int type);
void filter_reset(odin_filter_t *filter);

/* Works out the coefficients for a cutoff, with res_mod applied. */
void filter_update(odin_filter_t *filter, float freq);

float filter_process(odin_filter_t *filter, float x);
