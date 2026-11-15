# ODIN S3

A port of the [Odin 2](https://github.com/TheWaveWarden/odin2) synthesizer's sound engine to the Waveshare ESP32-S3-Touch-LCD-7 (800x480, capacitive touch), with 83 of Odin's factory presets. Odin 2 is a free desktop synthesizer plugin by TheWaveWarden. This app runs Odin's oscillators, filters, envelopes, LFOs, mod matrix, distortion and effects on the board, and plays them from a two-octave touch keyboard with pitch-bend and mod strips and Odin's XY pad. Sound goes out through the shared [music_out](../../components/music_out) component to an AirPlay speaker, or to the wired PCM5102A DAC once music_out has it.

The DSP is Odin's own code, ported from C++ and JUCE to C. Odin's interface isn't ported: its artwork is replaced by a touch screen of its own.

This app lives in `apps/odin_s3`. The [root README](../../README.md) lists every app in this repository. It targets the S3 board only (`supported-targets.txt`).

## License

Odin 2 is free software under the **GNU General Public License, version 3 or later**, Copyright (C) 2020-2021 TheWaveWarden. This app is a derivative of it, so it's under the same license: see [LICENSE](LICENSE). Its image includes this repository's shared components (`cyd_display`, `music_out`, `airplay_sender`), and their source is offered under the same GPL-3.0 terms as part of this app. The source of every image CI publishes is this folder and those components, at the commit the release names.

The wavetables are Odin's, most of them made from the Adventure Kid waveforms (AKWF), which are public domain. The presets are Odin's factory presets, and each one's author is named in brackets after its name.

## Starting up

ODIN S3 boots to the keys with the last patch you picked (the first bass the first time), Wi-Fi off and no speaker. To hear it, open **MENU**:

1. **WI-FI** turns the radio on and joins the network saved by the [launcher](../../launcher). No credentials are compiled in.
2. **OUTPUT** lists the AirPlay speakers (and the wired DAC, where music_out has it). Tap one.

## The screen

```
 <    ANALOG BASS [TX]    >   MENU     patch: step, or tap the name to browse
 BASS            OCT-  C3  OCT+        category (browse), keyboard octave
 2/8 VOICES  31% CPU  LEGATO x3        voices, render load, play mode, unison
 BEND  MOD   [  X / Y  ]  OSC1 ANALOG SAW
 |  |  |  |  [    +    ]  FIL DIODE - -  what the patch uses
 |C#| |D#|  |F#| |G#| |A#|  ...         25 keys, two octaves
```

| Control | What it does |
|---|---|
| **< / >** | The previous or next preset, through every category. The patch you pick is remembered. |
| **Patch name**, **category** | The preset browser: a category's presets, ten to a page. **<** and **>** in its top row step through the categories; tap a preset to play it. |
| **OCT- / OCT+** | Move the keys an octave, C1 to C6 at the left. |
| **BEND** | Pitch bend, springing back to the middle when you let go. The range is the patch's (usually 12 semitones). |
| **MOD** | The mod wheel. It stays where you leave it. Many presets use it in their mod matrix. |
| **X / Y** | Odin's XY pad, a mod matrix source. It stays where you leave it. |
| **Keys** | Play with up to five fingers. How low on the key you press sets the velocity: near the top is soft, near the bottom is loud. Slide along the keys for a glissando. |
| **MENU** | Wi-Fi, OUTPUT, volume, the tempo (for synced LFOs and effects; shared with the other music apps), the AirPlay delay. |

## What runs, and what's left out

| Odin 2 | ODIN S3 |
|---|---|
| 24 voices | **8 voices** on core 1 |
| Unison up to 12 | Unison up to **4** (presets with 6 play 4) |
| Oscillators: analog, wavetable, multi, vector, chiptune, FM, PM, noise, wavedraw, chipdraw, specdraw | **Analog** (saw, PWM square, triangle, sine, drift), **wavetable** (Odin's 35 2D wavetables, position and its envelope or LFO modulation), **noise** (with its LP and HP) |
| Filters: ladder LP, BP and HP 24/12, SEM 12, Korg 35 LP and HP, diode, comb, formant, ring mod | **Ladder** LP, BP and HP 24/12, **SEM 12**, **Korg 35** LP and HP, **diode**, each with saturation, env, velocity and key tracking; filter 3 after the voices |
| Envelopes: amp, filter, mod, global, with loop | All four |
| LFOs: 3 per voice and a global one, 16 shapes and sample and hold, synced or free | All four |
| Mod matrix: 9 rows, 2 destinations and a scale each | All 9 rows. Sources: the envelopes, LFOs, X, Y, mod wheel, pitch bend, key, velocity, unison index, random and constant. Destinations: pitch, volume, pulse width, wavetable position, noise filters, filter frequency, resonance, gain, env, velocity, key, saturation and SEM transition, envelope times, LFO rates, amp, distortion, glide, the effects and master. |
| Distortion: clamp, fold, zero, sine, cube, 3x oversampled | All five, oversampled as in Odin |
| Effects: delay, phaser, flanger, chorus, reverb, in any order | **Delay, phaser, flanger and chorus** in the patch's order |
| Stereo | **Mono**: the left channel of Odin's effects; pan and unison width are left out |
| Glide, legato, retrig | All three |
| Arpeggiator and step sequencer | Not yet |

Presets that use an oscillator or filter the port doesn't have yet, or the arpeggiator, are left out: 83 of Odin's 260 factory presets play.

### How the port differs inside

- **Float, not double.** The ESP32-S3's FPU is single-precision. Odin's double-precision phases are a 32-bit phase here, and its filters run in float.
- **Control rate.** Odin works out pitch, filter coefficients and the mod matrix every sample. Here the mod matrix, the filter and mod envelopes, the LFOs, the oscillators' pitch and the filter coefficients are worked out every 16 samples (0.36 ms), and Odin's per-sample smoothing is applied per step. The amp envelope runs every sample.
- **Wavetables.** Odin keeps 33 band-limited copies of each 512-sample wavetable, one per minor third, in float: 10.8 MB. `tools/odin_convert.py` keeps one copy per tritone, cuts each to the shortest power-of-two length that holds its harmonics, and stores int16: 1.2 MB for the 141 tables the port uses, in `main/wavetables.bin`. A note picks the copy whose harmonics stay below Nyquist, as in Odin.
- **Hard sync** restarts oscillators 2 and 3 when oscillator 1 wraps, without Odin's 3x oversampling.
- **Mod sources** that are audio (an oscillator or filter output) or a MIDI controller the board doesn't have (breath, pressure, pedals, arpeggiator) give 0.

## Load

The audio task renders 352 samples (8 ms) at a time. Row 2 shows the average render time of the last second as a share of that 8 ms; it turns red when the worst block passes 90%. The serial log has `Render: average N us, worst N us of 7981 us per block, N voices` every 10 s while the worst block is over a quarter. Heavy patches (three oscillators, two diode filters, distortion and unison) cost the most per voice.

## Files

| File | What it is |
|---|---|
| `main/engine.c` | Odin's voices, oscillators, envelopes, LFOs, mod matrix, voice allocation and signal flow |
| `main/filters.c` | Odin's ladder, SEM 12, Korg 35 and diode filters |
| `main/fx.c` | Odin's distortion, delay, chorus, phaser and flanger |
| `main/patch.h` | An Odin patch, in Odin's units |
| `main/presets.c` | The factory presets (generated) |
| `main/wavetables.bin`, `main/wt_index.h` | The wavetables (generated) |
| `main/audio.c` | The engine in music_out's audio task, with a command queue from the UI |
| `main/ui.c` | The play screen, preset browser and menu |
| `tools/odin_convert.py` | Makes the generated files from an Odin 2 checkout |

To regenerate the wavetables and presets (needs Python 3 and numpy):

```bash
git clone --depth 1 https://github.com/TheWaveWarden/odin2 ../odin2
cd apps/odin_s3
python tools/odin_convert.py ../../../odin2
```

## Building

```bash
cd apps/odin_s3
pio run -e esp32s3
```

The image goes in a launcher slot, like the other apps.

## Changelog

### 1.0 (2026-11-15)

- First version: Odin 2's analog, wavetable and noise oscillators, ladder, SEM, Korg 35 and diode filters, four envelopes, four LFOs, the 9-row mod matrix, distortion, delay, phaser, flanger and chorus, 8 voices with unison, glide and legato, and 83 factory presets. A two-octave keyboard with velocity by touch height, BEND and MOD strips, the XY pad, a preset browser and music_out's outputs.
