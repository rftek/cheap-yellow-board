# ESP32 CYD BirdNET Display

This PlatformIO/ESP-IDF application targets the 320x240 display and resistive touchscreen on the ESP32 Cheap Yellow Display (CYD), following the [board documentation](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display).

On startup, the application connects to Wi-Fi and queries the local BirdNET server. It then refreshes every two minutes or immediately after a touchscreen tap. The latest visit from today with a confidence of at least 80% is displayed as a full-screen Wikimedia image. The display intentionally contains no text or overlays. If there is no qualifying detection, the screen clears; a failed refresh preserves the last successfully displayed image.

Configure the BirdNET server with `BIRDNET_BASE_URL` in the local Wi-Fi credentials header. It defaults to `http://birdnet.local`; replace that value with the server's address on your network. The firmware uses:

- `GET {BIRDNET_BASE_URL}/api/v1/detections/visits`
- `GET {BIRDNET_BASE_URL}/api/v1/image/{URL-encoded-scientific-name}`

Wikimedia's 1280-pixel thumbnail is streamed over HTTPS directly into the ESP32 ROM JPEG decoder and downscaled to fill the 320x240 display. Streaming avoids keeping the full image in the ESP32's limited RAM; the lower-resolution 500-pixel variant is intentionally not used because it looked blurry when enlarged.

## Command-line build and flash

Install the PlatformIO IDE extension or PlatformIO Core. From PowerShell, at the project root:

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run
& $pio run --target upload --upload-port COM11
& $pio device monitor --port COM11 --baud 115200
```

Change `COM11` to the board's assigned serial port. The first build installs the ESP32 platform and ESP-IDF toolchain. The monitor exits with Ctrl+].

Before building, create `%USERPROFILE%\.cyd_private\wifi_credentials.h` from `main\wifi_credentials.example.h` and set the local Wi-Fi SSID/password and BirdNET server URL. Never commit that private file. Build products and machine-specific ESP-IDF/VS Code settings are ignored.

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36. Touch coordinates use the CYD documentation's XPT2046 calibration range mapped to the tested 320x240 landscape orientation.
