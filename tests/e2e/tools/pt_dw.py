#!/usr/bin/env python3
"""Platinum's Distortion World data, read from the game's own files: the floating platforms (floors, walls and
ceilings the player walks on, each with a tile-attribute grid), the jump points between them, and the overlay's
moving-platform events (games/platinum/src/overlay009/ov9_02249960.c). For planning routes the probe cannot see.

    python3 tests/e2e/tools/pt_dw.py MAP [--platforms] [--jumps] [--events] [--grid N]

MAP: B2F, MAP_HEADER_DISTORTION_WORLD_B2F or the header id. Coordinates are the overlay's tiles: x, y (the probe's
y halved), z. Kinds: floor, west wall, east wall, ceiling. A jump point fires when the player stands in its box and
presses its direction (dir: the d-pad key, U/D/L/R); it carries the player by (dx, dy, dz) onto platform `to` (-1:
the map's ground). An event fires when the player's tile becomes its tile and carries the player by the move
platform command's offset (the platform then goes back).
"""
import argparse
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
PT = os.path.join(ROOT, "games", "platinum")
TW = os.path.join(PT, "res", "prebuilt", "fielddata", "tornworld")
OV9 = os.path.join(PT, "src", "overlay009", "ov9_02249960.c")
KINDS = ["floor", "west", "east", "ceiling", "invalid"]
DIRS = "UDLR"  # FACE_UP, FACE_DOWN, FACE_LEFT, FACE_RIGHT (= DIR_NORTH.. = the d-pad keys)
# Step per d-pad key (U, D, L, R) on each platform kind (player_move.c sDistortionStepDirection*).
STEP = {
    "floor": [(0, 0, -1), (0, 0, 1), (-1, 0, 0), (1, 0, 0)],
    "west": [(0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)],
    "east": [(0, 1, 0), (0, -1, 0), (0, 0, -1), (0, 0, 1)],
    "ceiling": [(0, 0, 1), (0, 0, -1), (-1, 0, 0), (1, 0, 0)],
}


def narc(path):
    data = open(path, "rb").read()
    off = struct.unpack_from("<H", data, 0x0C)[0]
    fat = gmif = None
    while off < len(data):
        magic, size = data[off:off + 4], struct.unpack_from("<I", data, off + 4)[0]
        if magic == b"BTAF":
            n = struct.unpack_from("<H", data, off + 8)[0]
            fat = [struct.unpack_from("<II", data, off + 12 + 8 * i) for i in range(n)]
        elif magic == b"GMIF":
            gmif = off + 8
        off += size
    return [data[gmif + a:gmif + b] for a, b in fat]


def map_ids():
    ids = {}
    path = os.path.join(PT, "include", "data", "map_headers.h")
    names = []
    for line in open(path):
        m = re.match(r"\s*\[(MAP_HEADER_\w+)\]", line)
        if m:
            names.append(m.group(1))
    hdr = os.path.join(PT, "build", "rom", "generated", "map_headers.h")
    for cand in [hdr]:
        if os.path.exists(cand):
            for line in open(cand):
                m = re.match(r"\s*(MAP_HEADER_\w+)\s*=\s*(\d+)", line)
                if m:
                    ids[m.group(1)] = int(m.group(2))
    if not ids:
        ids = {n: i for i, n in enumerate(names)}
    return ids


class Bounds:
    def __init__(self, v):
        self.x, self.y, self.z, self.sx, self.sy, self.sz = v

    def has(self, x, y, z):
        return (self.x <= x <= self.x + self.sx and self.y <= y <= self.y + self.sy
                and self.z <= z <= self.z + self.sz)

    def __repr__(self):
        return "x %d..%d y %d..%d z %d..%d" % (self.x, self.x + self.sx, self.y, self.y + self.sy, self.z,
                                               self.z + self.sz)


class Platform:
    def __init__(self, i, kind, attr_id, bounds, nv, nh, attrs):
        self.i, self.kind, self.attr_id, self.b, self.nv, self.nh = i, KINDS[kind], attr_id, bounds, nv, nh
        self.attrs = attrs

    def attr(self, x, y, z):
        """The tile's u16 attributes, or None out of bounds (GetCurrentFloatingPlatformTileAttributes)."""
        b = self.b
        if not b.has(x, y, z):
            return None
        if self.kind == "floor":
            v, h = x - b.x, z - b.z
        elif self.kind == "west":
            v, h = b.sy - (y - b.y), z - b.z
        elif self.kind == "east":
            v, h = y - b.y, z - b.z
        else:
            v, h = b.sx - (x - b.x), z - b.z
        k = v + h * self.nv
        if k < 0 or k >= len(self.attrs):
            return None
        return self.attrs[k]

    def free(self, x, y, z):
        a = self.attr(x, y, z)
        return a is not None and not (a & 0x8000)


