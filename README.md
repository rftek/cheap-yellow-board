# ESP32 CYD Bootloader and Launcher

PlatformIO/ESP-IDF firmware for the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display). It turns the board into a two-slot boot menu.

Each application lives on its own branch of this repository:

| Branch | Application |
|---|---|
| `bootloader` | This launcher, its custom second-stage bootloader and the partition table |
| `wifi_checker` | Wi-Fi and Bluetooth LE scanner |
| `birdnet_viewer` | BirdNET bird photo viewer |
| `screen_check` | Screen color test |

Every application branch builds an app binary that runs in either launcher slot. `components/cyd_display` (display, 5x7 font, patterns and touch) is copied on each branch.

## How it boots

On power-on, including the EN button, the custom bootloader (`bootloader_components/main`) always starts the launcher. Any other reset, such as the launcher's own restart or an image crashing, goes straight back to the chosen image.

1. The menu immediately lists the stored images, marking the default with `*`, and counts down from 20 seconds.
2. When the countdown ends, the last-used image boots. The very first time, it is the first stored image.
3. Tapping anywhere except an image adds another 20 seconds. Tap an image to boot it right away.
4. Meanwhile the launcher joins Wi-Fi and reads the latest release of `rftek/cheap-yellow-board`. This usually takes about 10 seconds, mostly the TLS handshake. Each `.bin` asset not already stored appears in cyan as `+ name`. Tapping one downloads it into a slot, with progress, and boots it. If Wi-Fi or GitHub doesn't answer, the status row says `offline` (or `no release`) and nothing else changes.

A download replaces an older copy of the same file name if one is stored. Otherwise it fills an empty slot, or overwrites the slot that does not boot by default. An asset counts as already stored when both its name and size match.

Flash layout (4 MB, `partitions.csv`):

| Partition | Offset | Size | Contents |
|---|---|---|---|
| `nvs` | 0x9000 | 16 KB | Slot names and sizes; Wi-Fi data |
| `otadata` | 0xd000 | 8 KB | Last-chosen image |
| `factory` | 0x10000 | 1.25 MB | Launcher (about 1.0 MB) |
| `ota_0` | 0x150000 | 1.31 MB | Boot image slot A |
| `ota_1` | 0x2A0000 | 1.31 MB | Boot image slot B |

## Publishing a boot image

Build an application branch and attach its **app** binary, the `firmware.bin` that PlatformIO builds, to a GitHub release of this repository as a `.bin` asset under 1.31 MB, for example `scanner.bin`. Do not use a merged image that includes a bootloader. The launcher only reads the latest release.

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core. The first build installs the ESP32 platform and ESP-IDF toolchain. The project path must be short: builds fail on Windows when source paths pass 260 characters.

The launcher needs Wi-Fi credentials. Create `%USERPROFILE%\.cyd_private\wifi_credentials.h` from `main\wifi_credentials.example.h`. Never commit that file.

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run
```

**Do not use `pio run --target upload`.** It would flash a stock bootloader. Flash with esptool instead. This erases the board, including both image slots:

```powershell
$py = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$esptool = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
$L = "$env:USERPROFILE\.platformio\cyd-launcher-build\esp32dev"
& $py $esptool --chip esp32 -p COM11 erase_flash
& $py $esptool --chip esp32 -p COM11 -b 230400 write_flash `
    0x1000 "$L\bootloader.bin" 0x8000 "$L\partitions.bin" `
    0xd000 "$L\ota_data_initial.bin" 0x10000 "$L\firmware.bin"
& $pio device monitor --port COM11 --baud 115200
```

To put an application into a slot over USB, build it on its branch and write its `firmware.bin` to `0x150000` (slot A) or `0x2A0000` (slot B):

```powershell
& $py $esptool --chip esp32 -p COM11 -b 230400 write_flash 0x150000 path\to\firmware.bin
```

Change `COM11` to the board's serial port. The monitor exits with Ctrl+]. If you change `sdkconfig.defaults`, delete the generated `sdkconfig.esp32dev` so the new defaults apply.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21. The panel fills a drawing window column by column, so pixel data is sent column-major.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36. Touch coordinates use the CYD documentation's XPT2046 calibration range (`TOUCH_RAW_*` in `components/cyd_display/cyd_display.c`) mapped to the 320x240 landscape orientation.
