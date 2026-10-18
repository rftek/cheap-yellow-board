# ESP32 CYD Screen Check

- PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display. Each application has its own branch: `bootloader`, `wifi_checker`, `birdnet_viewer`, `screen_check` (this).
- A full-screen display test stepped by touch (right half: next, left half: previous). Patterns are pixel functions drawn with `cyd_draw_pattern`; no frame buffer.
- `components/cyd_display` (LCD, 5x7 font at 3x, patterns, touch) is copied on every application branch; keep the copies in sync. The panel fills a window column-major; send pixel data in that order.
- No Wi-Fi or credentials. The binary also runs as a `bootloader` launcher image; keep it under 1.31 MB.
- Build with `pio run`; verify firmware changes with a PlatformIO build before flashing.
