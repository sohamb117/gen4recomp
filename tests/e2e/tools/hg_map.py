#!/usr/bin/env python3
"""HeartGold/SoulSilver map as ASCII, for authoring walk_to/slide/steps: collision, behaviours, objects, warps, coords.

    python3 tests/e2e/tools/hg_map.py MAP_NEW_BARK [--box X0 Z0 X1 Z1] [--hex]
    python3 tests/e2e/tools/hg_map.py MAP_MAHOGANY_GYM          # a prefix prints every map it names
    python3 tests/e2e/tools/hg_map.py MAP_MAHOGANY_GYM_ROOM_1 --slide 4 18 "UP LEFT"   # simulate ice presses
    python3 tests/e2e/tools/hg_map.py MAP_MAHOGANY_GYM_ROOM_1 --solve 4 18 3 2        # shortest press sequence

Reads the decomp tree offline (games/heartgold), the way the game loads a map:
- the map header (src/data/map_headers.h via src/map_header.c MapHeader_GetMatrixId) names the map matrix and the
  zone_event bank;
- the map matrix (files/fielddata/mapmatrix/map_matrix, src/map_matrix.c MapMatrix_MapMatrixData_Load): width,
  height, has-headers, has-altitudes, name; then the per-cell map header ids (if any), altitudes (if any) and the
  land data id per cell (0xFFFF none). Only the cells whose header is this map are drawn (all of them for a matrix
  without a header section: src/map_matrix.c fills it with the map's own id);
- the land data NARC files/a/0/6/5 (NARC_fielddata_landdata_land_data = 65, include/filesystem_files_def.h): per
  member a 0x10 header of four section sizes (attributes 0x800, objects, model, BDHC; ov01_021F49FC,
  asm/overlay_01_021F4704.s:413-454), then a u32 whose low half is 0x1234 and high half the size of an extra
  section that follows it (ov01_021F4AAC :506-534 reads it), then the 32x32 u16 tile attributes (ov01_021F4AE4
  :536-): low byte the behaviour (include/constants/metatile_behavior.h, GetMetatileBehavior
  asm/unk_02054648.s:431), bit 15 the collision bit (sub_020548C0 :361). src/terrain_attributes.c reads the
  attributes at a fixed 0x14 (TERRAIN_ATTRIBUTES_OFFSET), which is only right when the extra section is empty;
  the field's MapLoadManager path above skips it;
- events: files/fielddata/eventdata/zone_event/<bank>.json; an object's talk_to id is its number in
  files/fielddata/script/scr_seq/event_<CODE>.h.
Coordinates are the probe's/walk_to's: matrix cell * 32 + tile, i.e. world tiles on the overworld matrix 0 and
map-local tiles indoors, as the zone_event JSON gives them.

Ice (--slide/--solve, Map.slide): the player's forced movement (asm/unk_0205CB48.s sub_0205D01C, the behaviour table
_020FCB88: MetatileBehavior_IsIce -> sub_0205D0A8) keeps moving in the facing direction while the tile stood on is
ICE (0x20), and stops when the next step is blocked (sub_0205DA34); a press from a standstill walks one tile. A step
is blocked here by: leaving the map's cells, the collision bit, a map object at its spawn tile (unless --hide), a
jump tile entered against its direction. Mahogany Gym's ice blocks (SPRITE_ICE) are pushed by a slide that runs
into them (src/unk_0206D494.c; Map.slide). BDHC height changes on ice (sub_0205D240/sub_0205D2A0: uphill slows and
turns back) are not modelled: the HG/SS ice floors checked with this tool are flat [INFERENCE]. Trainers' sight is
listed (Map.sight) but not acted out: a trainer who sees the player interrupts the slide with a battle.

Legend: '#' collision, '.' floor (behaviour 0), ',' cave floor, 'g' grass, '~' water, 'o' whirlpool,
'|' waterfall, 'i' ice, '>' '<' '^' 'v' jumps, 'H' ladder, 'S' stairs/warp entrance, 'D' door, 'P' warp panel,
'x' stop-sliding, 's' sand/snow/mud, '=' other (listed under the map); then events on top: 'W' warp, 'C' coord,
digits/letters objects (listed with their talk_to id).
"""
import argparse
import collections
import json
import os
import re
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
HG = os.path.join(ROOT, "games", "heartgold")
TILES = 32
NONE = 0xFFFF

