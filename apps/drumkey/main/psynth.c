#include "psynth.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "synth.h"

#define RATE 44100.0f
#define PI_F 3.14159265f
#define VOICES 6
#define BLOCK_MAX 64 /* frames per render call: Keyboard's synth CHUNK */

/* Wavetables: 3 tables of 8 frames, each frame band-limited at 4 levels
   (256, 64, 16 and 4 harmonics), 2048 samples plus a guard sample. */
#define WT_TABLES 3
#define WT_FRAMES 8
#define WT_MIPS 4
#define WT_SIZE 2048
#define WT_ROW (WT_SIZE + 1)
static const int mip_harmonics[WT_MIPS] = {256, 64, 16, 4};

#define COMB_MAX 2048          /* comb filter delay, down to 21.5 Hz */
#define CHORUS_SIZE 2048       /* 46 ms */
#define DELAY_SIZE 52920       /* 1.2 s */
#define TAIL_FRAMES (44100 * 4) /* how long effects ring on after the notes */
#define SCOPE_SIZE 1024
#define VOICE_GAIN 0.22f

static const char *TAG = "psynth";

/* ---- State -------------------------------------------------------------------- */

typedef enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_RELEASE } env_stage_t;

typedef struct {
    float ic1, ic2; /* state-variable filter integrators */
} svf_t;

typedef struct {
    svf_t svf[3]; /* one, or three for formant */
    float *comb;
    int comb_pos;
} filter_state_t;

typedef struct {
    bool active;
    bool held;
    bool sustained;
    uint8_t note;
    uint8_t slot;
    uint8_t source;
    uint32_t age;
    float random; /* -1..1, for SOURCE_RANDOM */
    float phase[2][PSYNTH_UNISON_MAX];
    float sub_phase;
    float noise_lp;
    env_stage_t env_stage[2];
    float env[2];
    float lfo_phase[2];
    float lfo_hold[2]; /* sample-and-hold value */
    filter_state_t filter[2];
} voice_t;

typedef struct {
    float *chorus;
    int chorus_pos;
    float chorus_lfo;
    float *delay;
    int delay_pos;
    float comb[4][1400];
    int comb_pos[4];
    float comb_lp[4];
    float allpass[2][600];
    int allpass_pos[2];
    int tail; /* frames the effects still ring */
    bool fx_ready;
} slot_fx_t;

static psynth_patch_t patches[PSYNTH_SLOTS];
static voice_t voices[VOICES];
static slot_fx_t *fx[PSYNTH_SLOTS];
static float *wavetables; /* [table][frame][mip][WT_ROW] */
static float sine_table[1025];
static uint32_t note_counter;
static bool sustain_down;
static volatile uint16_t tempo = 120;
static uint32_t noise_state = 0x9E3779B9u;

static float scope[SCOPE_SIZE];
static volatile int scope_pos;
static volatile int scope_slot;

static const char *const preset_names[PSYNTH_SLOTS] = {"SUPERSAW", "REESE", "PLUCK", "WOBBLE"};
static const char *const preset_about[PSYNTH_SLOTS] = {
    "7-VOICE SAW STACK", "DETUNED SAW BASS", "BRIGHT DECAY + DELAY", "SYNCED LFO FILTER"};

/* ---- Small helpers -------------------------------------------------------------- */

static inline float clampf(float x, float low, float high)
{
    return x < low ? low : x > high ? high : x;
}

