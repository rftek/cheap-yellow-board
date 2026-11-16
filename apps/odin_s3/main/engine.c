/* ODIN S3's sound engine: Odin 2's voices, modulation and signal flow.

   Odin 2 Synthesizer Plugin, Copyright (C) 2020-2021 TheWaveWarden.
   This port is free software under the GNU General Public License,
   version 3 or later. It follows Odin's PluginProcessorProcess.cpp,
   Voice.h, ADSR.cpp, LFO.cpp, the oscillators and ModMatrix.cpp, with
   these changes for the ESP32-S3:

   - float instead of double, and a 32-bit phase for the oscillators
   - modulation, envelopes 2-4, LFOs, pitch and filter coefficients are
     worked out every DSP_CONTROL samples; the amp envelope runs per sample
   - wavetables are int16 with a band-limited copy every 6 semitones
     (tools/odin_convert.py), read with linear interpolation as in Odin
   - mono output: pan and unison width are left out
   - hard sync restarts the oscillator without Odin's 3x oversampling
   - mod matrix sources that are audio (oscillators, filters) or MIDI
     controllers the board doesn't have are left out */

#include "engine.h"

#include <math.h>
#include <string.h>

#include "dsp.h"
#include "filters.h"
#include "fx.h"
#include "wt_index.h"

extern const int16_t wavetables_start[] asm("_binary_wavetables_bin_start");

/* ---- Modulation sources and destinations --------------------------------- */

typedef enum {
    S_ZERO,
    S_AMP_ENV,
    S_FIL_ENV,
    S_MOD_ENV,
    S_GLOBAL_ENV,
    S_LFO1,
    S_LFO2,
    S_LFO3,
    S_GLOBAL_LFO,
    S_X,
    S_Y,
    S_MODWHEEL,
    S_PITCHBEND,
    S_KEY,
    S_VELOCITY,
    S_UNISON,
    S_RANDOM,
    S_CONSTANT,
    S_COUNT,
} source_t;

/* Per oscillator: D_OSC + 7 * osc + one of these. */
enum { O_PITCH_EXP, O_PITCH_LIN, O_VOL, O_PWM, O_POSITION, O_LP, O_HP, O_COUNT };
/* Per filter: D_FIL + 8 * filter + one of these. */
enum { F_FREQ, F_RES, F_GAIN, F_ENV, F_VEL, F_KBD, F_SAT, F_SEM, F_COUNT };

typedef enum {
    D_NONE,
    D_OSC,
    D_FIL = D_OSC + 3 * O_COUNT, /* filters 1, 2 and 3 */
    D_ENV = D_FIL + 3 * F_COUNT, /* envelopes 1-4: attack, decay, sustain, release */
    D_LFO_FREQ = D_ENV + 16,     /* LFOs 1-4 */
    D_AMP_GAIN = D_LFO_FREQ + 4,
    D_AMP_VEL,
    D_DIST_BOOST,
    D_DIST_DRYWET,
    D_GLIDE,
    D_DELAY_TIME,
    D_DELAY_FEEDBACK,
    D_DELAY_HP,
    D_DELAY_DRY,
    D_DELAY_WET,
    D_PHASER_RATE,
    D_PHASER_AMOUNT,
    D_PHASER_FREQ,
    D_PHASER_FEEDBACK,
    D_PHASER_DRYWET,
    D_FLANGER_RATE,
    D_FLANGER_AMOUNT,
    D_FLANGER_FEEDBACK,
    D_FLANGER_DRYWET,
    D_CHORUS_RATE,
    D_CHORUS_AMOUNT,
    D_CHORUS_FEEDBACK,
    D_CHORUS_DRYWET,
    D_MASTER,
    D_COUNT,
} dest_t;

static int odin_source(int id)
{
    switch (id) {
    case 200: return S_AMP_ENV;
    case 201: return S_FIL_ENV;
    case 202: return S_MOD_ENV;
    case 203: return S_GLOBAL_ENV;
    case 300: return S_LFO1;
    case 301: return S_LFO2;
    case 302: return S_LFO3;
    case 303: return S_GLOBAL_LFO;
    case 400: return S_X;
    case 401: return S_Y;
    case 402: return S_MODWHEEL;
    case 403: return S_PITCHBEND;
    case 404: return S_KEY;
    case 405: return S_VELOCITY;
    case 420: return S_UNISON;
    case 999: return S_RANDOM;
    case 1000: return S_CONSTANT;
    default: return S_ZERO; /* off, or a source this port doesn't have */
    }
}

/* Odin's destination number to ours; D_NONE if the port doesn't have it. */
static int odin_dest(int id)
{
    if (id >= 0 && id < 300) {
        static const int8_t sub[100] = {[2] = O_PITCH_EXP + 1, [3] = O_PITCH_LIN + 1,
                                        [4] = O_VOL + 1,       [10] = O_PWM + 1,
                                        [20] = O_POSITION + 1, [60] = O_LP + 1,
                                        [61] = O_HP + 1};
        int s = sub[id % 100];
        return s ? D_OSC + O_COUNT * (id / 100) + s - 1 : D_NONE;
    }
    if (id >= 300 && id < 600) {
        static const int8_t sub[100] = {[1] = F_FREQ + 1, [2] = F_RES + 1, [3] = F_GAIN + 1,
                                        [4] = F_ENV + 1,  [5] = F_VEL + 1, [6] = F_KBD + 1,
                                        [7] = F_SAT + 1,  [10] = F_SEM + 1};
        int s = sub[id % 100];
        return s ? D_FIL + F_COUNT * ((id - 300) / 100) + s - 1 : D_NONE;
    }
    if (id >= 601 && id <= 634 && id % 10 >= 1 && id % 10 <= 4) {
        return D_ENV + 4 * ((id - 600) / 10) + id % 10 - 1;
    }
    if (id == 651 || id == 661 || id == 671 || id == 681) {
        return D_LFO_FREQ + (id - 651) / 10;
    }
    switch (id) {
    case 701: return D_DELAY_TIME;
    case 702: return D_DELAY_FEEDBACK;
    case 703: return D_DELAY_HP;
    case 704: return D_DELAY_DRY;
    case 705: return D_DELAY_WET;
    case 751: return D_PHASER_RATE;
    case 752: return D_PHASER_AMOUNT;
    case 753: return D_PHASER_FREQ;
    case 754: return D_PHASER_FEEDBACK;
    case 755: return D_PHASER_DRYWET;
    case 801: return D_FLANGER_RATE;
    case 802: return D_FLANGER_AMOUNT;
    case 803: return D_FLANGER_FEEDBACK;
    case 804: return D_FLANGER_DRYWET;
    case 851: return D_CHORUS_RATE;
    case 852: return D_CHORUS_AMOUNT;
    case 853: return D_CHORUS_FEEDBACK;
    case 854: return D_CHORUS_DRYWET;
    case 900: return D_AMP_GAIN;
    case 902: return D_AMP_VEL;
    case 950: return D_DIST_BOOST;
    case 951: return D_DIST_DRYWET;
    case 999: return D_GLIDE;
    case 1000: return D_MASTER;
    default: return D_NONE;
    }
}

