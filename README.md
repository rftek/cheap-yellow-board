# ESP32 Cheap Yellow Display applications

Applications for the ESP32 Cheap Yellow Display (CYD, ESP32-2432S028R), written in ESP-IDF C with PlatformIO. **Every application is a folder under `apps/`**, a complete PlatformIO project of its own. The launcher in `launcher/` is a boot menu with two image slots: it lists the applications published on this repository's releases and downloads the one you tap. GitHub Actions builds the apps and publishes each one's `firmware.bin` on a release named after its folder.

**To build or install anything, start with [docs/INSTALL.md](docs/INSTALL.md).**

## Applications

| Folder | Application | Targets |
|---|---|---|
| [apps/airplay_test](apps/airplay_test) | AirPlay speaker search and test tone (airplay_sender demo) | esp32s3 |
| [apps/birdnet_viewer](apps/birdnet_viewer) | BirdNET bird photo viewer | esp32, esp32s3 |
| [apps/bluetooth_diag](apps/bluetooth_diag) | Bluetooth Classic and LE scanner and connection tester | esp32 |
| [apps/catmouse](apps/catmouse) | PAC-MAN-style maze game | esp32, esp32s3 |
| [apps/catmouse_pad](apps/catmouse_pad) | The same maze game, also steered with an Xbox controller | esp32, esp32s3 |
| [apps/datalogger](apps/datalogger) | Voltage logger on GPIO35 | esp32 |
| [apps/drumloop](apps/drumloop) | Live 808 drum pads and a 1-16 bar step sequencer, played on an AirPlay speaker | esp32s3 |
| [apps/f1_race](apps/f1_race) | F1 racing game | esp32, esp32s3 |
| [apps/gear_roll](apps/gear_roll) | LVGL gear demo | esp32, esp32s3 |
| [apps/screen_check](apps/screen_check) | Screen color test | esp32, esp32s3 |
| [apps/stopwatch](apps/stopwatch) | LVGL stopwatch | esp32, esp32s3 |
| [apps/touchscreen_cal](apps/touchscreen_cal) | Touchscreen calibration | esp32, esp32s3 |
| [apps/wifi_checker](apps/wifi_checker) | Wi-Fi, Bluetooth LE and LAN scanner | esp32, esp32s3 |
| [apps/xbox_controller](apps/xbox_controller) | Xbox controller test over Bluetooth | esp32, esp32s3 |

Each app folder has its own README describing what the app does and how it is put together. `esp32` is the CYD; `esp32s3` is the Waveshare ESP32-S3-Touch-LCD-7. An app lists the targets it supports in its `supported-targets.txt`, and CI builds only those.

## The launcher

[launcher](launcher) is the boot menu: a factory app with two image slots, a custom second-stage bootloader (`launcher/bootloader_components`) and the partition table. It finds the apps through the catalog described below, downloads the one you tap into a slot, and boots it. Screen brightness, the Wi-Fi network and the touch calibration are set on the board and shared with every app through NVS. Power the board off and on, or press EN, to get back to the launcher.

The launcher embeds your Wi-Fi credentials, so CI never builds or publishes it. You build it yourself and flash it once; see [docs/INSTALL.md](docs/INSTALL.md).

## Layout

```
apps/<name>/                 one app: platformio.ini ([env:esp32], [env:esp32s3]),
                             CMakeLists.txt, supported-targets.txt,
                             sdkconfig.defaults*, main/, README.md
launcher/                    the launcher, its bootloader (bootloader_components/)
                             and partition tables
components/cyd_display/      display, fonts, patterns and touch, shared by every app
components/cyd_gamepad/      Xbox / Bluetooth gamepad input on Bluepad32
components/airplay_sender/   AirPlay 1 (RAOP) audio output to network speakers
external/bluepad32/          Bluepad32 git submodule, with BTstack nested in it
docs/                        build environment and installation guide
tools/ci_plan.py             picks the apps a change needs built
tools/make_catalog.py        writes manifest.json, the app catalog
.github/workflows/build.yml  the one CI workflow
```

There is one copy of each shared component. Apps reference it from their `CMakeLists.txt` with `EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/cyd_display"` (the launcher with `../components/...`). `xbox_controller` and `catmouse_pad` also use `cyd_gamepad` and the Bluepad32 submodule.

## Building an app

Clone once, then build from inside the app's folder:

```powershell
git clone https://github.com/rftek/cheap-yellow-board.git
cd cheap-yellow-board\apps\screen_check
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e esp32
```

`xbox_controller` and `catmouse_pad` also need the Bluepad32 submodule and two environment variables; their READMEs and [docs/INSTALL.md](docs/INSTALL.md) show how.

## CI, releases and the catalog

`.github/workflows/build.yml` is the only workflow.

- **Pull requests** build the apps the change touches, once for each target in their `supported-targets.txt`. A change to `components/`, `external/`, `tools/` or the workflow builds every app. Nothing is published.
- **Pushes to `main`** build the same way, then upload each image to a release tagged with the app's folder name: `firmware.bin` for esp32, `firmware-<target>.bin` for other targets (such as `firmware-esp32s3.bin`). The release is created the first time, and its tag moves to the commit that was built.
- **The catalog:** after those builds, `tools/make_catalog.py` writes `manifest.json`, which lists every app's images with their download URL, size and version, and publishes it on the release tagged `catalog`: <https://github.com/rftek/cheap-yellow-board/releases/download/catalog/manifest.json>. The launcher reads it to find the apps.
- Changes that touch only Markdown, `docs/` or `launcher/` build nothing. The workflow still runs, so its `ci-result` check, which `main` requires, always reports.

Published images are public, so they must contain no credentials, and they must fit a launcher slot (1.31 MB on the CYD).

## Contributing

`main` is protected: every change goes through a pull request, and its CI must pass before it merges. Branches are only for pull requests. Name them:

| Branch | For |
|---|---|
| `app/<name>-<change>` | A change to one app, such as `app/stopwatch-lap-times` |
| `launcher/<change>` | The launcher or its bootloader |
| `ci/<change>` | The workflow or `tools/` |
| `docs/<change>` | Documentation |

A new app is a new folder under `apps/` in a pull request; [docs/INSTALL.md](docs/INSTALL.md) describes the steps. Never commit `wifi_credentials.h`: it lives outside the repository, in `%USERPROFILE%\.cyd_private`.

The `bruce` branch is a separate import of the [Bruce](https://github.com/BruceDevices/firmware) firmware with its full history (AGPL-3.0). It builds with its own instructions and is not an app here.

## Hardware

Display and touch pins are listed in the [launcher README](launcher/README.md#hardware), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display).
