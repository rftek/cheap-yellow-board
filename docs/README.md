# ESP32 CYD: How to Build

Documentation for building, flashing and publishing the applications in this repository for the ESP32 Cheap Yellow Display (CYD). Every application is a folder under `apps/`; the launcher is in `launcher/`.

**Start here: [INSTALL.md](INSTALL.md).** It covers what to install (Git and PlatformIO are required; VS Code is optional), the private Wi-Fi file, building each app, flashing a new board once, publishing through GitHub Actions, troubleshooting, and the exact tool versions this project was built with.

## What to expect

- After flashing, the board starts the launcher at once. It shows the images already in its two slots and counts down; when the countdown ends, it starts the application that ran last.
- The launcher joins Wi-Fi and lists the application binaries in this repository's app catalog (`manifest.json` on the `catalog` release). Tap one to download it into a slot; it starts when the download completes.
- Screen brightness and the Wi-Fi network are set in the launcher, and the applications use them.
- Power the board off and on, or press EN, to get back to the launcher.

## Where things are

| Folder | Contents |
|---|---|
| `apps/<name>` | One application, a complete PlatformIO project. The [root README](../README.md#applications) lists them. |
| `launcher` | Launcher: a boot menu with two image slots, a custom bootloader and the partition table |
| `components` | `cyd_display` and `cyd_gamepad`, shared by the apps |
| `external/bluepad32` | Bluepad32 submodule, for `xbox_controller` and `catmouse_pad` |
| `docs` | This documentation |
| `tools`, `.github/workflows` | CI and the app catalog |

The `bruce` branch is a separate import of the [Bruce](https://github.com/BruceDevices/firmware) firmware with its full history (AGPL-3.0). It is not a launcher app.

Each app folder has its own README describing what the app does and how it is put together.