ICE = 0x20
JUMPS = {0x38: (1, 0), 0x39: (-1, 0), 0x3A: (0, -1), 0x3B: (0, 1)}
DIRS = {"UP": (0, -1), "DOWN": (0, 1), "LEFT": (-1, 0), "RIGHT": (1, 0)}
DIR_SOUTH = 1  # include/constants/global_fieldmap.h
SYMS = {0x00: ".", 0x08: ",", 0x02: "g", 0x03: "g", 0x10: "~", 0x15: "~", 0x11: "o", 0x13: "|", ICE: "i",
        0x38: ">", 0x39: "<", 0x3A: "^", 0x3B: "v", 0x3C: "H", 0x3D: "H", 0x3E: "H", 0x5E: "S", 0x5F: "S",
        0x62: "S", 0x63: "S", 0x64: "S", 0x65: "S", 0x69: "D", 0x67: "P", 0x4D: "x", 0x21: "s", 0xA4: "s",
        0xA8: "s"}
OBJ_SYMS = "0123456789abcdefhjklmnpqrtuyzABEFGIJKLMNOQRTUVXYZ"


def narc(path):
    """The members of a NARC (BTAF offsets into GMIF)."""
    b = open(path, "rb").read()
    off = struct.unpack_from("<H", b, 12)[0]
    size, n = struct.unpack_from("<IH", b, off + 4)
    ents = [struct.unpack_from("<II", b, off + 12 + 8 * i) for i in range(n)]
    fnt = off + size
    gmif = fnt + struct.unpack_from("<I", b, fnt + 4)[0]
    return [b[gmif + 8 + s:gmif + 8 + e] for s, e in ents]


_cache = {}


def land_members():
    if "land" not in _cache:
        _cache["land"] = narc(os.path.join(HG, "files", "a", "0", "6", "5"))
    return _cache["land"]


def land_attrs(member):
    """32x32 u16 tile attributes of one land data member."""
    m = land_members()[member]
    extra = struct.unpack_from("<I", m, 0x10)[0] >> 16
    return struct.unpack_from("<%dH" % (TILES * TILES), m, 0x14 + extra)


def map_ids():
    if "maps" not in _cache:
        ids = {}
        for line in open(os.path.join(HG, "include", "constants", "maps.h")):
            m = re.match(r"#define (MAP_\w+)\s+(\d+)\s*(?://\s*MAP_(\w+))?", line)
            if m:
                ids[m.group(1)] = (int(m.group(2)), m.group(3))
        _cache["maps"] = ids
    return _cache["maps"]


def header(name):
    text = open(os.path.join(HG, "src", "data", "map_headers.h")).read()
    m = re.search(r"\[%s\] = \{(.*?)\n\s*\}," % re.escape(name), text, re.S)
    if not m:
        sys.exit("no header %s" % name)
    return dict(re.findall(r"\.(\w+) = ([^,]+),", m.group(1)))


def naix_index(naix, sym):
    for line in open(naix):
        m = re.match(r"#define (\w+) (\d+)", line)
        if m and m.group(1) == sym:
            return int(m.group(2))
    sys.exit("%s not in %s" % (sym, naix))


def matrix(index):
    d = os.path.join(HG, "files", "fielddata", "mapmatrix", "map_matrix")
    name = sorted(f for f in os.listdir(d) if f.startswith("map_matrix_"))[index]
    b = open(os.path.join(d, name), "rb").read()
    w, h, has_headers, has_alts, nl = b[0], b[1], b[2], b[3], b[4]
    c = 5 + nl
    headers = None
    if has_headers:
        headers = struct.unpack_from("<%dH" % (w * h), b, c)
        c += 2 * w * h
    if has_alts:
        c += w * h
    models = struct.unpack_from("<%dH" % (w * h), b, c)
    return {"file": name, "w": w, "h": h, "headers": headers, "models": models}


