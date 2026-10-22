# ESP32 CYD Bluetooth Diagnostics

A Bluetooth diagnostic tool for the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display). On start it scans for Classic (BR/EDR) and Bluetooth LE devices, tries to connect to each one that might accept a connection, and shows which ones do. A configuration screen shows the ESP32 Bluetooth controller's and host's settings and changes the ones that can be changed while running.

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
| `bluetooth_diag` | This Bluetooth diagnostic tool |
| `How_to_Build` | Build environment and installation guide |

## What it does

1. **Scans.** A Classic inquiry (6.4 s by default), with name requests for devices that sent no name, then an active LE scan (6 s by default). The screen counts devices as they are found.
2. **Tests connections.** Every device that might accept one is tried in list order, and its row turns green or red. Tap anywhere to stop.
   - **Classic:** the board pages the device and asks for its SDP service list. A completed ACL connection means it accepts connections. If the device sent no name, its name is read too.
   - **LE:** only devices that advertise as connectable (`ADV_IND`) are tried. The board connects as a GATT client, requests an MTU of 517, lists the GATT services, reads Device Name, Manufacturer Name and Model Number if the device allows it, then disconnects. Devices that advertise as beacons, scannable-only or directed to another device are marked not connectable without trying.
   - The tool never pairs. It refuses every pairing or security request, so a connection test can't bond with anything.
3. **Lists the results.** Tap a device for its details.

## The screens

**Device list**

| Part | What it does |
|---|---|
| `BT12` (top left) | Device count. Tap it to scan again. |
| `CFG` | Opens the configuration pages |
| ▲ ▼ | Page through the list, eight devices at a time |
| Device rows | `C-72 name`: the radio (`C` Classic, `L` LE, `D` LE from a dual-mode device), the signal in dBm, then the name, or the address if there is none |
| Row color | Green: connects. Red: the test failed. Gray: not connectable, or unnamed and not tested yet. White: not tested. Yellow: being tested. |
| Bottom row | `OK 5 FAIL 2` and a `TEST` button that tests every device again |

**Device detail:** radio, name, address, signal (and the advertised TX power), the address kind (public, static random, or private) and advertising type for LE, or the device class for Classic. Then the test result: the time to connect, and for LE the MTU, connection interval, slave latency and supervision timeout, or for Classic the number of SDP services. A failure shows its reason, such as `PAGE TIMEOUT`, `NO RESPONSE`, `CONN FAILED` or `REJECTED`. `TEST` tests this device again; `SVCS` lists the services by UUID with their names, the manufacturer and the model.

Everything is also printed to the serial log (115200 baud): each scan's devices, and each test's result with its services.

## Configuration pages

`CFG` opens five pages; `<` and `>` move between them. Rows with `-` and `+` buttons are settings. They take effect at once and are saved in NVS (namespace `bt_diag`), so they last across restarts. The other rows show what the controller and host report.

| Page | Shows | Settings |
|---|---|---|
| 1 Controller | Status, mode (dual), maximum LE connections, maximum ACL and SCO links, modem sleep, controller task priority and core, minimum encryption key size, duplicate filter list size | |
| 2 Host | Bluedroid status, the board's Bluetooth address and name, Secure Simple Pairing and Secure Connections, open ACL links, the page timeout read back from the controller | `VISIBLE`: makes the board discoverable and connectable over Classic, for testing it from a phone |
| 3 TX power (dBm) | The LE and BR/EDR levels the controller reports now, and its inquiry and page TX levels | `LE ADV`, `LE SCAN`, `LE CONN`: −12 to +9 dBm. `BR/EDR LO`, `BR/EDR HI`: the BR/EDR power range. |
| 4 Scan and test | | `INQUIRY s` (Classic inquiry, 1.3–13 s), `LE SCAN s` (2–30 s), `LE ACTIVE` (active scans ask for scan responses, which often carry the name), `LE INT ms` and `LE DUTY %` (scan interval, and how much of it the radio listens), `LE CONN s` (LE connection timeout), `PAGE TO s` (Classic page timeout, 1.3–41 s), `AUTO TEST` (test after every scan) |
| 5 Build | ESP-IDF version, LE 4.2 and 5.0 features, GATT client and server, SMP, GATT cache, local MTU, BTC task stack, free and minimum free heap | |

On the first start the TX power settings begin at the controller's own levels. The ESP32 applies a new BR/EDR power range only before its radio has transmitted, so a change shows in `BR NOW` after the next restart.

The settings fixed when the firmware is built are in `sdkconfig.defaults`: a dual-mode controller (`CONFIG_BTDM_CTRL_MODE_BTDM`) with the Bluedroid host, Classic with SSP, LE 4.2 features, and the GATT client without the GATT server.

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core (see the `How_to_Build` branch). The project path must be short: builds fail on Windows when source paths pass 260 characters.

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run
```

The app binary (about 850 KB, mostly the Bluedroid dual-mode stack) is `%USERPROFILE%\.platformio\cyd-bt-diag-build\esp32dev\firmware.bin`. Use it in one of two ways:

- **As a launcher image** (board runs the `bootloader` branch): every push to this branch runs `.github/workflows/build.yml`, which builds the app and publishes it as `firmware.bin` on the release tagged `bluetooth_diag`. The launcher lists the branch and downloads that file. You can also write it to a slot over USB, as described in the `bootloader` README.
- **On its own:** `& $pio run --target upload --upload-port COM11`. This replaces everything on the board, including the launcher.

To watch the log: `& $pio device monitor --port COM11 --baud 115200`. Ctrl+] exits.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21, PWM-dimmed to the brightness saved by the launcher.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36 (XPT2046).
