# ESP32 CYD F1 Race

A mini Formula 1 race for the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display): one lap of the Circuit of the Americas against Hamilton, Kimi and Verstappen, about 30 seconds. The first 5 seconds, from the grid up the main straight, are seen over the horizon in pseudo-3D, in the manner of OutRun: the road is drawn in perspective, stripe by stripe, and Turn 1 bends toward the horizon ahead. Then the view changes to a top-down map of the whole circuit for the rest of the lap.

This app lives in `apps/f1_race`. The [root README](../../README.md) lists every app in this repository.

## Playing

- **The splash screen** shows a red F1 mark and your car. Tap **START** to go to the grid.
- **The start:** four cars line up on a 2 × 2 grid: Hamilton (red Ferrari, 44) and Kimi (silver and teal Mercedes, 12) on the front row, Verstappen (dark blue Red Bull, 3) a nose ahead of you on the second. Red numbers count down 3, 2, 1, then GO.
- **The two views:** the race starts over the horizon, accelerating up the main straight with Turn 1 bending left ahead. After 5 seconds, just before the braking point for Turn 1, the screen switches to the top-down map: the whole circuit with all four cars on it, your car outlined in white. The map's road is 7 pixels wide, and the edge of the drawn road is the edge of the track.
- **Over the horizon (the first 5 seconds):** hold the left half of the screen to steer left and the right half to steer right. The car accelerates and brakes by itself.
- **On the map (the rest of the lap):** the controls change, and the map shows them on the grass, with faint dotted lines between the zones:
  - **Left half: pedals.** Hold the upper part (above the middle, labelled GAS) to accelerate, up to 330 km/h, and the lower part (BRAKE) to brake. With neither, the car coasts and slowly loses speed. The bar at the top shows GAS or BRAKE while you hold one.
  - **Right half: steering.** Hold the inner part (`<`) to steer left and the outer part (`>`) to steer right. Left and right are the driver's, as in the car: heading down the screen, left moves the car toward the screen's right.
  - Corners push the car toward their outside, harder the faster you go. Too fast into a corner and you can't steer against it: the car runs wide onto the grass. So brake before the corner, then steer through it.
  - The touchscreen reads one touch at a time, so you can't hold a pedal and steer together: let go of the gas to steer, as you would lift for a corner. Two fingers at once read as a point between them.
- **Off the track:** once a wheel is on the grass past the red and white kerbs, the car shakes and brakes hard to 90 km/h until you're back on the road.
- **Contact:** running into the back of a car holds you behind it and costs speed, so pass to the side. Side by side, cars push each other apart.
- **Blocking:** a car coming up behind you picks a side to pass on and keeps to it for a moment. Rivals steer at half your rate, so you can move across in front and hold them up. Over the horizon, cars close behind you show as colored marks along the bottom edge of the screen, under where each one is, wider when it is close; on the map you can see them.
- **The track:** the Circuit of the Americas in Austin, run anticlockwise as in the Grand Prix: up the hill to the Turn 1 hairpin, the esses, the Turn 11 hairpin, the long back straight, the stadium section, the long right-hander of Turns 16 to 18, and Turns 19 and 20 onto the main straight. The grid is just out of Turn 20, and the race ends at the start line one lap later. The bar at the top shows everyone's progress (on the map it shows FINISH at the end), with your position at the top left and your speed at the top right.
- **Results:** after the flag, the finishing order shows each driver with the winner's time and the gaps, and your own place and time below. Tap to go back to the splash screen.

### Hamilton's auto-win (the secret)

With the auto-win on, which is the default, Hamilton always wins, Kimi finishes second and Verstappen third, however well you drive. You can pass them and block them early on, but from just under halfway they close up behind you, from three-quarters distance they squeeze past a block, and they're all ahead before the flag.

Tap the **top-right corner** (the 40 × 40 pixels there) to toggle it, on the splash screen, during the race or on the results screen. Nothing on screen labels it. When the auto-win is **off**, a tiny dark-gray 2 × 2 dot appears in the very corner; with it on, there is nothing. A press that starts in the corner during the race doesn't steer.

