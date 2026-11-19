# LiveDrum

A drum pad set for playing live on the Waveshare ESP32-S3-Touch-LCD-7. Large pads fill the screen and sound the moment a finger lands. Any pad can take a sound from the built-in kits or a WAV on the SD card, with its own level, pitch and choke group. A loop recorder takes what you play, with quantize, overdub and undo. Banks save the whole set to the SD card. It plays on an AirPlay speaker or on the wired PCM5102A DAC.

This app lives in `apps/livedrum`. The [root README](../../README.md) lists every app in this repository. It targets the S3 board only (`supported-targets.txt`). The same screens are in [Drum&Key](../drumkey) as its **LIVE** screen, sharing its tempo, transport and speaker.

## The pads screen

```
 PLAY REC - 100 + EDIT BANK MENU       transport, tempo, edit, banks, settings
 ▬▬▬▬ ▬▬▬▬ ▬▬▬▬ ▬▬▬▬                    the loop's beats, lit as you hear them
 ┌────┐┌────┐┌────┐┌────┐
 │ 9  ││ 10 ││ 11 ││ 12 │
 └────┘└────┘└────┘└────┘
 ┌────┐┌────┐┌────┐┌────┐               8, 12 or 16 pads, four across
 │ 5  ││ 6  ││ 7  ││ 8  │               (pad 1 bottom left, as on a drum machine)
 └────┘└────┘└────┘└────┘
 ┌────┐┌────┐┌────┐┌────┐
 │ 1  ││ 2  ││ 3  ││ 4  │
 └────┘└────┘└────┘└────┘
 2BAR Q16 MET UNDO CLR  1.3 24          the loop recorder
```

