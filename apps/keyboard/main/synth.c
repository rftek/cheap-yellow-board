#include "synth.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265f
#define SINE_SIZE 1024
#define CHUNK 64              /* frames mixed at a time in float */
#define PLUCK_MAX 1400        /* delay line frames: down to 31.5 Hz, below C1 */
#define MAX_MODES 16          /* resonators in a modal (piano) voice */
#define DETUNED_PARTIALS 4    /* partials that get a second string */
#define MODE_LIMIT_HZ 7000.0f /* partials above this are left out */
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

typedef enum {
    ENGINE_MODAL,   /* struck strings, tines, reeds and bars: damped resonators */
    ENGINE_FM,      /* two-operator FM */
    ENGINE_ORGAN,   /* additive drawbars or pipe ranks */
    ENGINE_STRINGS, /* detuned saws, low-passed */
    ENGINE_SYNTH,   /* saw and square through a swept resonant filter */
    ENGINE_PLUCK,   /* Karplus-Strong */
    ENGINE_CHIP,    /* pulse wave */
} engine_t;

/* An envelope. Decay and release are times to fall by 60 dB; decay falls
   toward sustain. A modal instrument's decay is its fundamental's ring at
   middle C and its release is the damper. */
typedef struct {
    float attack_s;
    float decay_s;
    float sustain;
    float release_s;
    float gain;
    bool key_scaled; /* higher notes decay faster, as on a piano */
} shape_t;

/* One partial of a modal instrument with its own tuning. */
typedef struct {
    float ratio; /* to the note's pitch */
    float amp;
    float decay; /* decays this many times faster than the fundamental */
} partial_t;

typedef struct {
    uint8_t partials;        /* harmonics to use, when table is NULL */
    const partial_t *table;  /* or these partials */
    uint8_t table_size;
    float inharmonicity;     /* string stiffness B at middle C */
    float rolloff;           /* harmonic n has amplitude 1/n^rolloff */
    float hammer;            /* strike point along the string, 0 for none */
    float decay_slope;       /* harmonic n decays 1 + slope*(n-1) times faster */
    float detune_cents;      /* a second string this far apart; 0 for one */
    float noise;             /* hammer or mallet thump */
    float noise_tone;        /* its low-pass, 0-1 */
    float drive;             /* pickup or reed bark; 0 is clean */
    float tremolo_hz;
    float tremolo_depth;
} modal_spec_t;

typedef struct {
    float ratio;      /* modulator to carrier */
    float depth;      /* starting depth, in cycles */
    float depth_floor;
    float depth_tau_s;
    float tremolo_hz;
    float tremolo_depth;
} fm_spec_t;

/* Drawbars, in organ order: 16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5',
   1 1/3', 1'. Each is a sine at a multiple of half the note's pitch. */
#define DRAWBARS 9
typedef struct {
    float drawbars[DRAWBARS];
    float percussion;      /* level of a decaying 2 2/3' on each note */
    float percussion_s;
    float click;           /* key click */
    float chiff;           /* breathy pipe onset */
    float vibrato_hz;
    float vibrato_depth;   /* fraction of the pitch */
    float tremolo_hz;      /* a rotating speaker, roughly */
    float tremolo_depth;
    float detune_cents;    /* pipes in a rank are never quite in tune */
} organ_spec_t;

typedef struct {
    bool two_saws;
    float detune_cents;
    float cutoff_base, cutoff_track; /* cutoff = base + track * pitch */
    float vibrato_hz, vibrato_depth, vibrato_delay_s;
} strings_spec_t;

typedef struct {
    float square;          /* mix of the square under the saw */
    float peak, floor;     /* filter cutoff, in multiples of the pitch */
    float sweep_attack_s;  /* rise to the peak */
    float sweep_decay_s;   /* fall to the floor */
    float damping;         /* lower is more resonant */
} synth_spec_t;

typedef struct {
    float damping;   /* per pass round the string, about 0.5 */
    int softening;   /* low-pass passes over the noise burst */
    float brightness;
} pluck_spec_t;

typedef struct {
    float width;
} chip_spec_t;

typedef struct {
    uint8_t voice;
    const char *name;
    const char *about;
    engine_t engine;
    shape_t shape;
    const void *spec;
} instrument_spec_t;

/* ---- The styles ------------------------------------------------------------ */

/* Pianos. The grand rings longest, with stiff bright strings struck an
   eighth of the way along; the upright is darker, shorter and thumps;
   the electric grand is short, bright and chorused by detuned strings. */
static const modal_spec_t PIANO_C7 = {.partials = 12, .inharmonicity = 0.0004f, .rolloff = 1.05f,
    .hammer = 0.125f, .decay_slope = 0.22f, .detune_cents = 1.0f, .noise = 0.12f, .noise_tone = 0.25f};
