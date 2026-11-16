#include "pcm5102a.h"

#include <stdlib.h>

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "pcm5102a";

/* 32-bit samples in 32-bit slots, left and right: 8 bytes a frame. The
   S3's DMA buffers hold at most 4092 bytes, so 511 frames. */
#define BYTES_PER_FRAME 8
#define MAX_DMA_FRAMES 511

struct pcm5102a {
    i2s_chan_handle_t tx;
    pcm5102a_config_t config;
    int32_t *scratch; /* one DMA buffer of converted frames */
    volatile uint32_t underruns;
    volatile bool writing; /* the first write has happened */
};

/* The DMA finished a buffer while every buffer queued behind it was still
   waiting for new audio: the writer is late, and auto-clear makes the DAC
   play zeros instead of repeating old audio. */
static bool IRAM_ATTR on_send_queue_overflow(i2s_chan_handle_t channel, i2s_event_data_t *event,
                                            void *context)
{
    struct pcm5102a *device = context;
    if (device->writing) {
        device->underruns++;
    }
    return false;
}

/* A plain input with no pulls and its output driver off, so nothing is
   driven once the DAC is closed. */
static void make_input(gpio_num_t gpio)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);
}

static bool rate_ok(uint32_t rate)
{
    /* The PLL locks to BCK = 64 fs from 16 kHz (1.024 MHz) to 192 kHz
       (12.288 MHz): datasheet table 11. */
    if (rate < 16000 || rate > 192000) {
        ESP_LOGE(TAG, "sample rate %lu Hz is outside 16-192 kHz", (unsigned long)rate);
        return false;
    }
    return true;
}

static bool check_config(const pcm5102a_config_t *config)
{
    if (!rate_ok(config->sample_rate_hz)) {
        return false;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(config->bck_gpio) || !GPIO_IS_VALID_OUTPUT_GPIO(config->lrck_gpio) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config->din_gpio)) {
        ESP_LOGE(TAG, "BCK, LRCK and DIN need output GPIOs");
        return false;
    }
    if (config->dma_buffers < 2 || config->dma_buffers > 16) {
        ESP_LOGE(TAG, "dma_buffers must be 2 to 16");
        return false;
    }
    if (config->dma_frames < 8 || config->dma_frames > MAX_DMA_FRAMES) {
        ESP_LOGE(TAG, "dma_frames must be 8 to %d", MAX_DMA_FRAMES);
        return false;
    }
    return true;
}

