#pragma once

/* Audio export, a stub for now.

   export_render() already works: it renders one pass of the loop, plus a
   tail so the last hits ring out, into a WAV file image in PSRAM.
   export_save() is the part still to write. It should hand the WAV to a
   destination, such as an SD card, an HTTP download from the board, or an
   upload to a computer on the network, and returns ESP_ERR_NOT_SUPPORTED
   until one exists. */

#include <stddef.h>
#include <stdint.h>

#include "engine.h"
#include "esp_err.h"

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

/* Saves a rendered export under name. Not written yet. */
esp_err_t export_save(const drum_export_t *export, const char *name);

void export_free(drum_export_t *export);
