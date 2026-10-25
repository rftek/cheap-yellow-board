# ESP32 CYD F1 Race

A mini Formula 1 race for the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display). The road runs over the horizon in pseudo-3D, in the manner of OutRun: it is drawn in perspective, stripe by stripe, and bends toward the horizon on curves. Race Hamilton, Kimi and Verstappen through four or five random curves to the chequered flag.

Each application lives on its own branch of this repository:

| Branch | Application |
|---|---|
| `bootloader` | Launcher: a boot menu with two image slots |
| `wifi_checker` | Wi-Fi, Bluetooth LE and LAN scanner |
| `birdnet_viewer` | BirdNET bird photo viewer |
| `screen_check` | Screen color test |
| `touchscreen_cal` | Touchscreen calibration |
| `stopwatch` | LVGL stopwatch |
| `gear_roll` | LVGL gear demo |
| `Datalogger` | Voltage logger on GPIO35 |
| `CatMouse` | PAC-MAN-style maze game |
| `f1_race` | This racing game |
| `How_to_Build` | Build environment and installation guide |

## Playing

- **The splash screen** shows a red F1 mark and your car. Tap **START** to go to the grid.
- **The start:** four cars line up on a 2 × 2 grid: Hamilton (red Ferrari, 44) and Kimi (silver and teal Mercedes, 12) on the front row, Verstappen (dark blue Red Bull, 3) a nose ahead of you on the second. Red numbers count down 3, 2, 1, then GO.
- **Steering:** hold the left half of the screen to steer left and the right half to steer right. The car accelerates by itself, up to 330 km/h. Curves push you toward their outside, so steer into them.
- **Off the track:** once a wheel is on the grass past the red and white kerbs, the car shakes and brakes hard to 90 km/h until you're back on the road.
- **Contact:** running into the back of a car holds you behind it and costs speed, so pass to the side. Side by side, cars push each other apart.
- **Blocking:** a car coming up behind you picks a side to pass on and keeps to it for a moment. Rivals steer at half your rate, so you can move across in front and hold them up. Cars close behind you show as colored marks along the bottom edge of the screen, under where each one is, wider when it is close.
- **The track:** a start straight, four or five random curves of random length, sharpness and direction with straights between, then a final straight to the chequered line. Each race is new. The bar at the top shows everyone's progress, with your position at the top left and your speed at the top right.
- **Results:** after the flag, the finishing order shows each driver with the winner's time and the gaps, and your own place and time below. Tap to go back to the splash screen.

### Hamilton's auto-win (the secret)

With the auto-win on, which is the default, Hamilton always wins, Kimi finishes second and Verstappen third, however well you drive. You can pass them and block them early on, but from just under halfway they close up behind you, from three-quarters distance they squeeze past a block, and they're all ahead before the flag.

Tap the **top-right corner** (the 40 × 40 pixels there) to toggle it, on the splash screen, during the race or on the results screen. Nothing on screen labels it. When the auto-win is **off**, a tiny dark-gray 2 × 2 dot appears in the very corner; with it on, there is nothing. A press that starts in the corner during the race doesn't steer.

With it off, the rivals race at their own pace, Hamilton fastest, then Kimi, then Verstappen, each varied slightly from race to race. A clean drive at full speed beats all three by about a second, but time on the grass or stuck behind a car loses that quickly, so the result depends on how you drive.

The setting is kept in NVS (namespace `f1_race`, key `rigged`), so it survives a restart. Each toggle is logged on the serial port as `Hamilton's auto-win on` or `off`.

## How it fits the board

| | |
|---|---|
| Road | Each road row below the horizon (y = 100) shows the road at a fixed depth: 112 / dy road units for the row dy pixels below it. The road's sideways offset at each depth is integrated from the track's curvature every frame, so curves bend toward the horizon. Stripes alternate every 4 units; the far rows fade into haze. |
| Track | One curvature value per road unit (about 2,500 units a race), with eased ends on each curve |
| Drawing | No frame buffer. The screen is composed in 20-row strips (a 12.8 KB buffer) and each strip is sent with `cyd_draw_bitmap`. The road and the score bar are redrawn every frame; the sky, mountains and hills only when the background scrolls on a curve or a message changes. |
| Logo | A red italic F1 mark drawn from shapes (a slanted stem, two bars and the 1), not a bitmap |
| Cars | One 40 × 20 sprite, recolored for each team with the car's number stamped on; scaled to depth, nearest pixel. Your car is drawn at twice that size. |
| Frame rate | Up to 25 frames per second, with physics on the measured frame time. The CPU runs at 240 MHz. |
| Touch | `cyd_touch_read`, polled once a frame; a press counts as released after 3 frames with no reading |
| Memory | About 53 KB of static RAM and 235 KB of flash |

Tuning constants are at the top of `main/main.c`: `TOP_SPEED`, `STEER_RATE`, `CENTRIFUGAL`, `OFFROAD_SPEED`, `RIVAL_STEER` and `PASS_REACTION` (how easily you can block), the rivals' `base_pace` in `cars[]` and `PACE_JITTER`, and the auto-win's `RIG_*` and `lead` values.

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core (see the `How_to_Build` branch). The project path must be short: builds fail on Windows when source paths pass 260 characters.

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run
```

The app binary is `%USERPROFILE%\.platformio\cyd-f1race-build\esp32dev\firmware.bin`. Use it in one of two ways:

- **As a launcher image** (board runs the `bootloader` branch): every push to this branch runs `.github/workflows/build.yml`, which builds the app and publishes it as `firmware.bin` on the release tagged `f1_race`. The launcher lists the branch and downloads that file. You can also write it to a slot over USB, as described in the `bootloader` README.
- **On its own:** `& $pio run --target upload --upload-port COM11`. This replaces everything on the board, including the launcher.

Monitor with `& $pio device monitor --port COM11 --baud 115200`. Ctrl+] exits.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21, PWM-dimmed to the brightness saved by the launcher.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36 (XPT2046).