esp_err_t pcm5102a_open(const pcm5102a_config_t *config, pcm5102a_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(config != NULL && handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "config and handle are required");
    ESP_RETURN_ON_FALSE(check_config(config), ESP_ERR_INVALID_ARG, TAG, "bad config");

    /* Internal RAM: the overflow callback runs in an interrupt. */
    struct pcm5102a *device = heap_caps_calloc(1, sizeof(*device), MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(device != NULL, ESP_ERR_NO_MEM, TAG, "no memory for handle");
    device->config = *config;
    device->scratch = heap_caps_malloc((size_t)config->dma_frames * BYTES_PER_FRAME,
                                       MALLOC_CAP_INTERNAL);
    if (device->scratch == NULL) {
        free(device);
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel.dma_desc_num = config->dma_buffers;
    channel.dma_frame_num = config->dma_frames;
    channel.auto_clear_after_cb = true; /* zeros, not old audio, when the writer is late */
    esp_err_t err = i2s_new_channel(&channel, &device->tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new I2S channel: %s", esp_err_to_name(err));
        free(device->scratch);
        free(device);
        return err;
    }

    /* 32-bit data in 32-bit slots: BCK = 64 fs, which the DAC's PLL needs
       across the whole 16-192 kHz range, and MCLK (256 fs, kept inside the
       ESP32) divides down to it evenly. */
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, /* the DAC's SCK is tied to GND */
            .bclk = config->bck_gpio,
            .ws = config->lrck_gpio,
            .dout = config->din_gpio,
            .din = I2S_GPIO_UNUSED,
        },
    };
    i2s_event_callbacks_t callbacks = {
        .on_send_q_ovf = on_send_queue_overflow,
    };
    err = i2s_channel_init_std_mode(device->tx, &std);
    if (err == ESP_OK) {
        err = i2s_channel_register_event_callback(device->tx, &callbacks, device);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(device->tx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start I2S transmitter: %s", esp_err_to_name(err));
        i2s_del_channel(device->tx);
        make_input(config->bck_gpio);
        make_input(config->lrck_gpio);
        make_input(config->din_gpio);
        free(device->scratch);
        free(device);
        return err;
    }
    gpio_set_drive_capability(config->bck_gpio, config->drive);
    gpio_set_drive_capability(config->lrck_gpio, config->drive);
    gpio_set_drive_capability(config->din_gpio, config->drive);

    ESP_LOGI(TAG, "%lu Hz, BCK %d, LRCK %d, DIN %d, %u x %u frames queued",
             (unsigned long)config->sample_rate_hz, config->bck_gpio, config->lrck_gpio,
             config->din_gpio, config->dma_buffers, config->dma_frames);
    *handle = device;
    return ESP_OK;
}

esp_err_t pcm5102a_write(pcm5102a_handle_t handle, const int16_t *samples, size_t frames,
                         int channels, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(handle != NULL && samples != NULL && (channels == 1 || channels == 2),
                        ESP_ERR_INVALID_ARG, TAG, "handle, samples and 1 or 2 channels are required");
    handle->writing = true;
    size_t done = 0;
    while (done < frames) {
        size_t count = frames - done;
        if (count > handle->config.dma_frames) {
            count = handle->config.dma_frames;
        }
        /* 16-bit audio in the top of each 32-bit slot. */
        const int16_t *in = samples + done * channels;
        for (size_t i = 0; i < count; ++i) {
            int32_t left = (int32_t)in[i * channels] << 16;
            int32_t right = channels == 2 ? (int32_t)in[i * 2 + 1] << 16 : left;
            handle->scratch[i * 2] = left;
            handle->scratch[i * 2 + 1] = right;
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(handle->tx, handle->scratch, count * BYTES_PER_FRAME,
                                          &written, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        done += count;
    }
    return ESP_OK;
}

esp_err_t pcm5102a_set_sample_rate(pcm5102a_handle_t handle, uint32_t sample_rate_hz)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is required");
    ESP_RETURN_ON_FALSE(rate_ok(sample_rate_hz), ESP_ERR_INVALID_ARG, TAG, "bad sample rate");
    i2s_std_clk_config_t clock = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    ESP_RETURN_ON_ERROR(i2s_channel_disable(handle->tx), TAG, "stop");
    esp_err_t err = i2s_channel_reconfig_std_clock(handle->tx, &clock);
    if (err == ESP_OK) {
        handle->config.sample_rate_hz = sample_rate_hz;
    }
    esp_err_t restart = i2s_channel_enable(handle->tx);
    return err != ESP_OK ? err : restart;
}

uint32_t pcm5102a_sample_rate(pcm5102a_handle_t handle)
{
    return handle != NULL ? handle->config.sample_rate_hz : 0;
}

uint32_t pcm5102a_latency_us(pcm5102a_handle_t handle)
{
    if (handle == NULL) {
        return 0;
    }
    const pcm5102a_config_t *config = &handle->config;
    uint64_t queued = (uint64_t)config->dma_buffers * config->dma_frames * 1000000u;
    /* Filter group delay: 3.5 or 20 sample periods (datasheet 8.5). */
    uint64_t filter = config->low_latency_filter ? 3500000u : 20000000u;
    return (uint32_t)((queued + filter) / config->sample_rate_hz);
}

uint32_t pcm5102a_underruns(pcm5102a_handle_t handle)
{
    return handle != NULL ? handle->underruns : 0;
}

void pcm5102a_close(pcm5102a_handle_t handle)
{
    if (handle == NULL) {
        return;
    }
    i2s_channel_disable(handle->tx);
    i2s_del_channel(handle->tx);
    make_input(handle->config.bck_gpio);
    make_input(handle->config.lrck_gpio);
    make_input(handle->config.din_gpio);
    free(handle->scratch);
    free(handle);
}