static const modal_spec_t PIANO_U1 = {.partials = 12, .inharmonicity = 0.0007f, .rolloff = 1.4f,
    .hammer = 0.14f, .decay_slope = 0.35f, .detune_cents = 2.0f, .noise = 0.22f, .noise_tone = 0.15f};
static const modal_spec_t PIANO_CP70 = {.partials = 10, .inharmonicity = 0.0009f, .rolloff = 0.9f,
    .hammer = 0.1f, .decay_slope = 0.4f, .detune_cents = 5.0f, .noise = 0.1f, .noise_tone = 0.4f,
    .drive = 0.4f};
static const modal_spec_t PIANO_HONKY = {.partials = 12, .inharmonicity = 0.0006f, .rolloff = 1.2f,
    .hammer = 0.13f, .decay_slope = 0.3f, .detune_cents = 16.0f, .noise = 0.18f, .noise_tone = 0.3f};

/* Electric pianos. A tine: the fundamental, a little octave, and a bell
   partial high up that dies fast, through a pickup that barks a little,
   with tremolo. A reed: harmonics through a pickup driven hard. */
static const partial_t TINE_PARTIALS[] = {
    {1.0f, 1.0f, 1.0f}, {2.0f, 0.3f, 1.6f}, {3.0f, 0.08f, 2.5f}, {7.1f, 0.25f, 9.0f},
};
static const modal_spec_t EP_MK1 = {.table = TINE_PARTIALS, .table_size = 4, .noise = 0.05f,
    .noise_tone = 0.5f, .drive = 1.5f, .tremolo_hz = 4.5f, .tremolo_depth = 0.2f};
static const modal_spec_t EP_200A = {.partials = 9, .rolloff = 1.0f, .decay_slope = 0.35f,
    .noise = 0.04f, .noise_tone = 0.5f, .drive = 2.5f, .tremolo_hz = 5.5f, .tremolo_depth = 0.3f};

/* A toy piano strikes metal rods, whose partials are far from harmonic. */
static const partial_t TOY_PARTIALS[] = {
    {1.0f, 1.0f, 1.0f}, {2.76f, 0.5f, 2.0f}, {5.40f, 0.25f, 3.0f}, {8.93f, 0.12f, 4.0f},
};
static const modal_spec_t PIANO_TOY = {.table = TOY_PARTIALS, .table_size = 4, .noise = 0.1f,
    .noise_tone = 0.6f};

/* Mallets: a marimba's bar is tuned so its overtones are 4 and 10 times
   the pitch. */
static const partial_t MARIMBA_PARTIALS[] = {
    {1.0f, 1.0f, 1.0f}, {3.9f, 0.3f, 3.0f}, {9.9f, 0.08f, 6.0f},
};
static const modal_spec_t BELL_MARIMBA = {.table = MARIMBA_PARTIALS, .table_size = 3,
    .noise = 0.08f, .noise_tone = 0.2f};

static const fm_spec_t FM_DX7 = {1.0f, 0.38f, 0.04f, 0.3f, 0, 0};
static const fm_spec_t FM_BELL = {3.5f, 0.6f, 0.05f, 1.2f, 0, 0};
static const fm_spec_t FM_VIBES = {4.0f, 0.22f, 0.0f, 0.12f, 5.0f, 0.35f};

/* Organs. Jazz: the first three drawbars out, with percussion and click.
   Church: principal ranks and a mixture, slow to speak, with chiff.
   Gospel: every drawbar out through a fast rotating speaker. Combo: a
   bright transistor organ with vibrato. */
static const organ_spec_t ORGAN_B3 = {{1, 1, 1, 0, 0, 0, 0, 0, 0}, .percussion = 0.6f,
    .percussion_s = 0.35f, .click = 0.25f, .vibrato_hz = 6.8f, .vibrato_depth = 0.0015f};
static const organ_spec_t ORGAN_CHURCH = {{0.5f, 0, 1, 0.7f, 0.35f, 0.5f, 0, 0.25f, 0.3f},
    .chiff = 0.25f, .detune_cents = 3.0f};
static const organ_spec_t ORGAN_GOSPEL = {{1, 1, 1, 1, 1, 1, 1, 1, 1}, .click = 0.2f,
    .tremolo_hz = 6.5f, .tremolo_depth = 0.25f, .vibrato_hz = 6.5f, .vibrato_depth = 0.0012f};
static const organ_spec_t ORGAN_COMBO = {{0, 0, 1, 0.8f, 0.5f, 0.6f, 0.4f, 0.3f, 0.5f},
    .vibrato_hz = 6.0f, .vibrato_depth = 0.004f};

static const strings_spec_t STRINGS_ENSEMBLE = {true, 7.0f, 1200.0f, 4.0f, 0, 0, 0};
static const strings_spec_t STRINGS_SOLO = {false, 0, 1800.0f, 5.0f, 5.5f, 0.006f, 0.25f};

