# ESP32 CYD Launcher and Boot Images

PlatformIO/ESP-IDF firmware for the 320x240 display and resistive touchscreen on the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display).

The board runs a **launcher** on every power-on. The launcher picks one of two stored **boot images** or downloads a new one from this repository's GitHub releases.

| Path | What it is |
|---|---|
| `launcher/` | Launcher firmware, custom second-stage bootloader and partition table |
| `main/` | Wi-Fi and Bluetooth scanner boot image |
| `apps/screen_check/` | Screen color test boot image |
| `apps/birdnet/` | BirdNET bird photo viewer boot image (the original app from `main`) |
| `components/cyd_display/` | Display, 5x7 font, pattern drawing and touchscreen driver shared by all of them |

## Launcher

On power-on, including the EN button, the custom bootloader always starts the launcher. Any other reset, such as the launcher's own restart or an image crashing, goes straight back to the chosen image.

1. The menu immediately lists the stored images, marking the default with `*`, and counts down from 20 seconds.
2. When the countdown ends, the last-used image boots. The very first time, it is the first stored image.
3. Tapping anywhere except an image adds another 20 seconds. Tap an image to boot it right away.
4. Meanwhile the launcher joins Wi-Fi and reads the latest release of `rftek/cheap-yellow-board`. This usually takes about 10 seconds, mostly the TLS handshake. Each `.bin` asset not already stored appears in cyan as `+ name`. Tapping one downloads it into a slot, with progress, and boots it. If Wi-Fi or GitHub doesn't answer, the status row says `offline` (or `no release`) and nothing else changes.

A download replaces an older copy of the same file name if one is stored. Otherwise it fills an empty slot, or overwrites the slot that does not boot by default. An asset counts as already stored when both its name and size match.

Flash layout (4 MB):

| Partition | Offset | Size | Contents |
|---|---|---|---|
| `nvs` | 0x9000 | 16 KB | Slot names and sizes; Wi-Fi data |
| `otadata` | 0xd000 | 8 KB | Last-chosen image |
| `factory` | 0x10000 | 1.25 MB | Launcher (about 1.0 MB) |
| `ota_0` | 0x150000 | 1.31 MB | Boot image slot A |
| `ota_1` | 0x2A0000 | 1.31 MB | Boot image slot B |

### Publishing a boot image

Create a GitHub release in this repository and attach the image's **app** binary, the `firmware.bin` that PlatformIO builds, as a `.bin` asset under 1.31 MB. Do not use a merged image that includes a bootloader. The launcher only reads the latest release.

## Scanner image

The scanner scans for nearby Wi-Fi networks, then for Bluetooth Low Energy devices (6-second active scan), and lists both in large text:

- **Wi-Fi:** signal (dBm) and SSID, strongest first.
- **Bluetooth LE:** signal (dBm) and advertised name, named devices first, then strongest first. Devices without a name show their address in gray.

The touchscreen drives the UI:

- **Entry buttons:** tap a network or device to see its details (Wi-Fi: SSID, RSSI, channel, auth, cipher, BSSID, PHY/WPS; BLE: name, RSSI, address and address type, advertising type, TX power, manufacturer, first 16-bit service UUID). Tap anywhere to go back.
- **Arrow buttons:** the up/down arrows on each list header page through that list. They are dimmed when there is nothing more in that direction.
- **Header title:** tap "WI-FI" or "BLE" to scan again.

The full results, including Wi-Fi BSSIDs, are also printed to the serial log, along with the coordinates of every tap.

The scanner needs no network connection or credentials; it only listens. Classic (BR/EDR) Bluetooth devices are not scanned; only BLE, using the lightweight NimBLE host.

## Screen check image

A full-screen display test. Tap the right half of the screen for the next pattern and the left half for the previous one. The list wraps around. Each pattern's name shows on the bottom row for 1.5 seconds, then disappears so it doesn't get in the way of the test. Press EN to return to the launcher.

