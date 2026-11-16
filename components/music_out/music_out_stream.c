/* The audio task: calls the app's render callback and writes each 8 ms
   block to the speaker or the wired DAC, or runs it on the clock when
   silent. */

#include <stdatomic.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "music_out.h"
#include "music_out_internal.h"

#if MUSIC_OUT_HAS_DAC
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_rom_serial_output.h"
#include "pcm5102a.h"
#include "soc/gpio_sig_map.h"
#endif

#define WRITE_TIMEOUT_MS 500

/* The wired DAC's queue: 9 buffers of an eighth of a block (44 frames),
   9 ms in all. A write returns once a block's last buffer is queued, so 8
   buffers, a whole block (8 ms), are still ahead of the DAC while the next
   block renders: small buffers buy that margin without a long queue. On
   the S3 the render competes with the display for PSRAM (frame buffer,
   code and samples) and can take several ms. */
#define DAC_BUFFERS 9
#define DAC_FRAMES (MUSIC_OUT_BLOCK_FRAMES / 8)
#define DAC_DEFAULT_VOLUME 70

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

/* The wired DAC belongs to the audio task, which opens and closes it on
   request between blocks, so a write never races a close. */
static volatile int dac_request = -1; /* -1 none, 0 stop, 1 start */
static SemaphoreHandle_t dac_done;
static esp_err_t dac_result;
static volatile bool dac_running;
static volatile int dac_volume = DAC_DEFAULT_VOLUME;
#if MUSIC_OUT_HAS_DAC
static pcm5102a_handle_t dac;
#endif

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

#if MUSIC_OUT_HAS_DAC
/* GPIO43 is the console's TX as well as the DAC's LRCK: hand it back to
   UART0 once the DAC stops, so the log shows again. */
static void restore_console_tx(void)
{
    gpio_set_direction(GPIO_NUM_43, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(GPIO_NUM_43, U0TXD_OUT_IDX, false, false);
}

static void serve_dac_request(void)
{
    int request = dac_request;
    if (request < 0) {
        return;
    }
    dac_request = -1;
    esp_err_t result = ESP_OK;
    if (request == 1 && dac == NULL) {
        /* Logged first and sent out: from here on GPIO43 carries LRCK, not
           the log. */
        ESP_LOGI(TAG, "Output: wired DAC on GPIO44/43/6");
        esp_rom_output_tx_wait_idle(0);
        pcm5102a_config_t dac_config = PCM5102A_WAVESHARE_S3_CONFIG();
        dac_config.sample_rate_hz = MUSIC_OUT_RATE;
        dac_config.dma_buffers = DAC_BUFFERS;
        dac_config.dma_frames = DAC_FRAMES;
        result = pcm5102a_open(&dac_config, &dac);
        if (result == ESP_OK) {
            latency_frames = (uint32_t)((uint64_t)pcm5102a_latency_us(dac) * MUSIC_OUT_RATE / 1000000);
            dac_running = true;
            output_changed = true;
        } else {
            dac = NULL;
            restore_console_tx();
            ESP_LOGW(TAG, "Wired DAC: %s", esp_err_to_name(result));
        }
    } else if (request == 0 && dac != NULL) {
        uint32_t underruns = pcm5102a_underruns(dac);
        pcm5102a_close(dac);
        dac = NULL;
        dac_running = false;
        restore_console_tx();
        latency_frames = 0;
        silent_restart = true;
        output_changed = true;
        ESP_LOGI(TAG, "Output: silent; the wired DAC had %lu underruns", (unsigned long)underruns);
    }
    dac_result = result;
    xSemaphoreGive(dac_done);
}

/* Volume 0-100 as gain (volume/100)^2: 70 is about -3 dB, 50 about -6 dB. */
static void apply_dac_volume(int16_t *pcm, size_t frames)
{
    int volume = dac_volume;
    if (volume >= 100) {
        return;
    }
    int32_t gain = volume * volume * 32768 / 10000;
    for (size_t i = 0; i < frames; ++i) {
        pcm[i] = (int16_t)((pcm[i] * gain) >> 15);
    }
}
#endif

static void audio_task(void *argument)
{
    (void)argument;
    static int16_t pcm[MUSIC_OUT_BLOCK_FRAMES];
    int64_t silent_start_us = 0;
    uint64_t silent_frames = 0;
    while (true) {
#if MUSIC_OUT_HAS_DAC
        serve_dac_request();
        if (dac != NULL) {
            /* The DAC's DMA queue paces the task. */
            render_block(pcm, true);
            apply_dac_volume(pcm, MUSIC_OUT_BLOCK_FRAMES);
            esp_err_t result = pcm5102a_write(dac, pcm, MUSIC_OUT_BLOCK_FRAMES, 1, WRITE_TIMEOUT_MS);
            frames_written += MUSIC_OUT_BLOCK_FRAMES;
            if (result != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            continue;
        }
#endif
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
    dac_done = xSemaphoreCreateBinary();
    if (lock == NULL || dac_done == NULL) {
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

esp_err_t music_out_stream_set_dac(bool on)
{
#if MUSIC_OUT_HAS_DAC
    dac_request = on ? 1 : 0;
    xSemaphoreTake(dac_done, portMAX_DELAY);
    return dac_result;
#else
    return on ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
#endif
}

bool music_out_stream_dac_on(void)
{
    return dac_running;
}

uint32_t music_out_stream_dac_gaps(void)
{
#if MUSIC_OUT_HAS_DAC
    pcm5102a_handle_t handle = dac;
    return dac_running && handle != NULL ? pcm5102a_underruns(handle) : 0;
#else
    return 0;
#endif
}

bool music_out_has_speaker(void)
{
    if (dac_running) {
        return true;
    }
    take_lock();
    bool attached = speaker != NULL;
    xSemaphoreGive(lock);
    return attached;
}

esp_err_t music_out_set_volume(int volume)
{
    if (dac_running) {
        dac_volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
        return ESP_OK;
    }
    take_lock();
    esp_err_t result = speaker != NULL ? airplay_sender_set_volume(speaker, volume)
                                       : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(lock);
    return result;
}

int music_out_get_volume(void)
{
    if (dac_running) {
        return dac_volume;
    }
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
