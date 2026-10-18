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

## Screen check (color test)

`apps/screen_check` is a separate full-screen display test built on `components/cyd_display`, a shared driver for the display, a 5x7 font, pattern drawing and touch. Tap the right half of the screen for the next pattern and the left half for the previous one. The list wraps around. Each pattern's name shows on the bottom row for 1.5 seconds, then disappears.

| Pattern | What to look for |
|---|---|
| Red, green, blue, white, black, cyan, magenta, yellow, 50% gray | Dead or stuck pixels, uneven backlight, tint |
| Color bars | Correct color order (white, yellow, cyan, green, magenta, red, blue, black); swapped red and blue |
| RGB ramps | Smooth red, green, blue and gray gradients; banding |
| Gray steps | 16 distinguishable steps from black to white |
| Checkerboard | 1-pixel alternation; flicker or crosstalk |
| Grid | Straight 40-pixel grid lines and a 1-pixel red border on all four edges; clipping or offset |

It needs no Wi-Fi or credentials. Build and flash it on its own; this replaces whatever is on the board:

```powershell
& $pio run --project-dir apps\screen_check
& $pio run --project-dir apps\screen_check --target upload --upload-port COM11
```

The shared driver sends pixels column by column, which is the order this panel fills a drawing window, and leaves the BGR bit off, so colors are correct. The BirdNET viewer above still has its own older display code.

Display pins: MOSI GPIO13, SCLK GPIO14, CS GPIO15, D/C GPIO2, backlight GPIO21.
Touch pins: MOSI GPIO32, MISO GPIO39, SCLK GPIO25, CS GPIO33, IRQ GPIO36. Touch coordinates use the CYD documentation's XPT2046 calibration range mapped to the tested 320x240 landscape orientation.
