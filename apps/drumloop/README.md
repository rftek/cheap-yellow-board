# DrumLoop

A live drum machine for the Waveshare ESP32-S3-Touch-LCD-7 (800x480, capacitive touch), made for building drum and bass and trap beats on the fly. Eight 808-style sounds sit on touch pads that play live. A step sequencer loops 1 to 16 bars at an adjustable tempo, and you can build its beat by tapping steps or by recording what you play on the pads. The sound plays on an AirPlay speaker through the [airplay_sender](../../components/airplay_sender) component.

This app lives in `apps/drumloop`. The [root README](../../README.md) lists every app in this repository. It targets the S3 board only (`supported-targets.txt`).

## Getting connected

1. The start screen has two buttons. **PLAY ON AIRPLAY SPEAKER** joins the Wi-Fi network saved by the [launcher](../../launcher) (`wifi_ssid` and `wifi_pass` in NVS namespace `cyd_config`); no credentials are compiled in, and the button is gray when none is saved. **NO SPEAKER: PLAY SILENT** skips Wi-Fi altogether, and the radio stays off.
2. It searches for AirPlay speakers for 3 seconds and lists them. Tap one to connect. Gray speakers marked `NO` can't be used; tap one to see why. Tap the title to search again.

To skip the speaker later, tap **NO SPEAKER: PLAY SILENT** under the list. The button is also on the "Could not join" screen. Silent mode opens the performance screen with no sound. You can still build beats, and the loop and playhead run in real time on the board's clock with no AirPlay delay. **CHANGE SPEAKER** in the menu goes back to the list, or to the start screen if Wi-Fi was skipped, and the beat is kept.

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
| **Pads** | Play the sound at once. The pad flashes white. |

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

- **VOL - / VOL +**: speaker volume in steps of 10%. It shows SILENT with no speaker.
- **BPM -10 / BPM +10**.
- **LOOP LENGTH**: 1, 2, 4, 8 or 16 bars. Each step up doubles the loop by copying the bars it has, so you can vary the copies. After 16 it goes back to 1 bar. The other bars are kept until the loop grows again.
- **LOAD TRAP BEAT** (140 BPM, half time) and **LOAD DNB BEAT** (174 BPM, two-step): one-bar starter patterns. They replace the pattern.
- **CLEAR PATTERN**: tap twice.
- **EXPORT LOOP**: a stub. See below.
- **AIRPLAY DELAY**: 2000, 1000, 500 or 250 ms, saved in NVS (namespace `drumloop`) and used from the next connect. 2000 ms is the AirPlay 1 norm and works on every speaker. Shorter delays make the pads feel more immediate but some speakers don't honor them; try it on yours.
- **CHANGE SPEAKER**: disconnects (or leaves silent mode) and goes back to the speaker list. The pattern is kept until power off.

## Latency

AirPlay buffers audio on the speaker, so a pad sounds after the AirPlay delay, 2 s by default. For playing pads live, pick a shorter delay in MENU and reconnect. Recording and the playhead allow for the delay, so recorded hits land on the steps you heard.

## Audio export (stub)

[main/export.h](main/export.h) is the start of audio export. `export_render()` works: it renders one pass of the loop plus a 1 s tail into a 44.1 kHz mono 16-bit WAV in PSRAM. `export_save()` still has no destination. It could go to an SD card, an HTTP download from the board, or an upload to a computer. Until then it returns `ESP_ERR_NOT_SUPPORTED`, and **EXPORT LOOP** reports the length of the WAV it rendered.

## Code

| File | Contents |
|---|---|
| `main/main.c` | Wi-Fi, speaker search and list, connect and disconnect, silent mode |
| `main/ui.c` | performance screen and menu |
| `main/audio.c` | audio task: runs the engine and writes 8 ms packets to the speaker, silence included, so pads play at any time; with no speaker it runs the engine on the clock |
| `main/engine.c` | step sequencer (sample-accurate steps) and mixer (retrigger fades, hat choke, soft clipping) |
| `main/kit.c` | 808-style synthesis |
| `main/export.c` | loop render to WAV, save stub |

## Limits

- The touch driver reports one finger at a time, so a pad hit while another finger is still down is missed. Finger-drum with taps that don't overlap.
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