/* Destinations outside the voices: filter 3, the global envelope and LFO,
   the effects and the master volume. */
static bool dest_is_global(int d)
{
    return (d >= D_FIL + 2 * F_COUNT && d < D_FIL + 3 * F_COUNT) ||
           (d >= D_ENV + 12 && d < D_ENV + 16) || d == D_LFO_FREQ + 3 ||
           (d >= D_DELAY_TIME && d <= D_MASTER);
}

typedef struct {
    uint8_t source, scale;
    uint8_t dest[2];
    bool global[2];
    float amount[2]; /* Odin squares the amount, keeping its sign */
    float scale_amount;
    bool active;
} mod_row_t;

/* ---- Envelopes (Odin's ADSREnvelope) -------------------------------------- */

enum { ENV_INIT = -1, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_FINISHED };
#define ENV_MIN_VALUE 0.001f

typedef struct {
    int8_t section;
    float current;
    float release_start;
    float last;
    /* Worked out by env_prepare for steps of n samples. */
    float attack_inc, decay_factor, release_factor, sustain;
    float cached_decay, cached_decay_factor, cached_release, cached_release_factor;
} env_t;

static void env_reset(env_t *e)
{
    e->section = ENV_INIT;
    e->current = 0.0f;
    e->cached_decay = e->cached_release = -1.0f;
}

static bool env_off(const env_t *e)
{
    return e->section == ENV_INIT || e->section == ENV_FINISHED;
}

static bool env_before_release(const env_t *e)
{
    return e->section >= ENV_ATTACK && e->section < ENV_RELEASE;
}

static void env_restart_legato(env_t *e)
{
    e->current = e->last;
    if (e->section == ENV_RELEASE || e->section == ENV_FINISHED) {
        e->section = ENV_ATTACK;
    }
}

static void env_restart_retrig(env_t *e)
{
    e->current = e->last;
    e->section = ENV_ATTACK;
}

static void env_release(env_t *e)
{
    if (e->section == ENV_RELEASE) {
        return;
    }
    if (e->section == ENV_DECAY) {
        e->release_start = e->current * (1.0f - e->sustain) + e->sustain;
    } else if (e->section == ENV_SUSTAIN) {
        e->release_start = e->sustain;
    } else {
        e->release_start = e->current;
    }
    e->current = 1.0f;
    e->section = ENV_RELEASE;
}

static float decay_factor(float seconds, float n, float *cached_seconds, float *cached_factor)
{
    if (seconds != *cached_seconds) {
        *cached_factor = powf(ENV_MIN_VALUE, n / (DSP_RATE * seconds));
        *cached_seconds = seconds;
    }
    return *cached_factor;
}

/* The envelope's rates for steps of n samples, with modulation. */
static void env_prepare(env_t *e, const odin_env_patch_t *p, const float *mod, float n)
{
    float attack = p->attack;
    if (mod[0] != 0.0f) {
        attack = fmaxf(attack * exp2f(3.0f * mod[0]) + mod[0] * 0.3f, 0.000001f);
    }
    e->attack_inc = n / (DSP_RATE * attack);
    float decay = p->decay;
    if (mod[1] != 0.0f) {
        decay = fmaxf(decay * exp2f(3.0f * mod[1]) + mod[1] * 0.3f, 0.000001f);
    }
    e->decay_factor = decay_factor(decay, n, &e->cached_decay, &e->cached_decay_factor);
    e->sustain = dsp_clamp(p->sustain + mod[2], 0.0f, 1.0f);
    float release = p->release;
    if (mod[3] != 0.0f) {
        release = fmaxf(release * exp2f(6.0f * mod[3]) + mod[3] * 0.3f, 0.000001f);
    }
    e->release_factor = decay_factor(release, n, &e->cached_release, &e->cached_release_factor);
}

/* One step; returns the envelope's value. */
static inline float env_step(env_t *e, bool loop)
{
    switch (e->section) {
    case ENV_INIT:
        e->current = 0.0f;
        e->section = ENV_ATTACK;
        /* fall through */
    case ENV_ATTACK:
        e->current += e->attack_inc;
        if (e->current >= 1.0f) {
            e->current = 1.0f;
            e->section = ENV_DECAY;
        }
        e->last = e->current;
        return e->last;
    case ENV_DECAY:
        e->current *= e->decay_factor;
        if (e->current < ENV_MIN_VALUE) {
            if (loop) {
                e->section = ENV_ATTACK;
                e->current = e->sustain + (1.0f - e->sustain) * e->current;
                e->last = e->sustain;
                return e->last;
            }
            e->section = ENV_SUSTAIN;
            e->current = 0.0f;
        }
        e->last = e->sustain + (1.0f - e->sustain) * e->current;
        return e->last;
    case ENV_SUSTAIN:
        if (loop) {
            e->section = ENV_ATTACK;
            e->current = e->sustain + (1.0f - e->sustain) * e->current;
        }
        e->last = e->sustain;
        return e->last;
    case ENV_RELEASE:
        e->current *= e->release_factor;
        if (e->current < ENV_MIN_VALUE) {
            e->current = 0.0f;
            e->section = ENV_FINISHED;
        }
        e->last = e->current * e->release_start;
        return e->last;
    default:
        e->last = 0.0f;
        return 0.0f;
    }
}

/* ---- LFOs (Odin's LFO) --------------------------------------------------------- */

typedef struct {
    float phase; /* 0-1 */
    float value;
    float hold; /* sample and hold */
} lfo_t;

static const int16_t *lfo_shape(int wave)
{
    /* Odin's menu: 0-6 shapes, 7 sample and hold, 8 and up shape 7 on. */
    int shape = wave < 7 ? wave : wave - 1;
    if (shape < 0 || shape >= WT_LFO_SHAPES) {
        shape = 0;
    }
    return wavetables_start + WT_LFO_OFFSET + shape * WT_LFO_LENGTH;
}

