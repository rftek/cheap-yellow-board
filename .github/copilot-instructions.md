# ESP32 CYD Bluetooth Diagnostics

- PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display. Each application has its own branch: `bootloader`, `wifi_checker`, `birdnet_viewer`, `screen_check`, `touchscreen_cal`, `stopwatch`, `gear_roll`, `Datalogger`, `bluetooth_diag` (this); docs on `How_to_Build`.
- Dual-mode controller with the Bluedroid host (not NimBLE, which has no Classic support). Scans Classic (inquiry) then LE, then tests connections: Classic by SDP service discovery (paging), LE by a GATT client open, MTU exchange, service search and Device Information reads.
- The tool never pairs: it refuses PIN, SSP and LE security requests.
- Settings are option indexes in `settings[]`, saved as one NVS blob (`bt_diag/settings`). Adding a setting: extend the enum and `setting_info`, then add it to a configuration page.
- `components/cyd_display` is copied on every application branch; keep the copies in sync.
- No Wi-Fi or credentials. The binary also runs as a `bootloader` launcher image; keep it under 1.31 MB (about 850 KB now).
- Build with `pio run`; verify firmware changes with a PlatformIO build before flashing.
