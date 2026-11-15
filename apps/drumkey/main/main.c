/* Drum&Key for the Waveshare ESP32-S3-Touch-LCD-7.

   DrumLoop and Keyboard in one app, on one speaker: the drum pads and step
   sequencer on one screen, the two-octave keyboard and its loop recorder
   on the other, and a button in the header to switch. Both loops share the
   transport and tempo, and their sound is mixed into one AirPlay stream.

   The engines are the apps' own: apps/drumloop/main (engine, kits, export,
   SD card) and apps/keyboard/main (synth, looper) are compiled in from
   there. dk_audio.c runs both in the audio task; drums_ui.c and keys_ui.c
   are the two screens, adapted from the apps' ui.c. */

#include "cyd_display.h"
#include "dk_audio.h"
#include "dk_ui.h"
#include "engine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kit.h"
#include "music_out.h"
#include "nvs_flash.h"
#include "sdcard.h"

#define DEFAULT_BPM 120

static QueueHandle_t touch_queue;

/* The drum loop, which also holds the shared tempo. It is not saved across
   power cycles. */
static drum_pattern_t pattern = {.bars = 1, .bpm = DEFAULT_BPM, .kit = KIT_808};

static void ui_task(void *argument)
{
    (void)argument;
    while (true) {
        drums_screen(touch_queue);
        keys_screen(touch_queue);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(cyd_display_init());
    ESP_ERROR_CHECK(cyd_text_row(CYD_TEXT_ROWS / 2 - 1, CYD_COLOR_YELLOW, CYD_COLOR_BLACK,
                                 " Building the drum kits"));
    ESP_ERROR_CHECK(kit_init());

    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);

    /* Kits on an SD card are listed now and read when first picked. */
    if (sdcard_mount() == ESP_OK) {
        kit_scan(KIT_FOLDER);
    }

    /* The tempo the music apps share. */
    pattern.bpm = music_out_load_tempo(DEFAULT_BPM);

    /* Starts silent: no speaker until one is picked in a menu. */
    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));
    ESP_ERROR_CHECK(dk_audio_start(&pattern, touch_queue));

    BaseType_t result = xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL);
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