static uint32_t rng = 0x12345678;

static void lfo_step(lfo_t *l, const odin_lfo_patch_t *p, float freq)
{
    l->phase += freq * (DSP_CONTROL / DSP_RATE);
    bool wrapped = false;
    while (l->phase >= 1.0f) {
        l->phase -= 1.0f;
        wrapped = true;
    }
    if (p->wave == 7) {
        if (wrapped) {
            l->hold = dsp_random(&rng);
        }
        l->value = l->hold;
        return;
    }
    const int16_t *table = lfo_shape(p->wave);
    float position = l->phase * WT_LFO_LENGTH;
    int i0 = (int)position;
    float frac = position - (float)i0;
    int i1 = (i0 + 1) & (WT_LFO_LENGTH - 1);
    l->value = ((float)table[i0] + ((float)table[i1] - (float)table[i0]) * frac) * (1.0f / 32000.0f);
}

/* ---- Oscillators ------------------------------------------------------------- */

typedef struct {
    uint32_t phase;
    uint32_t inc;
    const int16_t *table[2]; /* the subtable(s) playing; two for the 2D wavetable */
    uint8_t shift;           /* 32 - bits */
    uint8_t bits;
    float interp;      /* 2D wavetable: between table[0] and table[1] */
    uint32_t duty;     /* square: the second saw's phase offset */
    float square_gain; /* square: Odin's scale * 0.3 */
    float freq_base, freq_target;
    float duty_smooth, position_smooth;
    float gain;
    /* Drift: a random walk to a new value every 5 seconds. */
    float drift_last, drift_next;
    int drift_counter;
    /* Noise. */
    va_one_pole_t noise_lp, noise_hp;
} osc_t;

#define DRIFT_LENGTH (5 * 44100)

static inline const int16_t *subtable(int table, int k)
{
    return wavetables_start + table * WT_TABLE_SAMPLES + wt_sub_offset[k];
}

/* The kept subtable whose harmonics a note at freq can play. */
static inline int subtable_for(float freq)
{
    if (freq < 27.5f) {
        return 0;
    }
    int k = (int)(2.0f * log2f(freq * (1.0f / 27.5f))) + 1;
    return k >= WT_SUBTABLES ? WT_SUBTABLES - 1 : k;
}

static inline float table_read(const int16_t *table, uint32_t phase, uint8_t shift, uint8_t bits)
{
    uint32_t i0 = phase >> shift;
    uint32_t i1 = (i0 + 1) & ((1u << bits) - 1);
    float frac = (float)((phase << bits) >> 8) * (1.0f / 16777216.0f);
    float a = table[i0];
    return a + ((float)table[i1] - a) * frac;
}

/* ---- Voices --------------------------------------------------------------------- */

typedef struct {
    bool active;
    bool released;
    uint8_t note;
    uint8_t velocity;
    uint32_t order; /* when it started */
    float detune_factor, unison_gain, unison_position;
    float random;
    osc_t osc[3];
    odin_filter_t fil[2];
    float fil_gain[2];
    env_t env[3];
    lfo_t lfo[3];
    distortion_t dist;
    float amp_gain_smooth;
    float amp_factor;
    float mod[D_COUNT];
    float src[S_COUNT];
} voice_t;

typedef struct {
    const odin_patch_t *patch;
    voice_t voices[ENGINE_VOICES];
    mod_row_t rows[ODIN_MOD_ROWS];
    float gmod[D_COUNT];
    float global_src[S_COUNT];
    /* Controls smoothed as Odin does, per sample there and per tick here. */
    float osc_vol[3], osc_vol_smooth[3];
    float fil_gain[3], fil_gain_smooth[3];
    float fil_freq_smooth[3];
    float master, master_smooth;
    float amp_gain;
    float pitch_multiplier[3]; /* octave, semitones and cents */
    float pitchbend, pitchbend_smooth;
    float modwheel, modwheel_smooth;
    float x, y;
    float bpm;
    /* Global envelope, LFO and filter 3. */
    env_t global_env;
    lfo_t global_lfo;
    odin_filter_t fil3;
    float fil3_gain;
    /* Notes. */
    uint32_t order;
    int last_note;
    int last_velocity;
    int newest_voice;
    uint8_t mono_stack[16];
    uint8_t mono_velocity[16];
    int mono_count;
    /* Effects. */
    distortion_settings_t dist;
    delay_t delay;
    chorus_t chorus;
    phaser_t phaser;
    flanger_t flanger;
    int fx_slots[4]; /* effect in each slot: 0 delay, 1 phaser, 2 flanger, 3 chorus, -1 */
    uint8_t active_voices;
} engine_t;

static engine_t engine;

/* Odin's unison layouts (PluginProcessor.h): the detune position of each
   voice and the gain of each. */
static const float unison_positions[ENGINE_UNISON_MAX + 1][ENGINE_UNISON_MAX] = {
    {0},
    {0.0f},
    {-1.0f, 1.0f},
    {-1.0f, 0.0f, 1.0f},
    {0.3333333f, -1.0f, 1.0f, -0.33333f}, /* {-1, -1/3, 1/3, 1} shuffled by {2, 0, 3, 1} */
};
static const float unison_gains[ENGINE_UNISON_MAX + 1] = {1.0f, 1.0f, 0.86f, 0.75f, 0.65f};

static float note_freq(int note)
{
    return 440.0f * dsp_semitones((float)(note - 69));
}

static float sync_ratio(int num, int den)
{
    static const float denominators[9] = {1, 2, 4, 8, 12, 16, 24, 32, 48};
    float d = den >= 0 && den < 9 ? denominators[den] : 16.0f;
    return (float)(num + 1) / d;
}

/* ---- Patch loading -------------------------------------------------------------- */

static void setup_mod_rows(const odin_patch_t *p)
{
    for (int r = 0; r < ODIN_MOD_ROWS; ++r) {
        const odin_mod_row_t *in = &p->mod[r];
        mod_row_t *row = &engine.rows[r];
        memset(row, 0, sizeof(*row));
        row->source = (uint8_t)odin_source(in->source);
        row->scale = (uint8_t)odin_source(in->scale);
        row->scale_amount = in->scale_amount;
        int dests[2] = {odin_dest(in->dest1), odin_dest(in->dest2)};
        float amounts[2] = {in->amount1, in->amount2};
        for (int i = 0; i < 2; ++i) {
            row->dest[i] = (uint8_t)dests[i];
            row->global[i] = dest_is_global(dests[i]);
            row->amount[i] = amounts[i] * fabsf(amounts[i]);
        }
        row->active = row->source != S_ZERO &&
                      ((row->dest[0] != D_NONE && row->amount[0] != 0.0f) ||
                       (row->dest[1] != D_NONE && row->amount[1] != 0.0f));
    }
}

