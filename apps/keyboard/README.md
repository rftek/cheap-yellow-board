# Keyboard

A two-octave touch keyboard for the Waveshare ESP32-S3-Touch-LCD-7 (800x480, capacitive touch). It has 25 keys from C to C, each labelled with its note, seven synthesized voices in 23 styles (concert grand, upright, electric pianos, church and jazz organs and more), eight notes at once, a sustain pedal and a loop recorder with overdub and undo. The sound plays on an AirPlay speaker through the shared [music_out](../../components/music_out) component, the same output [DrumLoop](../drumloop) uses. You can also play with no speaker while you work out a part.

It shares its tempo and its AirPlay delay with DrumLoop. If you set 140 BPM in one app, the other starts at 140, so a keyboard loop and a drum loop made one after the other fit together.

This app lives in `apps/keyboard`. The [root README](../../README.md) lists every app in this repository. It targets the S3 board only (`supported-targets.txt`).

## Starting up

Keyboard boots straight to the keys with Wi-Fi off and no speaker. To hear it, open **MENU**:

1. **WI-FI** turns the radio on and joins the network saved by the [launcher](../../launcher) (`wifi_ssid` and `wifi_pass` in NVS namespace `cyd_config`). No credentials are compiled in.
2. **OUTPUT** searches for AirPlay speakers for 3 seconds and lists them. Tap one to connect. **NO SPEAKER** goes silent; **BACK** leaves the output as it was.

If the speaker drops the session, the keyboard goes silent and the MENU button turns red until you open the menu.

## The screen

```
 PLAY  REC   -  120 +   MET   MENU     transport
 PIANO   C7 GRAND      UNDO            voice and style
 OCT -   C3-C5   OCT +       SUS       octave, sustain
 BAR 1.3 OF 2  37 NOTES                the loop, as heard
 ============------------------------  loop progress
 |C#| |D#|  |F#| |G#| |A#|  ...        25 keys, C3 to C5,
 | C3 | D3 | E3 | F3 | G3 | ...        each with its note name
```

