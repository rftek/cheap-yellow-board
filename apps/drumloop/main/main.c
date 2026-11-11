/* DrumLoop for the Waveshare ESP32-S3-Touch-LCD-7.

   A live drum machine: four drum kits (808, 909, trap and DnB) on touch pads and a step
   sequencer of up to 16 bars. It boots straight to the pads with Wi-Fi off
   and no speaker; Wi-Fi and the AirPlay speaker are chosen in the menu.

   ui.c is the performance screen and menu, audio.c runs the engine
   (engine.c) through music_out (Wi-Fi, speaker and streaming), kit.c
   makes the sounds and export.c renders the loop for a future export. */

#include "audio.h"
#include "cyd_display.h"
#include "engine.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kit.h"
#include "music_out.h"
#include "nvs_flash.h"
#include "sdcard.h"
#include "ui.h"

#define DEFAULT_BPM 140

static QueueHandle_t touch_queue;

/* The loop. It outlives speaker connections, so a beat survives a change
   of output; it is not saved across power cycles. */
static drum_pattern_t pattern = {.bars = 1, .bpm = DEFAULT_BPM, .kit = KIT_808};

static void ui_task(void *argument)
{
    (void)argument;
    ui_run(touch_queue, &pattern);
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

    /* The tempo the music apps share (Keyboard's, or the last one set). */
    pattern.bpm = music_out_load_tempo(DEFAULT_BPM);

    /* Starts silent: no speaker until one is picked in the menu. */
    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));
    ESP_ERROR_CHECK(audio_start(&pattern, touch_queue));

    BaseType_t result = xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL);
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