static void setup_fx(const odin_patch_t *p)
{
    delay_t *d = &engine.delay;
    d->feedback = p->delay.feedback;
    d->hp_freq = p->delay.hp;
    d->ducking = p->delay.ducking;
    d->dry = p->delay.dry;
    d->wet = p->delay.wet;
    d->time = p->delay.time;
    delay_reset(d);

    chorus_t *c = &engine.chorus;
    c->amount_control = p->chorus.amount * p->chorus.amount;
    c->feedback = p->chorus.feedback;
    c->drywet = p->chorus.drywet;
    chorus_reset(c);

    phaser_t *ph = &engine.phaser;
    ph->amount = p->phaser.amount;
    ph->base_freq = p->phaser.freq;
    ph->feedback = p->phaser.feedback * 0.97f;
    ph->drywet = p->phaser.drywet * 0.5f;
    phaser_reset(ph);

    flanger_t *f = &engine.flanger;
    f->amount = p->flanger.amount;
    f->feedback = dsp_clamp(p->flanger.feedback, -0.98f, 0.98f);
    f->drywet = p->flanger.drywet;
    flanger_reset(f);

    /* Slots 0-4 in order; an effect's slot is its position in the patch. */
    for (int i = 0; i < 4; ++i) {
        engine.fx_slots[i] = -1;
    }
    for (int fx = 0; fx < 4; ++fx) {
        int slot = p->fx_order[fx];
        if (slot >= 0 && slot < 4 && engine.fx_slots[slot] < 0) {
            engine.fx_slots[slot] = fx;
        } else {
            for (int s = 0; s < 4; ++s) {
                if (engine.fx_slots[s] < 0) {
                    engine.fx_slots[s] = fx;
                    break;
                }
            }
        }
    }
}

void engine_load_patch(const odin_patch_t *p)
{
    engine.patch = p;
    for (int v = 0; v < ENGINE_VOICES; ++v) {
        voice_t *voice = &engine.voices[v];
        voice->active = false;
        for (int e = 0; e < 3; ++e) {
            env_reset(&voice->env[e]);
        }
        for (int f = 0; f < 2; ++f) {
            voice->fil[f].type = 0;
            filter_set_type(&voice->fil[f], p->fil[f].type);
            voice->fil[f].res = p->fil[f].res;
            voice->fil[f].overdrive = p->fil[f].saturation * 2.0f;
            voice->fil[f].transition = p->fil[f].sem_transition;
        }
        voice->amp_gain_smooth = dsp_db_to_gain(p->amp_gain);
        for (int o = 0; o < 3; ++o) {
            osc_t *osc = &voice->osc[o];
            osc->duty_smooth = p->osc[o].pulsewidth;
            osc->position_smooth = p->osc[o].position;
            va_one_pole_init(&osc->noise_lp);
            va_one_pole_init(&osc->noise_hp);
        }
    }
    engine.fil3.type = 0;
    filter_set_type(&engine.fil3, p->fil[2].type);
    engine.fil3.res = p->fil[2].res;
    engine.fil3.overdrive = p->fil[2].saturation * 2.0f;
    engine.fil3.transition = p->fil[2].sem_transition;
    env_reset(&engine.global_env);

    for (int o = 0; o < 3; ++o) {
        engine.osc_vol[o] = engine.osc_vol_smooth[o] = dsp_db_to_gain(p->osc[o].vol);
        engine.pitch_multiplier[o] =
            dsp_semitones(12.0f * p->osc[o].oct + p->osc[o].semi + p->osc[o].fine * 0.01f);
    }
    for (int f = 0; f < 3; ++f) {
        engine.fil_gain[f] = engine.fil_gain_smooth[f] = dsp_db_to_gain(p->fil[f].gain);
        engine.fil_freq_smooth[f] = p->fil[f].freq;
    }
    engine.master = engine.master_smooth = dsp_db_to_gain(p->master);
    engine.amp_gain = dsp_db_to_gain(p->amp_gain);
    engine.modwheel = engine.modwheel_smooth = p->modwheel;
    engine.x = p->xy_x;
    engine.y = p->xy_y;

    float boost = 1.0f - p->dist_boost;
    engine.dist.algorithm = p->dist_algo;
    engine.dist.threshold = boost * boost * boost;
    engine.dist.drywet = p->dist_drywet;
    for (int v = 0; v < ENGINE_VOICES; ++v) {
        distortion_reset(&engine.voices[v].dist, &engine.dist);
    }

    setup_mod_rows(p);
    setup_fx(p);
    engine.mono_count = 0;
    engine.last_note = -1;
}

bool engine_init(void)
{
    memset(&engine, 0, sizeof(engine));
    engine.bpm = 120.0f;
    chorus_init(&engine.chorus);
    phaser_init(&engine.phaser);
    flanger_init(&engine.flanger);
    bool ok = delay_init(&engine.delay);
    engine_load_patch(&odin_init_patch);
    return ok;
}

/* ---- Notes ------------------------------------------------------------------------- */

static int unison_count(void)
{
    int u = engine.patch->unison;
    return u < 1 ? 1 : u > ENGINE_UNISON_MAX ? ENGINE_UNISON_MAX : u;
}

static void voice_start(voice_t *v, int note, int velocity, int last_note, int unison_index,
                        int unison)
{
    const odin_patch_t *p = engine.patch;
    bool envelope_off = env_off(&v->env[0]);
    bool randomize = unison > 1 && envelope_off;
    for (int o = 0; o < 3; ++o) {
        osc_t *osc = &v->osc[o];
        if (randomize) {
            dsp_random(&rng);
            osc->phase = rng;
        }
        if (p->osc[o].reset) {
            osc->phase = 0;
        }
        osc->freq_target = note_freq(note);
        osc->freq_base = note_freq(last_note);
    }
    for (int l = 0; l < 3; ++l) {
        if (p->lfo[l].reset) {
            v->lfo[l].phase = 0.0f;
        }
    }
    if (p->play_mode == ODIN_PLAY_POLY) {
        filter_reset(&v->fil[0]);
        filter_reset(&v->fil[1]);
        for (int e = 0; e < 3; ++e) {
            env_reset(&v->env[e]);
        }
    } else if (p->play_mode == ODIN_PLAY_LEGATO) {
        for (int e = 0; e < 3; ++e) {
            env_restart_legato(&v->env[e]);
        }
    } else {
        for (int e = 0; e < 3; ++e) {
            env_restart_retrig(&v->env[e]);
        }
    }
    v->active = true;
    v->released = false;
    v->note = (uint8_t)note;
    v->velocity = (uint8_t)velocity;
    v->order = ++engine.order;
    v->random = dsp_random(&rng);
    v->unison_position = unison_positions[unison][unison_index];
    v->unison_gain = unison_gains[unison];
    v->detune_factor = exp2f(v->unison_position * p->unison_detune / 12.0f);
    engine.newest_voice = (int)(v - engine.voices);
}

