# ESP32 CYD F1 Race

- PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display. Each application has its own branch: `bootloader`, `wifi_checker`, `birdnet_viewer`, `screen_check`, `touchscreen_cal`, `stopwatch`, `gear_roll`, `Datalogger`, `CatMouse`, `f1_race` (this); docs on `How_to_Build`.
- An OutRun-style pseudo-3D race: each road row below the horizon (y = 100) is one depth; the road's sideways offset is integrated from per-unit track curvature each frame. Four or five random curves, then the chequered line. Rivals Hamilton (44), Kimi (12) and Verstappen (3); the player starts fourth.
- No frame buffer: the screen is composed in 20-row strips and sent with `cyd_draw_bitmap`. The road and score bar are redrawn every frame, the sky only when it scrolls or a message changes.
- Steering polls `cyd_touch_read` each frame: hold the left or right half of the screen.
- Secret: a press starting in the top-right 40x40 toggles Hamilton's auto-win (default on: Hamilton, Kimi, Verstappen finish 1-2-3). Off, the rivals race at their own pace. Kept in NVS namespace `f1_race`, key `rigged`; a 2x2 dot at (316,1) shows when it is off. Keep it unlabelled.
- The CMake project name must stay `f1_race`, the branch name: the launcher matches a slot's project name against the branch it downloaded.
- Never erase NVS: it also holds the launcher's settings and the touch calibration.
- `components/cyd_display` is copied on every application branch; keep the copies in sync.
- No Wi-Fi or credentials. The binary also runs as a `bootloader` launcher image; keep it under 1.31 MB.
- Build with `pio run`; verify firmware changes with a PlatformIO build before flashing.
