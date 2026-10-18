# ESP32 CYD Launcher and Boot Images

PlatformIO/ESP-IDF firmware for the 320x240 display and resistive touchscreen on the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display).

The board runs a **launcher** on every power-on. The launcher picks one of two stored **boot images** or downloads a new one from this repository's GitHub releases. The first boot image is the Wi-Fi and Bluetooth scanner in this folder.

| Path | What it is |
|---|---|
| `launcher/` | Launcher firmware, custom second-stage bootloader and partition table |
| `main/` | Wi-Fi and Bluetooth scanner boot image |
| `components/cyd_display/` | Display, 5x7 font and touchscreen driver shared by both |

## Launcher

On power-on, including the EN button, the custom bootloader always starts the launcher. Any other reset, such as the launcher's own restart or an image crashing, goes straight back to the chosen image.

1. The menu immediately lists the stored images, marking the default with `*`, and counts down for 2 seconds.
2. If nobody touches the screen, the last-used image boots. The very first time, it is the first stored image.
3. Any tap stops the countdown. Tap an image to boot it.
4. Meanwhile the launcher joins Wi-Fi and reads the latest release of `rftek/cheap-yellow-board`. Each `.bin` asset not already stored appears in cyan as `+ name`. Tapping one downloads it into a slot, with progress, and boots it. If Wi-Fi or GitHub doesn't answer, the status row says `offline` and nothing else changes.

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

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core. The first build installs the ESP32 platform and ESP-IDF toolchain.

The launcher needs Wi-Fi credentials. Create `%USERPROFILE%\.cyd_private\wifi_credentials.h` from `launcher\main\wifi_credentials.example.h`. Never commit that file.

From PowerShell at the repository root:

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run                                   # scanner
& $pio run --project-dir launcher            # launcher + bootloader
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

To update only the scanner in slot A, write `0x150000 "$S\firmware.bin"` on its own. Change `COM11` to the board's serial port. The monitor exits with Ctrl+]. If you change an `sdkconfig.defaults`, delete the generated `sdkconfig.esp32dev` next to it so the new defaults apply.

The project path must be short. Builds fail on Windows when source paths pass 260 characters.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21. The panel fills a drawing window column by column, so pixel data is sent column-major.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36. Touch coordinates use the CYD documentation's XPT2046 calibration range (`TOUCH_RAW_*` in `components/cyd_display/cyd_display.c`) mapped to the 320x240 landscape orientation.
