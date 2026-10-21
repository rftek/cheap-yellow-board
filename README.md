# ESP32 CYD Cat & Mouse

A small maze game in the manner of PAC-MAN for the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display). You are the mouse. Eat every piece of cheese while three cats chase you; a big cheese turns the tables for a few seconds.

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
| `CatMouse` | This maze game |
| `How_to_Build` | Build environment and installation guide |

## Playing

- **Steer with the WASD keys** in the bottom-right corner, laid out like a keyboard's: **W** up, with **A** left, **S** down and **D** right beneath it. Each key is 32 × 24 pixels, so the pad is 96 × 48, and a key lights yellow while you hold it. A touch up to 10 pixels outside the pad still goes to the nearest key, for the resistive panel's aim; touches elsewhere do nothing. Like PAC-MAN, the mouse remembers a turn until the next opening, and it keeps running until it meets a wall.
- **Cheese** is worth 10 points. Eat all 113 pieces to clear the maze.
- **Big cheese** (the four round ones in the corners, 50 points) scares the cats for 6 seconds, a second less each level down to 2. Scared cats turn blue, slow down and run at random; they flash white just before they recover. Catch one for 200 points, then 400 and 800 for the next ones on the same big cheese. A caught cat's eyes run back to the basket, and it comes out again shortly after.
- **The cats** take turns wandering to their own corners (7 seconds) and hunting (20 seconds). The orange cat heads straight for you, the white one for four tiles ahead of you, and the pink one only chases from a distance. One starts outside the basket; the other two follow.
- **The tunnel** on the middle row runs off both sides of the screen.
- **Lives:** you start with three, shown at the top right. Each level is faster, and the cats leave the basket sooner.
- **Score** is at the top left, and the level beside the cheese in the middle. The best score is kept in NVS (namespace `catmouse`, key `high`) and shown on the title screen.

## How it fits the board

| | |
|---|---|
| Maze | 20 × 14 tiles of 16 pixels (320 × 224), under a 16-pixel score bar. The WASD pad's corner is kept out of it: the tiles under the pad (rows 11-13 of columns 14-19) are wall, the bottom-right corner is reshaped around them with its big cheese moved up to row 9, and maze drawing leaves the keys' pixels to the pad. |
| Frame rate | 25 frames per second, with speeds in eighths of a pixel per frame |
| Drawing | No frame buffer. Each frame redraws only the box around the mouse and each cat, composed in a 2 KB buffer first (maze, then cats, then the mouse) and sent with `cyd_draw_bitmap`, so nothing flickers. |
| Sprites | 16 × 16, stored as text in `main/main.c`; the mouse is turned for each direction, and each cat is recolored when scared |
| Memory | About 19 KB of static RAM and 220 KB of flash |
| Touch | `cyd_touch_read`, polled once a frame; the nearest WASD key to the touch wins |
| Cats | Classic target-tile chasing: at each tile they take the open way, other than back, closest to their target |

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core (see the `How_to_Build` branch). The project path must be short: builds fail on Windows when source paths pass 260 characters.

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run
```

The app binary is `%USERPROFILE%\.platformio\cyd-catmouse-build\esp32dev\firmware.bin`. Use it in one of two ways:

- **As a launcher image** (board runs the `bootloader` branch): every push to this branch runs `.github/workflows/build.yml`, which builds the app and publishes it as `firmware.bin` on the release tagged `CatMouse`. The launcher lists the branch and downloads that file. You can also write it to a slot over USB, as described in the `bootloader` README.
- **On its own:** `& $pio run --target upload --upload-port COM11`. This replaces everything on the board, including the launcher.

Monitor with `& $pio device monitor --port COM11 --baud 115200`. Ctrl+] exits.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21, PWM-dimmed to the brightness saved by the launcher.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36 (XPT2046).