- **Pads** play on touch-down. Each finger is its own hit, up to the GT911's five, so flams, rolls and two-handed grooves work. A pad flashes white when you hit it and pale when the loop plays it.
- Each pad shows its number, its sound (`BD 808`, a WAV's name, `S1 SUPERSAW`) and any level, pitch or choke that isn't the default (`L60 +3 C1`). Its color says where the sound is from: the kit's drum colors, teal for a WAV, pink for a synth sample, dark for an empty pad.

| Control | What it does |
|---|---|
| **PLAY / STOP** | Starts the loop from bar 1, or stops it. STOP also flushes the speaker's buffer. |
| **REC** | Records your hits into the loop while it plays. From stop it starts the loop, after a bar of count-in clicks (switch the count-in off in MENU). Each REC, from on to off, is one take. |
| **- / +** | Tempo, 60 to 200 BPM. It is shared with the other music apps. |
| **EDIT** | Turns yellow. Tap a pad to open its sound picker. You can also hold a pad for 0.6 s while the loop is stopped. |
| **BANK** | Load and save pad sets on the SD card (see below). |
| **MENU** | Wi-Fi, speaker or wired DAC, volume, tempo, how many pads (8, 12 or 16), banks, count-in and AirPlay delay. |
| **2BAR** | Loop length: 1, 2, 4 or 8 bars. Going up copies the hits into the new bars. From 8 back to 1, the hits after bar 1 are dropped. |
| **Q16** | Quantize for recorded hits: 1/16, 1/32, 1/8 triplets (`Q8T`), 1/16 triplets (`Q16T`), off, 1/8. |
| **MET** | Metronome click on every beat while the loop plays, higher on the bar. |
| **UNDO** | Removes the last take. Tap again for the one before. |
| **CLR** | Tap twice (it turns red, `CLR?`) to empty the loop. |
| `1.3 24` | Bar and beat as heard, and how many hits the loop has. `IN 3` counts in. |

### Recording

Hits are written where you heard the loop when you hit the pad: the board subtracts the speaker's delay (up to 2 s over AirPlay, about 9 ms on the wired DAC) before quantizing, so a take lines up with what you played to. Takes layer on top of each other, so you can lay down kick and snare, then hats on the next pass, then fills. A hit you play just before the quantize step it lands on is not played twice in that pass.

The loop is kept in ticks (96 to a beat), so the tempo can change while it plays.

## Assigning sounds

EDIT, then tap a pad (or hold a pad while stopped):

```
 PAD 5: OH 808                  BACK
 Tap a sound to put it on pad 5
 KITS  SD CARD  SYNTH   OFF            where the sound comes from
 808   BD SD CP CH OH LT RS CB    ▲    the kits: tap a drum
 909   BD SD CP CH OH MT RS RD
 ...
 LVL -      LEVEL 80      LVL +
 PIT -      PITCH +0      PIT +        semitones, -12 to +12
 <PAD  PLAY  CHOKE OFF  PAD>
```

- **KITS**: every drum of the four built-in kits (808, 909, TRAP, DNB) and the SD card's kits (folders in `/kits`, as in [DrumLoop](../drumloop)). Tap one to put it on the pad. It plays so you hear it.
- **SD CARD**: the card's folders and WAV files, starting in `/samples` if there is one. Tap a folder to open it, **UP** to go back, a WAV to load it onto the pad. WAVs can be 8 to 32-bit PCM or 32-bit float, mono or stereo, at any rate from 8 to 192 kHz. They are read into memory at 44.1 kHz mono, scaled to full level, and cut at 4 s.
- **SYNTH** appears in Drum&Key only: its SYNTH screen's rendered samples S1 to S4. The pad keeps its own copy, so rendering S1 again later doesn't change the pad.
- **OFF** empties the pad.
- **LVL** sets the pad's level, 0 to 100 (80 by default). **PIT** shifts its pitch by semitones. Each change plays the pad.
- **CHOKE** puts the pad in group 1, 2 or 3. A hit cuts the other pads in its group, as a closed hat cuts an open one. In the default set, the 808 hats are in group 1 and the 909 hats in group 2.
- **<PAD / PAD>** step through the pads; **PLAY** plays the pad.

## Banks

**BANK** in the header (in Drum&Key: MENU, then BANKS) lists eight banks on the SD card, each with its tempo, loop length and hit count. A bank holds every pad's sound, level, pitch and choke group, the tempo, and the loop with its takes.

- **LOAD**, then **LOAD?**, replaces the pads and the loop with the bank's and sets its tempo.
- **SAVE**, then **SAVE?**, writes the current set over the bank.
- **DEFAULT PADS** (tap twice) puts the starting set back on the pads and keeps the loop.

Banks are text files, `/livedrum/bank1.txt` to `bank8.txt`, so you can also write one on a computer:

```
LIVEDRUM 1
bpm 100
bars 2
pad 1 80 0 0 kit 0 808
pad 2 75 -2 0 wav /sd/samples/snare 3.wav
pad 3 80 0 1 none
hit 0 1 1
hit 96 2 1
```

`pad` lines are: pad, level, pitch, choke group, then `kit <drum 0-7> <kit name>`, `wav <path>` or `none`. `hit` lines are: tick (96 to a beat), pad, take. A pad that plays a synth sample is saved as a WAV beside the banks under a new name (`synth001.wav`, `synth002.wav` and so on), since synth samples are gone after a power cycle. These files are never written over, so every bank that uses one keeps its sound. A WAV that isn't on the card any more leaves its pad empty, and the status line says how many were missing.

## The default set

| Pads | Sounds |
|---|---|
| 1-8 | the 808: kick, snare, clap, closed hat, open hat, tom, rim, cowbell |
| 9-12 | the 909: kick, snare, closed hat, open hat |
| 13-16 | trap kick and snare, DnB snare, trap snap |

## How it works

| File | Contents |
|---|---|
| `main/main.c` | start-up (kits, NVS, SD card, engine, touch, audio task) and the transport in the audio task |
| `main/ld_engine.c` | the pads' voices (pitch by interpolation, level, choke, 5 ms retrigger fade), the loop recorder and the metronome, run in the audio task |
| `main/ld_sounds.c` | each pad's sound (kit, WAV or synth sample), the SD card's folders, and the banks |
| `main/ld_ui.c` | the pads screen, the sound picker, the banks and the menu, shared with Drum&Key through `ld_host_t` |
| `../drumloop/main/kit.c`, `wav.c`, `sdcard.c` | DrumLoop's kits, WAV reader and SD card, compiled from that app's folder (`uses-apps.txt`) |

A pad is played from the touch task: `cyd_touch_set_tap_callback` gets each new finger, about 5-10 ms after it lands, and queues the pad for the audio task. The audio task picks it up at the start of its next 8 ms block, so a hit reaches the output within one block, plus the output's own delay: about 9 ms on the wired DAC, or the AirPlay delay set in MENU. The screen only draws the flash afterwards. The loop's hits and clicks are placed on their exact frame within a block.

Only the audio task changes the loop and the pads' sounds. The screens ask it to (`ld_set_sample`, `ld_record`, `ld_undo` and the rest) and wait for the next block. So a WAV's memory is freed only once no voice plays it.

## Limits

- 2048 hits in a loop.
- A WAV or synth sample is cut at 4 s on a pad. Sixteen 4 s samples take 5.6 MB of the 8 MB PSRAM.
- Touch has no velocity: every hit plays at the pad's level.
- Don't take the SD card out while LiveDrum runs: it stays mounted.

## Build and flash

```powershell
cd apps\livedrum
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e esp32s3
```

The image is `%USERPROFILE%\.platformio\cyd-livedrum-build\esp32s3\firmware.bin`.

- **As a launcher image**: when a change to this folder is merged into `main`, CI publishes `firmware-esp32s3.bin` on the release tagged `livedrum`, and the launcher lists it.
- **Over USB into a launcher slot**: see the [launcher README](../../launcher/README.md). The board is on COM13. Set the UART DIP switch to UART1 first. On UART2 it carries the PCM5102A's pins instead.

Monitor with `& $pio device monitor --port COM13 --baud 115200`.

## Changelog

### 1.0 (2026-11-17)

- First version: 8, 12 or 16 pads that play on touch-down with up to five fingers; a sound picker for kits, SD card WAVs and (in Drum&Key) synth samples; level, pitch and choke group per pad; a loop recorder with quantize, count-in, metronome, overdub takes and undo; eight banks on the SD card.
