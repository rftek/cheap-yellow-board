# ESP32 CYD Bootloader and Launcher

PlatformIO/ESP-IDF firmware for the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display). It turns the board into a two-slot boot menu.

Each application lives on its own branch of this repository:

| Branch | Application |
|---|---|
| `bootloader` | This launcher, its custom second-stage bootloader and the partition table |
| `wifi_checker` | Wi-Fi and Bluetooth LE scanner |
| `birdnet_viewer` | BirdNET bird photo viewer |
| `screen_check` | Screen color test |
| `touchscreen_cal` | Touchscreen calibration |
| `stopwatch` | LVGL stopwatch |
| `gear_roll` | LVGL gear demo |
| `Datalogger` | Voltage logger on GPIO35 |

Every application branch builds an app binary that runs in either launcher slot. `components/cyd_display` (display, 5x7 font, patterns and touch) is copied on each branch.

## How it boots

On power-on, including the EN button, the custom bootloader (`bootloader_components/main`) always starts the launcher. Any other reset, such as the launcher's own restart or an image crashing, goes straight back to the chosen image.

1. The menu immediately lists the stored images, marking the default with `*`, and counts down from 20 seconds.
2. When the countdown ends, the last-used image boots. The very first time, it is the first stored image.
3. Tapping anywhere except an image adds another 20 seconds. Tap an image to boot it right away.
   - The ▼ and ▲ buttons at the bottom right dim or brighten the backlight in 20% steps, between 20% and 100%. The new level shows briefly on the status row and is saved in NVS (`cyd_config/brightness`). Every image starts at that level, because the shared display driver reads it.
4. Meanwhile the launcher joins Wi-Fi and lists the branches of `rftek/cheap-yellow-board`, skipping `bootloader`, `main` and the `How_to_Build` documentation branch. For each remaining branch it looks for `firmware.bin` on the release tagged with the branch name. This usually takes about 10 seconds. Each branch whose current build isn't stored appears in cyan as `+ branch`: either a new application or a newer build of a stored one. Tapping one downloads it into a slot, with progress, and boots it. A branch with no published build yet is skipped and logged. If Wi-Fi or GitHub doesn't answer, the status row says `offline` (or `GitHub error`) and nothing else changes.

A download replaces the slot holding the same branch if there is one. Otherwise it fills an empty slot, or overwrites the slot that does not boot by default. Each slot remembers its branch and the asset's `updated_at` time in NVS, so a rebuilt branch shows up again as an update.

### Settings for boot images

Boot images are published publicly, so they carry no credentials. They read their settings from NVS namespace `cyd_config`. The launcher seeds those settings from its private header, but only on the first start after it's flashed with different credentials; it remembers a hash of them as `seed`. Settings changed on the device, such as with the `wifi_checker` CONNECT screen, therefore survive restarts. The launcher also joins Wi-Fi with the settings in NVS, so an edited network applies to it too.

| Key | From `wifi_credentials.h` |
|---|---|
| `wifi_ssid`, `wifi_pass` | `WIFI_SSID`, `WIFI_PASSWORD` |
| `birdnet_url` | `BIRDNET_BASE_URL`, if defined |

Flash layout (4 MB, `partitions.csv`):

| Partition | Offset | Size | Contents |
|---|---|---|---|
| `nvs` | 0x9000 | 16 KB | Slot branches and versions; settings for boot images; Wi-Fi data |
| `otadata` | 0xd000 | 8 KB | Last-chosen image |
| `factory` | 0x10000 | 1.25 MB | Launcher (about 1.0 MB) |
| `ota_0` | 0x150000 | 1.31 MB | Boot image slot A |
| `ota_1` | 0x2A0000 | 1.31 MB | Boot image slot B |

## Publishing a boot image

Each application branch has `.github/workflows/build.yml`. On every push, GitHub Actions builds the branch and uploads its app binary as `firmware.bin` to the release tagged with the branch name, creating the release the first time. To add an application, create a branch with its own PlatformIO project at the root, copy that workflow, and push. It must stay under 1.31 MB and contain no credentials.

This branch has no workflow, on purpose: the launcher binary contains your Wi-Fi credentials and must never be published.

Because the release tags share the branch names, plain names like `wifi_checker` are ambiguous in some Git commands, where the tag wins. Use `refs/heads/wifi_checker` in scripts, or set `git config remote.origin.tagOpt --no-tags` in your clone.

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core. The first build installs the ESP32 platform and ESP-IDF toolchain. The project path must be short: builds fail on Windows when source paths pass 260 characters.

The launcher needs Wi-Fi credentials, plus the BirdNET server for `birdnet_viewer`. Create `%USERPROFILE%\.cyd_private\wifi_credentials.h` from `main\wifi_credentials.example.h`. Never commit that file.

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

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21, PWM-dimmed to the brightness saved by the launcher. The panel fills a drawing window column by column, so pixel data is sent column-major.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36. Touch coordinates use the CYD documentation's XPT2046 calibration range (`TOUCH_RAW_*` in `components/cyd_display/cyd_display.c`) mapped to the 320x240 landscape orientation.
