#!/usr/bin/env python3
"""Where in the world a tile behaviour occurs, read out of res/ offline.

Why this exists. Obstacles are object events, so res/field/events/ hands their
coordinates over for free, but water, waterfalls and rock-climb walls are
tile behaviours inside the compiled map, and the port's own --map-scan only
sees the 32x32 chunks loaded around the player. Scanning the window at two
dozen warps found no waterfall at all; this finds all 55 of them in a second,
because the data is right there in the tree.

THE FORMAT, which is the whole trick. A land-data member (res/field/maps/data/
map_data_NNN.bin, in the order map_data.order gives) opens with four sizes and
then TERRAIN_ATTRIBUTES_SIZE bytes of terrain attributes at offset 0x10: one
u16 per tile, 32 by 32, whose low byte is the behaviour the field engine
branches on. A map matrix (res/field/matrices/) says which member sits at which
32x32 cell of a world, so member plus cell plus tile index is a world
coordinate. LandDataHeader_Load and TerrainAttributes_Load in the game are the
two functions this mirrors.

Checked against the running game: the grid this prints around (505,525) on
map matrix 0 is byte for byte the `row` block the port's --map-scan wrote at
the same spot.

    $ pc/tests/pc_maptiles.py 0x13            # every waterfall in the game
    $ pc/tests/pc_maptiles.py 0x13 --around   # and the ground around each
"""

import argparse
import glob
import json
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "res", "field", "maps", "data")
MATRICES = os.path.join(ROOT, "res", "field", "matrices")

TILES = 32
ATTR_OFFSET = 0x10
ATTR_COUNT = TILES * TILES

_chunks = {}


def chunk(member):
    """The 32x32 behaviour bytes of one land-data member."""
    if member not in _chunks:
        path = os.path.join(DATA, "map_data_%03d.bin" % member)
        raw = open(path, "rb").read()
        attrs = struct.unpack("<%dH" % ATTR_COUNT,
                              raw[ATTR_OFFSET:ATTR_OFFSET + ATTR_COUNT * 2])
        _chunks[member] = [a & 0xFF for a in attrs]
    return _chunks[member]


def matrices():
    for path in sorted(glob.glob(os.path.join(MATRICES, "map_matrix_*.json"))):
        yield os.path.basename(path)[:-len(".json")], json.load(open(path))


def members_with(behavior):
    """Every land-data member holding this behaviour, and where in it."""
    found = {}
    for path in sorted(glob.glob(os.path.join(DATA, "map_data_*.bin"))):
        member = int(os.path.basename(path)[len("map_data_"):-len(".bin")])
        hits = [i for i, b in enumerate(chunk(member)) if b == behavior]
        if hits:
            found[member] = hits
    return found


def sites(behavior):
    """(matrix, header, world x, world z) for every tile with this behaviour.

    A member can appear in more than one matrix cell, and every appearance is
    a different place in the world, so this walks the matrices rather than the
    members. A member no matrix references is unreachable and is left out.
    """
    found = members_with(behavior)
    out = []
    for name, m in matrices():
        headers = m.get("headers")
        for cz, row in enumerate(m["maps"]):
            for cx, cell in enumerate(row):
                if cell == "MAP_NONE":
                    continue
                member = int(cell.split("_")[1])
                for i in found.get(member, ()):
                    header = headers[cz][cx] if headers else "?"
                    out.append((name, header, cx * TILES + i % TILES,
                                cz * TILES + i // TILES))
    return out


def grid(matrix_name, x, z, r):
    """The behaviour bytes around a world tile, as --map-scan prints them."""
    m = dict(matrices())[matrix_name]
    maps = m["maps"]
    rows = []
    for gz in range(z - r, z + r + 1):
        line = []
        for gx in range(x - r, x + r + 1):
            cx, cz = gx // TILES, gz // TILES
            if not (0 <= cz < len(maps) and 0 <= cx < len(maps[cz])) \
                    or maps[cz][cx] == "MAP_NONE":
                line.append("--")
                continue
            member = int(maps[cz][cx].split("_")[1])
            line.append("%02x" % chunk(member)[(gz % TILES) * TILES + gx % TILES])
        rows.append((gz, "".join(line)))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("behavior", help="tile behaviour, e.g. 0x13 for a waterfall")
    ap.add_argument("--around", type=int, nargs="?", const=6, default=0,
                    metavar="R", help="also print the R tiles around each site")
    args = ap.parse_args()

    behavior = int(args.behavior, 0)
    found = sites(behavior)
    print("behaviour %02x: %d tile(s) in %d matrix cell(s)"
          % (behavior, len(found), len(set((f[0], f[2] // TILES, f[3] // TILES)
                                           for f in found))))
    seen = set()
    for name, header, x, z in found:
        print("  %-17s %-34s %4d %4d" % (name, header, x, z))
        if args.around and (name, x // TILES, z // TILES) not in seen:
            seen.add((name, x // TILES, z // TILES))
            for gz, line in grid(name, x, z, args.around):
                print("      z=%4d %s" % (gz, line))


if __name__ == "__main__":
    main()
