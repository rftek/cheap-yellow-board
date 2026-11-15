#pragma once

/* Drum&Key's patch synth: a subtractive and wavetable synthesizer whose
   patches play as the PATCH voice's styles on the keys and in the Piano
   Roll (Keyboard's synth hands them over, synth_set_external).

   A patch:
     - two oscillators: analog saw, square (pulse width) and triangle, a
       sine, or one of three morphing wavetables (BASIC, VOCAL, DIGITAL),
       each with up to 7-voice unison and detune
     - a sub oscillator (sine or square, an octave down) and noise
     - two filters (LP, HP, BP, notch, comb, formant) with drive, in
       series or in parallel, and key tracking for the first
     - an amp envelope and a mod envelope (ADSR), two LFOs (sine, triangle,
       saw, square, random, gate steps, stair steps; free or tempo synced)
     - an 8-slot mod matrix: LFOs, the mod envelope, the note and a
       per-note random value onto pitch, wavetable positions, detune,
       cutoffs, resonance, the second oscillator's level and the amp
     - effects: drive, chorus, tempo-synced delay and reverb
   Up to 6 notes sound at once across the patches. The output is mono. */

#include <stdbool.h>
#include <stdint.h>

#define PSYNTH_SLOTS 4
#define PSYNTH_UNISON_MAX 7
#define PSYNTH_MODS 8
#define PSYNTH_NAME_MAX 10

typedef enum {
    OSC_SAW,
    OSC_SQUARE,
    OSC_TRIANGLE,
    OSC_SINE,
    OSC_WT_BASIC,   /* sine, triangle, saw, square: position morphs them */
    OSC_WT_VOCAL,   /* vowels A E I O U */
    OSC_WT_DIGITAL, /* a resonant peak sweeping up the harmonics */
    OSC_SHAPES,
} psynth_shape_t;

typedef enum {
    FILTER_OFF,
    FILTER_LOWPASS,
    FILTER_HIGHPASS,
    FILTER_BANDPASS,
    FILTER_NOTCH,
    FILTER_COMB,
    FILTER_FORMANT, /* cutoff morphs the vowel */
    FILTER_TYPES,
} psynth_filter_type_t;

typedef enum {
    LFO_SINE,
    LFO_TRIANGLE,
    LFO_SAW,
    LFO_SQUARE,
    LFO_RANDOM, /* sample and hold */
    LFO_GATE,   /* 8 on/off steps, for rhythmic gating */
    LFO_STAIRS, /* 8 rising steps */
    LFO_SHAPES,
} psynth_lfo_shape_t;

typedef enum {
    SYNC_FREE,
    SYNC_1_1,
    SYNC_1_2,
    SYNC_1_4,
    SYNC_1_8,
    SYNC_1_16,
    SYNC_1_8T,
    SYNC_MODES,
} psynth_sync_t;

typedef enum {
    SOURCE_NONE,
    SOURCE_LFO1,
    SOURCE_LFO2,
    SOURCE_MOD_ENV,
    SOURCE_NOTE,   /* the note's pitch, middle C at 0 */
    SOURCE_RANDOM, /* a value picked at each note */
    MOD_SOURCES,
} psynth_mod_source_t;

typedef enum {
    DEST_NONE,
    DEST_PITCH,
    DEST_OSC1_POS,
    DEST_OSC2_POS,
    DEST_DETUNE,
    DEST_CUTOFF1,
    DEST_CUTOFF2,
    DEST_RESO1,
    DEST_OSC2_LEVEL,
    DEST_AMP,
    MOD_DESTS,
} psynth_mod_dest_t;

typedef enum {
    DELAY_1_16,
    DELAY_1_8,
    DELAY_3_16, /* a dotted eighth */
    DELAY_1_4,
    DELAY_3_8,
    DELAY_1_2,
    DELAY_TIMES,
} psynth_delay_time_t;

/* Every field is a byte, set from the SYNTH screen while it plays. */
typedef struct {
    uint8_t shape;    /* psynth_shape_t */
    uint8_t position; /* 0-100: wavetable morph, or the square's width */
    uint8_t unison;   /* 1-7 voices */
    uint8_t detune;   /* 0-100: up to 50 cents either side */
    int8_t semi;      /* -24 to +24 */
    uint8_t level;    /* 0-100 */
} psynth_osc_t;

typedef struct {
    uint8_t type;      /* psynth_filter_type_t */
    uint8_t cutoff;    /* 0-100: 30 Hz to 18 kHz (the vowel, for formant) */
    uint8_t resonance; /* 0-100 */
    uint8_t drive;     /* 0-100 */
} psynth_filter_t;

typedef struct {
    uint8_t attack, decay, sustain, release; /* 0-100; times 1 ms to 8 s */
} psynth_env_t;

typedef struct {
    uint8_t shape; /* psynth_lfo_shape_t */
    uint8_t rate;  /* 0-100: 0.05 to 30 Hz, when free */
    uint8_t sync;  /* psynth_sync_t */
} psynth_lfo_t;

typedef struct {
    uint8_t source; /* psynth_mod_source_t */
    uint8_t dest;   /* psynth_mod_dest_t */
    int8_t amount;  /* -100 to 100 */
} psynth_mod_t;

typedef struct {
    char name[PSYNTH_NAME_MAX + 1];
    psynth_osc_t osc[2];
    uint8_t sub_level, sub_shape; /* shape 0 sine, 1 square */
    uint8_t noise_level, noise_color;
    psynth_filter_t filter[2];
    uint8_t routing;  /* 0 series, 1 parallel */
    uint8_t keytrack; /* 0-100: the first filter's cutoff follows the note */
    psynth_env_t env[2]; /* amp, mod */
    psynth_lfo_t lfo[2];
    psynth_mod_t mod[PSYNTH_MODS];
    uint8_t drive;  /* 0-100 */
    uint8_t chorus; /* 0-100 mix */
    uint8_t delay_time, delay_feedback, delay_mix;
    uint8_t reverb_size, reverb_mix;
    uint8_t volume; /* 0-100 */
} psynth_patch_t;

/* Builds the wavetables (in PSRAM), loads the presets (SUPERSAW, REESE,
   PLUCK, WOBBLE) and hands the engine to Keyboard's synth. Call after
   synth_init. */
void psynth_init(void);

psynth_patch_t *psynth_patch(int slot);

/* Resets a slot to its preset. */
void psynth_reset(int slot);

/* The tempo for synced LFOs and the delay. */
void psynth_set_tempo(uint16_t bpm);

/* The slot the oscilloscope listens to, and its latest samples (oldest
   first) at full scale; returns how many. */
void psynth_set_scope_slot(int slot);
int psynth_scope(float *out, int max);
