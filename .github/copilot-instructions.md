# ESP32 CYD Touchscreen Calibration

- PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display. Each application has its own branch: `bootloader`, `wifi_checker`, `birdnet_viewer`, `screen_check`, `touchscreen_cal` (this).
- Taps four inset corner targets, fits an affine calibration (least squares over raw XPT2046 readings), checks it on a center tap, and saves it to NVS `cyd_config/touch_cal` with `cyd_touch_save_calibration`.
- `components/cyd_display` (LCD, 5x7 font at 3x, patterns, touch with trimmed-mean sampling and calibration) is copied on every application branch; keep the copies in sync. The panel fills a window column-major; send pixel data in that order.
- No Wi-Fi or credentials. The binary also runs as a `bootloader` launcher image; keep it under 1.31 MB.
- Build with `pio run`; verify firmware changes with a PlatformIO build before flashing.
