# ESP32 CYD Wi-Fi and Bluetooth Scanner

PlatformIO/ESP-IDF application for the 320x240 display and resistive touchscreen on the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display).

Each application lives on its own branch of this repository:

| Branch | Application |
|---|---|
| `bootloader` | Launcher: a boot menu with two image slots |
| `wifi_checker` | This Wi-Fi and Bluetooth LE scanner |
| `birdnet_viewer` | BirdNET bird photo viewer |
| `screen_check` | Screen color test |

## What it does

On boot the scanner scans for nearby Wi-Fi networks, then for Bluetooth Low Energy devices (6-second active scan), and lists both in large text:

- **Wi-Fi:** signal (dBm) and SSID, strongest first.
- **Bluetooth LE:** signal (dBm) and advertised name, named devices first, then strongest first. Devices without a name show their address in gray.

The touchscreen drives the UI:

- **Entry buttons:** tap a network or device to see its details (Wi-Fi: SSID, RSSI, channel, auth, cipher, BSSID, PHY/WPS; BLE: name, RSSI, address and address type, advertising type, TX power, manufacturer, first 16-bit service UUID). Tap anywhere to go back.
- **Arrow buttons:** the up/down arrows on each list header page through that list. They are dimmed when there is nothing more in that direction.
- **Header title:** tap "WI-FI" or "BLE" to scan again.

The full results, including Wi-Fi BSSIDs, are also printed to the serial log, along with the coordinates of every tap.

The scanner needs no network connection or credentials; it only listens. Classic (BR/EDR) Bluetooth devices are not scanned; only BLE, using the lightweight NimBLE host.

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core. The first build installs the ESP32 platform and ESP-IDF toolchain. The project path must be short: builds fail on Windows when source paths pass 260 characters.

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run
```

The app binary is `%USERPROFILE%\.platformio\cyd-build\esp32dev\firmware.bin`, about 825 KB. Use it in one of two ways:

- **As a launcher image** (board runs the `bootloader` branch): every push to this branch runs `.github/workflows/build.yml`, which builds the app and publishes it as `firmware.bin` on the release tagged `wifi_checker`. The launcher lists the branch and downloads that file. You can also write it to a slot over USB, as described in the `bootloader` README.
- **On its own:** `& $pio run --target upload --upload-port COM11`. This replaces everything on the board, including the launcher.

Monitor with `& $pio device monitor --port COM11 --baud 115200`. Ctrl+] exits. If you change `sdkconfig.defaults`, delete the generated `sdkconfig.esp32dev` so the new defaults apply.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21, PWM-dimmed to the brightness saved by the launcher. The panel fills a drawing window column by column, so pixel data is sent column-major.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36. Touch coordinates use the CYD documentation's XPT2046 calibration range (`TOUCH_RAW_*` in `components/cyd_display/cyd_display.c`) mapped to the 320x240 landscape orientation.