static const synth_spec_t SYNTH_LEAD = {0.4f, 8.0f, 2.5f, 0.0f, 0.25f, 0.55f};
static const synth_spec_t SYNTH_BRASS = {0.0f, 6.0f, 1.5f, 0.07f, 0.6f, 0.8f};

static const pluck_spec_t PLUCK_GUITAR = {0.4985f, 1, 0.8f};
static const pluck_spec_t PLUCK_HARP = {0.4993f, 3, 0.9f};

static const chip_spec_t CHIP_PULSE = {0.25f};
static const chip_spec_t CHIP_SQUARE = {0.5f};

static const instrument_spec_t instruments[] = {
    /* voice, name, about, engine, {attack, decay, sustain, release, gain, key scaled} */
    {VOICE_PIANO, "C7 GRAND", "BRIGHT CONCERT GRAND", ENGINE_MODAL,
     {0.0005f, 14.0f, 0, 0.35f, 0.9f, true}, &PIANO_C7},
    {VOICE_PIANO, "U1 UPRIGHT", "WARM UPRIGHT", ENGINE_MODAL,
     {0.0005f, 9.0f, 0, 0.3f, 0.95f, true}, &PIANO_U1},
    {VOICE_PIANO, "CP70", "ELECTRIC GRAND", ENGINE_MODAL,
     {0.0005f, 6.0f, 0, 0.25f, 0.9f, true}, &PIANO_CP70},
    {VOICE_PIANO, "DX7 EP", "FM ELECTRIC PIANO", ENGINE_FM,
     {0.002f, 6.0f, 0, 0.4f, 0.95f, true}, &FM_DX7},
    {VOICE_PIANO, "MK1 EP", "TINE PIANO, TREMOLO", ENGINE_MODAL,
     {0.0005f, 7.0f, 0, 0.2f, 0.9f, true}, &EP_MK1},
    {VOICE_PIANO, "200A EP", "REED PIANO, BARKY", ENGINE_MODAL,
     {0.0005f, 4.0f, 0, 0.12f, 0.6f, true}, &EP_200A},
    {VOICE_PIANO, "HONKYTONK", "DETUNED SALOON UPRIGHT", ENGINE_MODAL,
     {0.0005f, 8.0f, 0, 0.3f, 0.9f, true}, &PIANO_HONKY},
    {VOICE_PIANO, "TOY", "TOY PIANO, METAL RODS", ENGINE_MODAL,
     {0.0005f, 2.5f, 0, 0.5f, 0.9f, true}, &PIANO_TOY},

    {VOICE_ORGAN, "B3 JAZZ", "DRAWBARS 888, PERCUSSION", ENGINE_ORGAN,
     {0.004f, 0.2f, 1, 0.06f, 0.5f, false}, &ORGAN_B3},
    {VOICE_ORGAN, "CHURCH", "PIPE ORGAN, PRINCIPALS", ENGINE_ORGAN,
     {0.07f, 0.2f, 1, 0.9f, 0.45f, false}, &ORGAN_CHURCH},
    {VOICE_ORGAN, "GOSPEL", "ALL DRAWBARS, ROTARY", ENGINE_ORGAN,
     {0.004f, 0.2f, 1, 0.08f, 0.32f, false}, &ORGAN_GOSPEL},
    {VOICE_ORGAN, "COMBO", "TRANSISTOR ORGAN", ENGINE_ORGAN,
     {0.003f, 0.2f, 1, 0.04f, 0.4f, false}, &ORGAN_COMBO},

    {VOICE_STRINGS, "ENSEMBLE", "STRING SECTION", ENGINE_STRINGS,
     {0.22f, 1.0f, 1, 1.2f, 0.42f, false}, &STRINGS_ENSEMBLE},
    {VOICE_STRINGS, "SOLO", "SOLO VIOLIN, VIBRATO", ENGINE_STRINGS,
     {0.08f, 1.0f, 1, 0.4f, 0.5f, false}, &STRINGS_SOLO},

    {VOICE_SYNTH, "LEAD", "SAW AND SQUARE LEAD", ENGINE_SYNTH,
     {0.004f, 0.8f, 0.6f, 0.5f, 0.5f, false}, &SYNTH_LEAD},
    {VOICE_SYNTH, "BRASS", "SWELLING SYNTH BRASS", ENGINE_SYNTH,
     {0.05f, 1.0f, 0.8f, 0.3f, 0.5f, false}, &SYNTH_BRASS},

    {VOICE_PLUCK, "GUITAR", "PLUCKED STRING", ENGINE_PLUCK,
     {0.0005f, 1.0f, 1, 0.25f, 0.9f, false}, &PLUCK_GUITAR},
    {VOICE_PLUCK, "HARP", "SOFT, LONG HARP", ENGINE_PLUCK,
     {0.0005f, 1.0f, 1, 0.8f, 1.0f, false}, &PLUCK_HARP},

    {VOICE_BELL, "BELL", "FM BELL", ENGINE_FM,
     {0.001f, 7.0f, 0, 2.5f, 0.7f, true}, &FM_BELL},
    {VOICE_BELL, "VIBES", "VIBRAPHONE, MOTOR ON", ENGINE_FM,
     {0.001f, 4.0f, 0, 0.6f, 0.8f, true}, &FM_VIBES},
    {VOICE_BELL, "MARIMBA", "WOODEN BARS", ENGINE_MODAL,
     {0.0005f, 1.6f, 0, 0.4f, 1.0f, true}, &BELL_MARIMBA},

    {VOICE_CHIP, "PULSE", "25% PULSE, 8-BIT", ENGINE_CHIP,
     {0.001f, 0.3f, 0.75f, 0.08f, 0.3f, false}, &CHIP_PULSE},
    {VOICE_CHIP, "SQUARE", "SQUARE WAVE", ENGINE_CHIP,
     {0.001f, 0.3f, 0.75f, 0.08f, 0.28f, false}, &CHIP_SQUARE},
};

