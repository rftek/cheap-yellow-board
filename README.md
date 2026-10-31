# ESP32 CYD: How to Build

Documentation for building, flashing and publishing the applications in this repository for the ESP32 Cheap Yellow Display (CYD). This branch holds only documentation; the code lives on the application branches.

**Start here: [INSTALL.md](INSTALL.md).** It covers what to install (Git and PlatformIO are required; VS Code is optional), the private Wi-Fi file, building each branch, flashing a new board once, publishing through GitHub Actions, troubleshooting, and the exact tool versions this project was built with.

## What to expect

- After flashing, the board starts the launcher at once. It shows the images already in its two slots and counts down; when the countdown ends, it starts the application that ran last.
- The launcher joins Wi-Fi and lists the application binaries published on this repository's GitHub releases. Tap one to download it into a slot; it starts when the download completes.
- Screen brightness and the Wi-Fi network are set in the launcher, and the applications use them.
- Power the board off and on, or press EN, to get back to the launcher.

## Branches

| Branch | Application |
|---|---|
| `bootloader` | Launcher: a boot menu with two image slots, a custom bootloader and the partition table |
| `wifi_checker` | Wi-Fi, Bluetooth LE and LAN scanner |
| `birdnet_viewer` | BirdNET bird photo viewer |
| `screen_check` | Screen color test |
| `touchscreen_cal` | Touchscreen calibration |
| `stopwatch` | LVGL stopwatch |
| `gear_roll` | LVGL gear demo |
| `Datalogger` | Voltage logger on GPIO35 |
| `CatMouse` | Maze game: a mouse, cheese and three cats |
| `bluetooth_diag` | Bluetooth Classic and LE scanner and connection tester |
| `How_to_Build` | This documentation |
| `main` | The original BirdNET viewer, superseded by `birdnet_viewer` |

Each application branch has its own README describing what the app does and how it is put together.
