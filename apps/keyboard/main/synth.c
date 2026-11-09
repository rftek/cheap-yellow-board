#include "synth.h"

#include <math.h>
#include <string.h>

#define SINE_SIZE 1024
#define CHUNK 64              /* frames mixed at a time in float */
#define PLUCK_MAX 1100        /* delay line frames: down to 40 Hz */
#define SILENT 0.0001f        /* -80 dB: a voice this quiet is done */
#define MASTER_GAIN 0.32f
#define T60 6.9f              /* time constants in a fall of 60 dB */
#define MIDDLE_C_HZ 261.63f

typedef enum {
    STAGE_OFF,
    STAGE_ATTACK,
    STAGE_DECAY, /* toward the sustain level, then held there */
    STAGE_RELEASE,
} stage_t;

/* An instrument's envelope. Decay and release are the times to fall by
   60 dB; decay falls toward sustain. */
typedef struct {
    float attack_s;
    float decay_s;
    float sustain;
    float release_s;
    float gain;
    bool key_scaled; /* higher notes decay faster, as on a piano */
} shape_t;

static const char *const names[INSTRUMENTS] = {
    [INSTRUMENT_EPIANO] = "E.PIANO", [INSTRUMENT_ORGAN] = "ORGAN",
    [INSTRUMENT_STRINGS] = "STRINGS", [INSTRUMENT_SYNTH] = "SYNTH",
    [INSTRUMENT_PLUCK] = "PLUCK",    [INSTRUMENT_BELL] = "BELL",
    [INSTRUMENT_CHIP] = "CHIP",
};

static const shape_t shapes[INSTRUMENTS] = {
    [INSTRUMENT_EPIANO] = {0.002f, 6.0f, 0.0f, 0.4f, 0.95f, true},
    [INSTRUMENT_ORGAN] = {0.006f, 0.2f, 1.0f, 0.12f, 0.5f, false},
    [INSTRUMENT_STRINGS] = {0.22f, 1.0f, 1.0f, 1.2f, 0.42f, false},
    [INSTRUMENT_SYNTH] = {0.004f, 0.8f, 0.6f, 0.5f, 0.5f, false},
    [INSTRUMENT_PLUCK] = {0.0005f, 1.0f, 1.0f, 0.25f, 0.9f, false},
    [INSTRUMENT_BELL] = {0.001f, 7.0f, 0.0f, 2.5f, 0.7f, true},
    [INSTRUMENT_CHIP] = {0.001f, 0.3f, 0.75f, 0.08f, 0.3f, false},
};

typedef struct {
    stage_t stage;
    uint8_t note;
    uint8_t instrument;
    uint8_t source;
    bool held;      /* key down */
    bool sustained; /* key up, held by the pedal */
    uint32_t age;   /* order of note-ons, for stealing the oldest */

    float level;
    float sustain;
    float attack_step;
    float decay_coef;
    float release_coef;
    float gain;

    float phase, step;   /* main oscillator, in cycles */
    float phase2, step2; /* second oscillator, or the FM modulator */
    float depth, depth_floor, depth_coef; /* FM depth in cycles */
    float lp1, lp2, lp_coef;              /* one-pole low-passes */
    float cutoff, cutoff_floor, cutoff_coef; /* the SYNTH's filter sweep, Hz */
    float svf_low, svf_band;

    int16_t *pluck;
    uint16_t pluck_length;
    uint16_t pluck_pos;
} voice_t;

static float sine_table[SINE_SIZE + 1];
static voice_t voices[SYNTH_VOICES];
static int16_t pluck_lines[SYNTH_VOICES][PLUCK_MAX];
static uint32_t note_count;
static bool sustain_down;
static uint32_t noise_state = 0x12345678;

static struct {
    float level;
    float phase;
    float step;
    float coef;
} click;

static inline float sine(float phase)
{
    phase -= floorf(phase);
    float x = phase * SINE_SIZE;
    int i = (int)x;
    float fraction = x - (float)i;
    i &= SINE_SIZE - 1;
    return sine_table[i] + (sine_table[i + 1] - sine_table[i]) * fraction;
}

/* Rounds off the step of a saw or pulse so it doesn't alias. */
static inline float poly_blep(float t, float dt)
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

static inline float saw(float phase, float dt)
{
    return 2.0f * phase - 1.0f - poly_blep(phase, dt);
}

static inline float pulse(float phase, float dt, float width)
{
    float value = phase < width ? 1.0f : -1.0f;
    float shifted = phase + 1.0f - width;
    shifted -= floorf(shifted);
    return value + poly_blep(phase, dt) - poly_blep(shifted, dt);
}

