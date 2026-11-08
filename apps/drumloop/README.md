# DrumLoop

A live drum machine for the Waveshare ESP32-S3-Touch-LCD-7 (800x480, capacitive touch), made for building drum and bass and trap beats on the fly. Eight 808-style sounds sit on touch pads that play live. A step sequencer loops 1 to 16 bars at an adjustable tempo, and you can build its beat by tapping steps or by recording what you play on the pads. The sound plays on an AirPlay speaker through the [airplay_sender](../../components/airplay_sender) component, or nowhere while you build a beat.

Bluetooth output isn't possible on this board: the ESP32-S3 has no Classic Bluetooth, so no A2DP, and its BLE 5.0 radio lacks the isochronous channels LE Audio needs. BLE MIDI, sending the hits to a phone or computer that makes the sound, would work and could be added.

This app lives in `apps/drumloop`. The [root README](../../README.md) lists every app in this repository. It targets the S3 board only (`supported-targets.txt`).

## Starting up

DrumLoop boots straight to the performance screen with Wi-Fi off and no speaker. The pads, the grid and the loop all work without sound: the loop and playhead run in real time on the board's clock. To hear it, open **MENU**:

1. **WI-FI** turns the radio on and joins the network saved by the [launcher](../../launcher) (`wifi_ssid` and `wifi_pass` in NVS namespace `cyd_config`). No credentials are compiled in. Tap it again to turn Wi-Fi off. The row is gray when the launcher has no network saved.
2. **OUTPUT** opens the speaker picker. It joins Wi-Fi first if it is off, searches for AirPlay speakers for 3 seconds, and lists them. Tap one to connect. Gray speakers marked `NO` can't be used; tap one to see why. Tap the title to search again. **NO SPEAKER** disconnects and goes silent; **BACK** leaves the output as it was.

The loop keeps playing when the output changes, and the beat is kept. If the speaker drops the session, DrumLoop goes silent and the MENU button turns red until you open the menu, which says so.

## The performance screen

```
 PLAY  REC   -  140 +   1/4  MENU      transport
 BD  [][][][][][][][][][][][][][][][]   step grid: 8 tracks x 16 steps
 SD  [][][][][][][][][][][][][][][][]   of the bar on view
 ...                                   (tap a label to mute its track)
 CB  [][][][][][][][][][][][][][][][]
 [BD][SD][CP][CH][OH][LT][RS][CB]      pads
```

| Control | What it does |
|---|---|
| **PLAY / STOP** | Starts the loop from its first step, or stops it. STOP also flushes the speaker's buffer so it goes quiet at once. |
| **REC** | While the loop plays, each pad hit is also written into the pattern at the nearest sixteenth to where you heard it. |
| **- / +** | Tempo, 60 to 200 BPM, one at a time (tens in MENU). Takes effect on the next step. |
| **bar page** (`1/4`) | Which bar the grid shows. Cyan means it follows the playhead. Tap to go to bar 1, 2, ... and back to follow. |
| **MENU** | The settings below. |
| **Track label** | Mutes or unmutes the track in the loop. The pads still play. |
| **Step** | Turns that step on or off in the bar on view. |
| **Pads** | Play the sound at once. The pad flashes white. Play with several fingers: each new finger is a hit, even while others are down. |

The grid's playhead and its bar page follow what the speaker is playing. That is the AirPlay delay behind what the board renders, so the screen matches what you hear.

### The kit

All sounds are synthesized at boot, the way the 808's circuits make them, so there is no sample data: about 350 KB of 44.1 kHz audio in PSRAM.

| Pad | Sound | How it's made |
|---|---|---|
| BD | Bass drum, the long trap "808" | sine swept from 160 to 47 Hz, 0.45 s decay, saturated |
| SD | Snare | two damped tones and high-passed noise |
| CP | Hand clap | band-passed noise in three bursts and a tail |
| CH | Closed hi-hat | six detuned square waves, high-passed; it chokes the open hat |
| OH | Open hi-hat | the same with a long decay |
| LT | Low tom | sine with a small pitch bend |
| RS | Rim shot | two high tones, very short |
| CB | Cowbell | 540 and 800 Hz squares through a band pass |

### MENU