class Jump:
    def __init__(self, raw):
        (self.handler, d, _, bx, by, bz, sx, sy, sz, self.dx, self.dy, self.dz, self.rot, self.steps, self.axis,
         self.inv, self.face, kind, self.to) = raw
        self.dir = DIRS[d]
        self.b = Bounds((bx, by, bz, sx, sy, sz))
        self.kind = KINDS[kind] if 0 <= kind < len(KINDS) else str(kind)
        if self.to >= 0x8000:
            self.to -= 0x10000


def load(map_name):
    files = narc(os.path.join(TW, "tw_arc.narc"))
    attrs = narc(os.path.join(TW, "tw_arc_attr.narc"))
    ids = map_ids()
    name = map_name if map_name.startswith("MAP_HEADER_") else "MAP_HEADER_DISTORTION_WORLD_" + map_name.upper()
    mid = ids[name] if name in ids else int(map_name)
    info = files[0]
    n = struct.unpack_from("<i", info, 0)[0]
    entry = None
    for i in range(n):
        hid, fidx, ox, oy, oz = struct.unpack_from("<IHhhh", info, 4 + 12 * i)
        if hid == mid:
            entry = (fidx + 1, ox, oy, oz)  # FindMapFileIndex: member 0 is this info file
    if entry is None:
        sys.exit("pt_dw: %s (%d) has no Distortion World map file" % (name, mid))
    f = files[entry[0]]
    _, ps, js, cs, gs = struct.unpack_from("<5i", f, 0)
    plats, jumps = [], []
    off = 20
    if ps:
        cnt = struct.unpack_from("<i", f, off)[0]
        for i in range(cnt):
            kind, aid, bx, by, bz, sx, sy, sz, nv, nh = struct.unpack_from("<hH6hHH", f, off + 4 + 20 * i)
            a = attrs[aid]
            grid = list(struct.unpack_from("<%dH" % (len(a) // 2), a, 0))
            plats.append(Platform(i, kind, aid, Bounds((bx, by, bz, sx, sy, sz)), nv, nh, grid))
    off += ps
    if js:
        cnt = struct.unpack_from("<i", f, off)[0]
        for i in range(cnt):
            jumps.append(Jump(struct.unpack_from("<Hhi6h5hHHhhH", f, off + 4 + 40 * i)))
    return name, mid, entry, plats, jumps


def events(map_name):
    """The overlay's event tiles for a floor: (x, y, z, (dx, dy, dz) of the command that moves the player, name)."""
    src = open(OV9).read()
    suffix = map_name.upper().replace("MAP_HEADER_DISTORTION_WORLD_", "")
    tag = {"GIRATINA_ROOM": "GiratinaRoom"}.get(suffix, suffix)
    m = re.search(r"static const DistWorldEvent sMapEvents%s\[\] = \{(.*?)\n\};" % tag, src, re.S)
    if not m:
        return []
    out = []
    for e in re.finditer(r"\.tileX = (-?0x[0-9A-Fa-f]+|-?\d+),\s*\.tileY = (-?0x[0-9A-Fa-f]+|-?\d+),\s*"
                         r"\.tileZ = (-?0x[0-9A-Fa-f]+|-?\d+),.*?\.cmds = (\w+)", m.group(1), re.S):
        x, y, z = (int(v, 0) for v in e.group(1, 2, 3))
        cmds = e.group(4)
        move = None
        c = re.search(r"static const DistWorldEventCmd %s\[\] = \{(.*?)\n\};" % cmds, src, re.S)
        body = c.group(1) if c else ""
        for p in re.findall(r"\.params = &(\w+)", body):
            pm = re.search(r"static const CmdParamsMovePlatform %s = \{(.*?)\};" % p, src, re.S)
            if pm and re.search(r"\.movePlayer = TRUE", pm.group(1)):
                d = [int(re.search(r"\.finalTile%sOffset = (-?0x[0-9A-Fa-f]+|-?\d+)" % a, pm.group(1)).group(1), 0)
                     for a in "XYZ"]
                move = tuple(d)
        out.append((x, y, z, move, cmds))
    return out


def ground(name, ox, oz):
    """The floor's land grid in the overlay's coordinates: (x, z) -> (behavior, collision). Land tile (x - ox, z - oz)
    of the header's matrix (the map info's offsets: DistWorld_LoadFloorOffsets)."""
    sys.path.insert(0, HERE)
    import pt_map
    import json as _json
    h = pt_map.header_fields(name)
    m = _json.load(open(os.path.join(PT, "res", "field", "matrices", "%s.json" % h["mapMatrixID"])))["maps"]
    t = pt_map.TILES
    cells = {}
    for cz, row in enumerate(m):
        for cx, c in enumerate(row):
            if c == "MAP_NONE":
                continue
            land = pt_map.land(int(c.split("_")[1]))
            for i, v in enumerate(land):
                cells[(cx * t + i % t + ox, cz * t + i // t + oz)] = (v & 0xFF, bool(v & 0x8000))
    return cells


# JUMP_NORTH/SOUTH/WEST/EAST_TWICE: a pair of tiles (collision or not) jumped over, entered in its direction, landing
# three tiles from the start (seen on B1F: x 38 -> 35 west over 5c 5d)
JUMP2 = {0x5A: 0, 0x5B: 1, 0x5C: 2, 0x5D: 3}
LEDGE = {0x3A: 0, 0x3B: 1, 0x39: 2, 0x38: 3}  # JUMP_NORTH/SOUTH/WEST/EAST: one way, landing two tiles on
DELTA = [(0, -1), (0, 1), (-1, 0), (1, 0)]
WATER = {0x10, 0x11, 0x12, 0x13, 0x15, 0x19, 0x22}


def route(cells, start, goal, blocked=(), surf=False):
    """BFS over the ground; returns `moves` dirs with coordinate targets ([["L", "x32"], ...]) or None."""
    import collections
    blocked = set(blocked)
    prev = {start: None}
    q = collections.deque([start])
    while q:
        cur = q.popleft()
        if cur == goal:
            break
        for d, (dx, dz) in enumerate(DELTA):
            nxt = (cur[0] + dx, cur[1] + dz)
            c = cells.get(nxt)
            if c is None or nxt in blocked:
                continue
            b, solid = c
            if b in JUMP2 or b in LEDGE:
                if (JUMP2.get(b) if b in JUMP2 else LEDGE[b]) != d:
                    continue
                n = 3 if b in JUMP2 else 2
                nxt = (cur[0] + n * dx, cur[1] + n * dz)
                if cells.get(nxt, (0, True))[1]:
                    continue
            elif solid or (b in WATER and not surf):
                continue
            if nxt not in prev:
                prev[nxt] = (cur, d)
                q.append(nxt)
    if goal not in prev:
        return None
    legs, cur = [], goal
    while prev[cur]:
        cur0, d = prev[cur]
        legs.append((d, cur))
        cur = cur0
    legs.reverse()
    out = []
    for d, (x, z) in legs:
        target = "x%d" % x if d >= 2 else "z%d" % z
        if out and out[-1][0] == "UDLR"[d]:
            out[-1][1] = target
        else:
            out.append(["UDLR"[d], target])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("map")
    ap.add_argument("--platforms", action="store_true")
    ap.add_argument("--jumps", action="store_true")
    ap.add_argument("--events", action="store_true")
    ap.add_argument("--grid", type=int, action="append", default=[], help="print platform N's grid (# blocked)")
    ap.add_argument("--route", nargs=2, metavar=("X,Z", "X,Z"), help="a ground route (moves dirs) between two tiles")
    ap.add_argument("--block", default="", help="tiles the route avoids, X,Z;X,Z (boulders, event tiles)")
    ap.add_argument("--surf", action="store_true")
    a = ap.parse_args()
    name, mid, entry, plats, jumps = load(a.map)
    if a.route:
        cells = ground(name, entry[1], entry[3])
        (sx, sz), (gx, gz) = ([int(v) for v in t.split(",")] for t in a.route)
        block = [tuple(int(v) for v in t.split(",")) for t in a.block.split(";") if t]
        r = route(cells, (sx, sz), (gx, gz), block, a.surf)
        print(json.dumps(r).replace("],[", "], [") if r else "no route")
        return
    every = not (a.platforms or a.jumps or a.events or a.grid)
    print("%s (%d): file %d, offsets %s; %d platforms, %d jump points" % (name, mid, entry[0], entry[1:], len(plats),
                                                                        len(jumps)))
    if a.platforms or every:
        for p in plats:
            free = sum(1 for v in p.attrs if not v & 0x8000)
            print("  P%-3d %-7s %s  grid %dx%d (%d free) attr %d" % (p.i, p.kind, p.b, p.nv, p.nh, free, p.attr_id))
    if a.jumps or every:
        for j in jumps:
            print("  J %s at %s -> d(%d,%d,%d) to P%d %s face %d" % (j.dir, j.b, j.dx, j.dy, j.dz, j.to, j.kind,
                                                                  j.face))
    if a.events or every:
        for x, y, z, move, cmds in events(name):
            print("  E (%d,%d,%d) %s %s" % (x, y, z, move, cmds))
    for n in a.grid:
        p = plats[n]
        print("P%d %s %s: rows = vertical index 0..%d, cols = horizontal 0..%d" % (n, p.kind, p.b, p.nv - 1, p.nh - 1))
        for v in range(p.nv):
            print("  %3d " % v + "".join("#" if p.attrs[v + h * p.nv] & 0x8000 else "." for h in range(p.nh)
                                         if v + h * p.nv < len(p.attrs)))


if __name__ == "__main__":
    main()
