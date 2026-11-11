/* The audio task: calls the app's render callback and writes each 8 ms
   block to the speaker, or runs it on the clock when silent. */

#include <stdatomic.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "music_out.h"
#include "music_out_internal.h"

#define WRITE_TIMEOUT_MS 500

static const char *TAG = "music_out";

static music_out_config_t config;
/* Covers speaker and every call on it, so the UI's volume changes and
   detaching never run during a write. */
static SemaphoreHandle_t lock;
/* Callers waiting for lock. While a speaker plays, the audio task holds
   lock through each paced write and takes it again at once, and it runs
   above the UI, so it steps aside for a tick whenever someone waits. */
static atomic_int lock_waiters;
static airplay_sender_handle_t speaker;
static volatile uint32_t frames_written;
static volatile uint32_t latency_frames;
/* Silent: no speaker, so the clock paces the render. */
static bool silent_restart;
static bool output_changed;

static void take_lock(void)
{
    atomic_fetch_add(&lock_waiters, 1);
    xSemaphoreTake(lock, portMAX_DELAY);
    atomic_fetch_sub(&lock_waiters, 1);
}

static bool render_block(int16_t *pcm, bool attached)
{
    if (output_changed) {
        output_changed = false;
        if (config.on_change != NULL) {
            config.on_change(config.context);
        }
    }
    return config.render(pcm, MUSIC_OUT_BLOCK_FRAMES, attached, config.context);
}

static void audio_task(void *argument)
{
    (void)argument;
    static int16_t pcm[MUSIC_OUT_BLOCK_FRAMES];
    int64_t silent_start_us = 0;
    uint64_t silent_frames = 0;
    while (true) {
        take_lock();
        if (speaker != NULL) {
            if (render_block(pcm, true)) {
                airplay_sender_stop(speaker);
            }
            esp_err_t result = airplay_sender_write(speaker, pcm, MUSIC_OUT_BLOCK_FRAMES,
                                                    pdMS_TO_TICKS(WRITE_TIMEOUT_MS), NULL);
            xSemaphoreGive(lock);
            frames_written += MUSIC_OUT_BLOCK_FRAMES;
            if (result != ESP_OK) {
                /* The speaker went away; the UI hears of it from the
                   sender's event and detaches. Don't spin meanwhile. */
                vTaskDelay(pdMS_TO_TICKS(20));
            } else if (atomic_load(&lock_waiters) > 0) {
                vTaskDelay(1);
            }
            continue;
        }
        if (silent_restart) {
            silent_restart = false;
            silent_start_us = esp_timer_get_time();
            silent_frames = 0;
        }
        /* No speaker to pace the writes, so the clock does: render every
           block that is due, as if a speaker played it. */
        uint64_t due = (uint64_t)(esp_timer_get_time() - silent_start_us) * MUSIC_OUT_RATE / 1000000;
        while (silent_frames + MUSIC_OUT_BLOCK_FRAMES <= due) {
            render_block(pcm, false);
            silent_frames += MUSIC_OUT_BLOCK_FRAMES;
            frames_written += MUSIC_OUT_BLOCK_FRAMES;
        }
        xSemaphoreGive(lock);
        vTaskDelay(1);
    }
}

esp_err_t music_out_stream_start(const music_out_config_t *app_config)
{
    config = *app_config;
    lock = xSemaphoreCreateMutex();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    silent_restart = true;
    BaseType_t created = xTaskCreatePinnedToCore(audio_task, "audio", config.task_stack, NULL,
                                                 config.task_priority, NULL,
                                                 config.task_core < 0 ? tskNO_AFFINITY
                                                                      : config.task_core);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void music_out_stream_set_output(airplay_sender_handle_t new_speaker)
{
    take_lock();
    speaker = new_speaker;
    silent_restart = new_speaker == NULL;
    latency_frames = new_speaker != NULL ? (uint32_t)((uint64_t)airplay_sender_latency_ms(new_speaker) *
                                                      MUSIC_OUT_RATE / 1000)
                                         : 0;
    output_changed = true;
    ESP_LOGI(TAG, "Output: %s, latency %lu frames", new_speaker != NULL ? "speaker" : "silent",
             (unsigned long)latency_frames);
    xSemaphoreGive(lock);
}

bool music_out_has_speaker(void)
{
    take_lock();
    bool attached = speaker != NULL;
    xSemaphoreGive(lock);
    return attached;
}

esp_err_t music_out_set_volume(int volume)
{
    take_lock();
    esp_err_t result = speaker != NULL ? airplay_sender_set_volume(speaker, volume)
                                       : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(lock);
    return result;
}

int music_out_get_volume(void)
{
    take_lock();
    int volume = speaker != NULL ? airplay_sender_get_volume(speaker) : 0;
    xSemaphoreGive(lock);
    return volume;
}

uint32_t music_out_frames_written(void)
{
    return frames_written;
}

uint32_t music_out_latency_frames(void)
{
    return latency_frames;
}

uint32_t music_out_heard_frame(void)
{
    return frames_written - latency_frames;
}
