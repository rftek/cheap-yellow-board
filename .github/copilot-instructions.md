# ESP32 CYD Stopwatch

- PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display. Each application has its own branch: `bootloader`, `wifi_checker`, `birdnet_viewer`, `screen_check`, `touchscreen_cal`, `stopwatch` (this).
- LVGL 9 UI: a round dial (lv_arc for seconds, lv_scale ticks), MM:SS.cc label, START/STOP/CLEAR buttons. Timing from esp_timer, never from LVGL ticks.
- LVGL comes from the ESP-IDF component manager (`main/idf_component.yml`); configure it with CONFIG_LV_* in `sdkconfig.defaults`.
- Display flush goes through `cyd_draw_pattern` (the panel fills windows column-major); touch input polls `cyd_touch_read` (calibrated). Do not also call `cyd_touch_start`: two tasks on the touch SPI device would collide.
- `components/cyd_display` is copied on every application branch; keep the copies in sync.
- No Wi-Fi or credentials. The binary also runs as a `bootloader` launcher image; keep it under 1.31 MB.
- Build with `pio run`; verify firmware changes with a PlatformIO build before flashing.
