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
| [apps/f1_race](apps/f1_race) | F1 racing game | esp32, esp32s3 |
| [apps/gear_roll](apps/gear_roll) | LVGL gear demo | esp32, esp32s3 |
| [apps/screen_check](apps/screen_check) | Screen color test | esp32, esp32s3 |
| [apps/stopwatch](apps/stopwatch) | LVGL stopwatch | esp32, esp32s3 |
| [apps/touchscreen_cal](apps/touchscreen_cal) | Touchscreen calibration | esp32, esp32s3 |
| [apps/wifi_checker](apps/wifi_checker) | Wi-Fi, Bluetooth LE and LAN scanner | esp32, esp32s3 |
| [apps/xbox_controller](apps/xbox_controller) | Xbox controller test over Bluetooth | esp32, esp32s3 |

Each app folder has its own README describing what the app does and how it is put together. `esp32` is the CYD; `esp32s3` is the Waveshare ESP32-S3-Touch-LCD-7. An app lists the targets it supports in its `supported-targets.txt`, and CI builds only those.

## Versions

Every app and the launcher has a version of the form **MAJOR.MINOR**, such as `1.0`, `1.1` or `2.0`. Each one starts at `1.0`.

- **Where it lives:** one line in `version.txt` in the app's folder (`apps/<name>/version.txt`, `launcher/version.txt`). ESP-IDF reads that file into `PROJECT_VER`, which becomes `esp_app_desc_t.version` in the image, so the firmware reports it: it is in the boot log (`App version: 1.2`), `esp_app_get_description()->version` returns it, and the launcher reads it from each slot's app header. `tools/pio_version.py`, which every `platformio.ini` runs, makes an incremental build pick up a changed `version.txt`.
- **Bump MINOR** (`1.2` to `1.3`) for new features, fixes and other changes that keep the app working the way it did.
- **Bump MAJOR** (`1.3` to `2.0`) for breaking or big changes: a redesign, settings or saved data that no longer carry over, a changed NVS layout or partition needs, or anything that makes an older launcher or app stop working with it. MINOR goes back to `0`.
- **Every change to an app bumps its version once per pull request**, and adds an entry at the top of the `## Changelog` section of the app's README, newest first: a `### MAJOR.MINOR (YYYY-MM-DD)` heading and a line or two on what changed. The launcher works the same way, with its changelog in [launcher/README.md](launcher/README.md#changelog). Changes to Markdown alone need no bump. A change to shared code (`components/`) bumps the apps whose behavior it changes on purpose.
- **CI checks it:** the `versions` job (`tools/check_versions.py`) fails a pull request when a `version.txt` is missing or not MAJOR.MINOR, when a README's newest changelog entry doesn't match it, or when an app or the launcher changed (anything but Markdown) without a higher version than at the base. Each build also checks that the image carries the version in `version.txt`.
- **The catalog and the launcher show it:** a published image's release asset is labeled with its version (`firmware.bin v1.2`), the catalog lists it as `app_version`, and the launcher shows it beside each app, plus its own version and each slot's on its bottom row.

## The launcher

[launcher](launcher) is the boot menu: a factory app with two image slots, a custom second-stage bootloader (`launcher/bootloader_components`) and the partition table. It finds the apps through the catalog described below, downloads the one you tap into a slot, and boots it. Screen brightness, the Wi-Fi network and the touch calibration are set on the board and shared with every app through NVS. Power the board off and on, or press EN, to get back to the launcher.

The launcher embeds your Wi-Fi credentials, so CI never builds or publishes it. You build it yourself and flash it once; see [docs/INSTALL.md](docs/INSTALL.md).

## Layout

```
apps/<name>/                 one app: platformio.ini ([env:esp32], [env:esp32s3]),
                             CMakeLists.txt, supported-targets.txt, version.txt,
                             sdkconfig.defaults*, main/, README.md (with changelog)
launcher/                    the launcher, its bootloader (bootloader_components/)
                             and partition tables
components/cyd_display/      display, fonts, patterns and touch, shared by every app
components/cyd_gamepad/      Xbox / Bluetooth gamepad input on Bluepad32
components/airplay_sender/   AirPlay 1 (RAOP) audio output to network speakers
external/bluepad32/          Bluepad32 git submodule, with BTstack nested in it
docs/                        build environment and installation guide
tools/ci_plan.py             picks the apps a change needs built
tools/make_catalog.py        writes manifest.json, the app catalog
tools/check_versions.py      checks the versions and changelogs
tools/image_version.py       reads the version out of a built image
tools/pio_version.py         PlatformIO pre-script: rebuilds when version.txt changes
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
- **Every run** also runs the `versions` job, which checks the [versions](#versions) and changelogs, and on pull requests that every changed app was bumped. Each build checks that its image carries the version in the app's `version.txt`.
- **Pushes to `main`** build the same way, then upload each image to a release tagged with the app's folder name: `firmware.bin` for esp32, `firmware-<target>.bin` for other targets (such as `firmware-esp32s3.bin`), labeled with the app's version (`firmware.bin v1.2`). The release is created the first time, and its tag moves to the commit that was built.
- **The catalog:** after those builds, `tools/make_catalog.py` writes `manifest.json`, which lists every app's images with their download URL, size, `version` (the upload time, which tells the launcher a build is new) and `app_version` (the app's MAJOR.MINOR version, from the asset's label), and publishes it on the release tagged `catalog`: <https://github.com/rftek/cheap-yellow-board/releases/download/catalog/manifest.json>. The launcher reads it to find the apps.
- Changes that touch only Markdown, `docs/` or `launcher/` build nothing. The workflow still runs, so its `ci-result` check, which `main` requires, always reports; it passes when the `versions` job and every build passed.

Published images are public, so they must contain no credentials, and they must fit a launcher slot (1.31 MB on the CYD).

## Contributing

`main` is protected: every change goes through a pull request, and its CI must pass before it merges. Every pull request that changes an app or the launcher bumps its [version](#versions) and adds a changelog entry to its README. Branches are only for pull requests. Name them:

| Branch | For |
|---|---|
| `app/<name>-<change>` | A change to one app, such as `app/stopwatch-lap-times` |
| `launcher/<change>` | The launcher or its bootloader |
| `ci/<change>` | The workflow or `tools/` |
| `docs/<change>` | Documentation |

A new app is a new folder under `apps/` in a pull request, with `version.txt` at `1.0` and a changelog entry for it; [docs/INSTALL.md](docs/INSTALL.md) describes the steps. Never commit `wifi_credentials.h`: it lives outside the repository, in `%USERPROFILE%\.cyd_private`.

The `bruce` branch is a separate import of the [Bruce](https://github.com/BruceDevices/firmware) firmware with its full history (AGPL-3.0). It builds with its own instructions and is not an app here.

## Hardware

Display and touch pins are listed in the [launcher README](launcher/README.md#hardware), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display).
