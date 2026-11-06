# ESP32 CYD Touchscreen Calibration

PlatformIO/ESP-IDF application that calibrates the XPT2046 resistive touchscreen on the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display).

This app lives in `apps/touchscreen_cal`. The [root README](../../README.md) lists every app in this repository.

## What it does

1. It shows a grid and asks you to tap a red `+` near each corner in turn: upper left, upper right, lower right, lower left. The targets sit 20 pixels in from the edges, because the very edge of a resistive panel is its least linear part. A yellow dot shows where the current calibration put each tap.
2. It fits an **affine calibration** to the four raw readings by least squares: `x = xa*raw_x + xb*raw_y + xc`, and likewise for `y`. A plain per-axis scale only corrects gain and offset; this also corrects a touch layer that is slightly rotated or skewed against the LCD. If the worst corner lands more than 25 pixels from its target after fitting, a corner was mistapped, so it asks you to retry.
3. A fifth tap on a center target checks the result. The center was not used for fitting, so it's an honest test. The screen shows the error in pixels with the current calibration (yellow) and the new one (green). Tap RETRY or SAVE.
4. SAVE stores the calibration in NVS (`cyd_config/touch_cal`). A test screen then draws a dot wherever you tap, using the new calibration. AGAIN recalibrates, CLEAR wipes the dots, and EN returns to the launcher.

Every image picks up the saved calibration, because the shared driver in `components/cyd_display` (one copy at the repository root, used by every app) loads it at startup. It falls back to the CYD documentation's ranges if there is none, or if the saved one looks corrupt.

The driver also reads each tap more robustly. It takes 9 readings per axis, drops the 2 highest and 2 lowest, and averages the middle 5, so a single noisy reading, common as a finger lands, can't drag the position. Each tap carries both the calibrated position and the raw readings (`raw_x`, `raw_y`), and the serial log shows both.

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core. The first build installs the ESP32 platform and ESP-IDF toolchain. The project path must be short: builds fail on Windows when source paths pass 260 characters.

```powershell
cd apps\touchscreen_cal
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e esp32
```

The app binary is `%USERPROFILE%\.platformio\cyd-touch-cal-build\esp32\firmware.bin`. Use it in one of two ways:

- **As a launcher image** (board runs the [launcher](../../launcher)): when a change to this folder is merged into `main`, `.github/workflows/build.yml` builds the app and publishes it as `firmware.bin` on the release tagged `touchscreen_cal`, and the ESP32-S3 image as `firmware-esp32s3.bin`. The launcher finds it in the app catalog and downloads that file. You can also write it to a slot over USB, as described in the [launcher README](../../launcher/README.md).
- **On its own:** `& $pio run --target upload --upload-port COM11`. This replaces everything on the board, including the launcher.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21, PWM-dimmed to the brightness saved by the launcher. The panel fills a drawing window column by column, so pixel data is sent column-major.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36 (XPT2046, 12-bit, differential mode).
