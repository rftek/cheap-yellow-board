# ESP32 CYD Wi-Fi and Bluetooth Scanner

- PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display. Each application has its own branch: `bootloader`, `wifi_checker` (this), `birdnet_viewer`, `screen_check`.
- On boot it scans Wi-Fi networks and BLE devices (NimBLE, observer role only) and lists them on screen. It never connects to a network and needs no credentials.
- Touch UI: entry rows open detail pages, header arrows page each list, header titles rescan.
- `components/cyd_display` (LCD, 5x7 font at 3x, patterns, touch) is copied on every application branch; keep the copies in sync. The panel fills a window column-major; send pixel data in that order.
- Keep RAM and flash use low: no HTTP/TLS, fixed-size result buffers. The binary also runs as a `bootloader` launcher image; keep it under 1.31 MB.
- Build with `pio run`; verify firmware changes with a PlatformIO build before flashing.
