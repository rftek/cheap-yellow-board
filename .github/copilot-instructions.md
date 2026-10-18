# ESP32 CYD Launcher and Boot Images

- PlatformIO projects using native ESP-IDF C for an ESP32 Cheap Yellow Display.
- `launcher/` is the factory app: a 2-second boot menu of the images in `ota_0`/`ota_1`, plus downloads of `.bin` assets from the latest GitHub release of this repo. Its `bootloader_components/main` overrides the bootloader: power-on reset boots factory, any other reset boots the otadata choice.
- `main/` is the Wi-Fi/BLE scanner boot image. It never connects to a network and needs no credentials.
- `components/cyd_display` is shared by both: LCD, 5x7 font at 3x, touchscreen. The panel fills a window column-major; send pixel data in that order.
- Only the launcher uses Wi-Fi credentials, from `%USERPROFILE%\.cyd_private\wifi_credentials.h`; never commit secrets.
- Build with `pio run` (scanner) and `pio run --project-dir launcher`. Flash with esptool at the offsets in README.md, never `pio run --target upload`.
- Keep boot images under 1.31 MB and the launcher under 1.25 MB.
- Verify firmware changes with a PlatformIO build before flashing.
