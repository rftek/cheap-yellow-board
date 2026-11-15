#pragma once

/* An Odin 2 patch, in Odin's own units, as tools/odin_convert.py reads it
   from a .odin preset. The type and mod matrix numbers are Odin's.

   ODIN S3 is a port of the Odin 2 synthesizer's sound engine,
   Copyright (C) 2020-2021 TheWaveWarden, under the GNU GPL version 3 or
   later. See LICENSE in this folder. */

#include <stdbool.h>
#include <stdint.h>

#define ODIN_OSC_OFF 1
#define ODIN_OSC_ANALOG 2
#define ODIN_OSC_WAVETABLE 3
#define ODIN_OSC_NOISE 15

#define ODIN_FILTER_NONE 1
#define ODIN_FILTER_LP24 2
#define ODIN_FILTER_LP12 3
#define ODIN_FILTER_BP24 4
#define ODIN_FILTER_BP12 5
#define ODIN_FILTER_HP24 6
#define ODIN_FILTER_HP12 7
#define ODIN_FILTER_SEM12 15
#define ODIN_FILTER_DIODE 19
#define ODIN_FILTER_KORG_LP 24
#define ODIN_FILTER_KORG_HP 25

#define ODIN_PLAY_LEGATO 0 /* "legato" in the patch: 0 legato, 1 poly, 2 retrig */
#define ODIN_PLAY_POLY 1
#define ODIN_PLAY_RETRIG 2

#define ODIN_MOD_ROWS 9
#define ODIN_CATEGORIES 14

typedef struct {
    uint8_t type;        /* ODIN_OSC_* */
    uint8_t analog_wave; /* 0 saw, 1 square, 2 triangle, 3 sine */
    uint8_t wavetable;   /* the 2D wavetable, 0-based */
    bool pos_mod_lfo;    /* position modulated by LFO 1, else the mod envelope */
    int8_t oct;
    int8_t semi;
    bool reset; /* phase restarts with each note */
    bool sync;  /* hard sync to oscillator 1 (oscillators 2 and 3) */
    float fine; /* cents */
    float vol;  /* dB */
    float drift;
    float pulsewidth;
    float position; /* wavetable position, 0-1 */
    float pos_mod;  /* how much the envelope or LFO moves the position */
    float lp;       /* noise filters, Hz */
    float hp;
} odin_osc_patch_t;

typedef struct {
    uint8_t type; /* ODIN_FILTER_* */
    bool osc[3];  /* which oscillators feed it (filters 1 and 2) */
    bool to_amp;
    float freq; /* Hz */
    float res;  /* 0-1 */
    float gain; /* dB */
    float env;
    float kbd;
    float vel;
    float saturation;
    float sem_transition; /* -1 low pass, 0 notch, 1 high pass */
} odin_filter_patch_t;

typedef struct {
    float attack; /* seconds */
    float decay;
    float sustain; /* 0-1 */
    float release;
    bool loop;
} odin_env_patch_t;

typedef struct {
    float freq;   /* Hz */
    uint8_t wave; /* Odin's LFO menu: 0-6 shapes, 7 sample & hold, 8- more shapes */
    bool sync;
    bool reset;
    uint8_t num; /* synced: (num + 1) / denominator(den) of a bar */
    uint8_t den;
} odin_lfo_patch_t;

typedef struct {
    int16_t source;
    int16_t dest1;
    int16_t dest2;
    int16_t scale;
    float amount1;
    float amount2;
    float scale_amount;
} odin_mod_row_t;

typedef struct {
    char name[32];
    uint8_t category; /* index into odin_categories */
    odin_osc_patch_t osc[3];
    odin_filter_patch_t fil[3];
    bool fil2_fil1; /* filter 1 into filter 2 */
    odin_env_patch_t env[4]; /* amp, filter, mod, global */
    odin_lfo_patch_t lfo[4]; /* three per voice, one global */
    odin_mod_row_t mod[ODIN_MOD_ROWS];
    float amp_gain; /* dB */
    float amp_vel;
    bool dist_on;
    uint8_t dist_algo; /* 1 clamp, 2 fold, 3 zero, 4 sine, 5 cube */
    float dist_boost;
    float dist_drywet;
    float glide;
    float master; /* dB */
    uint8_t play_mode; /* ODIN_PLAY_* */
    uint8_t unison;    /* voices per note */
    float unison_detune;
    uint8_t pitchbend_range; /* semitones */
    float modwheel;
    float xy_x;
    float xy_y;
    struct {
        bool on, sync;
        uint8_t num, den;
        float time, feedback, hp, ducking, dry, wet;
    } delay;
    struct {
        bool on, sync;
        uint8_t num, den;
        float rate, amount, feedback, drywet;
    } chorus;
    struct {
        bool on, sync;
        uint8_t num, den;
        float rate, amount, freq, feedback, drywet;
    } phaser;
    struct {
        bool on, sync;
        uint8_t num, den;
        float rate, amount, feedback, drywet;
    } flanger;
    uint8_t fx_order[4]; /* slot of the delay, phaser, flanger and chorus */
} odin_patch_t;

extern const char *const odin_categories[ODIN_CATEGORIES];
extern const odin_patch_t odin_init_patch;
extern const odin_patch_t odin_presets[];
extern const int odin_preset_count;
