/* ODIN S3 for the Waveshare ESP32-S3-Touch-LCD-7.

   A port of the Odin 2 synthesizer's sound engine (TheWaveWarden,
   https://github.com/TheWaveWarden/odin2, GPL-3.0) with Odin's factory
   presets, played on a two-octave touch keyboard with pitch-bend and mod
   strips and Odin's XY pad. Sound goes out through components/music_out:
   an AirPlay speaker, or the wired DAC where music_out has one.

   engine.c is Odin's voices and signal flow, filters.c its filters and
   fx.c its distortion and effects; presets.c and wavetables.bin are made
   from Odin's sources by tools/odin_convert.py. audio.c runs the engine in
   the audio task and ui.c draws the screens.

   This program is free software: you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation, either version 3 of the License, or (at your
   option) any later version. See LICENSE. */

#include "audio.h"
#include "cyd_display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "music_out.h"
#include "nvs_flash.h"
#include "ui.h"

#define DEFAULT_BPM 120

static QueueHandle_t touch_queue;
static int bpm;

static void ui_task(void *argument)
{
    (void)argument;
    ui_run(touch_queue, bpm);
}

void app_main(void)
{
    ESP_ERROR_CHECK(cyd_display_init());

    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);

    /* The tempo the music apps share, for synced LFOs and effects. */
    bpm = music_out_load_tempo(DEFAULT_BPM);

    /* Starts silent: no speaker until one is picked in the menu. */
    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));
    ESP_ERROR_CHECK(audio_start(touch_queue));

    BaseType_t result = xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL);
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
