# ESP32 CYD BirdNET Viewer

- PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display. Each application has its own branch: `bootloader`, `wifi_checker`, `birdnet_viewer` (this), `screen_check`.
- Shows the latest BirdNET visit of the day (confidence of at least 80%) as a full-screen Wikimedia photo; refreshes every 2 minutes or on tap. Keep the image-only screen and bounded HTTP/JPEG streaming.
- Wi-Fi and the BirdNET URL are read at runtime from NVS namespace `cyd_config` (`wifi_ssid`, `wifi_pass`, `birdnet_url`), written by the launcher. Never compile credentials in: GitHub Actions publishes this binary publicly.
- `components/cyd_display` (LCD, 5x7 font at 3x, patterns, touch) is copied on every application branch; keep the copies in sync. The panel fills a window column-major; draw images through `cyd_draw_pattern`.
- Build with `pio run`. The binary also runs as a `bootloader` launcher image; keep it under 1.31 MB.
- Verify firmware changes with a PlatformIO build before flashing.