| Pattern | What to look for |
|---|---|
| Red, green, blue, white, black, cyan, magenta, yellow, 50% gray | Dead or stuck pixels, uneven backlight, tint |
| Color bars | Correct color order (white, yellow, cyan, green, magenta, red, blue, black); swapped red and blue |
| RGB ramps | Smooth red, green, blue and gray gradients; banding |
| Gray steps | 16 distinguishable steps from black to white |
| Checkerboard | 1-pixel alternation; flicker or crosstalk |
| Grid | Straight 40-pixel grid lines and a 1-pixel red border on all four edges; clipping or offset |

## BirdNET image

The original viewer from the `main` branch, ported to the shared display component. That port also fixes swapped red and blue and transposed image tiles on this panel. It connects to Wi-Fi and queries a local BirdNET server every two minutes, or immediately after a tap. The latest visit from today with a confidence of at least 80% is shown as a full-screen Wikimedia photo, with no text. If nothing qualifies, the screen clears. A failed refresh keeps the last photo.

Set `BIRDNET_BASE_URL` in `%USERPROFILE%\.cyd_private\wifi_credentials.h` to your server, for example `#define BIRDNET_BASE_URL "http://192.168.1.50"`. It defaults to `http://birdnet.local`. The image uses:

- `GET {BIRDNET_BASE_URL}/api/v1/detections/visits`
- `GET {BIRDNET_BASE_URL}/api/v1/image/{URL-encoded-scientific-name}`

Wikimedia's 1280-pixel thumbnail is streamed over HTTPS straight into the ESP32 ROM JPEG decoder and scaled down to 320x240, so the full image is never held in RAM.

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core. The first build installs the ESP32 platform and ESP-IDF toolchain.

The launcher and the BirdNET image need Wi-Fi credentials. Create `%USERPROFILE%\.cyd_private\wifi_credentials.h` from `apps\birdnet\main\wifi_credentials.example.h`, which also has `BIRDNET_BASE_URL`. Never commit that file.

From PowerShell at the repository root:

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run                                   # scanner
& $pio run --project-dir launcher            # launcher + bootloader
& $pio run --project-dir apps\screen_check   # screen check
& $pio run --project-dir apps\birdnet        # BirdNET viewer
```

**Do not use `pio run --target upload` for either project.** It would flash a stock bootloader or put the scanner over the launcher. Flash everything with esptool instead. This erases the board:

```powershell
$py = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$esptool = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
$L = "$env:USERPROFILE\.platformio\cyd-launcher-build\esp32dev"
$S = "$env:USERPROFILE\.platformio\cyd-build\esp32dev"
& $py $esptool --chip esp32 -p COM11 erase_flash
& $py $esptool --chip esp32 -p COM11 -b 230400 write_flash `
    0x1000 "$L\bootloader.bin" 0x8000 "$L\partitions.bin" 0xd000 "$L\ota_data_initial.bin" `
    0x10000 "$L\firmware.bin" 0x150000 "$S\firmware.bin"
& $pio device monitor --port COM11 --baud 115200
```

To update a single slot, write only that image: `0x150000` for slot A (the scanner above), or `0x2A0000` for slot B, for example `0x2A0000 "$env:USERPROFILE\.platformio\cyd-screen-check-build\esp32dev\firmware.bin"`. Change `COM11` to the board's serial port. The monitor exits with Ctrl+]. If you change an `sdkconfig.defaults`, delete the generated `sdkconfig.esp32dev` next to it so the new defaults apply.

The project path must be short. Builds fail on Windows when source paths pass 260 characters.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21. The panel fills a drawing window column by column, so pixel data is sent column-major.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36. Touch coordinates use the CYD documentation's XPT2046 calibration range (`TOUCH_RAW_*` in `components/cyd_display/cyd_display.c`) mapped to the 320x240 landscape orientation.
