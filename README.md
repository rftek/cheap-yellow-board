# ESP32 CYD Screen Check

PlatformIO/ESP-IDF display test for the 320x240 screen and resistive touchscreen on the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display).

Each application lives on its own branch of this repository:

| Branch | Application |
|---|---|
| `bootloader` | Launcher: a boot menu with two image slots |
| `wifi_checker` | Wi-Fi and Bluetooth LE scanner |
| `birdnet_viewer` | BirdNET bird photo viewer |
| `screen_check` | This screen color test |
| `touchscreen_cal` | Touchscreen calibration |
| `stopwatch` | LVGL stopwatch |
| `gear_roll` | LVGL gear demo |
| `Datalogger` | Voltage logger on GPIO35 |

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
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run
```

The app binary is `%USERPROFILE%\.platformio\cyd-screen-check-build\esp32dev\firmware.bin`, about 180 KB. Use it in one of two ways:

- **As a launcher image** (board runs the `bootloader` branch): every push to this branch runs `.github/workflows/build.yml`, which builds the app and publishes it as `firmware.bin` on the release tagged `screen_check`. The launcher lists the branch and downloads that file. You can also write it to a slot over USB, as described in the `bootloader` README.
- **On its own:** `& $pio run --target upload --upload-port COM11`. This replaces everything on the board, including the launcher.

Monitor with `& $pio device monitor --port COM11 --baud 115200`. Ctrl+] exits.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21, PWM-dimmed to the brightness saved by the launcher. The panel fills a drawing window column by column, so pixel data is sent column-major.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36. Touch coordinates use the CYD documentation's XPT2046 calibration range (`TOUCH_RAW_*` in `components/cyd_display/cyd_display.c`) mapped to the 320x240 landscape orientation.