static inline float advance(float phase, float step)
{
    phase += step;
    return phase >= 1.0f ? phase - 1.0f : phase;
}

static float noise(void)
{
    noise_state = noise_state * 1664525u + 1013904223u;
    return (float)(int32_t)noise_state / 2147483648.0f;
}

static float coef_for(float seconds_60db)
{
    return expf(-T60 / (seconds_60db * SYNTH_RATE));
}

static inline float envelope(voice_t *voice)
{
    switch (voice->stage) {
    case STAGE_ATTACK:
        voice->level += voice->attack_step;
        if (voice->level >= 1.0f) {
            voice->level = 1.0f;
            voice->stage = STAGE_DECAY;
        }
        break;
    case STAGE_DECAY:
        voice->level = voice->sustain + (voice->level - voice->sustain) * voice->decay_coef;
        break;
    case STAGE_RELEASE:
        voice->level *= voice->release_coef;
        break;
    default:
        break;
    }
    return voice->level * voice->gain;
}

/* ---- One voice, n frames, added into mix --------------------------------- */

static void render_fm(voice_t *voice, float *mix, int n)
{
    for (int i = 0; i < n; ++i) {
        float modulator = sine(voice->phase2);
        float out = sine(voice->phase + voice->depth * modulator);
        voice->depth = voice->depth_floor + (voice->depth - voice->depth_floor) * voice->depth_coef;
        voice->phase = advance(voice->phase, voice->step);
        voice->phase2 = advance(voice->phase2, voice->step2);
        mix[i] += out * envelope(voice);
    }
}

static void render_organ(voice_t *voice, float *mix, int n)
{
    for (int i = 0; i < n; ++i) {
        float p = voice->phase;
        float out = 0.45f * sine(p) + 0.32f * sine(2.0f * p) + 0.2f * sine(3.0f * p) +
                    0.14f * sine(4.0f * p) + 0.08f * sine(6.0f * p);
        voice->phase = advance(p, voice->step);
        mix[i] += out * envelope(voice);
    }
}

static void render_strings(voice_t *voice, float *mix, int n)
{
    for (int i = 0; i < n; ++i) {
        float out = 0.5f * (saw(voice->phase, voice->step) + saw(voice->phase2, voice->step2));
        voice->lp1 += voice->lp_coef * (out - voice->lp1);
        voice->lp2 += voice->lp_coef * (voice->lp1 - voice->lp2);
        voice->phase = advance(voice->phase, voice->step);
        voice->phase2 = advance(voice->phase2, voice->step2);
        mix[i] += voice->lp2 * envelope(voice);
    }
}

static void render_synth(voice_t *voice, float *mix, int n)
{
    for (int i = 0; i < n; ++i) {
        float out = 0.6f * saw(voice->phase, voice->step) +
                    0.4f * pulse(voice->phase2, voice->step2, 0.5f);
        voice->cutoff = voice->cutoff_floor + (voice->cutoff - voice->cutoff_floor) * voice->cutoff_coef;
        /* Chamberlin state-variable low-pass, a little resonant. */
        float f = 2.0f * 3.14159265f * voice->cutoff / SYNTH_RATE;
        if (f > 0.9f) {
            f = 0.9f;
        }
        voice->svf_low += f * voice->svf_band;
        float high = out - voice->svf_low - 0.55f * voice->svf_band;
        voice->svf_band += f * high;
        voice->phase = advance(voice->phase, voice->step);
        voice->phase2 = advance(voice->phase2, voice->step2);
        mix[i] += voice->svf_low * envelope(voice);
    }
}

static void render_pluck(voice_t *voice, float *mix, int n)
{
    int16_t *line = voice->pluck;
    int length = voice->pluck_length;
    int pos = voice->pluck_pos;
    for (int i = 0; i < n; ++i) {
        int next = pos + 1 < length ? pos + 1 : 0;
        float out = line[pos] * (1.0f / 32767.0f);
        line[pos] = (int16_t)((line[pos] + line[next]) * 0.4985f);
        pos = next;
        mix[i] += out * envelope(voice);
    }
    voice->pluck_pos = (uint16_t)pos;
}

static void render_chip(voice_t *voice, float *mix, int n)
{
    for (int i = 0; i < n; ++i) {
        float out = pulse(voice->phase, voice->step, 0.25f);
        voice->phase = advance(voice->phase, voice->step);
        mix[i] += out * envelope(voice);
    }
}