- **WI-FI** and **OUTPUT**: see [Starting up](#starting-up).
- **VOL - / VOL +**: speaker volume in steps of 10%. It shows SILENT with no speaker.
- **BPM -10 / BPM +10**.
- **LOOP LENGTH**: 1, 2, 4, 8 or 16 bars. Each step up doubles the loop by copying the bars it has, so you can vary the copies. After 16 it goes back to 1 bar. The other bars are kept until the loop grows again.
- **TRAP 140** (half time) and **DNB 174** (two-step): one-bar starter beats. They replace the pattern.
- **CLEAR PATTERN**: tap twice.
- **EXPORT LOOP**: a stub. See below.
- **AIRPLAY DELAY**: 2000, 1000, 500, 250, 150 or 100 ms, saved in NVS (namespace `drumloop`) and used from the next connect. 2000 ms is the AirPlay 1 norm and works on every speaker. Shorter delays make the pads feel more immediate but some speakers don't honor them; try it on yours.

## Latency

AirPlay buffers audio on the speaker, so a pad sounds after the AirPlay delay, 2 s by default. For playing pads live, pick a shorter delay in MENU and pick the speaker again in OUTPUT. Recording and the playhead allow for the delay, so recorded hits land on the steps you heard.

Everything else between a tap and the speaker is kept short:

| Stage | Time |
|---|---|
| Touch: the GT911 scans about every 10 ms and the driver polls it every 5 ms in fast mode ([cyd_touch_set_fast](../../components/cyd_display/include/cyd_display.h)) | about 5-15 ms |
| The touch task hands a pad straight to the audio task ([cyd_touch_set_tap_callback](../../components/cyd_display/include/cyd_display.h)), without waiting for the screen to redraw | under 1 ms |
| The hit goes into the next 8 ms packet, sent 1 packet ahead of the clock (`lead_packets = 1`) | 8-16 ms |
| The AirPlay delay, plus whatever the speaker adds itself | 100 ms and up |

Below about 200 ms, a packet lost on Wi-Fi may not be resent in time, which you hear as a click or a gap. Wi-Fi power saving is off while DrumLoop is on Wi-Fi.

AirPlay can't get much closer to real time than that. A wired I2S amplifier or DAC on spare pins would bring a tap to about 10-15 ms and could be added as another OUTPUT.

## Audio export (stub)

[main/export.h](main/export.h) is the start of audio export. `export_render()` works: it renders one pass of the loop plus a 1 s tail into a 44.1 kHz mono 16-bit WAV in PSRAM. `export_save()` still has no destination. It could go to an SD card, an HTTP download from the board, or an upload to a computer. Until then it returns `ESP_ERR_NOT_SUPPORTED`, and **EXPORT LOOP** reports the length of the WAV it rendered.

## Code

| File | Contents |
|---|---|
| `main/main.c` | start-up: kit, NVS, audio task, touch, UI task |
| `main/output.c` | Wi-Fi on and off, speaker picker, connect and disconnect |
| `main/ui.c` | performance screen and menu |
| `main/audio.c` | audio task: runs the engine and writes 8 ms packets to the speaker, silence included, so pads play at any time; with no speaker it runs the engine on the clock |
| `main/engine.c` | step sequencer (sample-accurate steps) and mixer (retrigger fades, hat choke, soft clipping) |
| `main/kit.c` | 808-style synthesis |
| `main/export.c` | loop render to WAV, save stub |

## Limits

- The GT911 tracks up to 5 fingers. The menu takes one finger at a time, with a short gap between taps.
- Patterns are not saved across power cycles yet.

## Build and flash

```powershell
cd apps\drumloop
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e esp32s3
```

The first build downloads `espressif/mdns` through the ESP-IDF component manager. The image is `%USERPROFILE%\.platformio\cyd-drumloop-build\esp32s3\firmware.bin`, about 990 KB.

- **As a launcher image**: when a change to this folder is merged into `main`, CI publishes `firmware-esp32s3.bin` on the release tagged `drumloop`, and the launcher lists it.
- **Over USB into a launcher slot**: see the [launcher README](../../launcher/README.md). The board is on COM13. Set the UART DIP switch first.

Monitor with `& $pio device monitor --port COM13 --baud 115200`.
