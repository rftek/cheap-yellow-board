# ESP32 CYD Bootloader and Launcher

PlatformIO/ESP-IDF firmware for the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display). It turns the board into a two-slot boot menu.

This folder, `launcher/`, holds the launcher, its custom second-stage bootloader (`bootloader_components`) and the partition table. The applications are the folders under `apps/`; the [root README](../README.md) lists them. Each one builds an app binary that runs in either launcher slot. The launcher and the apps share the one copy of `components/cyd_display` (display, 5x7 font, patterns and touch) at the repository root.

## How it boots

On power-on, including the EN button, the custom bootloader (`bootloader_components/main`) always starts the launcher. Any other reset, such as the launcher's own restart or an image crashing, goes straight back to the chosen image.

1. The menu immediately lists the stored images, marking the default with `*`, and counts down from 20 seconds. It shows seven entries at a time: the row below them says which (such as `1-7/9`), and its ▲ and ▼ buttons page through the rest.
2. When the countdown ends, the last-used image boots. The very first time, it is the first stored image.
3. Tapping anywhere except an image or the network activity, the scroll buttons, brightness slider and the bottom row's other pages included, adds another 20 seconds. Tap an image to boot it right away.
   - The brightness slider runs down the right edge: `+` at the top, a bar with one segment per level, `-` below it, then the level in percent. `+` and `-` step the backlight by 20%, between 20% and 100%, and tapping a bar segment jumps to its level. The level is saved in NVS (`cyd_config/brightness`). Every image starts at that level, because the shared display driver reads it.
   - The bottom row shows what the launcher is doing, taking turns every 2.5 seconds (tap it to skip ahead): the network activity (`join <network>`, why a join failed such as `no AP found` or `bad password?`, the GitHub requests, `7 on GitHub`, or a GitHub error such as `GitHub err 403`), then, once joined, the IP address and the signal strength with the network name, then the free RAM. New network activity shows at once.
   - Tapping the bottom row while it shows the network activity opens the Wi-Fi network chooser, and the countdown waits until it closes, then starts over from 20 seconds. The chooser scans once any join in progress has finished, then lists the networks in range, strongest first. Each name appears once, with its strongest access point, and the joined network is in yellow. It pages seven at a time like the menu, `SCAN` scans again, and `BACK` returns to the menu. Tapping a network shows its channel, signal, security and address; `CONNECT` opens an on-screen keyboard with the name and password (filled in if it is the saved network; type the name of a hidden network). `SAVE` stores them in `cyd_config` for the launcher and every image. The launcher then rejoins, with progress on the bottom row, and lists the apps again.
