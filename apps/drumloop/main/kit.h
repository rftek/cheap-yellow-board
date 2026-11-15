#pragma once

/* The drum kits: eight sounds each, synthesized at boot into PSRAM as
   44100 Hz mono 16-bit samples. */

#include <stdbool.h>
#include <stddef.h>
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

/* Kits from the SD card come after the built-in ones: up to KIT_CARD_MAX
   folders of WAVs, each sample cut at KIT_CARD_SECONDS. */
#define KIT_CARD_MAX 8
#define KIT_CARD_SECONDS 4.0f
#define KIT_NAME_MAX 10

/* Looks in dir for kit folders and adds the new ones; returns how many.
   Their WAVs are read later, by kit_load. */
int kit_scan(const char *dir);

/* The built-in kits and the card's. */
int kit_count(void);
bool kit_on_card(int kit);

/* Makes sure a kit's sounds are in memory, reading a card kit's WAVs the
   first time; they then stay until power off. In each folder, a WAV whose
   name starts with 1-8 goes on that pad, labelled with the next two
   letters or digits ("1 BD kick.wav" is pad 1, BD). Pads with no WAV keep
   the 808's sound. Writes a line for the screen into status. */
esp_err_t kit_load(int kit, char *status, size_t status_size);

/* A kit's sound for a track. An unknown kit, or a card kit not loaded yet,
   gives the 808's. */
const drum_sound_t *kit_sound(int kit, int track);

/* The kit's name ("808", or a card kit's folder name) and a few words
   about it ("CLASSIC", "SD CARD"). */
const char *kit_name(int kit);
const char *kit_about(int kit);
