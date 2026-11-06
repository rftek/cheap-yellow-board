# ESP32 CYD Cat & Mouse

A small maze game in the manner of PAC-MAN for the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display). You are the mouse. Eat every piece of cheese while three cats chase you; a big cheese turns the tables for a few seconds.

This is the `catmouse` game with Xbox controller input added through `components/cyd_gamepad` (Bluepad32 on BTstack): the D-pad, or the left stick past a dead zone, steers the mouse, and touch still works while the pad is idle.

This app lives in `apps/catmouse_pad`. The [root README](../../README.md) lists every app in this repository.

## Playing

- **Steer like a joystick:** touch anywhere on the screen, and that point becomes the stick's center. Slide the finger more than 12 pixels left, right, up or down from it and the mouse is asked to go that way; the way you moved furthest wins. Lift and touch again to set a new center. A small indicator in the score bar, left of the level, shows the stick for debugging: its middle dot turns green while a finger is down, and the arrow for the way it's pushed lights yellow. Like PAC-MAN, the mouse remembers a turn until the next opening, and it keeps running until it meets a wall.
- **Cheese** is worth 10 points. Eat all 128 pieces to clear the maze.
- **Big cheese** (the four round ones in the corners, 50 points) scares the cats for 6 seconds, a second less each level down to 2. Scared cats turn blue, slow down and run at random; they flash white just before they recover. Catch one for 200 points, then 400 and 800 for the next ones on the same big cheese. A caught cat's eyes run back to the basket, and it comes out again shortly after.
- **The cats** take turns wandering to their own corners (7 seconds) and hunting (20 seconds). The orange cat heads straight for you, the white one for four tiles ahead of you, and the pink one only chases from a distance. One starts outside the basket; the other two follow.
- **The tunnel** on the middle row runs off both sides of the screen.
- **Lives:** you start with three, shown at the top right. Each level is faster, and the cats leave the basket sooner.
- **Score** is at the top left, and the level beside the cheese in the middle. The best score is kept in NVS (namespace `catmouse`, key `high`) and shown on the title screen.

## How it fits the board

| | |
|---|---|
| Maze | 20 × 14 tiles of 16 pixels (320 × 224), under a 16-pixel score bar |
| Frame rate | 25 frames per second, with speeds in eighths of a pixel per frame |
| Drawing | No frame buffer. Each frame redraws only the box around the mouse and each cat, composed in a 2 KB buffer first (maze, then cats, then the mouse) and sent with `cyd_draw_bitmap`, so nothing flickers. |
| Sprites | 16 × 16, stored as text in `main/main.c`; the mouse is turned for each direction, and each cat is recolored when scared |
| Memory | About 19 KB of static RAM and 220 KB of flash |
| Touch | `cyd_touch_read`, polled once a frame. The first reading of a touch is the joystick's center; a finger counts as lifted after two frames without a reading, so the panel's brief dropouts don't move it |
| Cats | Classic target-tile chasing: at each tile they take the open way, other than back, closest to their target |

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core (see [docs/INSTALL.md](../../docs/INSTALL.md)). The project path must be short: builds fail on Windows when source paths pass 260 characters. Like `xbox_controller`, it needs the Bluepad32 submodule, its BTstack patch and the `BLUEPAD32_ROOT` and `BTSTACK_ROOT` environment variables; see [docs/INSTALL.md](../../docs/INSTALL.md#bluepad32-for-xbox_controller-and-catmouse_pad).

```powershell
cd apps\catmouse_pad
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e esp32
```

The app binary is `%USERPROFILE%\.platformio\cyd-catmouse-pad-build\esp32\firmware.bin`. Use it in one of two ways:

- **As a launcher image** (board runs the [launcher](../../launcher)): when a change to this folder is merged into `main`, `.github/workflows/build.yml` builds the app and publishes it as `firmware.bin` on the release tagged `catmouse_pad`, and the ESP32-S3 image as `firmware-esp32s3.bin`. The launcher finds it in the app catalog and downloads that file. You can also write it to a slot over USB, as described in the [launcher README](../../launcher/README.md).
- **On its own:** `& $pio run --target upload --upload-port COM11`. This replaces everything on the board, including the launcher.

Monitor with `& $pio device monitor --port COM11 --baud 115200`. Ctrl+] exits.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21, PWM-dimmed to the brightness saved by the launcher.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36 (XPT2046).
