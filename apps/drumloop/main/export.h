#pragma once

/* Audio export: export_render() renders one pass of the loop, plus a tail
   so the last hits ring out, into a WAV file image in PSRAM, and
   export_save() writes it to the SD card. Other destinations, such as an
   HTTP download from the board, could take the same WAV image. */

#include <stddef.h>
#include <stdint.h>

#include "engine.h"
#include "esp_err.h"
#include "sdcard.h"

#define EXPORT_TAIL_FRAMES DRUM_RATE /* 1 s */

typedef struct {
    uint8_t *wav;    /* 44-byte RIFF header, then 44100 Hz mono 16-bit PCM */
    size_t size;     /* bytes in wav */
    uint32_t frames; /* audio frames in wav */
} drum_export_t;

/* Renders the pattern's loop (a copy is taken, so the live pattern can keep
   changing). Fails with ESP_ERR_NO_MEM when the loop is too long for free
   PSRAM: 16 bars at 60 BPM is about 5.7 MB. */
esp_err_t export_render(const drum_pattern_t *pattern, drum_export_t *out);

/* Writes a rendered export to the SD card as EXPORT_DIR/loop-NNN.wav, the
   first number not taken, and puts the file's name in saved. Fails with
   ESP_ERR_NOT_FOUND when there is no card. */
#define EXPORT_DIR SDCARD_ROOT "/exports"
esp_err_t export_save(const drum_export_t *export, char *saved, size_t saved_size);

void export_free(drum_export_t *export);