static inline float soft_clip(float x)
{
    if (x > 3.0f) {
        return 1.0f;
    }
    if (x < -3.0f) {
        return -1.0f;
    }
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

static inline float fast_sine(float phase) /* phase in cycles, 0..1 */
{
    float x = phase * 1024.0f;
    int i = (int)x;
    float frac = x - (float)i;
    i &= 1023;
    return sine_table[i] + (sine_table[i + 1] - sine_table[i]) * frac;
}

static inline float noise(void)
{
    noise_state ^= noise_state << 13;
    noise_state ^= noise_state >> 17;
    noise_state ^= noise_state << 5;
    return (float)(int32_t)noise_state * (1.0f / 2147483648.0f);
}

static inline float wrap1(float phase)
{
    return phase >= 1.0f ? phase - (float)(int)phase : phase;
}

static inline float blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

/* 0-100 to seconds, 1 ms to 8 s on a curve. */
static float env_time(int value)
{
    return 0.001f * powf(8000.0f, (float)value / 100.0f);
}

static float cutoff_hz(float value) /* 0..1 */
{
    return 30.0f * powf(600.0f, clampf(value, 0.0f, 1.0f));
}

/* ---- Wavetables ---------------------------------------------------------------- */

static float *wt_row(int table, int frame, int mip)
{
    return wavetables + (((size_t)table * WT_FRAMES + frame) * WT_MIPS + mip) * WT_ROW;
}

/* The amplitude of harmonic h in a frame (t = 0..1 across the frames). */
static float spectrum(int table, float t, int h)
{
    switch (table) {
    case 0: { /* BASIC: sine, triangle, saw, square */
        float sine = h == 1 ? 1.0f : 0.0f;
        float tri = (h & 1) ? ((((h - 1) / 2) & 1) ? -1.0f : 1.0f) * 8.0f / (PI_F * PI_F * h * h) : 0.0f;
        float saw = 2.0f / (PI_F * h);
        float square = (h & 1) ? 4.0f / (PI_F * h) : 0.0f;
        float x = t * 3.0f;
        if (x < 1.0f) {
            return sine + (tri - sine) * x;
        }
        if (x < 2.0f) {
            return tri + (saw - tri) * (x - 1.0f);
        }
        return saw + (square - saw) * (x - 2.0f);
    }
    case 1: { /* VOCAL: formants of A E I O U over a 110 Hz voice */
        static const float formants[5][3] = {
            {800, 1150, 2900}, {400, 1600, 2700}, {350, 2300, 3000}, {450, 800, 2830}, {325, 700, 2530}};
        float v = t * 4.0f;
        int a = (int)v;
        a = a > 3 ? 3 : a;
        float w = v - (float)a;
        float f = 110.0f * h;
        float amp = 0.0f;
        for (int k = 0; k < 3; ++k) {
            float center = formants[a][k] + (formants[a + 1][k] - formants[a][k]) * w;
            float width = 90.0f + 40.0f * k;
            float d = (f - center) / width;
            amp += expf(-d * d) * (k == 0 ? 1.0f : 0.6f);
        }
        return amp / sqrtf((float)h) + (h == 1 ? 0.3f : 0.0f);
    }
    default: { /* DIGITAL: a resonant peak sweeping up the harmonics */
        float center = 1.0f + t * 6.0f; /* in octaves above the fundamental */
        float d = log2f((float)h) - center;
        return (1.0f / h) * (0.3f + 3.0f * expf(-d * d * 4.0f));
    }
    }
}

static bool build_wavetables(void)
{
    size_t bytes = sizeof(float) * WT_TABLES * WT_FRAMES * WT_MIPS * WT_ROW;
    wavetables = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (wavetables == NULL) {
        return false;
    }
    for (int table = 0; table < WT_TABLES; ++table) {
        for (int frame = 0; frame < WT_FRAMES; ++frame) {
            float t = (float)frame / (WT_FRAMES - 1);
            float peak = 0.0f;
            for (int mip = 0; mip < WT_MIPS; ++mip) {
                float *row = wt_row(table, frame, mip);
                for (int h = 1; h <= mip_harmonics[mip]; ++h) {
                    float amp = spectrum(table, t, h);
                    if (fabsf(amp) < 1e-5f) {
                        continue;
                    }
                    /* sin(2 pi h i / N) from the sine table, stepping by h. */
                    for (int i = 0; i < WT_SIZE; ++i) {
                        int index = (int)(((uint32_t)i * (uint32_t)h * 1024u / WT_SIZE) & 1023u);
                        row[i] += amp * sine_table[index];
                    }
                }
                if (mip == 0) {
                    for (int i = 0; i < WT_SIZE; ++i) {
                        peak = fmaxf(peak, fabsf(row[i]));
                    }
                }
            }
            float scale = peak > 0.0f ? 0.9f / peak : 1.0f;
            for (int mip = 0; mip < WT_MIPS; ++mip) {
                float *row = wt_row(table, frame, mip);
                for (int i = 0; i < WT_SIZE; ++i) {
                    row[i] *= scale;
                }
                row[WT_SIZE] = row[0];
            }
        }
    }
    return true;
}

/* ---- Presets --------------------------------------------------------------------- */

static void load_preset(int slot)
{
    psynth_patch_t *p = &patches[slot];
    memset(p, 0, sizeof(*p));
    strncpy(p->name, preset_names[slot], PSYNTH_NAME_MAX);
    p->volume = 70;
    p->osc[0] = (psynth_osc_t){.shape = OSC_SAW, .position = 50, .unison = 1, .level = 80};
    p->osc[1] = (psynth_osc_t){.shape = OSC_SAW, .position = 50, .unison = 1, .level = 0};
    p->filter[0] = (psynth_filter_t){.type = FILTER_LOWPASS, .cutoff = 80, .resonance = 10};
    p->env[0] = (psynth_env_t){.attack = 2, .decay = 50, .sustain = 80, .release = 35};
    p->env[1] = (psynth_env_t){.attack = 0, .decay = 40, .sustain = 0, .release = 30};
    p->lfo[0] = (psynth_lfo_t){.shape = LFO_SINE, .rate = 40};
    p->lfo[1] = (psynth_lfo_t){.shape = LFO_TRIANGLE, .rate = 25};
    p->delay_time = DELAY_3_16;
    switch (slot) {
    case 0: /* SUPERSAW */
        p->osc[0] = (psynth_osc_t){.shape = OSC_SAW, .position = 50, .unison = 7, .detune = 35, .level = 80};
        p->osc[1] = (psynth_osc_t){.shape = OSC_SAW, .position = 50, .unison = 5, .detune = 25, .semi = 12, .level = 40};
        p->filter[0].cutoff = 78;
        p->chorus = 30;
        p->reverb_size = 60;
        p->reverb_mix = 25;
        break;
    case 1: /* REESE */
        p->osc[0] = (psynth_osc_t){.shape = OSC_SAW, .position = 50, .unison = 2, .detune = 18, .semi = -12, .level = 80};
        p->osc[1] = (psynth_osc_t){.shape = OSC_WT_BASIC, .position = 70, .unison = 2, .detune = 30, .semi = -12, .level = 60};
        p->sub_level = 60;
        p->filter[0] = (psynth_filter_t){.type = FILTER_LOWPASS, .cutoff = 48, .resonance = 20, .drive = 40};
        p->lfo[0] = (psynth_lfo_t){.shape = LFO_SINE, .rate = 18};
        p->mod[0] = (psynth_mod_t){.source = SOURCE_LFO1, .dest = DEST_CUTOFF1, .amount = 25};
        p->mod[1] = (psynth_mod_t){.source = SOURCE_LFO1, .dest = DEST_OSC2_POS, .amount = 40};
        p->env[0].sustain = 100;
        break;
    case 2: /* PLUCK */
        p->osc[0] = (psynth_osc_t){.shape = OSC_WT_DIGITAL, .position = 30, .unison = 3, .detune = 12, .level = 80};
        p->osc[1] = (psynth_osc_t){.shape = OSC_SQUARE, .position = 40, .unison = 1, .semi = 12, .level = 30};
        p->filter[0] = (psynth_filter_t){.type = FILTER_LOWPASS, .cutoff = 30, .resonance = 25};
        p->keytrack = 50;
        p->env[0] = (psynth_env_t){.attack = 0, .decay = 45, .sustain = 0, .release = 40};
        p->env[1] = (psynth_env_t){.attack = 0, .decay = 35, .sustain = 0, .release = 30};
        p->mod[0] = (psynth_mod_t){.source = SOURCE_MOD_ENV, .dest = DEST_CUTOFF1, .amount = 70};
        p->mod[1] = (psynth_mod_t){.source = SOURCE_MOD_ENV, .dest = DEST_OSC1_POS, .amount = 40};
        p->delay_time = DELAY_3_16;
        p->delay_feedback = 35;
        p->delay_mix = 30;
        p->reverb_size = 50;
        p->reverb_mix = 20;
        break;
    default: /* WOBBLE */
        p->osc[0] = (psynth_osc_t){.shape = OSC_SAW, .position = 50, .unison = 3, .detune = 15, .semi = -12, .level = 80};
        p->osc[1] = (psynth_osc_t){.shape = OSC_SQUARE, .position = 50, .unison = 1, .semi = -12, .level = 60};
        p->sub_level = 70;
        p->sub_shape = 1;
        p->filter[0] = (psynth_filter_t){.type = FILTER_LOWPASS, .cutoff = 30, .resonance = 55, .drive = 50};
        p->lfo[0] = (psynth_lfo_t){.shape = LFO_SINE, .rate = 50, .sync = SYNC_1_8};
        p->mod[0] = (psynth_mod_t){.source = SOURCE_LFO1, .dest = DEST_CUTOFF1, .amount = 75};
        p->env[0].sustain = 100;
        p->drive = 35;
        break;
    }
}

/* ---- Voices ---------------------------------------------------------------------- */

static void env_start(voice_t *v, int which)
{
    v->env_stage[which] = ENV_ATTACK;
}

static void env_release(voice_t *v)
{
    v->env_stage[0] = ENV_RELEASE;
    v->env_stage[1] = ENV_RELEASE;
}

/* Advances an envelope by frames; returns its level. */
static float env_step(voice_t *v, int which, const psynth_env_t *e, int frames)
{
    float level = v->env[which];
    float dt = (float)frames / RATE;
    switch (v->env_stage[which]) {
    case ENV_ATTACK:
        level += dt / env_time(e->attack);
        if (level >= 1.0f) {
            level = 1.0f;
            v->env_stage[which] = ENV_DECAY;
        }
        break;
    case ENV_DECAY: {
        float target = e->sustain / 100.0f;
        float coef = expf(-dt * 6.9f / env_time(e->decay));
        level = target + (level - target) * coef;
        break;
    }
    case ENV_RELEASE: {
        float coef = expf(-dt * 6.9f / env_time(e->release));
        level *= coef;
        if (level < 0.0005f) {
            level = 0.0f;
            v->env_stage[which] = ENV_OFF;
        }
        break;
    }
    default:
        level = 0.0f;
        break;
    }
    v->env[which] = level;
    return level;
}

static float lfo_hz(const psynth_lfo_t *lfo)
{
    static const float beats[SYNC_MODES] = {0, 4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 1.0f / 3.0f};
    if (lfo->sync != SYNC_FREE && lfo->sync < SYNC_MODES) {
        return (float)tempo / 60.0f / beats[lfo->sync];
    }
    return 0.05f * powf(600.0f, (float)lfo->rate / 100.0f);
}

/* Advances an LFO by frames; returns -1..1. */
static float lfo_step(voice_t *v, int which, const psynth_lfo_t *lfo, int frames)
{
    static const float gate[8] = {1, -1, 1, -1, 1, 1, -1, 1};
    float phase = v->lfo_phase[which] + lfo_hz(lfo) * (float)frames / RATE;
    bool wrapped = phase >= 1.0f;
    phase = wrap1(phase);
    v->lfo_phase[which] = phase;
    switch (lfo->shape) {
    case LFO_TRIANGLE:
        return phase < 0.5f ? phase * 4.0f - 1.0f : 3.0f - phase * 4.0f;
    case LFO_SAW:
        return 1.0f - phase * 2.0f;
    case LFO_SQUARE:
        return phase < 0.5f ? 1.0f : -1.0f;
    case LFO_RANDOM:
        if (wrapped) {
            v->lfo_hold[which] = noise();
        }
        return v->lfo_hold[which];
    case LFO_GATE:
        return gate[(int)(phase * 8.0f) & 7];
    case LFO_STAIRS:
        return ((float)((int)(phase * 8.0f) & 7) / 3.5f) - 1.0f;
    default:
        return fast_sine(phase);
    }
}

static voice_t *pick_voice(int note, int slot, note_source_t source)
{
    voice_t *best = NULL;
    for (int i = 0; i < VOICES; ++i) {
        voice_t *v = &voices[i];
        if (v->active && v->note == note && v->slot == slot && v->source == source) {
            return v; /* the same key again */
        }
    }
    for (int i = 0; i < VOICES; ++i) {
        if (!voices[i].active) {
            return &voices[i];
        }
    }
    /* All busy: the quietest released one, else the oldest. */
    for (int i = 0; i < VOICES; ++i) {
        voice_t *v = &voices[i];
        if (v->env_stage[0] == ENV_RELEASE && (best == NULL || v->env[0] < best->env[0])) {
            best = v;
        }
    }
    if (best != NULL) {
        return best;
    }
    best = &voices[0];
    for (int i = 1; i < VOICES; ++i) {
        if (voices[i].age < best->age) {
            best = &voices[i];
        }
    }
    return best;
}

static void ext_note_on(int note, int slot, note_source_t source)
{
    voice_t *v = pick_voice(note, slot, source);
    bool retrigger = v->active && v->note == note && v->slot == slot;
    float *combs[2] = {v->filter[0].comb, v->filter[1].comb};
    if (!retrigger) {
        memset(v, 0, sizeof(*v));
        v->filter[0].comb = combs[0];
        v->filter[1].comb = combs[1];
        for (int f = 0; f < 2; ++f) {
            if (combs[f] != NULL) {
                memset(combs[f], 0, sizeof(float) * COMB_MAX);
            }
        }
        for (int o = 0; o < 2; ++o) {
            for (int u = 0; u < PSYNTH_UNISON_MAX; ++u) {
                v->phase[o][u] = (noise() + 1.0f) * 0.5f; /* random phases */
            }
        }
    }
    v->active = true;
    v->held = true;
    v->sustained = false;
    v->note = (uint8_t)note;
    v->slot = (uint8_t)slot;
    v->source = (uint8_t)source;
    v->age = ++note_counter;
    v->random = noise();
    v->lfo_phase[0] = v->lfo_phase[1] = 0.0f;
    env_start(v, 0);
    env_start(v, 1);
}

static void ext_note_off(int note, int slot, note_source_t source)
{
    for (int i = 0; i < VOICES; ++i) {
        voice_t *v = &voices[i];
        if (!v->active || !v->held || v->note != note || v->slot != slot || v->source != source) {
            continue;
        }
        v->held = false;
        if (source == SOURCE_LIVE && sustain_down) {
            v->sustained = true;
        } else {
            env_release(v);
        }
    }
}

static void ext_sustain(bool down)
{
    sustain_down = down;
    if (down) {
        return;
    }
    for (int i = 0; i < VOICES; ++i) {
        if (voices[i].active && voices[i].sustained) {
            voices[i].sustained = false;
            env_release(&voices[i]);
        }
    }
}

static void ext_release_all(note_source_t source)
{
    for (int i = 0; i < VOICES; ++i) {
        if (voices[i].active && voices[i].source == source) {
            voices[i].held = false;
            voices[i].sustained = false;
            env_release(&voices[i]);
        }
    }
}

static void ext_silence(void)
{
    for (int i = 0; i < VOICES; ++i) {
        voices[i].active = false;
    }
    for (int s = 0; s < PSYNTH_SLOTS; ++s) {
        if (fx[s] != NULL) {
            fx[s]->tail = 0;
        }
    }
}

/* ---- Rendering a voice ------------------------------------------------------------ */

typedef struct {
    float pitch;      /* semitones */
    float pos[2];     /* 0..1 offsets */
    float detune;     /* multiplier offset */
    float cutoff[2];  /* 0..1 offsets */
    float reso1;
    float osc2_level;
    float amp;
} mods_t;

static void apply_mods(const psynth_patch_t *p, const voice_t *v, float lfo1, float lfo2,
                       float mod_env, mods_t *m)
{
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < PSYNTH_MODS; ++i) {
        const psynth_mod_t *slot = &p->mod[i];
        if (slot->source == SOURCE_NONE || slot->dest == DEST_NONE || slot->amount == 0) {
            continue;
        }
        float value;
        switch (slot->source) {
        case SOURCE_LFO1:
            value = lfo1;
            break;
        case SOURCE_LFO2:
            value = lfo2;
            break;
        case SOURCE_MOD_ENV:
            value = mod_env;
            break;
        case SOURCE_NOTE:
            value = clampf((v->note - 60) / 24.0f, -1.0f, 1.0f);
            break;
        default:
            value = v->random;
            break;
        }
        float amount = value * slot->amount / 100.0f;
        switch (slot->dest) {
        case DEST_PITCH:
            m->pitch += amount * 12.0f;
            break;
        case DEST_OSC1_POS:
            m->pos[0] += amount;
            break;
        case DEST_OSC2_POS:
            m->pos[1] += amount;
            break;
        case DEST_DETUNE:
            m->detune += amount;
            break;
        case DEST_CUTOFF1:
            m->cutoff[0] += amount * 0.6f;
            break;
        case DEST_CUTOFF2:
            m->cutoff[1] += amount * 0.6f;
            break;
        case DEST_RESO1:
            m->reso1 += amount;
            break;
        case DEST_OSC2_LEVEL:
            m->osc2_level += amount;
            break;
        case DEST_AMP:
            m->amp += amount;
            break;
        default:
            break;
        }
    }
}

