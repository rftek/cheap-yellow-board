"""Generates main/cota_track.h from the Circuit of the Americas centerline.

The centerline is us-2012.geojson from https://github.com/bacinger/f1-circuits
(MIT license, Copyright (c) 2019-2025 Tomislav Bacinger). The script smooths
it, samples it once per game road unit, puts the grid far enough behind the
start line that the over-the-horizon view ends just before Turn 1, sizes the
unit so an ideal race from the grid takes RACE_SECONDS under the game's speed
rules, and fits the map to the screen.

    python tools/cota_track.py tools/us-2012.geojson main/cota_track.h [preview.png]
"""
import json
import math
import struct
import sys
import zlib

# Keep these in step with main/main.c.
TOP_SPEED = 55.0
ACCELERATION = 7.0
CORNER_BRAKING = 30.0
DRIFT_MAX = 1.0
CENTRIFUGAL = 0.34
GRIP = CENTRIFUGAL / TOP_SPEED  # drift per unit of curvature and speed squared
RACE_SECONDS = 30.0  # from the grid to the flag, driving ideally
HORIZON_SECONDS = 5.0  # the first part, seen over the horizon
STRAIGHT_CURVATURE = 0.003  # the main straight, where the grid goes
GRID_MARGIN = 3  # units after Turn 20's exit

MAP_W, MAP_H, MAP_TOP = 320, 224, 16  # the map sits under a 16-pixel bar
MARGIN = 10
ROAD_PX = 3  # half width of the drawn road


def load(path):
    data = json.load(open(path, encoding="utf-8"))
    coords = data["features"][0]["geometry"]["coordinates"]
    lat0 = sum(c[1] for c in coords) / len(coords)
    lon0 = sum(c[0] for c in coords) / len(coords)
    # Meters, x east and y south, like the screen.
    pts = [((lon - lon0) * 111320 * math.cos(math.radians(lat0)), -(lat - lat0) * 110540)
           for lon, lat in coords]
    if math.dist(pts[0], pts[-1]) < 1:
        pts.pop()
    return pts


def resample(pts, count):
    """count evenly spaced points around the closed polyline."""
    ring = pts + [pts[0]]
    seg = [math.dist(ring[i], ring[i + 1]) for i in range(len(pts))]
    total = sum(seg)
    out = []
    i, start = 0, 0.0
    for k in range(count):
        d = k * total / count
        while start + seg[i] < d:
            start += seg[i]
            i += 1
        t = (d - start) / seg[i]
        out.append((ring[i][0] + (ring[i + 1][0] - ring[i][0]) * t,
                    ring[i][1] + (ring[i + 1][1] - ring[i][1]) * t))
    return out, total


def smooth(values, sigma):
    n = len(values)
    radius = int(sigma * 3) + 1
    weights = [math.exp(-0.5 * (k / sigma) ** 2) for k in range(-radius, radius + 1)]
    total = sum(weights)
    return [sum(w * values[(i + k - radius) % n] for k, w in enumerate(weights)) / total
            for i in range(n)]


def build(pts, meters_per_unit):
    total = sum(math.dist(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts)))
    n = round(total / meters_per_unit)
    samples, total = resample(pts, n)
    step = total / n
    sigma = 10.0 / step  # 10 m
    xs = smooth([p[0] for p in samples], sigma)
    ys = smooth([p[1] for p in samples], sigma)
    heading = [math.atan2(ys[(i + 1) % n] - ys[i - 1], xs[(i + 1) % n] - xs[i - 1])
               for i in range(n)]
    curvature = []
    for i in range(n):
        d = heading[(i + 1) % n] - heading[i - 1]
        d = (d + math.pi) % (2 * math.pi) - math.pi
        curvature.append(d / 2)  # radians per unit; positive turns right on screen
    curvature = smooth(curvature, 1.5)
    return n, xs, ys, heading, curvature


def safe_speeds(curvature):
    n = len(curvature)
    safe = [min(TOP_SPEED, math.sqrt(DRIFT_MAX / (GRIP * abs(c)))) if c else TOP_SPEED
            for c in curvature]
    for _ in range(2):  # around the lap twice, so braking zones wrap
        for i in range(n - 1, -1, -1):
            safe[i] = min(safe[i], math.sqrt(safe[(i + 1) % n] ** 2 + 2 * CORNER_BRAKING))
    return safe


def grid_back(curvature):
    """Units behind the start line for the grid: just out of Turn 20, at the
    start of the main straight."""
    n = len(curvature)
    back = 0
    while abs(curvature[(n - back - 1) % n]) < STRAIGHT_CURVATURE:
        back += 1
    return back - GRID_MARGIN


def race_time(safe, back, braking_at=None):
    """Seconds from the grid to the flag; braking_at gets when the car first brakes."""
    n = len(safe)
    s, v, t, dt = float(-back), 0.0, 0.0, 0.01
    while s < n:
        target = safe[int(s) % n]
        if v > target and braking_at is not None and not braking_at:
            braking_at.append(t)
        v = min(target, v + ACCELERATION * dt) if v < target else max(target, v - CORNER_BRAKING * dt)
        s += v * dt
        t += dt
    return t