static void render_voice(voice_t *voice, float *mix, int n)
{
    switch (voice->instrument) {
    case INSTRUMENT_EPIANO:
    case INSTRUMENT_BELL:
        render_fm(voice, mix, n);
        break;
    case INSTRUMENT_ORGAN:
        render_organ(voice, mix, n);
        break;
    case INSTRUMENT_STRINGS:
        render_strings(voice, mix, n);
        break;
    case INSTRUMENT_SYNTH:
        render_synth(voice, mix, n);
        break;
    case INSTRUMENT_PLUCK:
        render_pluck(voice, mix, n);
        break;
    default:
        render_chip(voice, mix, n);
        break;
    }
    bool faded = voice->level < SILENT &&
                 (voice->stage == STAGE_RELEASE || (voice->stage == STAGE_DECAY && voice->sustain == 0.0f));
    if (faded) {
        voice->stage = STAGE_OFF;
    }
}

/* ---- Notes ---------------------------------------------------------------- */

static void start_voice(voice_t *voice, int note, int instrument, note_source_t source)
{
    const shape_t *shape = &shapes[instrument];
    float freq = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
    bool retrigger = voice->stage != STAGE_OFF && voice->note == note &&
                     voice->instrument == instrument;
    if (!retrigger) {
        int16_t *line = voice->pluck;
        memset(voice, 0, sizeof(*voice));
        voice->pluck = line;
    }
    voice->note = (uint8_t)note;
    voice->instrument = (uint8_t)instrument;
    voice->source = (uint8_t)source;
    voice->held = true;
    voice->sustained = false;
    voice->age = ++note_count;
    voice->stage = STAGE_ATTACK;
    voice->gain = shape->gain;
    voice->sustain = shape->sustain;
    voice->attack_step = 1.0f / (shape->attack_s * SYNTH_RATE);
    float decay = shape->decay_s;
    if (shape->key_scaled) {
        float scale = sqrtf(MIDDLE_C_HZ / freq);
        decay *= scale < 0.35f ? 0.35f : scale > 2.0f ? 2.0f : scale;
    }
    voice->decay_coef = coef_for(decay);
    voice->release_coef = coef_for(shape->release_s);
    voice->step = freq / SYNTH_RATE;

    switch (instrument) {
    case INSTRUMENT_EPIANO:
        voice->step2 = voice->step; /* modulator at the note's pitch */
        voice->depth = 0.38f;       /* about 2.4 radians: a bright, barky attack */
        voice->depth_floor = 0.04f;
        voice->depth_coef = expf(-1.0f / (0.3f * SYNTH_RATE));
        break;
    case INSTRUMENT_BELL:
        voice->step2 = voice->step * 3.5f; /* inharmonic partials */
        voice->depth = 0.6f;
        voice->depth_floor = 0.05f;
        voice->depth_coef = expf(-1.0f / (1.2f * SYNTH_RATE));
        break;
    case INSTRUMENT_STRINGS: {
        voice->step = freq * 1.004f / SYNTH_RATE; /* +7 cents */
        voice->step2 = freq * 0.996f / SYNTH_RATE; /* -7 cents */
        if (!retrigger) {
            voice->phase2 = 0.37f;
        }
        float cutoff = 1200.0f + 4.0f * freq;
        cutoff = cutoff > 6000.0f ? 6000.0f : cutoff;
        voice->lp_coef = 1.0f - expf(-2.0f * 3.14159265f * cutoff / SYNTH_RATE);
        break;
    }
    case INSTRUMENT_SYNTH:
        voice->step2 = freq * 1.003f / SYNTH_RATE;
        voice->cutoff = fminf(freq * 8.0f, 7000.0f);
        voice->cutoff_floor = fminf(freq * 2.5f, 7000.0f);
        voice->cutoff_coef = expf(-1.0f / (0.25f * SYNTH_RATE));
        break;
    case INSTRUMENT_PLUCK: {
        int length = (int)(SYNTH_RATE / freq + 0.5f);
        length = length < 2 ? 2 : length > PLUCK_MAX ? PLUCK_MAX : length;
        voice->pluck_length = (uint16_t)length;
        voice->pluck_pos = 0;
        /* A burst of noise, softened once so the string isn't harsh. */
        float previous = 0.0f;
        for (int i = 0; i < length; ++i) {
            float value = noise();
            float soft = 0.5f * (value + previous);
            previous = value;
            float mixed = (retrigger ? voice->pluck[i] / 32767.0f * 0.3f : 0.0f) + 0.8f * soft;
            mixed = mixed > 1.0f ? 1.0f : mixed < -1.0f ? -1.0f : mixed;
            voice->pluck[i] = (int16_t)(mixed * 32767.0f);
        }
        break;
    }
    default:
        break;
    }
}

static void release_voice(voice_t *voice)
{
    voice->held = false;
    voice->sustained = false;
    if (voice->stage != STAGE_OFF) {
        voice->stage = STAGE_RELEASE;
    }
}