With it off, the rivals race at their own pace, Hamilton fastest, then Kimi, then Verstappen, each varied slightly from race to race. They take every corner at 90 to 92% of its safe speed, so a lap braking and turning well beats them, but time on the grass or stuck behind a car loses that quickly, so the result depends on how you drive.

The setting is kept in NVS (namespace `f1_race`, key `rigged`), so it survives a restart. Each toggle is logged on the serial port as `Hamilton's auto-win on` or `off`.

## How it fits the board

| | |
|---|---|
| Road | Each road row below the horizon (y = 100) shows the road at a fixed depth: 112 / dy road units for the row dy pixels below it. The road's sideways offset at each depth is integrated from the track's curvature every frame, so curves bend toward the horizon. Stripes alternate every 4 units; the far rows fade into haze. |
| Track | `main/cota_track.h`: 880 road units of 6.27 m around COTA, each with its curvature, map position and heading. `tools/cota_track.py` makes it from the circuit's centerline (`tools/us-2012.geojson`): it smooths the line over 10 m, puts the grid at the start of the main straight, sizes the unit so an ideal race from the grid takes 30 seconds, and fits the map to the screen (turned 50 degrees). At startup the firmware works out each unit's safe speed, braking in time for the next corner. |
| Map | The whole circuit under a 20-row bar, from 1-bit masks of the road and its edge lines (19 KB). Each frame redraws only a 15 × 15 box where each car was and where it is, composed from the masks, the control labels and the cars. The labels' spots were picked on grass in each touch zone, clear of the road. |
| Drawing | No frame buffer. Over the horizon, the screen is composed in 20-row strips (a 12.8 KB buffer) and each strip is sent with `cyd_draw_bitmap`. The road and the score bar are redrawn every frame; the sky, mountains and hills only when the background scrolls on a curve or a message changes. |
| Logo | A red italic F1 mark drawn from shapes (a slanted stem, two bars and the 1), not a bitmap |
| Cars | One 40 × 20 sprite, recolored for each team with the car's number stamped on; scaled to depth, nearest pixel. Your car is drawn at twice that size. |
| Frame rate | Up to 25 frames per second, with physics on the measured frame time. The CPU runs at 240 MHz. |
| Touch | `cyd_touch_read`, polled once a frame; a press counts as released after 3 frames with no reading |
| Memory | About 61 KB of static RAM and 245 KB of flash |

Tuning constants are at the top of `main/main.c`: `TOP_SPEED`, `ACCELERATION`, `BRAKING`, `CENTRIFUGAL` and `DRIFT_MAX` (also in `tools/cota_track.py`, which sizes the track from them: change both and run `python tools/cota_track.py tools/us-2012.geojson main/cota_track.h`), `STEER_RATE`, `HORIZON_SECONDS`, `OFFROAD_SPEED`, `RIVAL_STEER` and `PASS_REACTION` (how easily you can block), the rivals' `base_pace` in `cars[]` and `PACE_JITTER`, and the auto-win's `RIG_*` and `lead` values.

The COTA centerline is from [bacinger/f1-circuits](https://github.com/bacinger/f1-circuits) (MIT license, Copyright (c) 2019-2025 Tomislav Bacinger).

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core (see [docs/INSTALL.md](../../docs/INSTALL.md)). The project path must be short: builds fail on Windows when source paths pass 260 characters.

```powershell
cd apps\f1_race
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e esp32
```

The app binary is `%USERPROFILE%\.platformio\cyd-f1race-build\esp32\firmware.bin`. Use it in one of two ways:

- **As a launcher image** (board runs the [launcher](../../launcher)): when a change to this folder is merged into `main`, `.github/workflows/build.yml` builds the app and publishes it as `firmware.bin` on the release tagged `f1_race`, and the ESP32-S3 image as `firmware-esp32s3.bin`. The launcher finds it in the app catalog and downloads that file. You can also write it to a slot over USB, as described in the [launcher README](../../launcher/README.md).
- **On its own:** `& $pio run --target upload --upload-port COM11`. This replaces everything on the board, including the launcher.

Monitor with `& $pio device monitor --port COM11 --baud 115200`. Ctrl+] exits.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21, PWM-dimmed to the brightness saved by the launcher.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36 (XPT2046).
