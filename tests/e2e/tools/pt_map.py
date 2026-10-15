#!/usr/bin/env python3
"""Platinum map as ASCII, for authoring walk_to steps: collision, behaviours, objects, warps, coord triggers.

    python3 tests/e2e/tools/pt_map.py MAP_HEADER_RAVAGED_PATH [--box X0 Z0 X1 Z1] [--world]

Reads the tree offline (games/platinum/res, the same land data games/platinum/pc/tests/pc_maptiles.py reads):
the header's map matrix gives the 32x32 land-data members, each tile's terrain attribute is behaviour (low byte)
plus collision (bit 15). Coordinates are the ones the probe and walk_to use: world tiles for a map of the
overworld matrix (matrix 0; only the cells whose header is this map are drawn unless --world), local otherwise.

Legend: '#' collision, '.' floor, 'g' tall grass, '~' water, 'v' ledge/one-way, 'r' rock-climb wall,
'=' bridge / other behaviour (printed under the map), digits/letters: object events (listed), 'W' warp, 'C' coord.
"""
import argparse
import json
import os
import re
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
PT = os.path.join(ROOT, "games", "platinum")
TILES = 32

GRASS = {0x02, 0x03}
WATER = {0x10, 0x11, 0x12, 0x13, 0x15, 0x19, 0x22}
LEDGE = set(range(0x38, 0x40))
ROCK_CLIMB = {0x4A, 0x4B}


def header_fields(name):
    text = open(os.path.join(PT, "include", "data", "map_headers.h")).read()
    m = re.search(r"\[%s\] = \{(.*?)\n    \}," % re.escape(name), text, re.S)
    if not m:
        sys.exit("no header %s" % name)
    return dict(re.findall(r"\.(\w+) = ([^,]+),", m.group(1)))


def land(member):
    raw = open(os.path.join(PT, "res", "field", "maps", "data", "map_data_%03d.bin" % member), "rb").read()
    return struct.unpack("<%dH" % (TILES * TILES), raw[0x10:0x10 + TILES * TILES * 2])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("map")
    ap.add_argument("--box", type=int, nargs=4, metavar=("X0", "Z0", "X1", "Z1"))
    ap.add_argument("--world", action="store_true", help="draw every cell of the matrix in the box")
    a = ap.parse_args()
    h = header_fields(a.map)
    mx = h["mapMatrixID"]
    m = json.load(open(os.path.join(PT, "res", "field", "matrices", "%s.json" % mx)))
    maps, headers = m["maps"], m.get("headers")
    cells = [(cx, cz) for cz, row in enumerate(maps) for cx, c in enumerate(row) if c != "MAP_NONE"
             and (a.world or not headers or headers[cz][cx] == a.map)]
    if not cells:
        sys.exit("%s: no cells in %s" % (a.map, mx))
    if a.box:
        x0, z0, x1, z1 = a.box
    else:
        x0 = min(c[0] for c in cells) * TILES
        z0 = min(c[1] for c in cells) * TILES
        x1 = (max(c[0] for c in cells) + 1) * TILES - 1
        z1 = (max(c[1] for c in cells) + 1) * TILES - 1
    ev = json.load(open(os.path.join(PT, "res", "field", "events", "%s.json" % h["eventsArchiveID"])))
    marks, legend = {}, []
    sym = "0123456789abcdefhijklmnopqstuxyzABDEFGHIJKLMNOPQRSTUVXYZ"
    for i, o in enumerate(ev.get("object_events", [])):
        s = sym[i % len(sym)]
        marks[(o["x"], o["z"])] = s
        legend.append("%s %-28s %-28s (%d,%d) %s sight %s flag %s" % (
            s, o.get("id"), o.get("graphics_id"), o["x"], o["z"], o.get("movement_type", ""),
            o.get("data", ["?"])[0] if o.get("data") else "-", o.get("hidden_flag")))
    for i, w in enumerate(ev.get("warp_events", [])):
        marks[(w["x"], w["z"])] = "W"
        legend.append("W warp %d (%d,%d) -> %s %s" % (i, w["x"], w["z"], w["dest_header_id"], w["dest_warp_id"]))
    for c in ev.get("coord_events", []):
        for dx in range(c.get("width", 1)):
            for dz in range(c.get("length", 1)):
                marks.setdefault((c["x"] + dx, c["z"] + dz), "C")
        legend.append("C coord (%d,%d) %dx%d script %s var %s==%s" % (
            c["x"], c["z"], c.get("width", 1), c.get("length", 1), c.get("script"), c.get("var"), c.get("value")))
    other = {}
    print("%s  matrix %s  box (%d,%d)-(%d,%d)" % (a.map, mx, x0, z0, x1, z1))
    print("      " + "".join(str((x // 10) % 10) for x in range(x0, x1 + 1)))
    print("      " + "".join(str(x % 10) for x in range(x0, x1 + 1)))
    for z in range(z0, z1 + 1):
        line = []
        for x in range(x0, x1 + 1):
            cx, cz = x // TILES, z // TILES
            if not (0 <= cz < len(maps) and 0 <= cx < len(maps[cz])) or maps[cz][cx] == "MAP_NONE" \
                    or (not a.world and headers and headers[cz][cx] != a.map):
                line.append(" ")
                continue
            if (x, z) in marks:
                line.append(marks[(x, z)])
                continue
            t = land(int(maps[cz][cx].split("_")[1]))[(z % TILES) * TILES + x % TILES]
            b = t & 0xFF
            if t & 0x8000:
                line.append("#")
            elif b == 0:
                line.append(".")
            elif b in GRASS:
                line.append("g")
            elif b in WATER:
                line.append("~")
            elif b in LEDGE:
                line.append("v")
            elif b in ROCK_CLIMB:
                line.append("r")
            else:
                other.setdefault(b, (x, z))
                line.append("=")
        print("%5d %s" % (z, "".join(line)))
    for l in legend:
        print(l)
    if other:
        print("other behaviours ('='): " + ", ".join("%02x e.g. (%d,%d)" % (b, *xz) for b, xz in sorted(other.items())))


if __name__ == "__main__":
    main()
