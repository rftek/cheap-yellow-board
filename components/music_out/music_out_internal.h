#pragma once

/* Between the picker (music_out.c) and the audio task (music_out_stream.c). */

#include "airplay_sender.h"
#include "esp_err.h"
#include "music_out.h"
#include "sdkconfig.h"

/* The wired DAC: a PCM5102A on the Waveshare ESP32-S3-Touch-LCD-7's free
   header pins (PCM5102A_WAVESHARE_S3_CONFIG). */
#if CONFIG_IDF_TARGET_ESP32S3
#define MUSIC_OUT_HAS_DAC 1
#else
#define MUSIC_OUT_HAS_DAC 0
#endif
#define MUSIC_OUT_DAC_NAME "Wired DAC"

esp_err_t music_out_stream_start(const music_out_config_t *config);

/* Sends the sound to a connected speaker, or with NULL to nowhere.
   Switching waits for the write in progress, so the caller can then
   disconnect the old speaker's handle. Detach the wired DAC first. */
void music_out_stream_set_output(airplay_sender_handle_t speaker);

/* Starts or stops the wired DAC, from the audio task, and waits until it
   has. While it runs the sound goes there; detach any speaker first.
   ESP_ERR_NOT_SUPPORTED where the board has no DAC wiring. */
esp_err_t music_out_stream_set_dac(bool on);
bool music_out_stream_dac_on(void);

/* DMA buffers the DAC played as silence because a block came late, since
   it started; each is a click. */
uint32_t music_out_stream_dac_gaps(void);
