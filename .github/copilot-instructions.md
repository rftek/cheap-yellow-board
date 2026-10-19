# ESP32 CYD Wi-Fi and Bluetooth Scanner

- PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display. Each application has its own branch: `bootloader`, `wifi_checker` (this), `birdnet_viewer`, `screen_check`.
- On boot it scans Wi-Fi networks and BLE devices (NimBLE, observer role only), then joins your network with `wifi_ssid`/`wifi_pass` from NVS namespace `cyd_config` (written by the launcher) and ARP-sweeps the /24 in two passes. Three lists: WI-FI, BLE, LAN. Never compile credentials in: the binary is published by GitHub Actions.
- Touch UI: entry rows open detail pages (the joined AP shows board IP, gateway, DNS), header arrows page each list, header titles rescan.
- lwIP calls (etharp_*) must run in the TCP/IP thread: use `esp_netif_tcpip_exec`.
- `components/cyd_display` (LCD, 5x7 font at 3x, patterns, touch) is copied on every application branch; keep the copies in sync. The panel fills a window column-major; send pixel data in that order.
- Keep RAM and flash use low: no HTTP/TLS, fixed-size result buffers. The binary (about 954 KB) also runs as a `bootloader` launcher image; keep it under 1.31 MB, and under 1 MB if standalone builds should keep fitting the default partition.
- Build with `pio run`; verify firmware changes with a PlatformIO build before flashing.
