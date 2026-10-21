# ESP32 CYD Datalogger

A one-channel voltage logger for the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display). Tap **CH1** to start a session: it samples GPIO35 every 3 seconds, converts each sample to volts, and shows the latest reading with session statistics.

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
| `Datalogger` | This voltage logger |
| `How_to_Build` | Build environment and installation guide |

## Wiring

**CH1 is GPIO35, on the CYD's P3 connector.** Connect the signal to GPIO35 and its ground to the board's GND, also on P3. The input is single-ended: it measures against the board's ground.

- **Keep it between 0 and 3.3 V.** Anything above the chip's supply can damage it. For higher voltages, add a resistor divider and scale the result.
- **Accurate range: about 0.15–2.45 V.** Above 2.45 V readings still work up to about 3.1 V, but the ADC is non-linear there, so the reading turns yellow.
- **Leave nothing floating.** GPIO35 has no internal pull resistors, so an unconnected pin reads random values.
- **For a steadier reading,** put a 0.1 µF capacitor from GPIO35 to GND.

The original plan named IO21, IO22 and IO23. None of those can measure voltage on an ESP32, because they have no ADC. On this board IO21 also drives the backlight and IO23 the SD card. So CH1 uses GPIO35, an ADC1 pin, and there's no CH2 or CH3.

## Using it

| Screen row | Shows |
|---|---|
| CH1 GPIO35 ADC1 | Channel. Reads `UNCAL` in red if the chip has no factory calibration. |
| State | `TAP CH1 TO START`, `SAMPLING EVERY 3S` or `STOPPED` |
| Voltage | Latest sample, in volts with millivolt resolution |
| RAW | The averaged 12-bit reading behind it (0–4095) |
| N / T | Samples taken and session time |
| MIN / MAX / AVG | Session statistics |

The first sample is taken right away, then one every 3 seconds. Tap **CH1** again to stop; the button is green while running. A new session clears the statistics.

Every sample is also printed to the serial log (115200 baud) as CSV, ready for a spreadsheet:

```
# CH1 GPIO35 session, every 3 s, calibrated
t_s,raw,mV
0.0,1532,1287
3.0,1534,1289
```

## How a sample is taken

| Setting | Value |
|---|---|
| Driver | ESP-IDF `adc_oneshot`: one conversion on demand |
| Unit / channel | ADC1, channel 7 (GPIO35). ADC1 is unaffected by Wi-Fi. |
| Resolution | 12 bits (0–4095) |
| Attenuation | 12 dB (range about 0.15–2.45 V) |
| Averaging | 16 raw readings per sample, to reduce the ESP32 ADC's noise |
| Raw to millivolts | Line-fitting calibration from the factory eFuse values (Two Point or Vref). Without them it falls back to an estimate (raw × 3100 mV / 4095) and shows `UNCAL`. |

## ESP32 ADC configuration options

The ADC options on this chip (ESP32, ESP-IDF 6.1), for choosing different settings:

| Option | Values |
|---|---|
| ADC units | **ADC1**: 8 channels, GPIO32–39. **ADC2**: 10 channels, GPIO0, 2, 4, 12–15, 25–27. |
| ADC2 restriction | Unavailable while Wi-Fi is running |
| Input type | Single-ended only, against ground. No differential mode. |
| Resolution | 9, 10, 11 or 12 bits (`ADC_BITWIDTH_9` … `_12`) |
| Attenuation and recommended range | `ADC_ATTEN_DB_0`: about 100–950 mV · `DB_2_5`: about 100–1250 mV · `DB_6`: about 150–1750 mV · `DB_12`: about 150–2450 mV |
| Absolute maximum input | 3.3 V |
| Reference | Internal, about 1100 mV nominal; varies about 1000–1200 mV between chips, hence calibration |
| Calibration schemes | Line fitting only (eFuse Two Point, eFuse Vref, or a default Vref). Curve fitting is for newer ESP32 chips. |
| Drivers | Oneshot (`adc_oneshot`) for single readings; continuous (`adc_continuous`) for DMA streaming at about 20 kHz–2 MHz |
| ADC pins free on the CYD | GPIO35 (P3), GPIO27 (CN1, ADC2), GPIO34 (wired to the onboard light sensor) |

## Build and flash

Install the PlatformIO IDE extension or PlatformIO Core (see the `How_to_Build` branch). The project path must be short: builds fail on Windows when source paths pass 260 characters.

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run
```

The app binary is `%USERPROFILE%\.platformio\cyd-datalogger-build\esp32dev\firmware.bin`. Use it in one of two ways:

- **As a launcher image** (board runs the `bootloader` branch): every push to this branch runs `.github/workflows/build.yml`, which builds the app and publishes it as `firmware.bin` on the release tagged `Datalogger`. The launcher lists the branch and downloads that file. You can also write it to a slot over USB, as described in the `bootloader` README.
- **On its own:** `& $pio run --target upload --upload-port COM11`. This replaces everything on the board, including the launcher.

To capture the CSV: `& $pio device monitor --port COM11 --baud 115200`. Ctrl+] exits.

## Hardware

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21, PWM-dimmed to the brightness saved by the launcher.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36 (XPT2046).
