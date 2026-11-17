# pcm5102a

ESP-IDF driver for TI's PCM5102A, a 32-bit stereo audio DAC with a 2.1 Vrms line output. It plays 16-bit audio, mono or stereo, through the ESP32's I2S transmitter, with the ESP32 as clock master. The DMA queue is small and its size can be set, so the delay from writing a sample to hearing it can be a few milliseconds. Any app can use it; [apps/pcm5102a_test](../../apps/pcm5102a_test) is the test app and has the wiring for the Waveshare S3 board.

## The chip

- **Interface:** I2S, or left-justified with FMT high. There is no control port: the SCK, FMT, FLT, XSMT and DEMP pins set the mode. On the common purple module, these are the SCK pad and solder jumpers H1-H4 on the back.
- **Clocks:** with SCK tied to GND, the DAC's PLL makes its system clock from BCK, so it needs only three wires: BCK, LRCK and DIN. The PLL needs BCK at 32 or 64 fs. This driver sends 32-bit slots, so BCK = 64 fs, which the PLL accepts from 16 kHz (1.024 MHz) to 192 kHz (12.288 MHz). 8 kHz needs an external SCK, so this driver doesn't support it.
- **Latency:** the interpolation filter delays the output by 20 sample periods (FLT low, 0.45 ms at 44.1 kHz), or by 3.5 sample periods with the low-latency filter (FLT high, 0.08 ms).
- **Mute:** XSMT high plays; low ramps the volume down over 104 samples. When the clocks stop, the DAC mutes on its own and goes into power-save. Its PLL starts again after BCK and LRCK have run for 16 LRCK periods with SCK low.
- **Output:** 2.1 Vrms, centred on ground (no DC-blocking capacitors needed), into loads of 1 kΩ or more: a line input, not a speaker.
- **Supplies:** 3.3 V analog; 1.8 V or 3.3 V digital. The inputs are failsafe Schmitt triggers, so 3.3 V logic connects directly. The purple module takes 3.3-5 V on VIN and has its own regulators.

## Use

```c
#include "pcm5102a.h"

pcm5102a_config_t config = PCM5102A_WAVESHARE_S3_CONFIG(); /* or fill in your own pins */
config.dma_frames = 64;        /* 3 x 64 frames queued: 4.4 ms at 44.1 kHz */
pcm5102a_handle_t dac;
ESP_ERROR_CHECK(pcm5102a_open(&config, &dac));

int16_t block[64];             /* mono */
for (;;) {
    my_synth_render(block, 64);
    pcm5102a_write(dac, block, 64, 1, 100);   /* blocks: paces the loop */
}
```

| Setting | Meaning |
|---|---|
| `dma_buffers`, `dma_frames` | Up to `dma_buffers × dma_frames` frames wait in the DMA queue. Smaller means less delay. The writer must keep the queue fed: an empty buffer plays as silence and counts in `pcm5102a_underruns`. 3 × 64 at 44.1 kHz is 4.4 ms. |
| `low_latency_filter` | Set it to match the FLT pin. It changes nothing on the chip; only `pcm5102a_latency_us` uses it. |
| `drive` | Drive strength of the three output pins. `GPIO_DRIVE_CAP_1` is plenty for jumper wires. |

`pcm5102a_set_sample_rate` changes the rate without closing the driver. `pcm5102a_close` makes the three pins inputs again.

Add it to an app's `CMakeLists.txt` like the other shared components:

```cmake
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/cyd_display"
                         "${CMAKE_CURRENT_LIST_DIR}/../../components/pcm5102a")
```

Then add `pcm5102a` to `PRIV_REQUIRES` in `main/CMakeLists.txt`.

## Clock accuracy

The S3 has no audio PLL, so the I2S clock comes from its 160 MHz PLL through a fractional divider. 44.1 kHz and 48 kHz come out at the right average rate, with some jitter on BCK. The DAC's PLL locks to BCK and filters most of that jitter out. That's fine for a test app and for instruments; a hi-fi player would want an external audio clock.