/* Adds one oscillator (all its unison voices) into out. */
static void render_osc(voice_t *v, int index, const psynth_osc_t *osc, float freq, float pos,
                       float detune, float level, float *out, int n)
{
    int count = osc->unison < 1 ? 1 : osc->unison > PSYNTH_UNISON_MAX ? PSYNTH_UNISON_MAX : osc->unison;
    float gain = level / sqrtf((float)count);
    float spread_cents = (osc->detune / 100.0f) * 50.0f * (1.0f + detune);
    pos = clampf(pos, 0.0f, 1.0f);
    int table = osc->shape - OSC_WT_BASIC;
    int mip = 0;
    const float *rows[2] = {NULL, NULL};
    float frame_mix = 0.0f;
    if (table >= 0 && table < WT_TABLES && wavetables != NULL) {
        float max_harmonics = 18000.0f / freq;
        while (mip < WT_MIPS - 1 && mip_harmonics[mip] > max_harmonics) {
            ++mip;
        }
        float frame = pos * (WT_FRAMES - 1);
        int f0 = (int)frame;
        f0 = f0 > WT_FRAMES - 2 ? WT_FRAMES - 2 : f0;
        frame_mix = frame - (float)f0;
        rows[0] = wt_row(table, f0, mip);
        rows[1] = wt_row(table, f0 + 1, mip);
    }
    for (int u = 0; u < count; ++u) {
        float offset = count > 1 ? ((float)u / (count - 1) * 2.0f - 1.0f) : 0.0f;
        float dt = freq * powf(2.0f, offset * spread_cents / 1200.0f) / RATE;
        if (dt > 0.45f) {
            dt = 0.45f;
        }
        float phase = v->phase[index][u];
        switch (osc->shape) {
        case OSC_SAW:
            for (int i = 0; i < n; ++i) {
                out[i] += gain * (2.0f * phase - 1.0f - blep(phase, dt));
                phase = wrap1(phase + dt);
            }
            break;
        case OSC_SQUARE: {
            float width = 0.5f - pos * 0.45f;
            for (int i = 0; i < n; ++i) {
                float s = phase < width ? 1.0f : -1.0f;
                s += blep(phase, dt);
                float shifted = phase - width;
                s -= blep(shifted < 0.0f ? shifted + 1.0f : shifted, dt);
                out[i] += gain * s;
                phase = wrap1(phase + dt);
            }
            break;
        }
        case OSC_TRIANGLE:
            for (int i = 0; i < n; ++i) {
                out[i] += gain * (phase < 0.5f ? phase * 4.0f - 1.0f : 3.0f - phase * 4.0f);
                phase = wrap1(phase + dt);
            }
            break;
        case OSC_SINE:
            for (int i = 0; i < n; ++i) {
                out[i] += gain * fast_sine(phase);
                phase = wrap1(phase + dt);
            }
            break;
        default:
            if (rows[0] == NULL) {
                break;
            }
            for (int i = 0; i < n; ++i) {
                float x = phase * WT_SIZE;
                int j = (int)x;
                float frac = x - (float)j;
                float a = rows[0][j] + (rows[0][j + 1] - rows[0][j]) * frac;
                float b = rows[1][j] + (rows[1][j + 1] - rows[1][j]) * frac;
                out[i] += gain * (a + (b - a) * frame_mix);
                phase = wrap1(phase + dt);
            }
            break;
        }
        v->phase[index][u] = phase;
    }
}

