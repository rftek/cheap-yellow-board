# ESP32 CYD Gear Roll

- PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display. Each application has its own branch: `bootloader`, `wifi_checker`, `birdnet_viewer`, `screen_check`, `touchscreen_cal`, `stopwatch`, `gear_roll` (this).
- LVGL 9 demo: two random spur gears that mesh and roll; tapping a gear re-cuts it with a new random tooth count.
- Meshing rules: one shared module; centers one pitch-radius sum apart; gear 2 angle = pi + pi/Z2 - (Z1/Z2) * gear 1 angle. Advance gear 1 by elapsed esp_timer time, not by frame count.
- Gears are drawn in an LV_EVENT_DRAW_MAIN handler with lv_draw_rect (circles) and lv_draw_triangle (two per tooth).
- LVGL comes from the ESP-IDF component manager (`main/idf_component.yml`); configure with CONFIG_LV_* in `sdkconfig.defaults`. Display flush goes through `cyd_draw_pattern`; touch polls `cyd_touch_read`. Do not also call `cyd_touch_start`.
- `components/cyd_display` is copied on every application branch; keep the copies in sync.
- No Wi-Fi or credentials. The binary also runs as a `bootloader` launcher image; keep it under 1.31 MB.
- Build with `pio run`; verify firmware changes with a PlatformIO build before flashing.
