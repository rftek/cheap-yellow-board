# DrumLoop

A live drum machine for the Waveshare ESP32-S3-Touch-LCD-7 (800x480, capacitive touch), made for building drum and bass and trap beats on the fly. Eight drum sounds sit on touch pads that play live, from one of four kits: 808, 909, trap and drum and bass. A step sequencer loops 1 to 16 bars at an adjustable tempo, and you can build its beat by tapping steps or by recording what you play on the pads. The sound plays on an AirPlay speaker through the [airplay_sender](../../components/airplay_sender) component, or nowhere while you build a beat.

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

### The kits

All sounds are synthesized at boot from oscillators, noise and filters, the way analog drum machines make them, so there is no sample data: about 1.5 MB of 44.1 kHz audio in PSRAM for the four kits.

Every kit has the same eight roles, so a beat plays on any kit. Pick the kit with **KIT** in MENU. It changes the sound at once, even while the loop plays; a hit already ringing finishes on its old kit. The kit is part of the pattern, and the TRAP and DNB starter beats bring their own.

| Pad | 808 (classic) | 909 (punchy) | TRAP (long 808 bass) | DNB (tight breaks) |
|---|---|---|---|---|
| 1 | BD: sine swept from 160 to 47 Hz, 0.45 s decay, saturated | BD: short, hard click, 0.2 s | BD: 44 Hz sub with 0.9 s decay, driven hard | BD: short and punchy, 0.12 s |
| 2 | SD: two tones and noise | SD: brighter, more snares | SD: tight crack | SD: high-tuned, loud snares |
| 3 | CP: three bursts and a tail | CP | CP: wide, long tail | CP: short |
| 4 | CH: six detuned squares, high-passed; chokes the open hat | CH: noisier | CH: very short | CH: noisy, like a real hat |
| 5 | OH | OH | OH: short | OH: long |
| 6 | LT: low tom | MT: mid tom | LT: deep tom | MT: mid tom |
| 7 | RS: rim shot | RS | RS | RS |
| 8 | CB: cowbell | RD: ride | SN: finger snap | RD: ride |

### MENU

- **WI-FI** and **OUTPUT**: see [Starting up](#starting-up).
- **VOL - / VOL +**: speaker volume in steps of 10%. It shows SILENT with no speaker.
- **BPM -10 / BPM +10**.
- **LOOP LENGTH**: 1, 2, 4, 8 or 16 bars. Each step up doubles the loop by copying the bars it has, so you can vary the copies. After 16 it goes back to 1 bar. The other bars are kept until the loop grows again.
- **TRAP 140** (half time) and **DNB 174** (two-step): one-bar starter beats on their own kit. They replace the pattern.
- **CLEAR** (left; tap twice) and **EXPORT** (right; saves the loop to the SD card, see below) share a row.
- **KIT**: the next kit: 808, 909, TRAP, DNB, then the kits on the SD card. Past the last kit it looks for a card again, so a card put in after boot is found.
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

## Custom kits on an SD card

Put a FAT32 microSD card in the board's TF slot with your own samples in it:

```
kits/
  amen/
    1 BD.wav        pad 1, labelled BD
    2 SD snare.wav  pad 2, labelled SD
    4 CH.wav
    5 OH.wav
  vinyl/
    ...
```

- Each folder in `kits` is a kit, named after the folder (cut to 10 characters). Up to 8 card kits are listed, after the four built-in ones.
- A WAV goes on the pad given by the first character of its name, `1` to `8`. The next two letters or digits are the pad's label. Pads with no WAV keep the 808's sound, and other files are ignored.
- Any common WAV works: 8, 16, 24 or 32-bit PCM or 32-bit float, mono or stereo (mixed to mono), at any sample rate from 8 to 192 kHz (resampled to 44.1 kHz). Compressed WAVs, such as ADPCM, aren't read. Each sample is cut at 4 seconds and its level matched to the built-in sounds.
- Pads keep their roles: pad 4 still chokes pad 5, and the starter beats put kick, snare and hats on pads 1, 2 and 4.
- A card kit's WAVs are read when you first pick it in **KIT**, which takes a moment. They stay in PSRAM until power off; a few MB of PSRAM are free for them.

The card is mounted at boot through the I/O expander's chip select, as in Waveshare's 03_SD_Test example (see [cyd_sd_select](../../components/cyd_display/include/cyd_display.h)).

## Audio export

**EXPORT** renders one pass of the loop plus a 1 s tail into a 44.1 kHz mono 16-bit WAV in PSRAM ([main/export.h](main/export.h)) and saves it on the SD card as `exports/loop-001.wav`, `loop-002.wav` and so on. A 16-bar loop at 140 BPM is about 27 s, or 2.4 MB. With no card it says so. Other destinations, such as a download from the board over Wi-Fi, could take the same WAV.

## Code

| File | Contents |
|---|---|
| `main/main.c` | start-up: kit, NVS, audio task, touch, UI task |
| `main/output.c` | Wi-Fi on and off, speaker picker, connect and disconnect |
| `main/ui.c` | performance screen and menu |
| `main/audio.c` | audio task: runs the engine and writes 8 ms packets to the speaker, silence included, so pads play at any time; with no speaker it runs the engine on the clock |
| `main/engine.c` | step sequencer (sample-accurate steps) and mixer (retrigger fades, hat choke, soft clipping) |
| `main/kit.c` | the four kits' synthesis, and kits read from the SD card |
| `main/wav.c` | WAV reader: PCM and float, any channels and rate, to 44.1 kHz mono |
| `main/sdcard.c` | mounts the microSD card |
| `main/export.c` | loop render to WAV, saved to the SD card |

## Limits

- The GT911 tracks up to 5 fingers. The menu takes one finger at a time, with a short gap between taps.
- Patterns are not saved across power cycles yet.
- Don't take the SD card out while DrumLoop runs: it stays mounted, and a card put back isn't seen again until a restart.
- The SD card code hasn't been tried on hardware yet.

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
