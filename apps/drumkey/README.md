# Drum&Key

[DrumLoop](../drumloop) and [Keyboard](../keyboard) in one app for the Waveshare ESP32-S3-Touch-LCD-7. The drum pads and step sequencer are on one screen and the keyboard (two octaves at a time, across C1 to C8) and its loop recorder on the other, with a button in the header to switch between them. Both play at the same time on one AirPlay speaker, and their loops share one transport and one tempo, so a keyboard part lines up with the beat.

This app lives in `apps/drumkey`. The [root README](../../README.md) lists every app in this repository. It targets the S3 board only (`supported-targets.txt`).

## The two screens

Both screens have the same header:

```
 PLAY REC - 120 + [1/4] ROLL MENU     drums screen
 PLAY REC - 120 + [MET] DRUM MENU     keys screen
 PLAY REC - 120 + [1/16] SYN MENU     Piano Roll
 PLAY REC - 120 + [P1] KEYS MENU      SYNTH
```

| Control | What it does |
|---|---|
| **PLAY / STOP** | Starts both loops from bar 1 in the same 8 ms block, or stops both. STOP also flushes the speaker's buffer. |
| **REC** | Per screen. On the drums screen, pad hits are written into the grid while the loop plays. On the keys screen, it records a keyboard take; from stop it also starts both loops, with no count-in, since the drums keep time. |
| **- / +** | The shared tempo, 60 to 200 BPM. Both loops follow it. |
| **1/4** (drums) | The bar page, as in DrumLoop. Past 9 bars it shows `B12` and so on. |
| **MET** (keys) | The keyboard's metronome. |
| **DRUM / ROLL / SYN / KEYS** | The screen button: goes round keys â†’ drums â†’ Piano Roll â†’ SYNTH â†’ keys, at once. Sounds keep playing; keys held when you leave are lifted. |
| **MENU** | That screen's settings, as in its own app: kits, styles, export and the drum loop length on the drums side; voices, quantize and the keyboard loop length on the keys side. Wi-Fi, the speaker, volume and the AirPlay delay are shared, so they can be set from either. |

Everything else works as in [DrumLoop](../drumloop/README.md) and [Keyboard](../keyboard/README.md): eight pads and a 16-step grid (hold a step to double it, hold a track label for its filter, EQ and level), hold Style for a keyboard style's filter, EQ and level, 25 keys that move across seven octaves (OCT - / OCT + and a tappable octave map), a sustain pedal, seven voices in 23 styles, kits from an SD card, and EXPORT (which renders the drums only).

The drum loop runs 1 to 16 bars and the keyboard loop 1 to 8. Both start together at bar 1, so with power-of-two lengths they stay in step.

## Piano Roll