4. Meanwhile the launcher joins Wi-Fi and reads the app catalog of `rftek/cheap-yellow-board`: `manifest.json` on the release tagged `catalog` (<https://github.com/rftek/cheap-yellow-board/releases/download/catalog/manifest.json>). It lists every app in it, up to 16, with the image for its chip (`firmware.bin` on the ESP32). This usually takes about 10 seconds. Each app whose current build isn't stored appears in cyan as `+ name`: either a new application or a newer build of a stored one. Tapping one downloads it into a slot, with progress, and boots it. If the saved network doesn't join, the launcher tries the network compiled into it (`WIFI_SSID`), so a mistyped password or a network that's gone can't cut it off. When that one joins, the launcher saves it in place of the saved network and logs it, because the boot images join only the saved network and have no fallback of their own. If Wi-Fi or GitHub still doesn't answer, the bottom row says why and nothing else changes.

**Pending:** the launcher code doesn't read the catalog yet. Today it still lists the repository's branches (skipping `bootloader`, `main` and `How_to_Build`), looks for `firmware.bin` on the release tagged with each branch's name, and shows `GET branches` and `GET releases` on the bottom row. Switching it to the catalog is a follow-up change.

A download replaces the slot holding the same app if there is one. Otherwise it fills an empty slot, or overwrites the slot that does not boot by default. Each slot remembers its app name and the asset's `updated_at` time in NVS, so a rebuilt app shows up again as an update.

### Settings for boot images

Boot images are published publicly, so they carry no credentials. They read their settings from NVS namespace `cyd_config`. The launcher seeds those settings from its private header, but only on the first start after it's flashed with different credentials; it remembers a hash of them as `seed`. Settings changed on the device, with the launcher's network chooser or the `wifi_checker` CONNECT screen, therefore survive restarts. The launcher also joins Wi-Fi with the settings in NVS, so an edited network applies to it too. If that network doesn't join but `WIFI_SSID` does, the launcher overwrites `wifi_ssid` and `wifi_pass` with `WIFI_SSID` and `WIFI_PASSWORD`, so the images it boots can join as well. A network chosen on the device is therefore kept only while it joins when the launcher starts; to go back to it, choose it again.

| Key | From `wifi_credentials.h` |
|---|---|
| `wifi_ssid`, `wifi_pass` | `WIFI_SSID`, `WIFI_PASSWORD` |
| `birdnet_url` | `BIRDNET_BASE_URL`, if defined |

Flash layout (4 MB, `partitions.csv`):

| Partition | Offset | Size | Contents |
|---|---|---|---|
| `nvs` | 0x9000 | 16 KB | Slot app names and versions; settings for boot images; Wi-Fi data |
| `otadata` | 0xd000 | 8 KB | Last-chosen image |
| `factory` | 0x10000 | 1.25 MB | Launcher (about 1.0 MB) |
| `ota_0` | 0x150000 | 1.31 MB | Boot image slot A |
| `ota_1` | 0x2A0000 | 1.31 MB | Boot image slot B |

## Publishing a boot image

The repository's one workflow, `.github/workflows/build.yml`, builds the apps. When a change to `apps/<name>` is merged into `main`, GitHub Actions builds that app and uploads its app binary to the release tagged `<name>`, creating the release the first time: `firmware.bin` for the ESP32, `firmware-<target>.bin` for other targets. It then rewrites the catalog, `manifest.json` on the release tagged `catalog`. To add an application, add a folder under `apps/` in a pull request; [docs/INSTALL.md](../docs/INSTALL.md) has the steps. It must stay under 1.31 MB and contain no credentials.

The launcher is never built by CI, on purpose: its binary contains your Wi-Fi credentials and must never be published. The workflow ignores changes to `launcher/`.

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core. The first build installs the ESP32 platform and ESP-IDF toolchain. The project path must be short: builds fail on Windows when source paths pass 260 characters.

The launcher needs Wi-Fi credentials, plus the BirdNET server for `birdnet_viewer`. Create `%USERPROFILE%\.cyd_private\wifi_credentials.h` from `launcher\main\wifi_credentials.example.h`. Never commit that file.

```powershell
cd launcher
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e esp32
```

**Do not use `pio run --target upload`.** It would flash a stock bootloader. Flash with esptool instead, and only the launcher's app partition, `factory` at 0x10000. **Never run `erase_flash`**: it wipes both image slots and the settings in NVS.

```powershell
$py = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$esptool = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
$L = "$env:USERPROFILE\.platformio\cyd-launcher-build\esp32"
& $py $esptool --chip esp32 -p COM11 -b 230400 write_flash 0x10000 "$L\firmware.bin"
& $pio device monitor --port COM11 --baud 115200
```

A new board needs the custom bootloader, the partition table and a blank `otadata` written once, together with the launcher; [docs/INSTALL.md](../docs/INSTALL.md#7-flash-a-new-board-once) shows that one-time write. After that, update only the app partition as above.

To put an application into a slot over USB, build it in its folder under `apps/` and write its `firmware.bin` to `0x150000` (slot A) or `0x2A0000` (slot B):

```powershell
& $py $esptool --chip esp32 -p COM11 -b 230400 write_flash 0x150000 path\to\firmware.bin
```

Change `COM11` to the board's serial port. The monitor exits with Ctrl+]. If you change `sdkconfig.defaults`, delete the generated `sdkconfig.esp32` in `launcher` so the new defaults apply.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21, PWM-dimmed to the brightness saved by the launcher. The panel fills a drawing window column by column, so pixel data is sent column-major.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36. Touch coordinates use the CYD documentation's XPT2046 calibration range (`TOUCH_RAW_*` in `components/cyd_display/cyd_display_esp32_cyd.c`) mapped to the 320x240 landscape orientation.
