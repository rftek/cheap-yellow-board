# ESP32 CYD BirdNET Viewer

- This is a PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display.
- Keep Wi-Fi credentials in `%USERPROFILE%\.cyd_private\wifi_credentials.h`; never commit secrets.
- Build with `pio run`; flash with `pio run --target upload --upload-port COM11`.
- The 320x240 ILI9341-compatible TFT and XPT2046 touchscreen pin mapping is defined in `main/main.c`.
- Preserve the image-only screen, 2-minute/touch refresh behavior, and bounded HTTP/JPEG streaming.
- Verify firmware changes with a PlatformIO build before flashing.
