#pragma once

/* Between the picker (music_out.c) and the audio task (music_out_stream.c). */

#include "airplay_sender.h"
#include "esp_err.h"
#include "music_out.h"

esp_err_t music_out_stream_start(const music_out_config_t *config);

/* Sends the sound to a connected speaker, or with NULL to nowhere.
   Switching waits for the write in progress, so the caller can then
   disconnect the old speaker's handle. */
void music_out_stream_set_output(airplay_sender_handle_t speaker);
