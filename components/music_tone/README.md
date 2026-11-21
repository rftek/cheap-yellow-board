# music_tone

A tone for one instrument of the music apps: a resonant filter, a three-band EQ and a level. It also has the full-screen editor that sets them. [DrumLoop](../../apps/drumloop) has one tone per drum track and [Keyboard](../../apps/keyboard) one per style. [Drum&Key](../../apps/drumkey) has both.

## The tone (`tone.h`)

| Setting | Range |
|---|---|
| `filter` | OFF, low pass, high pass, band pass (RBJ biquads) |
| `cutoff` | 0-100: 30 Hz to 18 kHz, evenly by pitch |
| `resonance` | 0-100: Q 0.5 to 10 |
| `low`, `mid`, `high` | -12 to +12 dB: low shelf at 100 Hz, peak at 1 kHz, high shelf at 6 kHz |
| `level` | -24 to +6 dB |
| `pitch` | -12 to +12 semitones. The app retunes its sound with it (DrumLoop resamples each hit); the filters ignore it |
| `decay` | 0 for the whole sound, up to 95: the app cuts the sound to (100 - decay)% (DrumLoop fades the hit out over the last half of that) |

Editors show `pitch` and `decay` only with `tone_editor_t.drum`.

An all-zero `tone_settings_t` is flat (`tone_is_flat`); `tone_sound_flat` ignores pitch and decay, for deciding whether to run the filters. The app mixes an instrument with a tone into its own buffer and runs it through `tone_process` in the audio task. It calls `tone_update` first, which recomputes the coefficients only when the settings changed. Filter state carries across a change, so a sweep while a sound rings doesn't click. While nothing plays, `tone_ringing` says whether the filters still need silence run through them.

## The editor (`tone_editor.h`)

`tone_editor_run` takes over rows 0-8 of the 800x480 S3 screen until BACK:
- **Header:** the item (`<` and `>` page through them).
- **Filter row:** OFF, LP, HP, BP.
- **Six sliders:** cutoff, resonance, low, mid, high and level. Each follows a finger dragged along it, polled with `cyd_touch_held`.
- **FLAT:** resets the item.
- **PITCH and DECAY**, when the app sets `drum` in `tone_editor_t`: two half-width sliders on row 8 (PIT and DEC), with FLAT moved into the header beside a shorter title.

Rows 9-11 belong to the app, through callbacks: DrumLoop draws its pads there, and Keyboard eight test keys. A `tick` callback runs about every 10 ms, so the app keeps its playhead and pads going.
