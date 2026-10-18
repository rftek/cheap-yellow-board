# ESP32 CYD Wi-Fi and Bluetooth Scanner

- This is a PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display.
- On boot it scans Wi-Fi networks and BLE devices (NimBLE, observer role only) and lists them on screen.
- Touch UI: entry rows open detail pages, header arrows page each list, header titles rescan.
- The panel fills a window column-major (top to bottom, then left to right); send pixel data in that order.
- It never connects to a network and needs no credentials.
- Build with `pio run`; flash with `pio run --target upload --upload-port COM11`.
- The 320x240 ILI9341-compatible TFT and XPT2046 touchscreen pin mapping is defined in `main/main.c`.
- Keep RAM and flash use low: no HTTP/TLS, no image decoding, fixed-size result buffers.
- Verify firmware changes with a PlatformIO build before flashing.
