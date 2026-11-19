#pragma once

/* Where each LiveDrum pad's sound comes from, and the banks that save a
   whole set to the SD card.

   A pad plays a drum of one of DrumLoop's kits (kit.c, built in or from
   the card), a WAV file from the SD card, or, in Drum&Key, a copy of one
   of the SYNTH screen's rendered samples. WAVs and synth samples are
   copied into PSRAM for the pad, up to LD_SAMPLE_SECONDS, and freed when
   the pad gets another sound.

   A bank is a text file, LD_BANK_DIR/bank<N>.txt: the tempo, each pad's
   sound, level, pitch and choke group, and the loop. A synth sample is
   saved beside it as a WAV under a new name (synth001.wav...), since synth
   samples don't outlive a power cycle; it is never written over. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ld_engine.h"

#define LD_SAMPLE_SECONDS 4.0f
#define LD_PATH_MAX 128
#define LD_NAME_MAX 12
#define LD_BANKS 8
#define LD_BANK_DIR "/sd/livedrum"
#define LD_SAMPLE_DIR "/sd/samples"

typedef enum {
    LD_SOURCE_NONE,
    LD_SOURCE_KIT,
    LD_SOURCE_WAV,
    LD_SOURCE_SYNTH,
} ld_source_kind_t;

typedef struct {
    uint8_t kind;  /* ld_source_kind_t */
    uint8_t kit;   /* LD_SOURCE_KIT: kit and track (drum) in kit.h */
    uint8_t track;
    uint8_t synth; /* LD_SOURCE_SYNTH: sample slot */
    char path[LD_PATH_MAX]; /* LD_SOURCE_WAV */
} ld_source_t;

/* Synth samples, from the host app (Drum&Key's SYNTH screen); count 0
   where there are none. */
typedef struct {
    int count;
    const char *(*name)(int sample);
    /* The sample's PCM (44100 Hz mono) and length, or NULL when empty. */
    const int16_t *(*pcm)(int sample, uint32_t *frames);
} ld_synth_source_t;

void ld_sounds_set_synth(const ld_synth_source_t *synth);
const ld_synth_source_t *ld_sounds_synth(void);

/* The starting set: the 808 on pads 1-8, then the 909's and trap kit's
   kick, snare, clap and hats, with the hats in choke group 1. */
void ld_sounds_default(void);

/* Gives a pad a sound. status gets a line for the screen. */
esp_err_t ld_sounds_assign_kit(int pad, int kit, int track, char *status, size_t status_size);
esp_err_t ld_sounds_assign_wav(int pad, const char *path, char *status, size_t status_size);
esp_err_t ld_sounds_assign_synth(int pad, int sample, char *status, size_t status_size);
void ld_sounds_clear(int pad);

const ld_source_t *ld_sounds_source(int pad);

/* A short name for the pad, such as "BD 808", "kick2" or "S1 SUPERSAW". */
const char *ld_sounds_name(int pad);

/* ---- SD card ----------------------------------------------------------------- */

/* Folder listings for the WAV picker: folders first, then .wav files, by
   name. Returns how many entries were read (up to max). */
#define LD_ENTRY_MAX 64
typedef struct {
    char name[LD_ENTRY_MAX];
    bool folder;
} ld_entry_t;

int ld_sounds_list(const char *folder, ld_entry_t *entries, int max);

/* A line about the bank on the card ("120 BPM 2B 85H"), "EMPTY", or false
   with no card. */
bool ld_bank_info(int bank, char *info, size_t info_size);

/* Saves the pads and loop as bank (0-based), or loads them; *bpm is the
   tempo saved, or read (left alone when the bank has none). */
esp_err_t ld_bank_save(int bank, uint16_t bpm, char *status, size_t status_size);
esp_err_t ld_bank_load(int bank, uint16_t *bpm, char *status, size_t status_size);
