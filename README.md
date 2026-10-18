# ESP32 CYD BirdNET Viewer

PlatformIO/ESP-IDF application for the 320x240 display and resistive touchscreen on the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display).

Each application lives on its own branch of this repository:

| Branch | Application |
|---|---|
| `bootloader` | Launcher: a boot menu with two image slots |
| `wifi_checker` | Wi-Fi and Bluetooth LE scanner |
| `birdnet_viewer` | This BirdNET bird photo viewer |
| `screen_check` | Screen color test |

## What it does

The viewer connects to Wi-Fi and queries a local BirdNET server every two minutes, or immediately after a tap. The latest visit from today with a confidence of at least 80% is shown as a full-screen Wikimedia photo, with no text. If nothing qualifies, the screen clears. A failed refresh keeps the last photo.

This is the original viewer from `main`, ported to the shared display component (`components/cyd_display`). That port fixes swapped red and blue and transposed image tiles on this panel.

Set `BIRDNET_BASE_URL` in `%USERPROFILE%\.cyd_private\wifi_credentials.h` to your server, for example `#define BIRDNET_BASE_URL "http://192.168.1.50"`. It defaults to `http://birdnet.local`. The viewer uses:

- `GET {BIRDNET_BASE_URL}/api/v1/detections/visits`
- `GET {BIRDNET_BASE_URL}/api/v1/image/{URL-encoded-scientific-name}`

Wikimedia's 1280-pixel thumbnail is streamed over HTTPS straight into the ESP32 ROM JPEG decoder and scaled down to 320x240, so the full image is never held in RAM.

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core. The first build installs the ESP32 platform and ESP-IDF toolchain. The project path must be short: builds fail on Windows when source paths pass 260 characters.

Create `%USERPROFILE%\.cyd_private\wifi_credentials.h` from `main\wifi_credentials.example.h` with your Wi-Fi SSID, password and BirdNET URL. Never commit that file.

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run
```

The app binary is `%USERPROFILE%\.platformio\cyd-birdnet-build\esp32dev\firmware.bin`, about 1.0 MB. Use it in one of two ways:

- **As a launcher image** (board runs the `bootloader` branch): attach it to a GitHub release as `birdnet.bin`, or write it to a slot over USB, as described in the `bootloader` README.
- **On its own:** `& $pio run --target upload --upload-port COM11`. This replaces everything on the board, including the launcher.

Monitor with `& $pio device monitor --port COM11 --baud 115200`. Ctrl+] exits.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21. The panel fills a drawing window column by column, so pixel data is sent column-major.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36. Touch coordinates use the CYD documentation's XPT2046 calibration range (`TOUCH_RAW_*` in `components/cyd_display/cyd_display.c`) mapped to the 320x240 landscape orientation.