/* Picks a voice for a new note: a silent one, else the oldest released,
   else the oldest. */
static voice_t *free_voice(bool *taken)
{
    voice_t *best = NULL;
    int best_rank = 0;
    for (int i = 0; i < ENGINE_VOICES; ++i) {
        voice_t *v = &engine.voices[i];
        if (taken[i]) {
            continue;
        }
        int rank = !v->active ? 3 : v->released ? 2 : 1;
        if (best == NULL || rank > best_rank || (rank == best_rank && v->order < best->order)) {
            best = v;
            best_rank = rank;
        }
    }
    if (best != NULL) {
        taken[best - engine.voices] = true;
    }
    return best;
}

static void start_note(int note, int velocity)
{
    const odin_patch_t *p = engine.patch;
    int unison = unison_count();
    if (engine.last_note < 0) {
        engine.last_note = note; /* the first note doesn't glide */
    }
    if (p->play_mode == ODIN_PLAY_RETRIG) {
        env_restart_retrig(&engine.global_env);
    } else {
        env_restart_legato(&engine.global_env);
    }
    if (p->lfo[3].reset) {
        engine.global_lfo.phase = 0.0f;
    }
    bool taken[ENGINE_VOICES] = {false};
    for (int u = 0; u < unison; ++u) {
        voice_t *v;
        if (p->play_mode == ODIN_PLAY_POLY) {
            v = free_voice(taken);
        } else {
            v = &engine.voices[u]; /* mono: the same voices every note */
        }
        voice_start(v, note, velocity, engine.last_note, u, unison);
    }
    engine.last_note = note;
    engine.last_velocity = velocity;
}

void engine_note_on(int note, int velocity)
{
    if (engine.patch->play_mode != ODIN_PLAY_POLY) {
        /* Last-note priority: the stack remembers what is still held. */
        int count = 0;
        for (int i = 0; i < engine.mono_count; ++i) {
            if (engine.mono_stack[i] != note) {
                engine.mono_stack[count] = engine.mono_stack[i];
                engine.mono_velocity[count++] = engine.mono_velocity[i];
            }
        }
        if (count == (int)sizeof(engine.mono_stack)) {
            memmove(engine.mono_stack, engine.mono_stack + 1, count - 1);
            memmove(engine.mono_velocity, engine.mono_velocity + 1, count - 1);
            --count;
        }
        engine.mono_stack[count] = (uint8_t)note;
        engine.mono_velocity[count++] = (uint8_t)velocity;
        engine.mono_count = count;
    }
    start_note(note, velocity);
}

static void release_voice(voice_t *v)
{
    for (int e = 0; e < 3; ++e) {
        env_release(&v->env[e]);
    }
    v->released = true;
}

static void check_end_global_env(void)
{
    for (int i = 0; i < ENGINE_VOICES; ++i) {
        if (engine.voices[i].active && env_before_release(&engine.voices[i].env[0])) {
            return;
        }
    }
    env_release(&engine.global_env);
}

void engine_note_off(int note)
{
    if (engine.patch->play_mode != ODIN_PLAY_POLY) {
        int count = 0;
        bool was_top = engine.mono_count > 0 && engine.mono_stack[engine.mono_count - 1] == note;
        for (int i = 0; i < engine.mono_count; ++i) {
            if (engine.mono_stack[i] != note) {
                engine.mono_stack[count] = engine.mono_stack[i];
                engine.mono_velocity[count++] = engine.mono_velocity[i];
            }
        }
        engine.mono_count = count;
        if (!was_top) {
            return;
        }
        if (count > 0) {
            /* Back to the note still held, as Odin does. */
            start_note(engine.mono_stack[count - 1], engine.mono_velocity[count - 1]);
            return;
        }
    }
    for (int i = 0; i < ENGINE_VOICES; ++i) {
        voice_t *v = &engine.voices[i];
        if (v->active && !v->released && v->note == note) {
            release_voice(v);
        }
    }
    check_end_global_env();
}

void engine_all_notes_off(void)
{
    engine.mono_count = 0;
    for (int i = 0; i < ENGINE_VOICES; ++i) {
        if (engine.voices[i].active) {
            release_voice(&engine.voices[i]);
        }
    }
    env_release(&engine.global_env);
}

void engine_set_modwheel(float value)
{
    engine.modwheel = dsp_clamp(value, 0.0f, 1.0f);
}

void engine_set_pitchbend(float value)
{
    engine.pitchbend = dsp_clamp(value, -1.0f, 1.0f);
}

void engine_set_xy(float x, float y)
{
    engine.x = dsp_clamp(x, 0.0f, 1.0f);
    engine.y = dsp_clamp(y, 0.0f, 1.0f);
}

void engine_set_bpm(float bpm)
{
    engine.bpm = bpm < 20.0f ? 20.0f : bpm;
}

int engine_active_voices(void)
{
    return engine.active_voices;
}

/* ---- Control rate ------------------------------------------------------------------ */

static float lfo_freq(const odin_lfo_patch_t *p, float mod)
{
    float freq = p->sync ? engine.bpm / sync_ratio(p->num, p->den) / 240.0f : p->freq;
    if (mod != 0.0f) {
        freq *= dsp_semitones(mod * 48.0f);
    }
    return freq > 20480.0f ? 20480.0f : freq;
}