/* The voice to use for a new note. */
static voice_t *pick_voice(int note, int instrument, note_source_t source)
{
    voice_t *best = NULL;
    /* The same note again: retrigger it rather than stack a copy. */
    for (int i = 0; i < SYNTH_VOICES; ++i) {
        voice_t *voice = &voices[i];
        if (voice->stage != STAGE_OFF && voice->note == note && voice->instrument == instrument &&
            voice->source == source) {
            return voice;
        }
    }
    for (int i = 0; i < SYNTH_VOICES; ++i) {
        if (voices[i].stage == STAGE_OFF) {
            return &voices[i];
        }
    }
    /* The quietest released voice. */
    for (int i = 0; i < SYNTH_VOICES; ++i) {
        voice_t *voice = &voices[i];
        if (voice->stage == STAGE_RELEASE && (best == NULL || voice->level < best->level)) {
            best = voice;
        }
    }
    if (best != NULL) {
        return best;
    }
    /* The oldest. */
    best = &voices[0];
    for (int i = 1; i < SYNTH_VOICES; ++i) {
        if ((int32_t)(voices[i].age - best->age) < 0) {
            best = &voices[i];
        }
    }
    return best;
}

void synth_init(void)
{
    for (int i = 0; i <= SINE_SIZE; ++i) {
        sine_table[i] = sinf(2.0f * 3.14159265f * (float)i / SINE_SIZE);
    }
    memset(voices, 0, sizeof(voices));
    for (int i = 0; i < SYNTH_VOICES; ++i) {
        voices[i].pluck = pluck_lines[i];
    }
    memset(&click, 0, sizeof(click));
}

const char *synth_instrument_name(int instrument)
{
    return instrument >= 0 && instrument < INSTRUMENTS ? names[instrument] : "?";
}

void synth_note_on(int note, int instrument, note_source_t source)
{
    if (instrument < 0 || instrument >= INSTRUMENTS || note < 0 || note > 127) {
        return;
    }
    start_voice(pick_voice(note, instrument, source), note, instrument, source);
}

void synth_note_off(int note, int instrument, note_source_t source)
{
    for (int i = 0; i < SYNTH_VOICES; ++i) {
        voice_t *voice = &voices[i];
        if (voice->stage == STAGE_OFF || !voice->held || voice->note != note ||
            voice->instrument != instrument || voice->source != source) {
            continue;
        }
        if (source == SOURCE_LIVE && sustain_down) {
            voice->held = false;
            voice->sustained = true;
        } else {
            release_voice(voice);
        }
    }
}

void synth_sustain(bool down)
{
    sustain_down = down;
    if (down) {
        return;
    }
    for (int i = 0; i < SYNTH_VOICES; ++i) {
        if (voices[i].sustained) {
            release_voice(&voices[i]);
        }
    }
}

void synth_release_all(note_source_t source)
{
    for (int i = 0; i < SYNTH_VOICES; ++i) {
        if (voices[i].stage != STAGE_OFF && voices[i].source == source) {
            release_voice(&voices[i]);
        }
    }
}

void synth_silence(void)
{
    for (int i = 0; i < SYNTH_VOICES; ++i) {
        voices[i].stage = STAGE_OFF;
        voices[i].held = false;
        voices[i].sustained = false;
    }
    click.level = 0.0f;
}

void synth_click(bool accent)
{
    click.level = accent ? 0.55f : 0.35f;
    click.phase = 0.0f;
    click.step = (accent ? 1760.0f : 1320.0f) / SYNTH_RATE;
    click.coef = expf(-1.0f / (0.012f * SYNTH_RATE));
}

/* Soft clipping: a tanh-like curve, so a big chord rounds off instead of
   wrapping. */
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

void synth_render(int16_t *out, size_t frames)
{
    float mix[CHUNK];
    while (frames > 0) {
        int n = frames > CHUNK ? CHUNK : (int)frames;
        memset(mix, 0, sizeof(float) * (size_t)n);
        for (int i = 0; i < SYNTH_VOICES; ++i) {
            if (voices[i].stage != STAGE_OFF) {
                render_voice(&voices[i], mix, n);
            }
        }
        if (click.level > SILENT) {
            for (int i = 0; i < n; ++i) {
                mix[i] += click.level * sine(click.phase) / MASTER_GAIN;
                click.phase = advance(click.phase, click.step);
                click.level *= click.coef;
            }
        }
        for (int i = 0; i < n; ++i) {
            out[i] = (int16_t)(soft_clip(mix[i] * MASTER_GAIN) * 32000.0f);
        }
        out += n;
        frames -= (size_t)n;
    }
}
