# ESP32 CYD Datalogger

- PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display. Each application has its own branch: `bootloader`, `wifi_checker`, `birdnet_viewer`, `screen_check`, `touchscreen_cal`, `stopwatch`, `gear_roll`, `Datalogger` (this); docs on `How_to_Build`.
- One channel, CH1 = GPIO35 (ADC1 channel 7, P3 connector): adc_oneshot, 12-bit, 12 dB attenuation, 16 readings averaged per sample, line-fitting calibration to mV (falls back to an UNCAL estimate). Tap CH1 to start/stop; one sample every 3 s; CSV lines `t_s,raw,mV` on the serial log.
- GPIO21/22/23 have no ADC on the ESP32; IO21 is the backlight and IO23 the SD card on the CYD. Do not use them for analog inputs.
- `components/cyd_display` is copied on every application branch; keep the copies in sync.
- No Wi-Fi or credentials. The binary also runs as a `bootloader` launcher image; keep it under 1.31 MB.
- Build with `pio run`; verify firmware changes with a PlatformIO build before flashing.
