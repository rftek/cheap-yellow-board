#pragma once

/* Odin 2's distortion and effects, ported to float and mono: the
   oversampling distortion (per voice), delay, chorus, phaser and flanger.
   Odin 2 Copyright (C) 2020-2021 TheWaveWarden, GPL-3.0-or-later. */

#include <stdbool.h>
#include <stdint.h>

#include "dsp.h"

/* ---- Distortion (per voice, 3x oversampled) ---- */

typedef struct {
    float xv[10], yv[10];
    float last_input;
    float threshold_smooth;
} distortion_t;

typedef struct {
    uint8_t algorithm; /* 1 clamp, 2 fold, 3 zero, 4 sine, 5 cube */
    float threshold;   /* (1 - boost)^3 */
    float drywet;
    float threshold_mod; /* modulation, at control rate */
    float drywet_mod;
} distortion_settings_t;

void distortion_reset(distortion_t *d, const distortion_settings_t *s);
float distortion_process(distortion_t *d, const distortion_settings_t *s, float x);

/* ---- Delay ---- */

typedef struct {
    float *buffer; /* DELAY_LENGTH floats in PSRAM */
    int write;
    float time;        /* seconds */
    float time_smooth;
    float feedback, dry, wet, ducking, hp_freq;
    float time_mod, feedback_mod, dry_mod, wet_mod, hp_mod;
    float hp_alpha;
    va_one_pole_t hp_state;
    dc_block_t dc;
    float total_sum, last_mean;
} delay_t;

bool delay_init(delay_t *d);
void delay_reset(delay_t *d);
void delay_control(delay_t *d); /* at control rate */
float delay_process(delay_t *d, float x);

/* ---- Chorus ---- */

#define CHORUS_LENGTH 2048 /* 46 ms; the chorus reads at most 35 ms back */

typedef struct {
    float buffer[CHORUS_LENGTH];
    int write;
    float lfo_pos, lfo_inc;
    float amount, amount_control, drywet, feedback;
    float freq_mod, amount_mod, drywet_mod, feedback_mod;
    allpass_t ap1, ap2;
} chorus_t;

void chorus_init(chorus_t *c);
void chorus_reset(chorus_t *c);
void chorus_set_rate(chorus_t *c, float hz);
float chorus_process(chorus_t *c, float x);

/* ---- Phaser ---- */

typedef struct {
    allpass_t ap[12];
    float lfo_pos, lfo_inc;
    int lfo_sign;
    float amount, base_freq, feedback, drywet;
    float rate_mod, amount_mod, freq_mod, feedback_mod, drywet_mod;
    float store_output;
} phaser_t;

void phaser_init(phaser_t *p);
void phaser_reset(phaser_t *p);
void phaser_set_rate(phaser_t *p, float hz);
void phaser_control(phaser_t *p); /* moves the LFO and the allpasses */
float phaser_process(phaser_t *p, float x);

/* ---- Flanger ---- */

#define FLANGER_LENGTH 2048

typedef struct {
    float buffer[FLANGER_LENGTH];
    int write;
    float lfo_pos, lfo_inc;
    int lfo_sign;
    float amount, feedback, drywet;
    float delay_time, delay_smooth;
    float freq_mod, amount_mod, feedback_mod, drywet_mod;
} flanger_t;

void flanger_init(flanger_t *f);
void flanger_reset(flanger_t *f);
void flanger_set_rate(flanger_t *f, float hz);
void flanger_control(flanger_t *f);
float flanger_process(flanger_t *f, float x);
