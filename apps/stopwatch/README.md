# ESP32 CYD Stopwatch

PlatformIO/ESP-IDF stopwatch built with [LVGL](https://lvgl.io) for the 320x240 display and resistive touchscreen on the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display).

This app lives in `apps/stopwatch`. The [root README](../../README.md) lists every app in this repository.

## What it does

The watch face is a round dial with a tick mark for every second (long ones every 5) and an orange arc that sweeps around once a minute. The time is shown in the middle as `MM:SS.cc`, minutes, seconds and hundredths, switching to `H:MM:SS` after 100 minutes. A status line under it reads READY, RUNNING or STOPPED.

| Button | Action |
|---|---|
| **START** (green) | Starts, or resumes after STOP. Dimmed while running. |
| **STOP** (red) | Pauses, keeping the time. Dimmed while stopped. |
| **CLEAR** (gray) | Stops and resets to zero. |

Timing uses the ESP32's microsecond timer, so the display rate doesn't affect accuracy. The face updates about 30 times a second while running. Under the launcher, press EN to return to the boot menu.

## How LVGL is wired in

- **LVGL 9:** fetched at build time by the ESP-IDF component manager (`main/idf_component.yml`). Its options, such as fonts and widgets, are in `sdkconfig.defaults`.
- **Display:** LVGL renders 24-row strips in RGB565. The flush callback hands each strip to `cyd_draw_pattern` in `components/cyd_display`, which sends it in the column-major order this panel needs and keeps the saved launcher brightness.
- **Touch:** LVGL polls `cyd_touch_read`, which applies the calibration saved by `touchscreen_cal`. Pressing reads as LVGL's pressed state, so buttons show feedback while held.

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core. The first build installs the ESP32 platform and ESP-IDF toolchain and downloads LVGL. The project path must be short: builds fail on Windows when source paths pass 260 characters.

```powershell
cd apps\stopwatch
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e esp32
```

The app binary is `%USERPROFILE%\.platformio\cyd-stopwatch-build\esp32\firmware.bin`. Use it in one of two ways:

- **As a launcher image** (board runs the [launcher](../../launcher)): when a change to this folder is merged into `main`, `.github/workflows/build.yml` builds the app and publishes it as `firmware.bin` on the release tagged `stopwatch`, and the ESP32-S3 image as `firmware-esp32s3.bin`. The launcher finds it in the app catalog and downloads that file. You can also write it to a slot over USB, as described in the [launcher README](../../launcher/README.md).
- **On its own:** `& $pio run --target upload --upload-port COM11`. This replaces everything on the board, including the launcher.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21, PWM-dimmed to the brightness saved by the launcher.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36 (XPT2046).

## Changelog

The version is in [`version.txt`](version.txt) and in the image's app description, where the launcher reads it. Every change to this app bumps it and adds an entry here, newest first; the [root README](../../README.md#versions) explains when to bump MAJOR or MINOR.

### 1.0 (2026-11-08)

- First numbered version. An LVGL stopwatch with a sweeping dial, a `MM:SS.cc` readout and START, STOP and CLEAR buttons.