static void apply_mod_matrix(void)
{
    memset(engine.gmod, 0, sizeof(engine.gmod));
    const float *recent = engine.voices[engine.newest_voice].src;
    for (int i = 0; i < ENGINE_VOICES; ++i) {
        if (engine.voices[i].active) {
            memset(engine.voices[i].mod, 0, sizeof(engine.voices[i].mod));
        }
    }
    for (int r = 0; r < ODIN_MOD_ROWS; ++r) {
        const mod_row_t *row = &engine.rows[r];
        if (!row->active) {
            continue;
        }
        for (int d = 0; d < 2; ++d) {
            if (row->dest[d] == D_NONE) {
                continue;
            }
            if (row->global[d]) {
                float s = recent[row->source] * row->amount[d];
                if (row->scale != S_ZERO) {
                    float sv = recent[row->scale];
                    s *= row->scale_amount >= 0.0f ? 1.0f + (sv - 1.0f) * row->scale_amount
                                                   : 1.0f + fabsf(sv) * row->scale_amount;
                }
                engine.gmod[row->dest[d]] += s;
                continue;
            }
            for (int i = 0; i < ENGINE_VOICES; ++i) {
                voice_t *v = &engine.voices[i];
                if (!v->active) {
                    continue;
                }
                float s = v->src[row->source] * row->amount[d];
                if (row->scale != S_ZERO) {
                    float sv = v->src[row->scale];
                    s *= row->scale_amount >= 0.0f ? 1.0f + (sv - 1.0f) * row->scale_amount
                                                   : 1.0f + fabsf(sv) * row->scale_amount;
                }
                v->mod[row->dest[d]] += s;
            }
        }
    }
}

static void osc_control(voice_t *v, int o, const float *mod, float glide)
{
    const odin_osc_patch_t *p = &engine.patch->osc[o];
    osc_t *osc = &v->osc[o];
    const float *m = &mod[D_OSC + O_COUNT * o];
    osc->gain = dsp_mod_gain(engine.osc_vol_smooth[o], m[O_VOL]);
    if (p->type == ODIN_OSC_NOISE) {
        osc->noise_lp.alpha = one_pole_alpha(p->lp * (m[O_LP] != 0.0f ? dsp_semitones(m[O_LP] * 64.0f) : 1.0f));
        osc->noise_hp.alpha = one_pole_alpha(p->hp * (m[O_HP] != 0.0f ? dsp_semitones(m[O_HP] * 64.0f) : 1.0f));
        return;
    }

    /* Glide: Odin moves the base frequency towards the note every sample. */
    if (glide < 0.01f) {
        osc->freq_base = osc->freq_target;
    } else {
        float g = powf(0.9985f + glide * 0.0014f, DSP_CONTROL);
        osc->freq_base = osc->freq_target + (osc->freq_base - osc->freq_target) * g;
    }
    float freq = osc->freq_base * engine.pitch_multiplier[o] * v->detune_factor;
    float semitones = engine.pitchbend_smooth * engine.patch->pitchbend_range + m[O_PITCH_EXP] * 24.0f;
    if (p->type == ODIN_OSC_ANALOG && p->drift != 0.0f) {
        osc->drift_counter += DSP_CONTROL;
        if (osc->drift_counter > DRIFT_LENGTH) {
            osc->drift_counter = 0;
            osc->drift_last = osc->drift_next;
            osc->drift_next = dsp_random(&rng);
        }
        float drift = (osc->drift_next - osc->drift_last) * (float)osc->drift_counter / DRIFT_LENGTH +
                      osc->drift_last;
        semitones += drift * p->drift * 0.2f;
    }
    if (semitones != 0.0f) {
        freq *= dsp_semitones(semitones);
    }
    freq += m[O_PITCH_LIN] * freq * 2.0f;
    freq = fabsf(dsp_clamp(freq, -20480.0f, 20480.0f));
    osc->inc = (uint32_t)(freq * (4294967296.0f / DSP_RATE));

    int k = subtable_for(freq);
    osc->bits = wt_sub_bits[k];
    osc->shift = (uint8_t)(32 - osc->bits);
    if (p->type == ODIN_OSC_ANALOG) {
        static const uint8_t tables[4] = {WT_FATSAW, WT_FATSAW, WT_TRIANGLE, WT_SINE};
        osc->table[0] = subtable(tables[p->analog_wave & 3], k);
        if (p->analog_wave == 1) {
            osc->duty_smooth = osc->duty_smooth * 0.9685f + 0.0315f * dsp_clamp(p->pulsewidth, 0.02f, 0.98f);
            float duty = osc->duty_smooth + m[O_PWM] * 0.5f;
            duty -= floorf(duty);
            osc->duty = (uint32_t)(duty * 4294967296.0f);
            osc->square_gain = 2.0f * fmaxf(1.0f - duty, duty) * 0.3f * (1.0f / 32000.0f);
        }
    } else if (p->type == ODIN_OSC_WAVETABLE) {
        osc->position_smooth += (p->position - osc->position_smooth) * 0.0159f; /* 1 - 0.999^16 */
        float pos_source = p->pos_mod_lfo ? v->lfo[0].value : v->env[2].last;
        float position = dsp_clamp(osc->position_smooth + m[O_POSITION] + p->pos_mod * pos_source, 0.0f, 1.0f);
        int left = position < 0.3333333f ? 0 : position < 0.6666666f ? 1 : 2;
        osc->interp = (position - left * 0.3333333f) * 3.0f;
        int set = p->wavetable < WT_2D_SETS ? p->wavetable : 0;
        osc->table[0] = subtable(wt_2d_sets[set][left], k);
        osc->table[1] = subtable(wt_2d_sets[set][left + 1], k);
    }
}

static float filter_freq(int f, const float *m, float env_value, int note, int velocity)
{
    const odin_filter_patch_t *p = &engine.patch->fil[f];
    float kbd = fmaxf(p->kbd + m[F_KBD], 0.0f);
    float vel = fmaxf(p->vel + m[F_VEL], 0.0f);
    float env = p->env + m[F_ENV];
    float freq = engine.fil_freq_smooth[f];
    if (m[F_FREQ] + kbd + env + vel != 0.0f) {
        freq *= dsp_semitones(m[F_FREQ] * 64.0f + kbd * (float)note +
                              (env_value * env + vel * (float)velocity / 127.0f) * 64.0f);
    }
    return dsp_clamp(freq, 20.0f, 20000.0f);
}

