/* LiveDrum for the Waveshare ESP32-S3-Touch-LCD-7.

   A drum pad set for playing live: 8, 12 or 16 large pads that sound on
   touch-down, each with a sound picked from DrumLoop's kits or a WAV on
   the SD card, a level, a pitch and a choke group; a loop recorder with
   quantize, overdub and undo; and banks that save it all to the SD card.

   The engine and screens (ld_engine.c, ld_sounds.c, ld_ui.c) are shared
   with Drum&Key, whose LIVE screen runs them too. The kits, WAV reader and
   SD card come from apps/drumloop/main. This file starts it all and runs
   the transport in music_out's audio task. */

#include "cyd_display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kit.h"
#include "ld_engine.h"
#include "ld_sounds.h"
#include "ld_ui.h"
#include "music_out.h"
#include "nvs_flash.h"
#include "sdcard.h"

#define DEFAULT_BPM 100

static QueueHandle_t touch_queue;
static volatile uint16_t tempo = DEFAULT_BPM;
static volatile bool want_playing;
static volatile int want_count_in;

/* ---- In the audio task ------------------------------------------------------ */

static bool render(int16_t *pcm, size_t frames, bool speaker_attached, void *context)
{
    (void)speaker_attached;
    (void)context;
    bool flush = false;
    if (want_playing && !ld_engine_playing()) {
        ld_engine_start(want_count_in);
    } else if (!want_playing && ld_engine_playing()) {
        ld_engine_stop();
        flush = true; /* a stop is heard at once, not after the buffer */
    }
    ld_engine_render(pcm, frames, tempo, music_out_frames_written());
    return flush;
}

/* ---- The host, for the screens ---------------------------------------------- */

static void play(bool on, int count_in)
{
    want_count_in = count_in;
    want_playing = on;
}

static bool is_playing(void)
{
    return want_playing;
}

static uint16_t get_tempo(void)
{
    return tempo;
}

static void set_tempo(int bpm)
{
    tempo = (uint16_t)(bpm < MUSIC_TEMPO_MIN ? MUSIC_TEMPO_MIN : bpm > MUSIC_TEMPO_MAX ? MUSIC_TEMPO_MAX : bpm);
}

static const ld_host_t host = {
    .title = " LIVEDRUM SETTINGS",
    .play = play,
    .is_playing = is_playing,
    .tempo = get_tempo,
    .set_tempo = set_tempo,
    .switch_label = NULL, /* BANK */
    .has_count_in = true,
};

static void ui_task(void *argument)
{
    (void)argument;
    while (true) {
        ld_screen(touch_queue, &host);
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
    set_tempo(music_out_load_tempo(DEFAULT_BPM));

    ESP_ERROR_CHECK(ld_engine_init());
    ld_sounds_default();

    /* Starts silent: no speaker until one is picked in the menu. */
    ESP_ERROR_CHECK(cyd_touch_start(&touch_queue));
    music_out_config_t config = MUSIC_OUT_DEFAULT_CONFIG();
    config.title = " LIVEDRUM";
    config.render = render;
    config.task_core = 1; /* away from Wi-Fi and the display's interrupts on core 0 */
    ESP_ERROR_CHECK(music_out_start(&config, touch_queue));

    BaseType_t result = xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL);
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
