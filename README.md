# ESP32 CYD Gear Roll

An [LVGL](https://lvgl.io) demo for the 320x240 display and resistive touchscreen on the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display). Two random gears mesh and roll together.

Each application lives on its own branch of this repository:

| Branch | Application |
|---|---|
| `bootloader` | Launcher: a boot menu with two image slots |
| `wifi_checker` | Wi-Fi and Bluetooth LE scanner |
| `birdnet_viewer` | BirdNET bird photo viewer |
| `screen_check` | Screen color test |
| `touchscreen_cal` | Touchscreen calibration |
| `stopwatch` | LVGL stopwatch |
| `gear_roll` | This gear demo |
| `Datalogger` | Voltage logger on GPIO35 |

## What it does

Every launch picks two gears with random tooth counts (8 to 36), random colors, and a random speed and direction. They turn together for as long as the app runs. The top row shows the tooth counts and the gear ratio. Tap a gear to re-cut it with a new random number of teeth; the pair resizes to fit and keeps rolling. Under the launcher, press EN to return to the boot menu.

The gears really mesh, as spur gears do:

- **Same module:** both share one module, the pitch diameter per tooth. Teeth only mesh when they are the same size.
- **Center distance:** the sum of the two pitch radii, so the pitch circles touch.
- **Speed:** gear 2 turns at −Z1/Z2 times gear 1's speed, so the pitch circles roll without slipping. Gear 1 advances by real elapsed time, so the speed holds whatever the frame rate.
- **Phase:** gear 2 is offset by half a tooth, so its teeth sit in gear 1's gaps on the line between the centers.
- **Size:** the module is picked so the pair fills the screen between the two text rows. It changes whenever a gear is re-cut.

Each gear is drawn in an LVGL draw event: a disc at the root circle, a tapered tooth (two triangles) per tooth from the root to the outer circle, a darker hub, and an off-center white dot that makes the rotation easy to see on fine gears. The serial log reports the frame rate every 5 seconds, and logs each re-cut.

## How LVGL is wired in

The same way as on the `stopwatch` branch:

- **LVGL 9:** comes from the ESP-IDF component manager (`main/idf_component.yml`, pinned by `dependencies.lock`).
- **Display:** strips are flushed through `cyd_draw_pattern`, which sends them in the panel's column-major order at the saved brightness.
- **Touch:** read with `cyd_touch_read`, which applies the saved calibration.

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core. The first build installs the ESP32 platform and ESP-IDF toolchain and downloads LVGL. The project path must be short: builds fail on Windows when source paths pass 260 characters.

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run
```

The app binary is `%USERPROFILE%\.platformio\cyd-gear-roll-build\esp32dev\firmware.bin`. Use it in one of two ways:

- **As a launcher image** (board runs the `bootloader` branch): every push to this branch runs `.github/workflows/build.yml`, which builds the app and publishes it as `firmware.bin` on the release tagged `gear_roll`. The launcher lists the branch and downloads that file. You can also write it to a slot over USB, as described in the `bootloader` README.
- **On its own:** `& $pio run --target upload --upload-port COM11`. This replaces everything on the board, including the launcher.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21, PWM-dimmed to the brightness saved by the launcher.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36 (XPT2046).
