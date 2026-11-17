#pragma once

/* TI PCM5102A: a 32-bit stereo audio DAC (16-192 kHz here) with a 2.1 Vrms
   line output, driven through the ESP32's I2S transmitter with the ESP32 as
   clock master.

   It has no control port. Its pins set the mode (on the common purple
   module, solder jumpers H1-H4 on the back):
     SCK    tied to GND: the DAC's PLL makes its system clock from BCK, so
            only BCK, LRCK and DIN are wired (3-wire I2S)
     FMT    low = I2S (what this driver sends), high = left-justified
     FLT    low = normal filter, 20 samples of delay;
            high = low-latency filter, 3.5 samples
     XSMT   high = playing, low = soft mute
     DEMP   low = no de-emphasis

   Samples go out as 32-bit slots (BCK = 64 fs), which the PLL accepts from
   16 kHz to 192 kHz. pcm5102a_write takes 16-bit audio, mono or stereo. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    uint32_t sample_rate_hz; /* 16000 to 192000 */
    gpio_num_t bck_gpio;
    gpio_num_t lrck_gpio;
    gpio_num_t din_gpio; /* the DAC's DIN, an output here */
    /* Latency: up to dma_buffers x dma_frames frames are queued ahead of
       the DAC. Fewer and shorter buffers mean less delay, but the writer
       must keep up or the DAC plays silence (pcm5102a_underruns). */
    uint8_t dma_buffers; /* 2 to 16 */
    uint16_t dma_frames; /* 8 to 511 frames per buffer */
    /* How FLT is wired; only pcm5102a_latency_us uses it. */
    bool low_latency_filter;
    /* Output drive of the three pins. GPIO_DRIVE_CAP_1 (about 10 mA) is
       plenty for jumper wires at 12 MHz and rings less. */
    gpio_drive_cap_t drive;
} pcm5102a_config_t;

/* The Waveshare ESP32-S3-Touch-LCD-7 wiring in apps/pcm5102a_test: the
   three GPIOs that reach its headers unused. BCK on GPIO44 and LRCK on
   GPIO43 are the UART2 header (UART switch on UART2); DIN on GPIO6 is the
   Sensor AD header. The same pins and roles as the PCM1808's wiring. */
#define PCM5102A_WAVESHARE_S3_CONFIG()      \
    {                                       \
        .sample_rate_hz = 44100,            \
        .bck_gpio = GPIO_NUM_44,            \
        .lrck_gpio = GPIO_NUM_43,           \
        .din_gpio = GPIO_NUM_6,             \
        .dma_buffers = 3,                   \
        .dma_frames = 64,                   \
        .low_latency_filter = false,        \
        .drive = GPIO_DRIVE_CAP_1,          \
    }

typedef struct pcm5102a *pcm5102a_handle_t;

/* Starts the I2S transmitter, playing silence. BCK and LRCK run from here
   on; the DAC starts its PLL after 16 LRCK periods. */
esp_err_t pcm5102a_open(const pcm5102a_config_t *config, pcm5102a_handle_t *handle);

/* Queues frames of 16-bit audio: channels 1 (the same sample to left and
   right) or 2 (left, right, left ...). Blocks until all are queued, which
   paces the caller to the sample rate, or until timeout_ms passes
   (ESP_ERR_TIMEOUT). */
esp_err_t pcm5102a_write(pcm5102a_handle_t handle, const int16_t *samples, size_t frames,
                         int channels, uint32_t timeout_ms);

/* Changes the sample rate without closing. The DAC mutes while it relocks. */
esp_err_t pcm5102a_set_sample_rate(pcm5102a_handle_t handle, uint32_t sample_rate_hz);
uint32_t pcm5102a_sample_rate(pcm5102a_handle_t handle);

/* The longest delay from a pcm5102a_write to the line output: the DMA
   queue plus the DAC's interpolation filter. */
uint32_t pcm5102a_latency_us(pcm5102a_handle_t handle);

/* DMA buffers that ran out of new audio since open (the DAC played zeros
   instead), counted from the first write. */
uint32_t pcm5102a_underruns(pcm5102a_handle_t handle);

/* Stops the transmitter, makes the three pins inputs and frees the handle.
   The DAC sees its clocks stop and mutes. */
void pcm5102a_close(pcm5102a_handle_t handle);
