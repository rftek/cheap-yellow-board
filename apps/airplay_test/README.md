# AirPlay Test

PlatformIO/ESP-IDF test of the [airplay_sender](../../components/airplay_sender) component on the Waveshare ESP32-S3-Touch-LCD-7 (800x480, capacitive touch). It plays a test tone from the board on any AirPlay speaker on your network.

This app lives in `apps/airplay_test`. The [root README](../../README.md) lists every app in this repository. It targets the S3 board only (`supported-targets.txt`).

## What it does

1. Joins the Wi-Fi network saved by the [launcher](../../launcher) (`wifi_ssid` and `wifi_pass` in NVS namespace `cyd_config`). No credentials are compiled in. Without a saved network it shows "No Wi-Fi saved".
2. Searches mDNS for AirPlay speakers (`_raop._tcp`) for 3 seconds and lists them. Speakers it can't play to are gray and marked `NO`; tap one to see why, such as "Needs a password". Tap the title to search again, and use the arrows to page through the list.
3. Tap a speaker to connect. The player screen has these buttons:
   - **PLAY TEST TONE**: a C-major arpeggio, a quarter second per note, so dropouts are easy to hear.
   - **STOP**: the speaker goes quiet at once.
   - **VOL -** and **VOL +**: volume in steps of 10%.
   - **DISCONNECT**: back to the list.

   The speaker starts playing about 2 s after you tap PLAY, which is the AirPlay buffer. If the speaker drops the session, the screen says so.

Everything is also printed to the serial log: each speaker's TXT record, the RTSP steps and the speaker's UDP ports.

## Build and flash

```powershell
cd apps\airplay_test
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e esp32s3
```

The first build downloads `espressif/mdns` through the ESP-IDF component manager. `dependencies.lock` pins its version. The image is `%USERPROFILE%\.platformio\cyd-airplay-build\esp32s3\firmware.bin`, about 970 KB.

- **As a launcher image**: when a change to this folder is merged into `main`, CI publishes `firmware-esp32s3.bin` on the release tagged `airplay_test`, and the launcher lists it.
- **Over USB into a launcher slot**: see the [launcher README](../../launcher/README.md). The board is on COM13. Set the UART DIP switch first.

Monitor with `& $pio device monitor --port COM13 --baud 115200`.

## Changelog

The version is in [`version.txt`](version.txt) and in the image's app description, where the launcher reads it. Every change to this app bumps it and adds an entry here, newest first; the [root README](../../README.md#versions) explains when to bump MAJOR or MINOR.

### 1.0 (2026-11-08)

- First numbered version. Searches the network for AirPlay speakers over mDNS and lists them, saying why any can't be used, then plays a C-major test tone through the `airplay_sender` component, with stop, volume and disconnect buttons. ESP32-S3 board only.
