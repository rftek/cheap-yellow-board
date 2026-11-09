# ESP32 CYD Screen Check

PlatformIO/ESP-IDF display test for the 320x240 screen and resistive touchscreen on the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display).

This app lives in `apps/screen_check`. The [root README](../../README.md) lists every app in this repository.

## What it does

Steps through full-screen test patterns. Tap the right half of the screen for the next pattern and the left half for the previous one. The list wraps around. Each pattern's name shows on the bottom row for 1.5 seconds, then disappears so it doesn't get in the way of the test. Under the launcher, press EN to return to the boot menu.

| Pattern | What to look for |
|---|---|
| Red, green, blue, white, black, cyan, magenta, yellow, 50% gray | Dead or stuck pixels, uneven backlight, tint |
| Color bars | Correct color order (white, yellow, cyan, green, magenta, red, blue, black); swapped red and blue |
| RGB ramps | Smooth red, green, blue and gray gradients; banding |
| Gray steps | 16 distinguishable steps from black to white |
| Checkerboard | 1-pixel alternation; flicker or crosstalk |
| Grid | Straight 40-pixel grid lines and a 1-pixel red border on all four edges; clipping or offset |

It needs no Wi-Fi or credentials. Patterns are drawn pixel by pixel through `cyd_draw_pattern` in `components/cyd_display`, without a frame buffer.

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core. The first build installs the ESP32 platform and ESP-IDF toolchain. The project path must be short: builds fail on Windows when source paths pass 260 characters.

```powershell
cd apps\screen_check
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e esp32
```

The app binary is `%USERPROFILE%\.platformio\cyd-screen-check-build\esp32\firmware.bin`, about 180 KB. Use it in one of two ways:

- **As a launcher image** (board runs the [launcher](../../launcher)): when a change to this folder is merged into `main`, `.github/workflows/build.yml` builds the app and publishes it as `firmware.bin` on the release tagged `screen_check`, and the ESP32-S3 image as `firmware-esp32s3.bin`. The launcher finds it in the app catalog and downloads that file. You can also write it to a slot over USB, as described in the [launcher README](../../launcher/README.md).
- **On its own:** `& $pio run --target upload --upload-port COM11`. This replaces everything on the board, including the launcher.

Monitor with `& $pio device monitor --port COM11 --baud 115200`. Ctrl+] exits.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21, PWM-dimmed to the brightness saved by the launcher. The panel fills a drawing window column by column, so pixel data is sent column-major.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36. Touch coordinates use the CYD documentation's XPT2046 calibration range (`TOUCH_RAW_*` in `components/cyd_display/cyd_display_esp32_cyd.c`) mapped to the 320x240 landscape orientation.

## Changelog

The version is in [`version.txt`](version.txt) and in the image's app description, where the launcher reads it. Every change to this app bumps it and adds an entry here, newest first; the [root README](../../README.md#versions) explains when to bump MAJOR or MINOR.

### 1.0 (2026-11-08)

- First numbered version. Full-screen test patterns (solid colors, color bars, RGB ramps, gray steps, checkerboard and grid), stepped forward and back by tapping either half of the screen.
