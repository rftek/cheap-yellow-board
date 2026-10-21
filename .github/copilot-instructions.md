# ESP32 CYD Cat & Mouse

- PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display. Each application has its own branch: `bootloader`, `wifi_checker`, `birdnet_viewer`, `screen_check`, `touchscreen_cal`, `stopwatch`, `gear_roll`, `Datalogger`, `CatMouse` (this); docs on `How_to_Build`.
- A PAC-MAN-style maze game: the mouse eats cheese, three cats chase it, big cheese scares them. Maze 20x14 tiles of 16 px under a 16 px score bar, 25 fps, no frame buffer: each frame redraws only the boxes around the actors, composed in a small buffer and sent with `cyd_draw_bitmap`.
- Steering polls `cyd_touch_read` each frame: a WASD pad (W above A S D, keys 32x24 px, pad 96x48) in the bottom-right corner, kept out of the maze (wall tiles under it; drawing leaves its pixels to the pad).
- The CMake project name must stay `CatMouse`, the branch name: the launcher matches a slot's project name against the branch it downloaded.
- The high score lives in NVS namespace `catmouse`. Never erase NVS: it also holds the launcher's settings and the touch calibration.
- `components/cyd_display` is copied on every application branch; keep the copies in sync.
- No Wi-Fi or credentials. The binary also runs as a `bootloader` launcher image; keep it under 1.31 MB.
- Build with `pio run`; verify firmware changes with a PlatformIO build before flashing.