| Control | What it does |
|---|---|
| **PLAY / STOP** | Starts the loop from bar 1, or stops it. STOP also flushes the speaker's buffer so it goes quiet at once. |
| **REC** | From stop: one bar of count-in clicks (orange), then the loop runs and records what you play. While playing: punches in and out. Each punch-in is a new take, layered over the others. |
| **- / +** | Tempo, 60 to 200 BPM, one at a time (tens in MENU). The loop follows a tempo change, since notes are kept in beats. |
| **MET** | Metronome: a click on every beat while the loop runs, accented on beat 1. |
| **Voice** (PIANO) | The next voice: PIANO, ORGAN, STRING, SYNTH, PLUCK, BELL, CHIP. The key you hold keeps its sound; new notes use the new one. |
| **Style** (C7 GRAND) | The next style of that voice. The loop row shows what it is for a moment. Each voice remembers its style. Recorded notes keep the style they were played with, so takes on different pianos layer. |
| **OCT - / OCT +** | Shift the keys an octave down or up: the lowest key from C2 to C5. The range shows between them. |
| **UNDO** | Removes the last take. |
| **SUS** | The sustain pedal, latched: notes ring on after you lift until you tap SUS again. Recorded notes keep the length you heard. |
| **Keys** | Play with up to five fingers at once (the GT911's limit). Slide a finger along the keys for a glissando. A key lights in the voice's color under your finger, and in blue while the loop plays it, at the moment the speaker does. Every key shows its note: white keys with the octave (C4), black keys without (C#), which are too narrow for three letters. Middle C's label is red. |

### The voices and styles

Every sound is computed as it plays, in floating point at 44.1 kHz; there are no samples. The styles are named after classic instruments, the way DrumLoop's kits are named 808 and 909; they are synthesized in their spirit, not copies of them.

| Voice | Style | What it is | How it is made |
|---|---|---|---|
| PIANO | C7 GRAND | Bright concert grand, like Yamaha's C7 | Modal synthesis: up to 12 stiff-string partials (slightly sharp, as real piano strings are), struck an eighth of the way along. Each partial is a damped resonator that rings on its own, the higher ones dying sooner. The lower partials have a second string a cent apart, which beats. A short hammer thump starts it, and lifting the key drops the damper. Low notes ring for half a minute. |
| | U1 UPRIGHT | Warm studio upright | The same, darker and shorter, with more thump |
| | CP70 | Electric grand | The same, short and bright, strings 5 cents apart for its chorus, a little pickup drive |
| | DX7 EP | FM electric piano | Two-operator FM, modulator at the pitch, a bright attack that mellows |
| | MK1 EP | Tine electric piano | A tine's fundamental, octave and fast-dying bell partial through a barking pickup, with tremolo |
| | 200A EP | Reed electric piano | Harmonics through a pickup driven hard, with tremolo |
| | HONKYTONK | Saloon upright | The upright with its strings 16 cents apart |
| | TOY | Toy piano | Metal rods: partials at 2.76, 5.4 and 8.9 times the pitch |
| ORGAN | B3 JAZZ | Drawbar organ, 888 | The first three drawbars (16', 5 1/3', 8'), a decaying third-harmonic percussion, key click and vibrato |
| | CHURCH | Pipe organ | Principal ranks (16', 8', 4', 2 2/3', 2', 1 1/3', 1') with a second 8' rank a few cents off, slow to speak, with breathy chiff and a long release into the room |
| | GOSPEL | All drawbars, rotary | Every drawbar out, through a fast rotating-speaker tremolo and vibrato |
| | COMBO | Transistor organ | Bright upper drawbars with a fast vibrato |
| STRING | ENSEMBLE | String section | Two saws 7 cents apart through a two-pole low-pass, slow attack and release |
| | SOLO | Solo violin | One saw, brighter, with vibrato after a quarter second |
| SYNTH | LEAD | Synth lead | Saw and square through a resonant low-pass that sweeps down |
| | BRASS | Synth brass | A saw whose filter swells open, then settles |
| PLUCK | GUITAR | Plucked string | Karplus-Strong: a noise burst in a delay line one period long |
| | HARP | Harp | The same, softer and ringing longer |
| BELL | BELL | Bell | FM with the modulator at 3.5 times the pitch |
| | VIBES | Vibraphone | FM at 4 times, short, with the motor's tremolo |
| | MARIMBA | Marimba | Wooden bars: partials at 3.9 and 9.9 times the pitch, dying fast |
| CHIP | PULSE | 8-bit pulse | A 25% pulse wave |
| | SQUARE | Square wave | A 50% square |

Eight voices play at once. A ninth note takes the quietest released voice, or the oldest one.

### MENU

- **WI-FI**, **OUTPUT**, **VOL - / VOL +**: as above.
- **BPM -10 / BPM +10**.
- **LOOP LENGTH**: 1, 2, 4 or 8 bars of 4/4. Each step up doubles the loop by copying the bars it has. After 8 it goes back to 1 bar, which drops the notes after bar 1.
- **QUANTIZE**: OFF, 1/8 or 1/16. It moves where recorded notes start; their lengths stay.
- **COUNT-IN**: 1 bar before recording from stop, or off.
- **CLEAR LOOP**: tap twice.
- **AIRPLAY DELAY**: 2000, 1000, 500, 250, 150 or 100 ms. It is shared with DrumLoop (NVS namespace `music`) and used from the next connect. Shorter feels more immediate, but some speakers play nothing below their own minimum: a Denon Home 250 is silent at 250 ms and plays at 500 ms.

## Latency and recording

AirPlay buffers audio on the speaker, so a key sounds after the AirPlay delay plus about 15-30 ms of touch scanning and packet timing. For playing live, set the delay as short as your speaker allows.

Recording allows for the delay. A note is recorded where you heard the loop when you pressed the key, not where the board was rendering. So a take lines up with what you played along to, even at 2 s. The keys that light for the loop, the bar and beat, and the progress strip all follow the speaker too.

## Code

| File | Contents |
|---|---|
| `main/main.c` | start-up: NVS, shared tempo, touch, audio, UI task |
| `main/ui.c` | screen and menu. Key labels use `cyd_draw_small_text`, the text font at half size. The touch task plays the keys through `cyd_touch_set_fingers_callback`, which reports every finger with its id, so notes start and stop as fingers land, slide and lift |
| `main/audio.c` | the command queue from the touch and UI tasks, and the render callback that `music_out` calls every 8 ms |
| `main/looper.c` | transport, loop notes in ticks (960 a beat), takes, quantize, count-in and metronome |
| `main/synth.c` | the eight voices; the instruments as voices and styles over seven engines (modal, FM, organ, strings, synth, pluck, chip); the click and the soft-clipping mixer |

## Limits

- The loop is not saved across power cycles.
- A loop holds up to 1024 notes.
- The S3 has no Classic Bluetooth, so there is no A2DP output. A wired I2S DAC would bring a key to about 10 ms and could be added to `music_out` as another output.

## Build and flash

```powershell
cd apps\keyboard
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e esp32s3
```

The first build downloads `espressif/mdns` through the ESP-IDF component manager. The image is `%USERPROFILE%\.platformio\cyd-keyboard-build\esp32s3\firmware.bin`.

- **As a launcher image**: when a change to this folder is merged into `main`, CI publishes `firmware-esp32s3.bin` on the release tagged `keyboard`, and the launcher lists it.
- **Over USB into a launcher slot**: see the [launcher README](../../launcher/README.md). The board is on COM13. Set the UART DIP switch first.
