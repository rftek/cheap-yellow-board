# Drum&Key

[DrumLoop](../drumloop) and [Keyboard](../keyboard) in one app for the Waveshare ESP32-S3-Touch-LCD-7. The drum pads and step sequencer are on one screen and the keyboard (two octaves at a time, across C1 to C8) and its loop recorder on the other, with a button in the header to switch between them. Both play at the same time on one AirPlay speaker, and their loops share one transport and one tempo, so a keyboard part lines up with the beat.

This app lives in `apps/drumkey`. The [root README](../../README.md) lists every app in this repository. It targets the S3 board only (`supported-targets.txt`).

## The two screens

Both screens have the same header:

```
 PLAY REC - 120 + [1/4] KEYS MENU     drums screen
 PLAY REC - 120 + [MET] DRUM MENU     keys screen
```

| Control | What it does |
|---|---|
| **PLAY / STOP** | Starts both loops from bar 1 in the same 8 ms block, or stops both. STOP also flushes the speaker's buffer. |
| **REC** | Per screen. On the drums screen, pad hits are written into the grid while the loop plays. On the keys screen, it records a keyboard take; from stop it also starts both loops, with no count-in, since the drums keep time. |
| **- / +** | The shared tempo, 60 to 200 BPM. Both loops follow it. |
| **1/4** (drums) | The bar page, as in DrumLoop. Past 9 bars it shows `B12` and so on. |
| **MET** (keys) | The keyboard's metronome. |
| **KEYS / DRUM** | Switches to the other screen at once. Sounds keep playing; keys held when you leave are lifted. |
| **MENU** | That screen's settings, as in its own app: kits, styles, export and the drum loop length on the drums side; voices, quantize and the keyboard loop length on the keys side. Wi-Fi, the speaker, volume and the AirPlay delay are shared, so they can be set from either. |

Everything else works as in [DrumLoop](../drumloop/README.md) and [Keyboard](../keyboard/README.md): eight pads and a 16-step grid (hold a step to double it), 25 keys that move across seven octaves (OCT - / OCT + and a tappable octave map), a sustain pedal, seven voices in 23 styles, kits from an SD card, and EXPORT (which renders the drums only).

The drum loop runs 1 to 16 bars and the keyboard loop 1 to 8. Both start together at bar 1, so with power-of-two lengths they stay in step.

## Latency and load

- Pads and keys play from the touch task's callbacks, as in the two apps, so a hit goes straight to the audio task without waiting for the screen to redraw.
- The audio task runs on core 1 at a priority above the UI, with Wi-Fi on core 0. It renders the synth and the drums into each 8 ms block and mixes them through the same soft clipper the drum engine uses.
- Every 10 s the audio task logs its render time: `Render: average N us, worst N us of 7981 us per block`. Over about 4000 us on average leaves too little time for the rest. Then use a lighter style or fewer notes.
- The AirPlay delay is the speaker's buffer, as in the other music apps. Pick it in MENU.

## Code

| File | What it does |
|---|---|
| `main/main.c` | Startup, and the UI task that alternates the two screens |
| `main/dk_audio.c` | The audio task's render: shared transport and tempo, the drum engine and the looper, mixed |
| `main/drums_ui.c` | The drums screen, adapted from DrumLoop's `ui.c` |
| `main/keys_ui.c` | The keys screen, adapted from Keyboard's `ui.c` |
| `main/dk_ui.c` | The header layout and the gaps between its buttons |

The engines are compiled from the other apps' folders, so a fix there reaches Drum&Key too:
- `apps/drumloop/main`: `engine.c`, `kit.c`, `export.c`, `sdcard.c` and `wav.c`
- `apps/keyboard/main`: `synth.c` and `looper.c`

`uses-apps.txt` names those two apps, so CI rebuilds Drum&Key when either changes.

## Limits

- Not tried on the board yet.
- Loops are not saved across power cycles.
- EXPORT renders the drum loop only.
- The GT911 tracks 5 fingers in all.

## Build and flash

```bash
cd apps/drumkey
pio run -e esp32s3
```

The image goes in a launcher slot, like the other apps.

## Changelog

### 1.0 (2026-11-08)

- First version: DrumLoop's drums and Keyboard's keys in one app with a screen switch, one shared transport and tempo, and both mixed into one AirPlay stream. The keys move across seven octaves, C1 to C8, as in Keyboard.
