# ESP32 CYD: How to Build

- Documentation-only branch for the ESP32 Cheap Yellow Display project. Code lives on the application branches: `bootloader`, `wifi_checker`, `birdnet_viewer`, `screen_check`, `touchscreen_cal`, `stopwatch`, `gear_roll`, `Datalogger`, `CatMouse`, `bluetooth_diag`, `f1_race`. `main` holds the original BirdNET viewer, superseded by `birdnet_viewer`. `bruce` is the upstream Bruce firmware (BruceDevices/firmware, AGPL-3.0) with its full history; it is not a launcher app.
- Every branch's README and copilot-instructions list the same branches; when one is added, update this branch's README and `INSTALL.md` too.
- `INSTALL.md` is the build-environment and installation guide; keep its tested-versions table and branch table in step with the application branches (platform pin in `platformio.ini`, LVGL in `dependencies.lock`, flash offsets in the `bootloader` partition table).
- Only state steps that were verified on real hardware or in CI; mark anything untested as such.