static void voice_control(voice_t *v)
{
    const odin_patch_t *p = engine.patch;
    const float *m = v->mod;

    /* Envelopes: the amp one runs per sample. */
    env_prepare(&v->env[0], &p->env[0], &m[D_ENV], 1.0f);
    env_prepare(&v->env[1], &p->env[1], &m[D_ENV + 4], DSP_CONTROL);
    env_prepare(&v->env[2], &p->env[2], &m[D_ENV + 8], DSP_CONTROL);
    env_step(&v->env[1], p->env[1].loop);
    env_step(&v->env[2], p->env[2].loop);
    for (int l = 0; l < 3; ++l) {
        lfo_step(&v->lfo[l], &p->lfo[l], lfo_freq(&p->lfo[l], m[D_LFO_FREQ + l]));
    }

    float glide = fminf(p->glide + m[D_GLIDE], 1.0f);
    for (int o = 0; o < 3; ++o) {
        if (p->osc[o].type != ODIN_OSC_OFF) {
            osc_control(v, o, m, glide);
        }
    }
    for (int f = 0; f < 2; ++f) {
        if (p->fil[f].type == ODIN_FILTER_NONE) {
            continue;
        }
        const float *fm = &m[D_FIL + F_COUNT * f];
        odin_filter_t *filter = &v->fil[f];
        filter->res_mod = fm[F_RES];
        filter->sat_mod = fm[F_SAT];
        filter->transition_mod = fm[F_SEM];
        filter_update(filter, filter_freq(f, fm, v->env[1].last, v->note, v->velocity));
        v->fil_gain[f] = dsp_mod_gain(engine.fil_gain_smooth[f], fm[F_GAIN]);
    }

    v->amp_gain_smooth = v->amp_gain_smooth * 0.9229f + 0.0771f * engine.amp_gain;
    float vel = dsp_clamp(p->amp_vel + m[D_AMP_VEL], 0.0f, 1.0f);
    v->amp_factor = dsp_mod_gain(v->amp_gain_smooth, m[D_AMP_GAIN]) * v->unison_gain *
                    (1.0f - vel + vel * (float)v->velocity / 127.0f);
}

static void voice_sources(voice_t *v)
{
    float *s = v->src;
    memcpy(s, engine.global_src, sizeof(v->src));
    s[S_AMP_ENV] = v->env[0].last;
    s[S_FIL_ENV] = v->env[1].last;
    s[S_MOD_ENV] = v->env[2].last;
    s[S_LFO1] = v->lfo[0].value;
    s[S_LFO2] = v->lfo[1].value;
    s[S_LFO3] = v->lfo[2].value;
    s[S_KEY] = (float)v->note / 127.0f;
    s[S_VELOCITY] = (float)v->velocity / 127.0f;
    s[S_UNISON] = v->unison_position;
    s[S_RANDOM] = v->random;
}

static void global_control(void)
{
    const odin_patch_t *p = engine.patch;
    /* Odin's per-sample smoothing (0.995, 0.998), applied per tick. */
    for (int i = 0; i < 3; ++i) {
        engine.osc_vol_smooth[i] = engine.osc_vol_smooth[i] * 0.9229f + 0.0771f * engine.osc_vol[i];
        engine.fil_gain_smooth[i] = engine.fil_gain_smooth[i] * 0.9229f + 0.0771f * engine.fil_gain[i];
        engine.fil_freq_smooth[i] = engine.fil_freq_smooth[i] * 0.9685f + 0.0315f * p->fil[i].freq;
    }
    engine.master_smooth = engine.master_smooth * 0.9229f + 0.0771f * engine.master;
    engine.pitchbend_smooth = engine.pitchbend_smooth * 0.9685f + 0.0315f * engine.pitchbend;
    engine.modwheel_smooth = engine.modwheel_smooth * 0.9685f + 0.0315f * engine.modwheel;

    float *g = engine.global_src;
    g[S_ZERO] = 0.0f;
    g[S_GLOBAL_ENV] = engine.global_env.last;
    g[S_GLOBAL_LFO] = engine.global_lfo.value;
    g[S_X] = engine.x;
    g[S_Y] = engine.y;
    g[S_MODWHEEL] = engine.modwheel_smooth;
    g[S_PITCHBEND] = engine.pitchbend_smooth;
    g[S_CONSTANT] = 1.0f;

    int active = 0;
    for (int i = 0; i < ENGINE_VOICES; ++i) {
        voice_t *v = &engine.voices[i];
        if (v->active) {
            voice_sources(v);
            ++active;
        }
    }
    engine.active_voices = (uint8_t)active;
    if (active == 0) {
        voice_sources(&engine.voices[engine.newest_voice]);
    }
    apply_mod_matrix();

    for (int i = 0; i < ENGINE_VOICES; ++i) {
        if (engine.voices[i].active) {
            voice_control(&engine.voices[i]);
        }
    }

    const float *m = engine.gmod;
    env_prepare(&engine.global_env, &p->env[3], &m[D_ENV + 12], DSP_CONTROL);
    env_step(&engine.global_env, p->env[3].loop);
    lfo_step(&engine.global_lfo, &p->lfo[3], lfo_freq(&p->lfo[3], m[D_LFO_FREQ + 3]));

    if (p->fil[2].type != ODIN_FILTER_NONE) {
        const float *fm = &m[D_FIL + 2 * F_COUNT];
        engine.fil3.res_mod = fm[F_RES];
        engine.fil3.sat_mod = fm[F_SAT];
        engine.fil3.transition_mod = fm[F_SEM];
        float env_value = engine.voices[engine.newest_voice].env[1].last;
        filter_update(&engine.fil3, filter_freq(2, fm, env_value, engine.last_note < 0 ? 0 : engine.last_note,
                                                engine.last_velocity));
        engine.fil3_gain = dsp_mod_gain(engine.fil_gain_smooth[2], fm[F_GAIN]);
    }

    delay_t *d = &engine.delay;
    if (p->delay.sync) {
        d->time = 240.0f * sync_ratio(p->delay.num, p->delay.den) / engine.bpm;
    }
    d->time_mod = m[D_DELAY_TIME];
    d->feedback_mod = m[D_DELAY_FEEDBACK];
    d->hp_mod = m[D_DELAY_HP];
    d->dry_mod = m[D_DELAY_DRY];
    d->wet_mod = m[D_DELAY_WET];
    delay_control(d);

    chorus_t *c = &engine.chorus;
    float chorus_rate = p->chorus.sync ? engine.bpm / sync_ratio(p->chorus.num, p->chorus.den) / 240.0f
                                       : p->chorus.rate;
    if (m[D_CHORUS_RATE] != 0.0f) {
        chorus_rate *= powf(4.0f, m[D_CHORUS_RATE] * 2.0f);
    }
    chorus_set_rate(c, chorus_rate);
    c->amount_mod = m[D_CHORUS_AMOUNT];
    c->feedback_mod = m[D_CHORUS_FEEDBACK];
    c->drywet_mod = m[D_CHORUS_DRYWET];

    phaser_t *ph = &engine.phaser;
    phaser_set_rate(ph, p->phaser.sync ? engine.bpm / sync_ratio(p->phaser.num, p->phaser.den) / 240.0f
                                       : p->phaser.rate);
    ph->rate_mod = m[D_PHASER_RATE];
    ph->amount_mod = m[D_PHASER_AMOUNT];
    ph->freq_mod = m[D_PHASER_FREQ];
    ph->feedback_mod = m[D_PHASER_FEEDBACK];
    ph->drywet_mod = m[D_PHASER_DRYWET];
    if (p->phaser.on) {
        phaser_control(ph);
    }

    flanger_t *fl = &engine.flanger;
    flanger_set_rate(fl, p->flanger.sync ? engine.bpm / sync_ratio(p->flanger.num, p->flanger.den) / 240.0f
                                         : p->flanger.rate);
    fl->freq_mod = m[D_FLANGER_RATE];
    fl->amount_mod = m[D_FLANGER_AMOUNT];
    fl->feedback_mod = m[D_FLANGER_FEEDBACK];
    fl->drywet_mod = m[D_FLANGER_DRYWET];
    if (p->flanger.on) {
        flanger_control(fl);
    }
}

