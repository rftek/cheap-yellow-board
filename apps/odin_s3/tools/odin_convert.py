"""Makes ODIN S3's data from an Odin 2 source checkout.

Odin 2 (https://github.com/TheWaveWarden/odin2) is GPL-3.0, Copyright (C)
2020-2021 TheWaveWarden. This tool reads its wavetables and factory
presets and writes them in the forms the ESP32-S3 port uses:

  main/wavetables.bin  int16 wavetables, embedded in the image
  main/wt_index.h      where each table is, and Odin's table layouts
  main/presets.c       the factory presets the port can play

Odin keeps 33 band-limited copies of each 512-sample table, one per minor
third from A0, as floats: 10.8 MB, more than an app slot holds. This tool
keeps every second copy (one per tritone), cuts each copy to the shortest
power-of-two length that still holds its harmonics, and stores int16. That
is 7.7 KB per table.

Run it from the app's folder with numpy installed, then commit the output:

    git clone --depth 1 https://github.com/TheWaveWarden/odin2 <odin2>
    python tools/odin_convert.py <odin2>
"""

import glob
import os
import re
import struct
import sys

import numpy as np

RATE = 44100.0
ODIN_SUBTABLES = 33
ODIN_LENGTH = 512
SUBTABLES = 17          # Odin's subtables 0, 2, ..., 32
MIN_LENGTH = 32
SCALE = 32000.0

OSC_TYPES = {1, 2, 3, 15}                       # off, analog, wavetable, noise
FILTER_TYPES = {1, 2, 3, 4, 5, 6, 7, 15, 19, 24, 25}
CATEGORIES = ["Arps & Sequences", "Atmospheres", "Bass", "Chimes", "Chiptune", "Drones",
              "Drums", "Instruments", "Keys", "Leads", "Organ", "Pads", "SFX", "Stabs"]


# ---- JUCE ValueTree (binary .odin files) -----------------------------------

def read_compressed_int(data, i):
    count = data[i]
    i += 1
    negative = count & 0x80
    count &= 0x7F
    value = int.from_bytes(data[i:i + count], "little")
    return (-value if negative else value), i + count


def read_string(data, i):
    end = data.index(0, i)
    return data[i:end].decode("utf8", "replace"), end + 1


def read_var(data, i):
    size, i = read_compressed_int(data, i)
    kind = data[i]
    body = data[i + 1:i + size]
    i += size
    if kind == 1:
        return struct.unpack("<i", body)[0], i
    if kind == 2:
        return 1, i
    if kind == 3:
        return 0, i
    if kind == 4:
        return struct.unpack("<d", body)[0], i
    if kind == 5:
        return body.rstrip(b"\0").decode("utf8", "replace"), i
    if kind == 6:
        return struct.unpack("<q", body)[0], i
    return None, i


def read_tree(data, i=0):
    name, i = read_string(data, i)
    count, i = read_compressed_int(data, i)
    properties = {}
    for _ in range(count):
        key, i = read_string(data, i)
        properties[key], i = read_var(data, i)
    count, i = read_compressed_int(data, i)
    children = []
    for _ in range(count):
        child, i = read_tree(data, i)
        children.append(child)
    return (name, properties, children), i


def number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return value


def load_preset(path):
    tree, _ = read_tree(open(path, "rb").read())
    params = {}
    for name, properties, _ in tree[2]:
        if name == "PARAM":
            params[properties["id"]] = number(properties.get("value"))
        else:
            params.update({k: number(v) for k, v in properties.items()})
    return params


# ---- Wavetables --------------------------------------------------------------

def table_names(source):
    """Odin's 1D wavetable names by index, and its LFO table names."""
    names, lfo_names = {}, {}
    # Only the headers WavetableCoefficients.h includes count, in its order:
    # some others are stale and reuse an index.
    include_list = open(os.path.join(source, "Oscillators", "WavetableCoefficients.h"),
                        encoding="utf8", errors="replace").read()
    for header in re.findall(r'^\s*#include "(Wavetables/Coefficients/[^"]+)"', include_list, re.M):
        path = os.path.join(source, "Oscillators", *header.split("/"))
        if not os.path.isfile(path):
            continue
        text = open(path, encoding="utf8", errors="replace").read()
        index = re.search(r"#define WT_NR (\d+)", text)
        name = re.search(r'm_wavetable_names_1D\[WT_NR\] = "([^"]+)"', text)
        lfo = re.search(r'm_LFO_names\[WT_NR\] = "([^"]+)"', text)
        if index and name:
            names[int(index.group(1))] = name.group(1)
        if index and lfo:
            lfo_names[int(index.group(1))] = lfo.group(1)
    return names, lfo_names


