#pragma once

/* Reads WAV files into the kit's format: 44100 Hz mono 16-bit. */

#include <stdint.h>

#include "esp_err.h"

/* Reads the WAV at path into a new PSRAM buffer of 44100 Hz mono 16-bit
   samples, scaled so its loudest sample is peak (0-1) of full scale, with
   a 5 ms fade at the end. Reads 8, 16, 24 and 32-bit PCM and 32-bit float,
   any number of channels (mixed to mono) and any sample rate from 8 to
   192 kHz (resampled by linear interpolation). Audio past max_seconds is
   cut. Fails with ESP_ERR_NOT_FOUND (can't open), ESP_ERR_INVALID_ARG (not
   a WAV), ESP_ERR_NOT_SUPPORTED (another encoding, such as ADPCM),
   ESP_ERR_INVALID_SIZE (no audio) or ESP_ERR_NO_MEM. Free *pcm with
   free(). */
esp_err_t wav_load(const char *path, float max_seconds, float peak, int16_t **pcm,
                   uint32_t *frames);