#define INSTRUMENTS ((int)(sizeof(instruments) / sizeof(instruments[0])))

static const char *const voice_names[VOICE_KINDS] = {
    [VOICE_PIANO] = "PIANO",   [VOICE_ORGAN] = "ORGAN", [VOICE_STRINGS] = "STRING",
    [VOICE_SYNTH] = "SYNTH",   [VOICE_PLUCK] = "PLUCK", [VOICE_BELL] = "BELL",
    [VOICE_CHIP] = "CHIP",
};

/* ---- Voice state ------------------------------------------------------------ */

typedef struct {
    stage_t stage;
    uint8_t note;
    uint8_t instrument;
    uint8_t source;
    bool held;      /* key down */
    bool sustained; /* key up, held by the pedal */
    uint32_t age;   /* order of note-ons, for stealing the oldest */
    const instrument_spec_t *spec;

    float level;
    float sustain;
    float attack_step;
    float decay_coef;
    float release_coef;
    float gain;

    float phase, step;   /* main oscillator, in cycles */
    float phase2, step2; /* second oscillator, or the FM modulator */
    float lfo, lfo_step; /* tremolo or vibrato */
    float depth, depth_floor, depth_coef; /* FM depth in cycles */
    float lp1, lp2, lp_coef;              /* one-pole low-passes */
    float cutoff, cutoff_floor, cutoff_peak, sweep, sweep_attack, sweep_coef; /* filter, Hz */
    float svf_low, svf_band;
    float noise_level, noise_coef, noise_lp; /* thump, click or chiff */
    float percussion, percussion_coef;
    uint32_t frames; /* since note-on */

    /* Modal resonators: y[n] = c1 y[n-1] - c2 y[n-2]. */
    uint8_t modes;
    float y1[MAX_MODES], y2[MAX_MODES], c1[MAX_MODES], c2[MAX_MODES], cosw[MAX_MODES];

    /* Organ: drawbar multiples of half the pitch, and their levels. */
    uint8_t bars;
    uint8_t bar_multiple[DRAWBARS];
    float bar_level[DRAWBARS];

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
static int8_t style_index[VOICE_KINDS][INSTRUMENTS];
static uint8_t style_counts[VOICE_KINDS];

static struct {
    float level;
    float phase;
    float step;
    float coef;
} click;

/* ---- Building blocks -------------------------------------------------------- */

/* The sine of a phase in cycles, which must not be negative; whole cycles
   wrap in the table index. No floorf: it is a library call in software on
   this chip, and the organ alone made 72 a sample. */
static inline float sine(float phase)
{
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
    shifted = shifted >= 1.0f ? shifted - 1.0f : shifted;
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

static float cents(float value)
{
    return powf(2.0f, value / 1200.0f);
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

/* A decaying burst of low-passed noise: hammer, key click or pipe chiff. */
static inline float thump(voice_t *voice)
{
    if (voice->noise_level < SILENT) {
        return 0.0f;
    }
    voice->noise_lp += voice->lp_coef * (noise() - voice->noise_lp);
    float out = voice->noise_lp * voice->noise_level;
    voice->noise_level *= voice->noise_coef;
    return out;
}

/* ---- One voice, n frames, added into mix ------------------------------------ */

static void render_modal(voice_t *voice, float *mix, int n)
{
    const modal_spec_t *spec = voice->spec->spec;
    float buffer[CHUNK];
    memset(buffer, 0, sizeof(float) * (size_t)n);
    for (int m = 0; m < voice->modes; ++m) {
        float y1 = voice->y1[m], y2 = voice->y2[m];
        const float c1 = voice->c1[m], c2 = voice->c2[m];
        for (int i = 0; i < n; ++i) {
            float y = c1 * y1 - c2 * y2;
            y2 = y1;
            y1 = y;
            buffer[i] += y;
        }
        voice->y1[m] = y1;
        voice->y2[m] = y2;
    }
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        float out = buffer[i] + thump(voice);
        if (spec->drive > 0.0f) {
            out = out * (1.0f + spec->drive) / (1.0f + spec->drive * fabsf(out));
        }
        if (spec->tremolo_depth > 0.0f) {
            out *= 1.0f - spec->tremolo_depth * (0.5f + 0.5f * sine(voice->lfo));
            voice->lfo = advance(voice->lfo, voice->lfo_step);
        }
        float magnitude = fabsf(out);
        peak = magnitude > peak ? magnitude : peak;
        mix[i] += out * voice->gain;
    }
    /* The resonators carry the sound; the level is for stealing and
       for knowing when it has died away. */
    voice->level = peak;
    if (peak < SILENT && voice->frames > SYNTH_RATE / 50) {
        voice->stage = STAGE_OFF;
    }
}

static void render_fm(voice_t *voice, float *mix, int n)
{
    const fm_spec_t *spec = voice->spec->spec;
    for (int i = 0; i < n; ++i) {
        float modulator = sine(voice->phase2);
        /* Two cycles on, so a deep negative swing stays positive. */
        float out = sine(voice->phase + 2.0f + voice->depth * modulator);
        voice->depth = voice->depth_floor + (voice->depth - voice->depth_floor) * voice->depth_coef;
        voice->phase = advance(voice->phase, voice->step);
        voice->phase2 = advance(voice->phase2, voice->step2);
        if (spec->tremolo_depth > 0.0f) {
            out *= 1.0f - spec->tremolo_depth * (0.5f + 0.5f * sine(voice->lfo));
            voice->lfo = advance(voice->lfo, voice->lfo_step);
        }
        mix[i] += out * envelope(voice);
    }
}

static void render_organ(voice_t *voice, float *mix, int n)
{
    const organ_spec_t *spec = voice->spec->spec;
    for (int i = 0; i < n; ++i) {
        float p = voice->phase; /* at half the pitch */
        float out = 0.0f;
        for (int b = 0; b < voice->bars; ++b) {
            out += voice->bar_level[b] * sine(voice->bar_multiple[b] * p);
        }
        /* A second set of pipes a few cents off, for a church's ranks. */
        if (spec->detune_cents > 0.0f) {
            out = 0.75f * out + 0.25f * sine(2.0f * voice->phase2); /* the 8' */
            voice->phase2 = advance(voice->phase2, voice->step2);
        }
        if (voice->percussion > SILENT) {
            out += voice->percussion * sine(6.0f * p);
            voice->percussion *= voice->percussion_coef;
        }
        out += thump(voice);
        float step = voice->step;
        if (spec->vibrato_depth > 0.0f || spec->tremolo_depth > 0.0f) {
            float lfo = sine(voice->lfo);
            step *= 1.0f + spec->vibrato_depth * lfo;
            out *= 1.0f - spec->tremolo_depth * (0.5f + 0.5f * lfo);
            voice->lfo = advance(voice->lfo, voice->lfo_step);
        }
        voice->phase = advance(p, step);
        mix[i] += out * envelope(voice);
    }
}

static void render_strings(voice_t *voice, float *mix, int n)
{
    const strings_spec_t *spec = voice->spec->spec;
    for (int i = 0; i < n; ++i) {
        float out = saw(voice->phase, voice->step);
        if (spec->two_saws) {
            out = 0.5f * (out + saw(voice->phase2, voice->step2));
            voice->phase2 = advance(voice->phase2, voice->step2);
        }
        voice->lp1 += voice->lp_coef * (out - voice->lp1);
        voice->lp2 += voice->lp_coef * (voice->lp1 - voice->lp2);
        float step = voice->step;
        if (spec->vibrato_depth > 0.0f && voice->frames + (uint32_t)i > spec->vibrato_delay_s * SYNTH_RATE) {
            step *= 1.0f + spec->vibrato_depth * sine(voice->lfo);
            voice->lfo = advance(voice->lfo, voice->lfo_step);
        }
        voice->phase = advance(voice->phase, step);
        mix[i] += voice->lp2 * envelope(voice);
    }
}

static void render_synth(voice_t *voice, float *mix, int n)
{
    const synth_spec_t *spec = voice->spec->spec;
    for (int i = 0; i < n; ++i) {
        float out = (1.0f - spec->square) * saw(voice->phase, voice->step) +
                    spec->square * pulse(voice->phase2, voice->step2, 0.5f);
        /* The filter's own envelope: up to the peak, then down to the floor. */
        if (voice->sweep < 1.0f && voice->sweep_attack > 0.0f) {
            voice->sweep += voice->sweep_attack;
            voice->cutoff = voice->cutoff_floor + (voice->cutoff_peak - voice->cutoff_floor) * voice->sweep;
        } else {
            voice->sweep = 1.0f;
            voice->cutoff = voice->cutoff_floor + (voice->cutoff - voice->cutoff_floor) * voice->sweep_coef;
        }
        /* Chamberlin state-variable low-pass. */
        float f = 2.0f * PI * voice->cutoff / SYNTH_RATE;
        if (f > 0.9f) {
            f = 0.9f;
        }
        voice->svf_low += f * voice->svf_band;
        float high = out - voice->svf_low - spec->damping * voice->svf_band;
        voice->svf_band += f * high;
        voice->phase = advance(voice->phase, voice->step);
        voice->phase2 = advance(voice->phase2, voice->step2);
        mix[i] += voice->svf_low * envelope(voice);
    }
}

static void render_pluck(voice_t *voice, float *mix, int n)
{
    const pluck_spec_t *spec = voice->spec->spec;
    int16_t *line = voice->pluck;
    int length = voice->pluck_length;
    int pos = voice->pluck_pos;
    for (int i = 0; i < n; ++i) {
        int next = pos + 1 < length ? pos + 1 : 0;
        float out = line[pos] * (1.0f / 32767.0f);
        line[pos] = (int16_t)((line[pos] + line[next]) * spec->damping);
        pos = next;
        mix[i] += out * envelope(voice);
    }
    voice->pluck_pos = (uint16_t)pos;
}

static void render_chip(voice_t *voice, float *mix, int n)
{
    const chip_spec_t *spec = voice->spec->spec;
    for (int i = 0; i < n; ++i) {
        float out = pulse(voice->phase, voice->step, spec->width);
        voice->phase = advance(voice->phase, voice->step);
        mix[i] += out * envelope(voice);
    }
}

static void render_voice(voice_t *voice, float *mix, int n)
{
    switch (voice->spec->engine) {
    case ENGINE_MODAL:
        render_modal(voice, mix, n);
        break;
    case ENGINE_FM:
        render_fm(voice, mix, n);
        break;
    case ENGINE_ORGAN:
        render_organ(voice, mix, n);
        break;
    case ENGINE_STRINGS:
        render_strings(voice, mix, n);
        break;
    case ENGINE_SYNTH:
        render_synth(voice, mix, n);
        break;
    case ENGINE_PLUCK:
        render_pluck(voice, mix, n);
        break;
    default:
        render_chip(voice, mix, n);
        break;
    }
    voice->frames += (uint32_t)n;
    if (voice->spec->engine == ENGINE_MODAL) {
        return;
    }
    bool faded = voice->level < SILENT &&
                 (voice->stage == STAGE_RELEASE || (voice->stage == STAGE_DECAY && voice->sustain == 0.0f));
    if (faded) {
        voice->stage = STAGE_OFF;
    }
}

/* ---- Starting notes ---------------------------------------------------------- */

static void add_mode(voice_t *voice, float freq, float amp, float t60)
{
    if (voice->modes >= MAX_MODES) {
        return;
    }
    int m = voice->modes++;
    float w = 2.0f * PI * freq / SYNTH_RATE;
    float r = coef_for(t60);
    voice->cosw[m] = cosf(w);
    voice->c1[m] = 2.0f * r * voice->cosw[m];
    voice->c2[m] = r * r;
    /* Struck at rest: a sine starting from zero, amplitude amp. */
    voice->y1[m] = amp * sinf(w);
    voice->y2[m] = 0.0f;
}

static void start_modal(voice_t *voice, int note, float freq, bool retrigger)
{
    const modal_spec_t *spec = voice->spec->spec;
    const shape_t *shape = &voice->spec->shape;
    float key = powf(MIDDLE_C_HZ / freq, 0.6f);
    key = key < 0.3f ? 0.3f : key > 2.5f ? 2.5f : key;
    float ring = shape->decay_s * key;
    float stiffness = spec->inharmonicity * powf(2.0f, (float)(note - 60) / 24.0f);
    int count = spec->table != NULL ? spec->table_size : spec->partials;

    /* Work out the partials first, to share the level out. */
    float freqs[MAX_MODES], amps[MAX_MODES], rings[MAX_MODES];
    int partials = 0;
    float total = 0.0f;
    for (int i = 0; i < count && partials < MAX_MODES; ++i) {
        float ratio, amp, decay;
        if (spec->table != NULL) {
            ratio = spec->table[i].ratio;
            amp = spec->table[i].amp;
            decay = spec->table[i].decay;
        } else {
            float h = (float)(i + 1);
            ratio = h * sqrtf(1.0f + stiffness * h * h);
            amp = 1.0f / powf(h, spec->rolloff);
            if (spec->hammer > 0.0f) {
                amp *= fabsf(sinf(PI * h * spec->hammer)) + 0.05f;
            }
            decay = 1.0f + spec->decay_slope * (h - 1.0f);
        }
        if (freq * ratio > MODE_LIMIT_HZ) {
            continue;
        }
        freqs[partials] = freq * ratio;
        amps[partials] = amp;
        rings[partials] = ring / decay;
        total += amp;
        ++partials;
    }
    (void)retrigger; /* a new strike starts the strings afresh; the thump hides the cut */
    voice->modes = 0;
    memset(voice->y1, 0, sizeof(voice->y1));
    float scale = total > 0.0f ? 1.0f / total : 1.0f;
    int spare = MAX_MODES - partials;
    spare = spare > DETUNED_PARTIALS ? DETUNED_PARTIALS : spare;
    for (int p = 0; p < partials; ++p) {
        /* The lower partials get a second string, slightly out of tune
           with the first, for the beating of a real piano's unisons. */
        if (spec->detune_cents > 0.0f && p < spare) {
            float apart = cents(spec->detune_cents * 0.5f);
            add_mode(voice, freqs[p] * apart, amps[p] * scale * 0.5f, rings[p]);
            add_mode(voice, freqs[p] / apart, amps[p] * scale * 0.5f, rings[p] * 0.85f);
        } else {
            add_mode(voice, freqs[p], amps[p] * scale, rings[p]);
        }
    }
    voice->noise_level = spec->noise;
    voice->noise_coef = expf(-1.0f / (0.006f * SYNTH_RATE));
    voice->lp_coef = spec->noise_tone > 0.0f ? spec->noise_tone : 0.2f;
    voice->lfo_step = spec->tremolo_hz / SYNTH_RATE;
}

static void start_organ(voice_t *voice, float freq)
{
    static const uint8_t multiples[DRAWBARS] = {1, 3, 2, 4, 6, 8, 10, 12, 16};
    const organ_spec_t *spec = voice->spec->spec;
    float total = 0.0f;
    voice->bars = 0;
    for (int b = 0; b < DRAWBARS; ++b) {
        if (spec->drawbars[b] > 0.0f && freq * 0.5f * multiples[b] < 12000.0f) {
            voice->bar_multiple[voice->bars] = multiples[b];
            voice->bar_level[voice->bars] = spec->drawbars[b];
            total += spec->drawbars[b];
            ++voice->bars;
        }
    }
    for (int b = 0; b < voice->bars; ++b) {
        voice->bar_level[b] /= total > 0.0f ? total : 1.0f;
    }
    voice->step = freq * 0.5f / SYNTH_RATE;
    voice->step2 = voice->step * cents(spec->detune_cents);
    voice->percussion = spec->percussion * 0.5f;
    voice->percussion_coef = spec->percussion_s > 0.0f ? coef_for(spec->percussion_s) : 0.0f;
    voice->noise_level = spec->click + spec->chiff;
    voice->noise_coef = expf(-1.0f / ((spec->chiff > 0.0f ? 0.04f : 0.003f) * SYNTH_RATE));
    voice->lp_coef = spec->chiff > 0.0f ? 0.08f : 0.5f;
    voice->lfo_step = (spec->vibrato_hz > 0.0f ? spec->vibrato_hz : spec->tremolo_hz) / SYNTH_RATE;
}

static void start_voice(voice_t *voice, int note, int instrument, note_source_t source)
{
    const instrument_spec_t *spec = &instruments[instrument];
    const shape_t *shape = &spec->shape;
    float freq = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
    bool retrigger = voice->stage != STAGE_OFF && voice->note == note &&
                     voice->instrument == instrument;
    if (!retrigger) {
        int16_t *line = voice->pluck;
        memset(voice, 0, sizeof(*voice));
        voice->pluck = line;
    }
    voice->spec = spec;
    voice->note = (uint8_t)note;
    voice->instrument = (uint8_t)instrument;
    voice->source = (uint8_t)source;
    voice->held = true;
    voice->sustained = false;
    voice->age = ++note_count;
    voice->frames = 0;
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

    switch (spec->engine) {
    case ENGINE_MODAL:
        start_modal(voice, note, freq, retrigger);
        break;
    case ENGINE_FM: {
        const fm_spec_t *fm = spec->spec;
        voice->step2 = voice->step * fm->ratio;
        voice->depth = fm->depth;
        voice->depth_floor = fm->depth_floor;
        voice->depth_coef = expf(-1.0f / (fm->depth_tau_s * SYNTH_RATE));
        voice->lfo_step = fm->tremolo_hz / SYNTH_RATE;
        break;
    }
    case ENGINE_ORGAN:
        start_organ(voice, freq);
        break;
    case ENGINE_STRINGS: {
        const strings_spec_t *strings = spec->spec;
        float apart = cents(strings->detune_cents);
        voice->step = freq * apart / SYNTH_RATE;
        voice->step2 = freq / apart / SYNTH_RATE;
        if (!retrigger) {
            voice->phase2 = 0.37f;
        }
        float cutoff = strings->cutoff_base + strings->cutoff_track * freq;
        cutoff = cutoff > 6000.0f ? 6000.0f : cutoff;
        voice->lp_coef = 1.0f - expf(-2.0f * PI * cutoff / SYNTH_RATE);
        voice->lfo_step = strings->vibrato_hz / SYNTH_RATE;
        break;
    }
    case ENGINE_SYNTH: {
        const synth_spec_t *synth = spec->spec;
        voice->step2 = freq * 1.003f / SYNTH_RATE;
        voice->cutoff_peak = fminf(freq * synth->peak, 7000.0f);
        voice->cutoff_floor = fminf(freq * synth->floor, 7000.0f);
        voice->sweep_attack = synth->sweep_attack_s > 0.0f ? 1.0f / (synth->sweep_attack_s * SYNTH_RATE) : 0.0f;
        voice->sweep = voice->sweep_attack > 0.0f ? 0.0f : 1.0f;
        voice->cutoff = voice->sweep_attack > 0.0f ? voice->cutoff_floor : voice->cutoff_peak;
        voice->sweep_coef = expf(-1.0f / (synth->sweep_decay_s * SYNTH_RATE));
        break;
    }
    case ENGINE_PLUCK: {
        const pluck_spec_t *pluck = spec->spec;
        int length = (int)(SYNTH_RATE / freq + 0.5f);
        length = length < 2 ? 2 : length > PLUCK_MAX ? PLUCK_MAX : length;
        voice->pluck_length = (uint16_t)length;
        voice->pluck_pos = 0;
        static float burst[PLUCK_MAX]; /* off the audio task's stack */
        for (int i = 0; i < length; ++i) {
            burst[i] = noise();
        }
        /* Softened so the string isn't harsh: more passes, a softer pluck. */
        for (int pass = 0; pass < pluck->softening; ++pass) {
            float previous = burst[length - 1];
            for (int i = 0; i < length; ++i) {
                float value = burst[i];
                burst[i] = 0.5f * (value + previous);
                previous = value;
            }
        }
        for (int i = 0; i < length; ++i) {
            float mixed = (retrigger ? voice->pluck[i] / 32767.0f * 0.3f : 0.0f) +
                          pluck->brightness * burst[i] * (pluck->softening > 1 ? 1.6f : 1.0f);
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
    if (voice->stage == STAGE_OFF) {
        return;
    }
    voice->stage = STAGE_RELEASE;
    if (voice->spec->engine == ENGINE_MODAL) {
        /* The damper: every resonator rings out over the release time,
           unless it would have died sooner anyway. */
        float r = coef_for(voice->spec->shape.release_s);
        for (int m = 0; m < voice->modes; ++m) {
            if (r * r < voice->c2[m]) {
                voice->c1[m] = 2.0f * r * voice->cosw[m];
                voice->c2[m] = r * r;
            }
        }
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

/* ---- Public ------------------------------------------------------------------ */

void synth_init(void)
{
    for (int i = 0; i <= SINE_SIZE; ++i) {
        sine_table[i] = sinf(2.0f * PI * (float)i / SINE_SIZE);
    }
    memset(voices, 0, sizeof(voices));
    for (int i = 0; i < SYNTH_VOICES; ++i) {
        voices[i].pluck = pluck_lines[i];
    }
    memset(&click, 0, sizeof(click));
    memset(style_counts, 0, sizeof(style_counts));
    for (int i = 0; i < INSTRUMENTS; ++i) {
        int voice = instruments[i].voice;
        style_index[voice][style_counts[voice]++] = (int8_t)i;
    }
}

const char *synth_voice_name(int voice)
{
    return voice >= 0 && voice < VOICE_KINDS ? voice_names[voice] : "?";
}

int synth_style_count(int voice)
{
    return voice >= 0 && voice < VOICE_KINDS ? style_counts[voice] : 0;
}

int synth_instrument(int voice, int style)
{
    if (voice < 0 || voice >= VOICE_KINDS || style_counts[voice] == 0) {
        return 0;
    }
    if (style < 0 || style >= style_counts[voice]) {
        style = 0;
    }
    return style_index[voice][style];
}

int synth_instrument_count(void)
{
    return INSTRUMENTS;
}

const char *synth_style_name(int instrument)
{
    return instrument >= 0 && instrument < INSTRUMENTS ? instruments[instrument].name : "?";
}

const char *synth_style_about(int instrument)
{
    return instrument >= 0 && instrument < INSTRUMENTS ? instruments[instrument].about : "";
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