def float_array(path, count):
    text = open(path, encoding="utf8", errors="replace").read()
    text = text[text.index("= {"):]
    text = text[:text.index("};")]
    values = np.array(re.findall(r"-?\d+\.?\d*(?:[eE][-+]?\d+)?", text), dtype=np.float64)
    assert len(values) == count, f"{path}: {len(values)} values, expected {count}"
    return values


def wavetable_2d_sets(source):
    """Odin's 40 2D wavetables, four 1D table names each."""
    text = open(os.path.join(source, "Oscillators", "WavetableOsc2D.cpp"), encoding="utf8").read()
    sets = {}
    for match in re.finditer(r'setWavetablePointer\((\d+),\s*(\d+),\s*m_WT_container->'
                             r'getWavetablePointers\("([^"]+)"\)\)', text):
        sets.setdefault(int(match.group(1)), {})[int(match.group(2))] = match.group(3)
    return [[sets[s][t] for t in range(4)] for s in sorted(sets)]


def subtable_layout():
    """The seed frequency, harmonic count and length of each kept subtable."""
    layout = []
    for k in range(SUBTABLES):
        seed = 27.5 * 2 ** (k / 2)
        harmonics = min(int(RATE * 0.5 / seed - 1), 256)
        length = MIN_LENGTH
        while length < 2 * harmonics + 2 and length < ODIN_LENGTH:
            length *= 2
        layout.append((seed, harmonics, length))
    return layout


