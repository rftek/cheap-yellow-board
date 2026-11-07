#pragma once

/* The drum kit: eight 808-style sounds, synthesized at boot into PSRAM as
   44100 Hz mono 16-bit samples. */

#include <stdint.h>

#include "esp_err.h"

#define DRUM_RATE 44100
#define DRUM_TRACKS 8

typedef enum {
    DRUM_KICK,
    DRUM_SNARE,
    DRUM_CLAP,
    DRUM_CLOSED_HAT,
    DRUM_OPEN_HAT,
    DRUM_TOM,
    DRUM_RIM,
    DRUM_COWBELL,
} drum_id_t;

typedef struct {
    const int16_t *pcm;
    uint32_t frames;
    const char *label; /* two letters, as on the 808's panel */
    float gain;        /* mix level */
} drum_sound_t;

/* Synthesizes the kit; takes a fraction of a second. */
esp_err_t kit_init(void);

const drum_sound_t *kit_sound(int track);
