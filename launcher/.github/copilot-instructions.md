# ESP32 CYD Bootloader and Launcher

- PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display. Each application has its own branch: `bootloader` (this), `wifi_checker`, `birdnet_viewer`, `screen_check`.
- The factory app is a 20-second boot menu (each tap off an entry adds 20 s) of the images in `ota_0`/`ota_1`, plus every repo branch (except `bootloader`, `main`, `How_to_Build`) whose release tagged with the branch name has `firmware.bin`, published by that branch's GitHub Actions workflow.
- It seeds Wi-Fi and BirdNET settings into NVS namespace `cyd_config` for the images (which must never contain credentials) only when its compiled-in credentials change (hash in `seed`), so on-device edits survive; it joins Wi-Fi with the NVS values. This branch has no publishing workflow because the launcher binary contains credentials.
- `bootloader_components/main` overrides the second-stage bootloader: power-on reset boots factory, any other reset boots the otadata choice.
- `components/cyd_display` (LCD, 5x7 font at 3x, patterns, touch) is copied on every application branch; keep the copies in sync. The panel fills a window column-major; send pixel data in that order.
- Wi-Fi credentials come from `%USERPROFILE%\.cyd_private\wifi_credentials.h`; never commit secrets.
- Build with `pio run`; flash with esptool at the offsets in README.md, never `pio run --target upload`.
- Keep the launcher under 1.25 MB and boot images under 1.31 MB.
- Verify firmware changes with a PlatformIO build before flashing.