/* ---- Audio rate ---------------------------------------------------------------------- */

static inline float osc_sample(osc_t *osc, const odin_osc_patch_t *p)
{
    switch (p->type) {
    case ODIN_OSC_ANALOG:
        if (p->analog_wave == 1) {
            float a = table_read(osc->table[0], osc->phase, osc->shift, osc->bits);
            float b = table_read(osc->table[0], osc->phase + osc->duty, osc->shift, osc->bits);
            return (a - b) * osc->square_gain;
        }
        return table_read(osc->table[0], osc->phase, osc->shift, osc->bits) * (1.0f / 32000.0f);
    case ODIN_OSC_WAVETABLE: {
        float a = table_read(osc->table[0], osc->phase, osc->shift, osc->bits);
        float b = table_read(osc->table[1], osc->phase, osc->shift, osc->bits);
        return (a + (b - a) * osc->interp) * (1.0f / 32000.0f);
    }
    case ODIN_OSC_NOISE: {
        float x = dsp_random(&rng);
        x = lp_pole_plain(&osc->noise_lp, x);
        return x - lp_pole_plain(&osc->noise_hp, x);
    }
    default:
        return 0.0f;
    }
}

static void voice_render(voice_t *v, float *mix, int frames)
{
    const odin_patch_t *p = engine.patch;
    bool sync[3] = {false, p->osc[1].sync, p->osc[2].sync};
    bool fil_on[2] = {p->fil[0].type != ODIN_FILTER_NONE, p->fil[1].type != ODIN_FILTER_NONE};
    distortion_settings_t dist = engine.dist;
    dist.threshold_mod = v->mod[D_DIST_BOOST];
    dist.drywet_mod = v->mod[D_DIST_DRYWET];

    for (int i = 0; i < frames; ++i) {
        float out[3] = {0.0f, 0.0f, 0.0f};
        bool master_wrapped = false;
        for (int o = 0; o < 3; ++o) {
            const odin_osc_patch_t *op = &p->osc[o];
            if (op->type == ODIN_OSC_OFF) {
                continue;
            }
            osc_t *osc = &v->osc[o];
            out[o] = osc_sample(osc, op) * osc->gain;
            uint32_t before = osc->phase;
            osc->phase += osc->inc;
            if (o == 0) {
                master_wrapped = osc->phase < before;
            } else if (sync[o] && master_wrapped) {
                osc->phase = 0;
            }
        }
        float fil_in[2];
        float fil_out[2] = {0.0f, 0.0f};
        for (int f = 0; f < 2; ++f) {
            const odin_filter_patch_t *fp = &p->fil[f];
            fil_in[f] = (fp->osc[0] ? out[0] : 0.0f) + (fp->osc[1] ? out[1] : 0.0f) +
                        (fp->osc[2] ? out[2] : 0.0f);
            if (f == 1 && p->fil2_fil1) {
                fil_in[1] += fil_out[0];
            }
            if (fil_on[f]) {
                fil_out[f] = filter_process(&v->fil[f], fil_in[f]) * v->fil_gain[f];
            } else {
                fil_out[f] = fil_in[f];
            }
        }
        float x = (p->fil[0].to_amp ? fil_out[0] : 0.0f) + (p->fil[1].to_amp ? fil_out[1] : 0.0f);
        x *= v->amp_factor;
        if (p->dist_on) {
            x = distortion_process(&v->dist, &dist, x);
        }
        mix[i] += x * env_step(&v->env[0], p->env[0].loop);
    }
    if (v->env[0].section == ENV_FINISHED) {
        v->active = false;
    }
}

static inline float fx_process(int fx, float x)
{
    const odin_patch_t *p = engine.patch;
    switch (fx) {
    case 0:
        return p->delay.on ? delay_process(&engine.delay, x) : x;
    case 1:
        return p->phaser.on ? phaser_process(&engine.phaser, x) : x;
    case 2:
        return p->flanger.on ? flanger_process(&engine.flanger, x) : x;
    case 3:
        return p->chorus.on ? chorus_process(&engine.chorus, x) : x;
    default:
        return x;
    }
}

void engine_render(int16_t *pcm, size_t frames)
{
    const odin_patch_t *p = engine.patch;
    size_t done = 0;
    while (done < frames) {
        int n = frames - done < DSP_CONTROL ? (int)(frames - done) : DSP_CONTROL;
        global_control();
        float mix[DSP_CONTROL] = {0};
        for (int i = 0; i < ENGINE_VOICES; ++i) {
            if (engine.voices[i].active) {
                voice_render(&engine.voices[i], mix, n);
            }
        }
        float master = dsp_mod_gain(engine.master_smooth, engine.gmod[D_MASTER]);
        bool fil3 = p->fil[2].type != ODIN_FILTER_NONE;
        for (int i = 0; i < n; ++i) {
            float x = mix[i];
            if (fil3) {
                x = filter_process(&engine.fil3, x) * engine.fil3_gain;
            }
            for (int slot = 0; slot < 4; ++slot) {
                x = fx_process(engine.fx_slots[slot], x);
            }
            x *= master * 32767.0f;
            pcm[done + i] = (int16_t)(x > 32767.0f ? 32767 : x < -32767.0f ? -32767 : (int)x);
        }
        done += n;
    }
}