def shrink(table, harmonics, length):
    """Keeps harmonics 1 to harmonics-1 of a 512-sample cycle (as Odin's
    generator does at 44.1 kHz) and resamples it to length."""
    spectrum = np.fft.rfft(table)
    spectrum[0] = 0
    spectrum[harmonics:] = 0
    return np.fft.irfft(spectrum[:length // 2 + 1], n=length) * (length / ODIN_LENGTH)


def write_wavetables(source, out_dir):
    names, lfo_names = table_names(source)
    data = float_array(os.path.join(source, "Oscillators", "Wavetables", "Tables", "WavetableData.cpp"),
                       160 * ODIN_SUBTABLES * ODIN_LENGTH).reshape(160, ODIN_SUBTABLES, ODIN_LENGTH)
    lfo_data = float_array(os.path.join(source, "Oscillators", "Wavetables", "Tables", "LFOTableData.cpp"),
                           13 * ODIN_LENGTH).reshape(13, ODIN_LENGTH)
    by_name = {name: index for index, name in names.items()}
    sets = wavetable_2d_sets(source)

    # The tables the port uses: the analog oscillator's, then the 2D sets'.
    wanted = ["FatSaw", "Triangle", "Sine"]
    for four in sets:
        wanted += [n for n in four if n not in wanted]
    layout = subtable_layout()
    offsets, total = [], 0
    for _, _, length in layout:
        offsets.append(total)
        total += length

    blob = bytearray()
    for name in wanted:
        source_table = data[by_name[name]]
        peak = 0.0
        shrunk = []
        for k, (_, harmonics, length) in enumerate(layout):
            cycle = shrink(source_table[min(2 * k, ODIN_SUBTABLES - 1)], harmonics, length)
            shrunk.append(cycle)
            peak = max(peak, float(np.max(np.abs(cycle))))
        for cycle in shrunk:
            samples = np.clip(np.round(cycle / peak * SCALE), -32767, 32767).astype("<i2")
            blob += samples.tobytes()

    # LFO shapes, one 512-sample cycle each, in the order of Odin's LFO menu.
    lfo_order = ["Sine", "Saw", "Triangle", "Square50", "Square25", "Square12", "Spike",
                 "Pyramid4", "Pyramid6", "Pyramid8", "Pyramid12", "Stair3", "Stair4", "Stair6",
                 "Stair8", "Stair12"]
    lfo_by_name = {name: index for index, name in lfo_names.items()}
    lfo_offset = len(blob) // 2
    for name in lfo_order:
        if name in lfo_by_name:
            cycle = lfo_data[lfo_by_name[name]]
        else:
            cycle = data[by_name[name]][0]
        peak = float(np.max(np.abs(cycle))) or 1.0
        blob += np.clip(np.round(cycle / peak * SCALE), -32767, 32767).astype("<i2").tobytes()

    open(os.path.join(out_dir, "wavetables.bin"), "wb").write(blob)

    index_of = {name: i for i, name in enumerate(wanted)}
    lines = [
        "#pragma once",
        "",
        "/* Generated by tools/odin_convert.py from Odin 2's wavetables",
        "   (GPL-3.0, Copyright (C) 2020-2021 TheWaveWarden). Do not edit. */",
        "",
        "#include <stdint.h>",
        "",
        f"#define WT_TABLES {len(wanted)}",
        f"#define WT_SUBTABLES {SUBTABLES}",
        f"#define WT_TABLE_SAMPLES {total} /* int16 samples per table */",
        f"#define WT_LFO_OFFSET {lfo_offset} /* the LFO shapes, 512 samples each */",
        f"#define WT_LFO_SHAPES {len(lfo_order)}",
        "#define WT_LFO_LENGTH 512",
        "",
        "#define WT_FATSAW 0",
        "#define WT_TRIANGLE 1",
        "#define WT_SINE 2",
        "",
        "/* Each subtable k holds the harmonics a note below 27.5 Hz * 2^(k/2)",
        "   can play at 44.1 kHz, in 2^bits samples starting at offset. */",
        "static const uint16_t wt_sub_offset[WT_SUBTABLES] = {"
        + ", ".join(str(o) for o in offsets) + "};",
        "static const uint8_t wt_sub_bits[WT_SUBTABLES] = {"
        + ", ".join(str(int(np.log2(length))) for _, _, length in layout) + "};",
        "",
        f"#define WT_2D_SETS {len(sets)}",
        "/* Odin's 2D wavetables: four tables each, morphed by position. */",
        "static const uint8_t wt_2d_sets[WT_2D_SETS][4] = {",
    ]
    for four in sets:
        lines.append("    {" + ", ".join(str(index_of[n]) for n in four) + "}, /* "
                     + ", ".join(four) + " */")
    lines += ["};", ""]
    open(os.path.join(out_dir, "wt_index.h"), "w", newline="\n").write("\n".join(lines))
    print(f"{len(wanted)} wavetables, {len(blob)} bytes")


# ---- Presets -----------------------------------------------------------------

def supported(p):
    if any(int(p.get(f"osc{i}_type", 1)) not in OSC_TYPES for i in (1, 2, 3)):
        return False
    if any(int(p.get(f"fil{i}_type", 1)) not in FILTER_TYPES for i in (1, 2, 3)):
        return False
    return p.get("arp_on", 0) < 0.5


def c_float(value):
    text = f"{float(value):.6g}"
    if "e" not in text and "." not in text:
        text += ".0"
    return text + "f"


def c_bool(value):
    return "true" if float(value) > 0.5 else "false"


def c_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def preset_c(p, name, category):
    g = lambda key, default=0.0: p.get(key, default) if isinstance(p.get(key, default), float) else default
    out = [f"    {{.name = {c_string(name)}, .category = {category},"]
    out.append("     .osc = {")
    for i in (1, 2, 3):
        o = f"osc{i}_"
        out.append(
            f"         {{.type = {int(g(o + 'type', 1))}, .analog_wave = {int(g(o + 'analog_wave'))},"
            f" .wavetable = {max(int(g(o + 'wavetable', 1)) - 1, 0)},"
            f" .pos_mod_lfo = {c_bool(g(o + 'mod_source') == 10)},"
            f" .oct = {int(g(o + 'oct'))}, .semi = {int(g(o + 'semi'))},"
            f" .reset = {c_bool(g(o + 'reset'))}, .sync = {c_bool(g(o + 'sync'))},"
            f" .fine = {c_float(g(o + 'fine'))}, .vol = {c_float(g(o + 'vol'))},"
            f" .drift = {c_float(g(o + 'drift'))}, .pulsewidth = {c_float(g(o + 'pulsewidth', 0.5))},"
            f" .position = {c_float(g(o + 'position'))}, .pos_mod = {c_float(g(o + 'pos_mod'))},"
            f" .lp = {c_float(g(o + 'lp', 18000))}, .hp = {c_float(g(o + 'hp', 80))}}},")
    out.append("     },")
    out.append("     .fil = {")
    for i in (1, 2, 3):
        f = f"fil{i}_"
        out.append(
            f"         {{.type = {int(g(f + 'type', 1))},"
            f" .osc = {{{c_bool(g(f + 'osc1'))}, {c_bool(g(f + 'osc2'))}, {c_bool(g(f + 'osc3'))}}},"
            f" .to_amp = {c_bool(g(f + 'to_amp', 1 if i < 3 else 0))},"
            f" .freq = {c_float(g(f + 'freq', 10000))}, .res = {c_float(g(f + 'res'))},"
            f" .gain = {c_float(g(f + 'gain'))}, .env = {c_float(g(f + 'env'))},"
            f" .kbd = {c_float(g(f + 'kbd'))}, .vel = {c_float(g(f + 'vel'))},"
            f" .saturation = {c_float(g(f + 'saturation'))},"
            f" .sem_transition = {c_float(g(f + 'sem_transition', -1))}}},")
    out.append("     },")
    out.append(f"     .fil2_fil1 = {c_bool(g('fil2_fil1'))},")
    out.append("     .env = {")
    for i in (1, 2, 3, 4):
        e = f"env{i}_"
        out.append(
            f"         {{{c_float(g(e + 'attack', 0.01))}, {c_float(g(e + 'decay', 1))},"
            f" {c_float(g(e + 'sustain', 0.5))}, {c_float(g(e + 'release', 0.01))},"
            f" {c_bool(g(e + 'loop'))}}},")
    out.append("     },")
    out.append("     .lfo = {")
    for i in (1, 2, 3, 4):
        l = f"lfo{i}_"
        out.append(
            f"         {{.freq = {c_float(g(l + 'freq', 1))}, .wave = {int(g(l + 'wave'))},"
            f" .sync = {c_bool(g(l + 'sync'))}, .reset = {c_bool(g(l + 'reset'))},"
            f" .num = {int(g(l + 'synctime_numerator', 2))},"
            f" .den = {int(g(l + 'synctime_denominator', 5))}}},")
    out.append("     },")
    out.append("     .mod = {")
    for r in range(9):
        out.append(
            f"         {{{int(g(f'source_row_{r}'))}, {int(g(f'dest_1_row_{r}'))},"
            f" {int(g(f'dest_2_row_{r}'))}, {int(g(f'scale_row_{r}'))},"
            f" {c_float(g(f'amount_0_row_{r}'))}, {c_float(g(f'amount_1_row_{r}'))},"
            f" {c_float(g(f'amount_2_row_{r}'))}}},")
    out.append("     },")
    out.append(f"     .amp_gain = {c_float(g('amp_gain'))}, .amp_vel = {c_float(g('amp_velocity'))},")
    out.append(f"     .dist_on = {c_bool(g('dist_on'))}, .dist_algo = {int(g('dist_algo', 1))},"
               f" .dist_boost = {c_float(g('dist_boost'))}, .dist_drywet = {c_float(g('dist_drywet'))},")
    out.append(f"     .glide = {c_float(g('glide'))}, .master = {c_float(g('master'))},"
               f" .play_mode = {int(g('legato', 1))}, .unison = {int(g('unison_voices', 1))},"
               f" .unison_detune = {c_float(g('unison_detune'))},"
               f" .pitchbend_range = {int(g('pitchbend_amount', 12))},")
    out.append(f"     .modwheel = {c_float(g('modwheel'))}, .xy_x = {c_float(g('xy_x'))},"
               f" .xy_y = {c_float(g('xy_y'))},")
    out.append(f"     .delay = {{.on = {c_bool(g('delay_on'))}, .sync = {c_bool(g('delay_sync'))},"
               f" .num = {int(g('delay_synctime_numerator', 2))}, .den = {int(g('delay_synctime_denominator', 5))},"
               f" .time = {c_float(g('delay_time', 1))}, .feedback = {c_float(g('delay_feedback', 0.45))},"
               f" .hp = {c_float(g('delay_hp', 80))}, .ducking = {c_float(g('delay_ducking'))},"
               f" .dry = {c_float(g('delay_dry', 1))}, .wet = {c_float(g('delay_wet', 0.7))}}},")
    out.append(f"     .chorus = {{.on = {c_bool(g('chorus_on'))}, .sync = {c_bool(g('chorus_sync'))},"
               f" .num = {int(g('chorus_synctime_numerator', 2))}, .den = {int(g('chorus_synctime_denominator', 5))},"
               f" .rate = {c_float(g('chorus_rate', 0.15))}, .amount = {c_float(g('chorus_amount', 0.4))},"
               f" .feedback = {c_float(g('chorus_feedback'))}, .drywet = {c_float(g('chorus_drywet', 0.5))}}},")
    out.append(f"     .phaser = {{.on = {c_bool(g('phaser_on'))}, .sync = {c_bool(g('phaser_sync'))},"
               f" .num = {int(g('phaser_synctime_numerator', 2))}, .den = {int(g('phaser_synctime_denominator', 5))},"
               f" .rate = {c_float(g('phaser_rate', 0.25))}, .amount = {c_float(g('phaser_mod', 0.3))},"
               f" .freq = {c_float(g('phaser_freq', 4000))}, .feedback = {c_float(g('phaser_feedback', 0.25))},"
               f" .drywet = {c_float(g('phaser_drywet', 1))}}},")
    out.append(f"     .flanger = {{.on = {c_bool(g('flanger_on'))}, .sync = {c_bool(g('flanger_sync'))},"
               f" .num = {int(g('flanger_synctime_numerator', 2))}, .den = {int(g('flanger_synctime_denominator', 5))},"
               f" .rate = {c_float(g('flanger_rate', 0.15))}, .amount = {c_float(g('flanger_amount', 0.3))},"
               f" .feedback = {c_float(g('flanger_feedback', 0.6))}, .drywet = {c_float(g('flanger_drywet', 1))}}},")
    out.append(f"     .fx_order = {{{int(g('delay_position'))}, {int(g('phaser_position', 1))},"
               f" {int(g('flanger_position', 3))}, {int(g('chorus_position', 2))}}}}},")
    return "\n".join(out)


def write_presets(odin_root, out_dir):
    entries = []
    skipped = 0
    for category, folder in enumerate(CATEGORIES):
        for path in sorted(glob.glob(os.path.join(odin_root, "assets", "Soundbanks",
                                                  "Factory Presets", folder, "*.odin"))):
            p = load_preset(path)
            if not supported(p):
                skipped += 1
                continue
            name = os.path.splitext(os.path.basename(path))[0]
            entries.append(preset_c(p, name, category))
    init = load_preset(os.path.join(odin_root, "assets", "init_patch.odin"))
    lines = [
        "/* Generated by tools/odin_convert.py from Odin 2's factory presets",
        "   (GPL-3.0, Copyright (C) 2020-2021 TheWaveWarden and the presets'",
        "   authors, named in brackets). Do not edit. Presets that use an",
        "   oscillator or filter the port doesn't have yet, or the arpeggiator,",
        f"   are left out: {skipped} of {skipped + len(entries)}. */",
        "",
        '#include "patch.h"',
        "",
        "const char *const odin_categories[ODIN_CATEGORIES] = {"
        + ", ".join(c_string(c) for c in CATEGORIES) + "};",
        "",
        "const odin_patch_t odin_init_patch =",
        preset_c(init, "Init", 0)[4:-1] + ";",
        "",
        f"const odin_patch_t odin_presets[{len(entries)}] = {{",
        *entries,
        "};",
        "",
        f"const int odin_preset_count = {len(entries)};",
        "",
    ]
    open(os.path.join(out_dir, "presets.c"), "w", encoding="utf8", newline="\n").write("\n".join(lines))
    print(f"{len(entries)} presets, {skipped} left out")


def main():
    odin_root = sys.argv[1]
    source = os.path.join(odin_root, "Source", "audio")
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main")
    write_wavetables(source, out_dir)
    write_presets(odin_root, out_dir)


if __name__ == "__main__":
    main()