def fit_map(xs, ys):
    best = None
    for degrees in range(0, 180):
        a = math.radians(degrees)
        rx = [x * math.cos(a) - y * math.sin(a) for x, y in zip(xs, ys)]
        ry = [x * math.sin(a) + y * math.cos(a) for x, y in zip(xs, ys)]
        w, h = max(rx) - min(rx), max(ry) - min(ry)
        scale = min((MAP_W - 2 * MARGIN) / w, (MAP_H - 2 * MARGIN) / h)
        if best is None or scale > best[0]:
            best = (scale, a, rx, ry)
    scale, a, rx, ry = best
    ox = (MAP_W - (max(rx) - min(rx)) * scale) / 2 - min(rx) * scale
    oy = MAP_TOP + (MAP_H - (max(ry) - min(ry)) * scale) / 2 - min(ry) * scale
    return scale, a, [x * scale + ox for x in rx], [y * scale + oy for y in ry]


def write_png(path, width, height, rows):
    raw = b"".join(b"\0" + bytes(row) for row in rows)
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def preview(path, px, py):
    road = [[0] * 320 for _ in range(240)]
    for x, y in zip(px, py):
        for dy in range(-ROAD_PX - 1, ROAD_PX + 2):
            for dx in range(-ROAD_PX - 1, ROAD_PX + 2):
                r2 = dx * dx + dy * dy
                X, Y = int(round(x)) + dx, int(round(y)) + dy
                if 0 <= X < 320 and 0 <= Y < 240:
                    if r2 <= ROAD_PX * ROAD_PX:
                        road[Y][X] = 2
                    elif r2 <= (ROAD_PX + 1) ** 2 and road[Y][X] == 0:
                        road[Y][X] = 1
    colors = {0: (0, 140, 0), 1: (240, 240, 240), 2: (100, 100, 100)}
    rows = []
    for y in range(240):
        row = []
        for x in range(320):
            row += colors[road[y][x]] if y >= MAP_TOP else (0, 0, 0)
        rows.append(row)
    sx, sy = int(px[0]), int(py[0])
    for y in range(sy - 1, sy + 2):
        rows[y][sx * 3:sx * 3 + 3] = [255, 0, 0]
    write_png(path, 320, 240, rows)


def main():
    pts = load(sys.argv[1])
    # Find the unit size that gives an ideal race of RACE_SECONDS.
    low, high = 2.0, 10.0
    for _ in range(30):
        mpu = (low + high) / 2
        n, xs, ys, heading, curvature = build(pts, mpu)
        safe = safe_speeds(curvature)
        t = race_time(safe, grid_back(curvature))
        low, high = (mpu, high) if t > RACE_SECONDS else (low, mpu)
    n, xs, ys, heading, curvature = build(pts, mpu)
    safe = safe_speeds(curvature)
    back = grid_back(curvature)
    braking_at = []
    race = race_time(safe, back, braking_at)
    if braking_at[0] < HORIZON_SECONDS:
        sys.exit(f"the car brakes for Turn 1 at {braking_at[0]:.1f} s, "
                 f"before the view changes at {HORIZON_SECONDS} s")
    print(f"{mpu:.2f} m per unit, {n} units, grid {back} back, ideal race {race:.2f} s, "
          f"first braking {braking_at[0]:.1f} s, "
          f"turning {sum(curvature) / math.pi:.3f} pi, max curvature {max(map(abs, curvature)):.3f}, "
          f"slowest {min(safe):.1f}")
    scale, angle, px, py = fit_map(xs, ys)
    heading = [h + angle for h in heading]
    print(f"map rotated {math.degrees(angle):.0f} deg, {scale:.3f} px/m, "
          f"x {min(px):.0f}-{max(px):.0f}, y {min(py):.0f}-{max(py):.0f}")

    def table(name, values):
        lines = []
        for i in range(0, len(values), 12):
            lines.append("    " + ", ".join(str(v) for v in values[i:i + 12]) + ",")
        return f"static const int16_t {name}[COTA_LENGTH] = {{\n" + "\n".join(lines) + "\n};\n"

    with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as out:
        out.write("/* Circuit of the Americas, one entry per road unit from the start line,\n"
                  "   in race direction. Generated by tools/cota_track.py from us-2012.geojson\n"
                  "   (github.com/bacinger/f1-circuits, MIT license, Copyright (c) 2019-2025\n"
                  "   Tomislav Bacinger). Do not edit by hand. */\n\n"
                  "#pragma once\n\n#include <stdint.h>\n\n")
        out.write(f"#define COTA_LENGTH {n}\n")
        out.write(f"#define COTA_METERS_PER_UNIT {mpu:.3f}f\n")
        out.write(f"#define COTA_GRID_BACK {back} /* units from the grid to the start line */\n")
        out.write("#define COTA_CURVATURE_SCALE 50000.0f /* curvature in radians per unit */\n")
        out.write("#define COTA_POSITION_SCALE 16.0f /* map pixels */\n")
        out.write("#define COTA_HEADING_SCALE 8192.0f /* radians, screen axes */\n\n")
        out.write(table("COTA_CURVATURE", [round(c * 50000) for c in curvature]))
        out.write(table("COTA_X", [round(x * 16) for x in px]))
        out.write(table("COTA_Y", [round(y * 16) for y in py]))
        out.write(table("COTA_HEADING",
                        [round(((h + math.pi) % (2 * math.pi) - math.pi) * 8192) for h in heading]))
    if len(sys.argv) > 3:
        preview(sys.argv[3], px, py)


if __name__ == "__main__":
    main()