class Map:
    """One map: its tiles {(x, z): attr} in probe coordinates, and its events."""

    def __init__(self, name):
        self.name = name
        self.id, self.code = map_ids()[name]
        h = header(name)
        self.matrix_sym = h["matrixId"].strip()
        mx = matrix(naix_index(os.path.join(HG, "files", "fielddata", "mapmatrix", "map_matrix.naix"),
                               self.matrix_sym))
        self.matrix_file = mx["file"]
        self.tiles = {}
        for i, model in enumerate(mx["models"]):
            if model == NONE or (mx["headers"] and mx["headers"][i] != self.id):
                continue
            cx, cz = i % mx["w"], i // mx["w"]
            a = land_attrs(model)
            for t in range(TILES * TILES):
                self.tiles[(cx * TILES + t % TILES, cz * TILES + t // TILES)] = a[t]
        bank = h["eventsBank"].strip()
        self.events_file = re.match(r"NARC_zone_event_(\w+)_bin", bank).group(1)
        self.ev = json.load(open(os.path.join(HG, "files", "fielddata", "eventdata", "zone_event",
                                              self.events_file + ".json")))
        self.obj_ids = {}
        hdr = os.path.join(HG, "files", self.ev.get("header", ""))
        if os.path.isfile(hdr):
            for line in open(hdr):
                m = re.match(r"#define (obj_\w+)\s+(\d+)", line)
                if m:
                    self.obj_ids[m.group(1)] = int(m.group(2))

    def objects(self):
        return [(self.obj_ids.get(o["id"], i), o) for i, o in enumerate(self.ev.get("objects", []))]

    def blocked(self, frm, to, objs):
        """The step frm -> to is refused: off the map, collision, an object, a jump entered against its dir."""
        t = self.tiles.get(to)
        if t is None or t & 0x8000 or to in objs:
            return True
        j = JUMPS.get(t & 0xFF)
        return j is not None and j != (to[0] - frm[0], to[1] - frm[1])

    def ice_blocks(self, hide=()):
        """(fixed object tiles, ice blocks {tile: pushable}) at the map's spawn, objects in `hide` left out."""
        fixed, blocks = set(), {}
        for oid, o in self.objects():
            if oid in hide:
                continue
            if o["spriteId"] == "SPRITE_ICE":
                blocks[(o["x"], o["z"])] = o.get("facingDirection") == DIR_SOUTH
            else:
                fixed.add((o["x"], o["z"]))
        return fixed, blocks

    def slide(self, pos, d, fixed, blocks=None):
        """One press from a standstill at pos: (tiles passed, where the player stops, the ice blocks after).

        A press walks one tile (a standstill on ice is flagged: sub_0205D01C PlayerAvatar_CheckFlag1, so the first
        step is a normal walk and bumping a block from there pushes nothing); then, while the tile stood on is ICE,
        the slide goes on until blocked. Blocked by an ice block (SPRITE_ICE facing south) while sliding, the player
        stops and pushes it (src/unk_0206D494.c sub_0206D494/sub_0206D590): the block slides on while the next tile
        is ICE without collision (sub_0206D7B8 flags 1/4: it stops, still pushable); meeting an object it stops and
        it and an ice block it met turn north (sub_0206D688 case 2: MOVEMENT_FACE_UP), no longer pushable."""
        blocks = dict(blocks or {})
        objs = fixed | set(blocks)
        dx, dz = DIRS[d]
        path = [pos]
        nxt = (pos[0] + dx, pos[1] + dz)
        if self.blocked(pos, nxt, objs):
            return path, blocks
        pos = nxt
        path.append(pos)
        while self.tiles[pos] & 0xFF == ICE:
            nxt = (pos[0] + dx, pos[1] + dz)
            if self.blocked(pos, nxt, objs):
                if blocks.get(nxt):
                    b = nxt
                    while True:
                        n2 = (b[0] + dx, b[1] + dz)
                        if n2 in fixed or n2 in blocks:
                            if n2 in blocks:
                                blocks[n2] = False
                            blocks[b] = False
                            break
                        t = self.tiles.get(n2)
                        if t is None or t & 0x8000 or t & 0xFF != ICE:
                            break
                        del blocks[b]
                        blocks[n2] = True
                        b = n2
                break
            pos = nxt
            path.append(pos)
        return path, blocks

    def solve(self, start, goal, fixed, blocks=None, sight=None, avoid=(), last=None):
        """Fewest presses from start to stand on goal, the ice blocks pushed as the presses push them. With `sight`
        (Map.sight), no press passes a tile a trainer sees from afar (he would walk up and stand in the way), and a
        tile next to a trainer facing it only ends a press (the battle starts where the player stops anyway). No
        press passes a tile in `avoid` (coord events fire mid-slide too: FieldSystem_ProcessStep checks them before
        the forced-movement return, src/field/field_control.c:646-655); `last`: the press that reaches goal goes
        that way (the player then faces it)."""
        sight = sight or {}
        key0 = (start, frozenset((blocks or {}).items()))
        prev = {key0: None}
        q = collections.deque([key0])
        while q:
            k = q.popleft()
            for d in DIRS:
                path, nb = self.slide(k[0], d, fixed, dict(k[1]))
                nk = (path[-1], frozenset(nb.items()))
                if any(p in avoid or (p in sight and (sight[p][1] > 1 or p != path[-1])) for p in path[1:]):
                    continue
                if path[-1] == goal and len(path) > 1 and (last is None or d == last):
                    seq = [d]
                    while prev[k]:
                        k, d = prev[k]
                        seq.append(d)
                    return seq[::-1]
                if nk not in prev:
                    prev[nk] = (k, d)
                    q.append(nk)
        return None

    def sight(self):
        """{tile: (trainer object id, distance)}: the tiles a trainer (object type 1) sees, param0 tiles ahead until a
        collision tile. Movement 14..17 (LOOK_NORTH..EAST in the Gen 4 numbering of games/platinum/generated/
        movement_types.txt [INFERENCE: HG/SS keeps it]) look one way; any other movement is taken to look all four
        ways from its spawn tile (conservative)."""
        seen = {}
        for oid, o in self.objects():
            if o.get("type") != 1:
                continue
            look = {14: "UP", 15: "DOWN", 16: "LEFT", 17: "RIGHT"}.get(o.get("movement"))
            for dx, dz in [DIRS[look]] if look else DIRS.values():
                for k in range(1, o.get("param0", 0) + 1):
                    t = (o["x"] + dx * k, o["z"] + dz * k)
                    if t not in self.tiles or self.tiles[t] & 0x8000:
                        break
                    seen[t] = (oid, k)
        return seen


def draw(mp, box=None, hexmode=False):
    if not mp.tiles:
        print("%s: no land data cells" % mp.name)
        return
    xs = [x for x, _ in mp.tiles]
    zs = [z for _, z in mp.tiles]
    x0, z0, x1, z1 = box or (min(xs), min(zs), max(xs), max(zs))
    marks, legend = {}, []
    for i, (oid, o) in enumerate(mp.objects()):
        s = OBJ_SYMS[i % len(OBJ_SYMS)]
        marks[(o["x"], o["z"])] = s
        legend.append("%s object %d %-34s %-22s (%d,%d) y%d mv %d face %d%s flag %s script %s" % (
            s, oid, o["id"], o["spriteId"], o["x"], o["z"], o.get("y", 0), o.get("movement", 0),
            o.get("facingDirection", 0), " sight %d" % o.get("param0", 0) if o.get("type") == 1 else "",
            o.get("eventFlag"), o.get("scriptId")))
    for i, w in enumerate(mp.ev.get("warps", [])):
        marks[(w["x"], w["z"])] = "W"
        t = mp.tiles.get((w["x"], w["z"]))
        legend.append("W warp %d (%d,%d) -> %s %s  tile %s" % (
            i, w["x"], w["z"], w["header"], w["anchor"], "%04x" % t if t is not None else "off-map"))
    for i, c in enumerate(mp.ev.get("coords", [])):
        for dx in range(c.get("w", 1)):
            for dz in range(c.get("h", 1)):
                marks.setdefault((c["x"] + dx, c["z"] + dz), "C")
        legend.append("C coord %d (%d,%d) w%d h%d %s == %s -> %s" % (
            i, c["x"], c["z"], c.get("w", 1), c.get("h", 1), c.get("var"), c.get("val"), c.get("scriptId")))
    for i, b in enumerate(mp.ev.get("bgs", [])):
        legend.append("  bg %d (%d,%d) type %s %s" % (i, b["x"], b["z"], b.get("type"), b.get("scriptId")))
    other = {}
    print("%s (%d, %s)  matrix %s  events %s  box (%d,%d)-(%d,%d)" % (
        mp.name, mp.id, mp.code, mp.matrix_file, mp.events_file, x0, z0, x1, z1))
    wid = 3 if hexmode else 1
    print("      " + "".join(str((x // 10) % 10).rjust(wid) for x in range(x0, x1 + 1)))
    print("      " + "".join(str(x % 10).rjust(wid) for x in range(x0, x1 + 1)))
    for z in range(z0, z1 + 1):
        line = []
        for x in range(x0, x1 + 1):
            t = mp.tiles.get((x, z))
            if t is None:
                line.append(" " * wid)
            elif hexmode:
                line.append(("#%02x" if t & 0x8000 else " %02x") % (t & 0xFF))
            elif (x, z) in marks:
                line.append(marks[(x, z)])
            elif t & 0x8000:
                line.append("#")
            else:
                b = t & 0xFF
                s = SYMS.get(b)
                if s is None:
                    other.setdefault(b, (x, z))
                    s = "="
                line.append(s)
        print("%5d %s" % (z, "".join(line)))
    for l in legend:
        print(l)
    if other:
        print("other behaviours ('='): " + ", ".join("%02x e.g. (%d,%d)" % (b, *xz) for b, xz in sorted(other.items())))
    if hexmode:
        print("hex: behaviour byte, '#' = collision bit set")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("map", help="MAP_* name, or a prefix of several")
    ap.add_argument("--box", type=int, nargs=4, metavar=("X0", "Z0", "X1", "Z1"))
    ap.add_argument("--hex", action="store_true", help="print each tile's behaviour byte")
    ap.add_argument("--slide", nargs=3, metavar=("X", "Z", "DIRS"), help="simulate presses (\"UP LEFT ...\")")
    ap.add_argument("--solve", type=int, nargs=4, metavar=("X", "Z", "TX", "TZ"), help="fewest presses to a tile")
    ap.add_argument("--hide", type=int, nargs="*", default=[], help="object ids not standing (hidden/removed)")
    ap.add_argument("--fight", action="store_true", help="--solve may pass trainers' sight (default: avoided)")
    ap.add_argument("--avoid", nargs="*", default=[], metavar="X,Z", help="--solve never passes these tiles")
    ap.add_argument("--avoid-coords", action="store_true", help="--solve never passes a coord event's tile")
    ap.add_argument("--last", choices=sorted(DIRS), help="--solve: the press reaching the goal goes this way")
    ap.add_argument("--pits", nargs="*", type=int, metavar="K",
                    help="print the coord events K (all without K) as a walk_to avoid list (their tiles without "
                         "the collision bit)")
    a = ap.parse_args()
    ids = map_ids()
    names = [a.map] if a.map in ids else sorted((n for n in ids if n.startswith(a.map)), key=lambda n: ids[n][0])
    if not names:
        sys.exit("no map %s" % a.map)
    for n in names:
        mp = Map(n)
        fixed, blocks = mp.ice_blocks(a.hide)
        slide = a.slide
        sight = mp.sight()
        if a.pits is not None:
            tiles = sorted({(c["x"] + dx, c["z"] + dz) for k, c in enumerate(mp.ev.get("coords", []))
                            if not a.pits or k in a.pits for dx in range(c.get("w", 1)) for dz in range(c.get("h", 1))
                            if (c["x"] + dx, c["z"] + dz) in mp.tiles and not mp.tiles[(c["x"] + dx, c["z"] + dz)] & 0x8000})
            print("avoid = [%s]" % ", ".join("[%d, %d]" % t for t in tiles))
            continue
        if a.solve:
            avoid = {tuple(int(v) for v in t.split(",")) for t in a.avoid}
            if a.avoid_coords:
                avoid |= {(c["x"] + dx, c["z"] + dz) for c in mp.ev.get("coords", [])
                          for dx in range(c.get("w", 1)) for dz in range(c.get("h", 1))}
            seq = mp.solve((a.solve[0], a.solve[1]), (a.solve[2], a.solve[3]), fixed, blocks,
                           None if a.fight else sight, avoid, a.last)
            print("%s: %s" % (n, " ".join(seq) if seq is not None else "unreachable"))
            if seq is None:
                continue
            slide = (a.solve[0], a.solve[1], " ".join(seq))
        if slide:
            pos = (int(slide[0]), int(slide[1]))
            for d in slide[2].split():
                path, blocks2 = mp.slide(pos, d.upper(), fixed, blocks)
                pushed = ["(%d,%d)->(%d,%d)%s" % (*b, *b2, "" if blocks2[b2] else " frozen")
                          for b, b2 in zip(sorted(set(blocks) - set(blocks2)), sorted(set(blocks2) - set(blocks)))]
                frozen = [b for b in blocks2 if b in blocks and blocks[b] and not blocks2[b]]
                seen = sorted({"%d at (%d,%d) from %d" % (sight[p][0], *p, sight[p][1]) for p in path[1:] if p in sight})
                print("%-5s (%d,%d) -> (%d,%d)  via %s%s%s%s" % (
                    d, *pos, *path[-1], " ".join("%d,%d" % p for p in path[1:]) or "-",
                    "  pushes " + " ".join(pushed) if pushed else "",
                    "  freezes " + " ".join("(%d,%d)" % b for b in frozen) if frozen else "",
                    "  in sight of trainer %s" % seen if seen else ""))
                pos, blocks = path[-1], blocks2
            continue
        draw(mp, a.box, a.hex)
        print()


if __name__ == "__main__":
    main()
