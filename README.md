# ESP32 CYD Wi-Fi and Bluetooth Scanner

This PlatformIO/ESP-IDF application targets the 320x240 display and resistive touchscreen on the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display).

On every boot the board scans for nearby Wi-Fi networks, then for Bluetooth Low Energy devices (6-second active scan), and lists both on screen in large text:

- **Wi-Fi:** signal (dBm) and SSID, strongest first.
- **Bluetooth LE:** signal (dBm) and advertised name, named devices first, then strongest first. Devices without a name show their address in gray.

The touchscreen drives the UI:

- **Entry buttons:** tap a network or device to see its details (Wi-Fi: SSID, RSSI, channel, auth, cipher, BSSID, PHY/WPS; BLE: name, RSSI, address and address type, advertising type, TX power, manufacturer, first 16-bit service UUID). Tap anywhere to go back.
- **Arrow buttons:** the up/down arrows on each list header page through that list. They are dimmed when there is nothing more in that direction.
- **Header title:** tap "WI-FI" or "BLE" to scan again.

The full results, including Wi-Fi BSSIDs, are also printed to the serial log, along with the coordinates of every tap.

No network connection or credentials are needed: the board only listens. Classic (BR/EDR) Bluetooth devices are not scanned; only BLE, using the lightweight NimBLE host.

## Command-line build and flash

Install the PlatformIO IDE extension or PlatformIO Core. From PowerShell, at the project root:

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run
& $pio run --target upload --upload-port COM11
& $pio device monitor --port COM11 --baud 115200
```

Change `COM11` to the board's assigned serial port. The first build installs the ESP32 platform and ESP-IDF toolchain. The monitor exits with Ctrl+].

The firmware uses about 840 KB of the default 1 MB app partition. If you change `sdkconfig.defaults`, delete the generated `sdkconfig.esp32dev` so the new defaults are applied.

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36. Touch coordinates use the CYD documentation's XPT2046 calibration range (`TOUCH_RAW_*` in `main/main.c`) mapped to the 320x240 landscape orientation.
