#pragma once

/* The drum kits: eight sounds each, synthesized at boot into PSRAM as
   44100 Hz mono 16-bit samples. */

#include <stdint.h>

#include "esp_err.h"

#define DRUM_RATE 44100
#define DRUM_TRACKS 8

typedef enum {
    KIT_808,
    KIT_909,
    KIT_TRAP,
    KIT_DNB,
    DRUM_KITS,
} drum_kit_t;

typedef enum {
    DRUM_KICK,
    DRUM_SNARE,
    DRUM_CLAP,
    DRUM_CLOSED_HAT,
    DRUM_OPEN_HAT,
    DRUM_TOM,
    DRUM_RIM,
    DRUM_EXTRA, /* cowbell, ride or snap, by kit */
} drum_id_t;

typedef struct {
    const int16_t *pcm;
    uint32_t frames;
    const char *label; /* two letters, as on a drum machine's panel */
    float gain;        /* mix level */
} drum_sound_t;

/* Synthesizes every kit; takes about a second. */
esp_err_t kit_init(void);

/* A kit's sound for a track. An unknown kit gives the 808's. */
const drum_sound_t *kit_sound(int kit, int track);

/* The kit's name ("808") and a few words about it ("CLASSIC"). */
const char *kit_name(int kit);
const char *kit_about(int kit);
