/* Keyboard for the Waveshare ESP32-S3-Touch-LCD-7.

   A two-octave touch keyboard with seven synthesized voices, eight notes
   at once, a sustain pedal and a loop recorder with overdub, played on an
   AirPlay speaker. It shares its tempo, the AirPlay delay and its sound
   output (components/music_out) with DrumLoop.

   ui.c is the screen and menu and plays the keys from the touch task;
   audio.c runs the looper (looper.c) and synth (synth.c) in the audio
   task. */

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

static void ui_task(void *argument)
{
    (void)argument;
    ui_run(touch_queue);
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

    /* The tempo the music apps share (DrumLoop's, or the last one set). */
    audio_settings.bpm = music_out_load_tempo(DEFAULT_BPM);

    /* Starts silent: no speaker until one is picked in the menu. */
    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));
    ESP_ERROR_CHECK(audio_start(touch_queue));

    BaseType_t result = xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL);
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