The screen button in the header goes round **KEYS â†’ DRUM â†’ ROLL**: tap **ROLL** on the drums screen to open the Piano Roll (or tap the keys screen's loop row). It works on an empty loop too, so you can draw a part from scratch. It is the keyboard loop drawn as a grid of pitch against time, in the spirit of LMMS's piano roll. Recorded takes show up there, and drawn notes play on the keys' loop in time with the drums.

```
 PLAY REC - 120 + 1/16 KEYS MENU     header: SNAP, on to the KEYS screen, the keys' MENU
 ==========|---------------------     where the speaker is in the loop
 C4 |  ##      ####        |   ##     14 pitches by 2 bars; key column on the left
    |      ##          ##  |
 OC- OC+  1-2/4  [C7 GRAND]  DEL UNDO   octave, bars (tap), style, delete, undo
```

The key column on the left names every key (C4, C#4, D4 â€¦) and plays it while you hold it, in the style picked on the keys screen. Slide along it to play a run. While REC is on, it records like the keys.

With one finger on the grid:
- **Empty spot:** adds a note in the style INST shows (the keys' style), at the snapped step. Drag right before lifting to draw it longer. The next note takes the last length you drew. With notes selected, it clears the selection instead.
- **Tap a note:** selects it (it turns white), or deselects it.
- **Double tap** anywhere on the grid: turns the whole selection off. If the first tap drew a note, that note goes too.
- **Drag a note:** moves it in time and pitch. Drag a selected note to move the whole selection together.
- **Drag a note's right end** (the dark handle): resizes it.
- Notes sound briefly as you place or move them, except while REC is on.

| Control | What it does |
|---|---|
| **SNAP** (`1/16`) | The grid notes snap to: 1/4, 1/8, 1/16, 1/32 or OFF. |
| **KEYS** | On to the keys screen. |
| **MENU** | The keys screen's settings menu: loop length, quantize, clear loop, Wi-Fi, speaker, volume and AirPlay delay. BACK returns to the roll. |
| **OCT- / OCT+** | Moves the 14 pitches shown by an octave, from C1 up to C8. |
| **1-2/4** | The bars shown. With loops longer than 2 bars, tap it for the next two. |
| **DEL** | Deletes the selected notes. |
| **INST** | The style new notes are drawn in: opens a list of every style, patches and samples too, two columns a page. It is the keys' style, so it changes there too. |
| **UNDO** | Removes the last take. Each drawn note is a take of its own, so it removes the last note drawn. Moves and deletes aren't undone. |
| **PLAY / REC / tempo** | As on the other screens. REC records the keys while you watch the notes appear. |

Notes of every style are shown in their voice's color. The loop's length and quantize are in MENU, here or on the keys screen.

## SYNTH

The fourth screen edits the patch synth (`main/psynth.c`), a subtractive and wavetable synthesizer. Its four patches play as the **PATCH** voice on the keys screen (tap the voice button until it says PATCH) and so in the Piano Roll and the loop. Each patch also has a filter, EQ and level tone editor (hold Style), like the other styles.

```
 PLAY REC - 120 + P1 KEYS MENU      P1-P4 picks the patch
 <  OSC 1  >   SUPERSAW    INIT     page, patch name, back to its preset
 SHAPE    [      SAW       ]        a choice: tap for the next
 POS   50 [=====|-----------]       a slider: drag along it
 ...                                up to six settings a page
 OC- [ ~~~ oscilloscope ~~~ ] OC+   the patch's sound; test-key octave
 [C3][D3][E3][F3][G3][A3][B3][C4]   test keys
```

| Page | Settings |
|---|---|
| RACK | **LAYER**: off, a key plays one patch; on, a key plays every patch switched on (P1-P4 buttons), stacked. Each patch has a level slider |
| OSC 1, OSC 2 | Shape (SAW, SQUARE, TRIANGLE, SINE, or the morphing wavetables WT BASIC, WT VOCAL and WT DIGITAL), position (wavetable morph, or the square's width), unison (1-7 voices, random phases), detune (up to 50 cents either side), semitones (Â±24), level |
| SUB+NOISE | Sub oscillator an octave down (sine or square), noise and its color, the patch volume |
| FILTER 1, FILTER 2 | Type (LOWPASS, HIGHPASS, BANDPASS, NOTCH, COMB, FORMANT, or OFF), cutoff (the vowel, for formant), resonance, drive; the first also has key tracking and the routing (SERIES or PARALLEL) |
| AMP ENV, MOD ENV | Attack, decay, sustain, release (1 ms to 8 s) |
| LFOS | Two LFOs: shape (SINE, TRIANGLE, SAW, SQUARE, RANDOM, GATE steps, STAIRS steps), free rate (0.05 to 30 Hz), or synced to the tempo (1/1 to 1/16, 1/8T) |
| MOD 1-2 â€¦ MOD 7-8 | Eight routings: a source (LFO 1, LFO 2, MOD ENV, NOTE, RANDOM per note) onto a destination (PITCH, OSC1 POS, OSC2 POS, DETUNE, CUTOFF 1, CUTOFF 2, RESO 1, OSC2 LVL, AMP) by an amount (Â±100) |
| FX 1, FX 2 | Drive, chorus, delay (tempo synced: 1/16 to 1/2, with feedback and mix), reverb (size and mix), volume |
| SAMPLES | Four sample slots. **SAVE P1** renders the current patch into a slot: middle C held for a second, then released, with its effects rung out (up to 5 s). **PLAY ON KEYS** puts a sample on the test keys. Samples play as PATCH styles (after the patches, named S1-S4 and the patch) on the keys and in the Piano Roll, pitched from middle C |

The presets: **SUPERSAW** (7-voice saw stack with an octave layer, chorus and reverb), **REESE** (detuned saw bass with an LFO on the filter and wavetable), **PLUCK** (digital wavetable with a mod-envelope filter snap, dotted-eighth delay) and **WOBBLE** (saw and square bass, resonant filter swept by an LFO synced to eighths). **INIT** (tap twice) puts a patch back to its preset.

How it runs:
- Sliders move the sound as you drag. Every control glides to its new value over a few milliseconds, so it doesn't step, and the oscilloscope pauses during a drag so the screen keeps up with your finger. What you hear still comes the AirPlay delay later, 500 ms on the Denon speakers.
- In LAYER mode each stacked patch takes a voice, so two layered patches give three-note chords.
- Up to 6 patch notes at once, across the four patches.
- The wavetables are band-limited (four levels of harmonics, chosen by pitch) and built at boot in PSRAM. Saw and square use polyBLEP, so high notes don't alias badly.
- Modulation, envelopes and filter coefficients update every 1.5 ms block. Envelopes ramp the level inside the block.
- The output is mono, like the rest of Drum&Key's AirPlay stream, so unison has no stereo spread.
- Patches and samples aren't saved across power cycles.

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
| `main/drums_ui.c` | The drums screen, made from DrumLoop's `ui.c` by `tools/make_screens.py` |
| `main/keys_ui.c` | The keys screen, made from Keyboard's `ui.c` by `tools/make_screens.py` |
| `main/psynth.c` | The patch synth: oscillators, wavetables, unison, filters, envelopes, LFOs, mod matrix and effects; Keyboard's synth hands it the PATCH notes (`synth_set_external`) |
| `main/synth_ui.c` | The SYNTH screen |
| `main/roll_ui.c` | The Piano Roll, which edits the keyboard loop through looper.h |
| `main/dk_ui.c` | The header layout, the gaps between its buttons, and which screen is next |

The engines are compiled from the other apps' folders, so a fix there reaches Drum&Key too:
- `apps/drumloop/main`: `engine.c`, `kit.c`, `export.c`, `sdcard.c` and `wav.c`
- `apps/keyboard/main`: `synth.c` and `looper.c`

`uses-apps.txt` names those two apps, so CI rebuilds Drum&Key when either changes.

The two screens are made from the apps' `ui.c` by `tools/make_screens.py`. It applies Drum&Key's changes: the shared header with the screen button, the shared transport and tempo, and a screen that returns when the switch is tapped. After a change to `apps/drumloop/main/ui.c` or `apps/keyboard/main/ui.c`, run it from the repository root and commit the result:

```bash
python apps/drumkey/tools/make_screens.py
```

CI runs it with `--check` and fails when the screens are stale. If one of its changes no longer finds its text in the new `ui.c`, the script names that change; update it to suit.

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

### 1.4 (2026-11-15)

- SYNTH: controls glide as you drag them, and the oscilloscope pauses during a drag, so sliders move the sound smoothly. New RACK page: LAYER plays every patch switched on at once, with a level for each. New SAMPLES page: render a patch into one of four samples.
- Samples play as PATCH styles. The Piano Roll's new INST button picks the style new notes are drawn in, samples included.

### 1.3 (2026-11-15)

- SYNTH screen: a patch synth with two wavetable or analog oscillators (7-voice unison), sub and noise, two filters (LP, HP, BP, notch, comb, formant), two envelopes, two LFOs, an 8-slot mod matrix and drive, chorus, delay and reverb. Its four patches play as the keys' PATCH voice. An oscilloscope shows the sound.

### 1.2 (2026-11-15)

- Piano Roll: the screen button goes KEYS â†’ DRUM â†’ ROLL; the roll edits the keyboard loop as a grid, even an empty one. Draw, move, resize and delete notes with a snap, while the loop plays.

### 1.1 (2026-11-14)

- Tone editors from DrumLoop and Keyboard: hold a drum track's label, or Style on the keys screen, for a resonant filter (low, high or band pass), a three-band EQ and a level, adjustable while the loop plays.

### 1.0 (2026-11-08)

- First version: DrumLoop's drums and Keyboard's keys in one app with a screen switch, one shared transport and tempo, and both mixed into one AirPlay stream. The keys move across seven octaves, C1 to C8, as in Keyboard.