/* A state-variable filter (trapezoidal, zero-delay feedback). */
typedef struct {
    float a1, a2, a3, k;
} svf_coefs_t;

static svf_coefs_t svf_coefs(float hz, float resonance)
{
    hz = clampf(hz, 20.0f, RATE * 0.45f);
    float g = tanf(PI_F * hz / RATE);
    float k = 2.0f - 1.94f * clampf(resonance, 0.0f, 1.0f);
    svf_coefs_t c;
    c.k = k;
    c.a1 = 1.0f / (1.0f + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}

static inline void svf_run(svf_t *s, const svf_coefs_t *c, float x, float *low, float *band,
                           float *high)
{
    float v3 = x - s->ic2;
    float v1 = c->a1 * s->ic1 + c->a2 * v3;
    float v2 = s->ic2 + c->a2 * s->ic1 + c->a3 * v3;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    *low = v2;
    *band = v1;
    *high = x - c->k * v1 - v2;
}

static void run_filter(filter_state_t *state, const psynth_filter_t *f, float cutoff,
                       float resonance, float *buf, int n)
{
    if (f->type == FILTER_OFF) {
        return;
    }
    float drive = 1.0f + f->drive * 0.06f;
    if (f->drive > 0) {
        for (int i = 0; i < n; ++i) {
            buf[i] = soft_clip(buf[i] * drive) / sqrtf(drive);
        }
    }
    float low, band, high;
    switch (f->type) {
    case FILTER_COMB: {
        if (state->comb == NULL) {
            return;
        }
        int delay = (int)(RATE / cutoff_hz(cutoff));
        delay = delay < 2 ? 2 : delay >= COMB_MAX ? COMB_MAX - 1 : delay;
        float feedback = clampf(resonance, 0.0f, 1.0f) * 0.95f;
        int pos = state->comb_pos;
        for (int i = 0; i < n; ++i) {
            int read = pos - delay;
            read += read < 0 ? COMB_MAX : 0;
            float y = buf[i] + feedback * state->comb[read];
            state->comb[pos] = y;
            pos = (pos + 1) & (COMB_MAX - 1);
            buf[i] = y * (1.0f - feedback * 0.5f);
        }
        state->comb_pos = pos;
        return;
    }
    case FILTER_FORMANT: {
        static const float vowels[5][3] = {
            {800, 1150, 2900}, {400, 1600, 2700}, {350, 2300, 3000}, {450, 800, 2830}, {325, 700, 2530}};
        float v = clampf(cutoff, 0.0f, 1.0f) * 4.0f;
        int a = (int)v;
        a = a > 3 ? 3 : a;
        float w = v - (float)a;
        svf_coefs_t c[3];
        for (int k = 0; k < 3; ++k) {
            float hz = vowels[a][k] + (vowels[a + 1][k] - vowels[a][k]) * w;
            c[k] = svf_coefs(hz, 0.6f + 0.38f * clampf(resonance, 0.0f, 1.0f));
        }
        for (int i = 0; i < n; ++i) {
            float x = buf[i];
            float y = 0.0f;
            for (int k = 0; k < 3; ++k) {
                svf_run(&state->svf[k], &c[k], x, &low, &band, &high);
                y += band * (k == 0 ? 1.0f : 0.7f);
            }
            buf[i] = y * 1.4f;
        }
        return;
    }
    default: {
        svf_coefs_t c = svf_coefs(cutoff_hz(cutoff), resonance);
        for (int i = 0; i < n; ++i) {
            svf_run(&state->svf[0], &c, buf[i], &low, &band, &high);
            switch (f->type) {
            case FILTER_HIGHPASS:
                buf[i] = high;
                break;
            case FILTER_BANDPASS:
                buf[i] = band;
                break;
            case FILTER_NOTCH:
                buf[i] = low + high;
                break;
            default:
                buf[i] = low;
                break;
            }
        }
        return;
    }
    }
}

/* Adds a voice's sound for n frames into out. */
static void render_voice(voice_t *v, const psynth_patch_t *p, float *out, int n)
{
    float lfo1 = lfo_step(v, 0, &p->lfo[0], n);
    float lfo2 = lfo_step(v, 1, &p->lfo[1], n);
    float amp_start = v->env[0];
    float amp_end = env_step(v, 0, &p->env[0], n);
    float mod_env = env_step(v, 1, &p->env[1], n);
    mods_t m;
    apply_mods(p, v, lfo1, lfo2, mod_env, &m);

    float buf[BLOCK_MAX];
    memset(buf, 0, sizeof(float) * (size_t)n);
    for (int o = 0; o < 2; ++o) {
        const psynth_osc_t *osc = &p->osc[o];
        float level = osc->level / 100.0f + (o == 1 ? m.osc2_level : 0.0f);
        if (level <= 0.001f) {
            continue;
        }
        float freq = 440.0f * powf(2.0f, ((float)v->note - 69.0f + osc->semi + m.pitch) / 12.0f);
        render_osc(v, o, osc, freq, osc->position / 100.0f + m.pos[o], m.detune, level, buf, n);
    }
    if (p->sub_level > 0) {
        float level = p->sub_level / 100.0f;
        float dt = 440.0f * powf(2.0f, ((float)v->note - 81.0f + p->osc[0].semi + m.pitch) / 12.0f) / RATE;
        float phase = v->sub_phase;
        for (int i = 0; i < n; ++i) {
            buf[i] += level * (p->sub_shape ? (phase < 0.5f ? 0.8f : -0.8f) : fast_sine(phase));
            phase = wrap1(phase + dt);
        }
        v->sub_phase = phase;
    }
    if (p->noise_level > 0) {
        float level = p->noise_level / 100.0f;
        float coef = 0.05f + 0.95f * (p->noise_color / 100.0f);
        float lp = v->noise_lp;
        for (int i = 0; i < n; ++i) {
            lp += coef * (noise() - lp);
            buf[i] += level * lp;
        }
        v->noise_lp = lp;
    }

    float track = (p->keytrack / 100.0f) * ((float)v->note - 60.0f) / 12.0f / 9.23f; /* octaves to 0..1 */
    float cut1 = p->filter[0].cutoff / 100.0f + m.cutoff[0] + track;
    float cut2 = p->filter[1].cutoff / 100.0f + m.cutoff[1];
    float res1 = p->filter[0].resonance / 100.0f + m.reso1;
    float res2 = p->filter[1].resonance / 100.0f;
    if (p->routing == 1 && p->filter[1].type != FILTER_OFF && p->filter[0].type != FILTER_OFF) {
        float copy[BLOCK_MAX];
        memcpy(copy, buf, sizeof(float) * (size_t)n);
        run_filter(&v->filter[0], &p->filter[0], cut1, res1, buf, n);
        run_filter(&v->filter[1], &p->filter[1], cut2, res2, copy, n);
        for (int i = 0; i < n; ++i) {
            buf[i] = (buf[i] + copy[i]) * 0.6f;
        }
    } else {
        run_filter(&v->filter[0], &p->filter[0], cut1, res1, buf, n);
        run_filter(&v->filter[1], &p->filter[1], cut2, res2, buf, n);
    }

    float gain = VOICE_GAIN * clampf(1.0f + m.amp, 0.0f, 2.0f);
    float step = (amp_end - amp_start) / (float)n;
    float amp = amp_start;
    for (int i = 0; i < n; ++i) {
        amp += step;
        out[i] += buf[i] * amp * gain;
    }
    if (v->env_stage[0] == ENV_OFF) {
        v->active = false;
    }
}

/* ---- Effects ------------------------------------------------------------------------ */

static void run_fx(slot_fx_t *f, const psynth_patch_t *p, float *buf, int n)
{
    if (p->drive > 0) {
        float drive = 1.0f + p->drive * 0.08f;
        for (int i = 0; i < n; ++i) {
            buf[i] = soft_clip(buf[i] * drive) * (0.6f + 0.4f / drive);
        }
    }
    if (p->chorus > 0 && f->chorus != NULL) {
        float mix = p->chorus / 200.0f;
        for (int i = 0; i < n; ++i) {
            f->chorus_lfo = wrap1(f->chorus_lfo + 0.6f / RATE);
            float delay = (0.012f + 0.004f * fast_sine(f->chorus_lfo)) * RATE;
            float read = (float)f->chorus_pos - delay;
            read += read < 0.0f ? CHORUS_SIZE : 0.0f;
            int j = (int)read;
            float frac = read - (float)j;
            float a = f->chorus[j & (CHORUS_SIZE - 1)];
            float b = f->chorus[(j + 1) & (CHORUS_SIZE - 1)];
            f->chorus[f->chorus_pos] = buf[i];
            f->chorus_pos = (f->chorus_pos + 1) & (CHORUS_SIZE - 1);
            buf[i] = buf[i] * (1.0f - mix) + (a + (b - a) * frac) * mix * 1.6f;
        }
    }
    if (p->delay_mix > 0 && f->delay != NULL) {
        static const float beats[DELAY_TIMES] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
        int time = p->delay_time < DELAY_TIMES ? p->delay_time : DELAY_1_4;
        int delay = (int)(beats[time] * 60.0f / (float)tempo * RATE);
        delay = delay >= DELAY_SIZE ? DELAY_SIZE - 1 : delay < 1 ? 1 : delay;
        float feedback = p->delay_feedback / 100.0f * 0.9f;
        float mix = p->delay_mix / 100.0f;
        for (int i = 0; i < n; ++i) {
            int read = f->delay_pos - delay;
            read += read < 0 ? DELAY_SIZE : 0;
            float echo = f->delay[read];
            f->delay[f->delay_pos] = buf[i] + echo * feedback;
            f->delay_pos = f->delay_pos + 1 >= DELAY_SIZE ? 0 : f->delay_pos + 1;
            buf[i] += echo * mix;
        }
    }
    if (p->reverb_mix > 0) {
        static const int comb_len[4] = {1116, 1188, 1277, 1356};
        static const int allpass_len[2] = {556, 441};
        float feedback = 0.7f + 0.27f * (p->reverb_size / 100.0f);
        float mix = p->reverb_mix / 100.0f;
        for (int i = 0; i < n; ++i) {
            float x = buf[i] * 0.25f;
            float wet = 0.0f;
            for (int c = 0; c < 4; ++c) {
                float y = f->comb[c][f->comb_pos[c]];
                f->comb_lp[c] = y * 0.7f + f->comb_lp[c] * 0.3f;
                f->comb[c][f->comb_pos[c]] = x + f->comb_lp[c] * feedback;
                f->comb_pos[c] = f->comb_pos[c] + 1 >= comb_len[c] ? 0 : f->comb_pos[c] + 1;
                wet += y;
            }
            for (int a = 0; a < 2; ++a) {
                float y = f->allpass[a][f->allpass_pos[a]];
                f->allpass[a][f->allpass_pos[a]] = wet + y * 0.5f;
                f->allpass_pos[a] = f->allpass_pos[a] + 1 >= allpass_len[a] ? 0 : f->allpass_pos[a] + 1;
                wet = y - wet;
            }
            buf[i] = buf[i] * (1.0f - mix * 0.5f) + wet * mix;
        }
    }
}

/* ---- The external engine --------------------------------------------------------------- */

static bool ext_sounding(int slot)
{
    for (int i = 0; i < VOICES; ++i) {
        if (voices[i].active && voices[i].slot == slot) {
            return true;
        }
    }
    return fx[slot] != NULL && fx[slot]->tail > 0;
}

static void ext_render(int slot, float *mix, int n)
{
    const psynth_patch_t *p = &patches[slot];
    float buf[BLOCK_MAX];
    if (n > BLOCK_MAX) {
        n = BLOCK_MAX;
    }
    memset(buf, 0, sizeof(float) * (size_t)n);
    bool any = false;
    for (int i = 0; i < VOICES; ++i) {
        if (voices[i].active && voices[i].slot == slot) {
            render_voice(&voices[i], p, buf, n);
            any = true;
        }
    }
    slot_fx_t *f = fx[slot];
    if (f != NULL) {
        bool ringing_fx = p->delay_mix > 0 || p->reverb_mix > 0 || p->chorus > 0;
        if (any) {
            f->tail = ringing_fx ? TAIL_FRAMES : 0;
        } else if (f->tail > 0) {
            f->tail -= n;
        }
        run_fx(f, p, buf, n);
    }
    float volume = p->volume / 100.0f;
    int pos = scope_pos;
    for (int i = 0; i < n; ++i) {
        float y = buf[i] * volume;
        mix[i] += y;
        if (slot == scope_slot) {
            scope[pos] = y;
            pos = (pos + 1) & (SCOPE_SIZE - 1);
        }
    }
    if (slot == scope_slot) {
        scope_pos = pos;
    }
}

static const char *ext_name(int slot)
{
    return patches[slot].name;
}

static const char *ext_about(int slot)
{
    return preset_about[slot];
}

static const synth_external_t engine = {
    .count = PSYNTH_SLOTS,
    .name = ext_name,
    .about = ext_about,
    .note_on = ext_note_on,
    .note_off = ext_note_off,
    .sustain = ext_sustain,
    .release_all = ext_release_all,
    .silence = ext_silence,
    .sounding = ext_sounding,
    .render = ext_render,
};

/* ---- Public ---------------------------------------------------------------------------- */

void psynth_init(void)
{
    for (int i = 0; i <= 1024; ++i) {
        sine_table[i] = sinf(2.0f * PI_F * (float)i / 1024.0f);
    }
    if (!build_wavetables()) {
        ESP_LOGW(TAG, "No PSRAM for the wavetables; the wavetable shapes are silent");
    }
    for (int i = 0; i < VOICES; ++i) {
        for (int f = 0; f < 2; ++f) {
            voices[i].filter[f].comb = heap_caps_calloc(COMB_MAX, sizeof(float),
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    }
    for (int s = 0; s < PSYNTH_SLOTS; ++s) {
        load_preset(s);
        fx[s] = heap_caps_calloc(1, sizeof(slot_fx_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (fx[s] != NULL) {
            fx[s]->chorus = heap_caps_calloc(CHORUS_SIZE, sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            fx[s]->delay = heap_caps_calloc(DELAY_SIZE, sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    }
    synth_set_external(&engine);
    ESP_LOGI(TAG, "Patch synth ready: %d patches, %d voices; %u KB PSRAM free", PSYNTH_SLOTS, VOICES,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

psynth_patch_t *psynth_patch(int slot)
{
    return &patches[slot < 0 || slot >= PSYNTH_SLOTS ? 0 : slot];
}

void psynth_reset(int slot)
{
    if (slot >= 0 && slot < PSYNTH_SLOTS) {
        load_preset(slot);
    }
}

void psynth_set_tempo(uint16_t bpm)
{
    tempo = bpm < 20 ? 20 : bpm;
}

void psynth_set_scope_slot(int slot)
{
    scope_slot = slot;
}

int psynth_scope(float *out, int max)
{
    int count = max < SCOPE_SIZE ? max : SCOPE_SIZE;
    int start = (scope_pos - count) & (SCOPE_SIZE - 1);
    for (int i = 0; i < count; ++i) {
        out[i] = scope[(start + i) & (SCOPE_SIZE - 1)];
    }
    return count;
}
