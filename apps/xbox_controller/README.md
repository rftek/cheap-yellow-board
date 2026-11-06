# ESP32 CYD XBOX Controller Test

A controller test tool for the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display). It pairs with an Xbox wireless controller over Bluetooth using the [Bluepad32](https://github.com/ricardoquesada/bluepad32) library, then shows a live screen that lights up each button, D-pad direction, stick and trigger as you press it — a quick way to check that every control on a controller works.

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
| `bluetooth_diag` | Bluetooth Classic and LE scanner and connection tester |
| `f1_race` | F1 racing game |
| `xbox_controller` | This Xbox controller test |
| `How_to_Build` | Build environment and installation guide |

## Pairing

1. Power on the CYD. It shows **Searching…** and is ready to pair straight away.
2. Put the controller into pairing mode: turn it on with the Xbox button, then press and hold the small **pair button** on the top edge (next to the USB port) until the Xbox light **blinks quickly**.
3. The board connects within a few seconds and switches to the test screen. An Xbox One S / Series controller connects over Bluetooth LE; older Bluetooth Classic pads work too.
4. The board remembers the controller, so next time it reconnects on its own — just turn the controller on.
5. To pair a different controller, tap **Forget & re-pair** at the bottom of the screen. The board clears the stored pairing and reboots back into searching mode.

> Some early Xbox One controllers need a firmware update before they expose Bluetooth, and a few firmware versions are known to be unreliable over BLE. If a controller will not connect, update it with the Xbox Accessories app first.

## The test screen

Each control turns **green** when it is active and stays grey when it is not:

- **A B X Y** — face buttons
- **LB RB** — bumpers; **L3 R3** — stick clicks
- **Up Dn Lf Rt** — D-pad
- **View Menu Xbox Sh** — View (select), Menu (start), the Xbox guide button, and Share/Capture
- **LT / RT** — triggers, drawn as a ten-segment bar from the analog value (0–1023)
- **LS / RS** — left and right sticks, shown as X/Y numbers (−512…511)
- **Battery** — the controller's reported level, when it sends one

## How it is built

This is a native ESP-IDF project built with PlatformIO. Bluepad32 does not use the ESP-IDF Bluedroid host; it runs on **BTstack** talking straight to the ESP32 Bluetooth controller (`CONFIG_BT_CONTROLLER_ONLY=y` in `sdkconfig.defaults`).

Bluepad32 and its BTstack copy are vendored as a git submodule at `external/bluepad32` (BTstack is nested at `external/bluepad32/external/btstack`). Integration follows Bluepad32's own esp32 example: the root `CMakeLists.txt` points `EXTRA_COMPONENT_DIRS` at `$BLUEPAD32_ROOT/src/components` and includes BTstack's `BTstack_project.cmake`, which turns BTstack into an ESP-IDF component. Two environment variables must be set — `BLUEPAD32_ROOT` and `BTSTACK_ROOT` — which the CI workflow exports at job scope.

The CI workflow checks out submodules recursively, applies Bluepad32's BTstack patch (`$BLUEPAD32_ROOT/external/patches/*.patch`, an l2cap fix for incoming controller connections), then builds with `pio run`. If a future Bluepad32 version changes this, follow its current [ESP-IDF instructions](https://bluepad32.readthedocs.io/en/latest/plat_esp32).

To build locally:

```
git submodule update --init --recursive
export BLUEPAD32_ROOT="$PWD/external/bluepad32"
export BTSTACK_ROOT="$BLUEPAD32_ROOT/external/btstack"
git -C "$BTSTACK_ROOT" apply "$BLUEPAD32_ROOT"/external/patches/*.patch
pio run
```

## Architecture

`btstack_run_loop_execute()` never returns, so `app_main` ends by handing its task to BTstack. The Bluepad32 callbacks therefore run on that task, and the user interface runs on a second FreeRTOS task. `on_controller_data` copies the latest `uni_gamepad_t` and battery level into one struct that the UI task reads, guarded by a `portMUX` spinlock. `components/cyd_display` provides the display, text and touch helpers shared by every image in this repository.

## Storage and CPU

The binary also runs as a launcher boot image, so it must fit in a **1.31 MB** OTA slot (1,376,256 bytes). Bluepad32 on BTstack is comparable in size to the Bluedroid dual-mode app on the `bluetooth_diag` branch (~850 KB), so it is expected to fit, though with less headroom. The authoritative size is the `firmware.bin` produced by the CI build. BTstack is event-driven: steady-state CPU use while a controller streams input (~100 Hz) is low, dominated by the Bluetooth controller and the periodic screen redraw.
