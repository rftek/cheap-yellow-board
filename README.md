# ESP32 CYD: How to Build

Documentation for building, flashing and publishing the applications in this repository for the ESP32 Cheap Yellow Display (CYD). This branch holds only documentation; the code lives on the application branches.

**Start here: [INSTALL.md](INSTALL.md).** It covers what to install (Git and PlatformIO are required; VS Code is optional), the private Wi-Fi file, building each branch, flashing a new board once, publishing through GitHub Actions, troubleshooting, and the exact tool versions this project was built with.

**WHAT TO EXPECT**
After flashing. A bootloader will launch immediately; show any local files that are ready to launch and start countdown, if countdown ends then launch the last application run. 
Will try to connect to the github repo and pull down list of binaries available. Click one to download and store in a slot, will run when download complete.

screen brightness, wifi network are controlled from bootloader. power off then on reset. 




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
| `How_to_Build` | This documentation |

Each application branch has its own README describing what the app does and how it is put together.
