#!/usr/bin/env python3
"""HeartGold/SoulSilver world facts from the pokeheartgold decomp (games/heartgold), for the e2e bots and authors.

    tests/e2e/tools/hg_world.py fly                       # every fly destination: map, flypoint flag, cursor rect, landing tile
    tests/e2e/tools/hg_world.py grid X0 Z0 X1 Z1          # the overworld's tiles as ASCII (world tile coordinates)
    tests/e2e/tools/hg_world.py route X0 Z0 X1 Z1 [--surf] [--avoid X,Z ...]   # a static A* path, printed as corners

Sources:
- The fly map (the Pokegear map app in fly mode, src/application/pokegear/map/fly_map.c): its destinations are
  gMapFlypointParams (src/application/pokegear/map/overlay_101_021F79B4.c: map named, map flown to, flypoint flag,
  the cursor rect x, y, width, height in overworld blocks); the landing tile is the flypoint's spawn
  (asm/unk_0203BA5C.s sSpawnMaps: flyPointMapNo, flyPointX, flyPointY; GetFlyWarpData).
- Map headers: src/data/map_headers.h (worldMapX/Y, matrixId, flyAllowed, escapeRopeAllowed, mapType).
- The overworld: map matrix 0 and the land data, read with tools/hg_map.py (its docstring: matrix layout, the land
  data member layout and the field's loader).
- Surfable behaviors: src/metatile_behavior.c (TILE_BEHAVIOR_FLAG_SURFABLE), the whirlpool (0x11) left out: it needs
  the HM Whirlpool, which walk_to does not use.

The grid and route are the static land data only: no people, no scripts, no bridges' step layers (the probe's walk_to
sees those). Use a route for `via` waypoints, then let walk_to plan each leg on the probe.
"""
import heapq
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hg_map  # noqa: E402  (the land data, map matrix and map id readers)

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
HG = os.path.join(ROOT, "games", "heartgold")

BLOCK = 32
COLLISION = 0x8000
WHIRLPOOL, WATERFALL = 0x11, 0x13
# src/metatile_behavior.c sMetatileBehaviorFlags entries with TILE_BEHAVIOR_FLAG_SURFABLE, the whirlpool left out
SURFABLE = frozenset({0x10, 0x12, 0x14, 0x15, 0x19, 0x22, 0x2A, 0x50, 0x51, 0x52, 0x53, 0x73, 0x78, 0x7C})
JUMPS = {0x38: (1, 0), 0x39: (-1, 0), 0x3A: (0, -1), 0x3B: (0, 1)}  # TILE_BEHAVIOR_JUMP_EAST/WEST/NORTH/SOUTH

_cache = {}


def _read(*parts):
    with open(os.path.join(HG, *parts), "rb") as f:
        return f.read()


def map_ids():
    """include/constants/maps.h: MAP_* name -> id."""
    if "ids" not in _cache:
        _cache["ids"] = {k: v[0] for k, v in hg_map.map_ids().items()}
    return _cache["ids"]


def map_name(mid):
    return next((k for k, v in map_ids().items() if v == mid), "MAP_%d" % mid)


def map_headers():
    """src/data/map_headers.h: MAP_* name -> {worldMapX, worldMapY, main, fly, dig} (main: on the overworld matrix 0;
    fly: flyAllowed, which FieldMove_CheckFly requires of the map Fly is used on, src/field_move.c:224; dig: a cave
    with escapeRopeAllowed, FieldMove_CheckDig's condition, src/field_move.c:514)."""
    if "headers" not in _cache:
        txt = _read("src", "data", "map_headers.h").decode()
        out = {}
        for m in re.finditer(r"\[(MAP_\w+)\]\s*=\s*\{(.*?)\n\s*\},", txt, re.S):
            body = m.group(2)
            wx = re.search(r"\.worldMapX\s*=\s*(\d+)", body)
            wy = re.search(r"\.worldMapY\s*=\s*(\d+)", body)
            out[m.group(1)] = {"worldMapX": int(wx.group(1)) if wx else 0, "worldMapY": int(wy.group(1)) if wy else 0,
                               "main": "map_matrix_0000_EVERYWHERE" in body,
                               "fly": bool(re.search(r"\.flyAllowed\s*=\s*TRUE", body)),
                               "dig": bool(re.search(r"\.mapType\s*=\s*MAP_TYPE_CAVE\b", body)
                                           and re.search(r"\.escapeRopeAllowed\s*=\s*TRUE", body))}
        _cache["headers"] = out
    return _cache["headers"]


def fly_points():
    """gMapFlypointParams in order: dicts name (map named), map (map flown to), flypoint, x, y, w, h (the cursor rect in
    overworld blocks: PokegearMap_GetFlyDestinationAtCoord, overlay_101_021E9270.c:721-747)."""
    if "fly" not in _cache:
        txt = _read("src", "application", "pokegear", "map", "overlay_101_021F79B4.c").decode()
        out = []
        for m in re.finditer(r"\{\s*(MAP_\w+),\s*(MAP_\w+),\s*(FLYPOINT_\w+),\s*(\w+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),"
                             r"\s*(\d+),\s*(\d+),", txt):
            out.append({"name": m.group(1), "map": m.group(2), "flypoint": m.group(3), "x": int(m.group(5)),
                        "y": int(m.group(6)), "w": int(m.group(9)), "h": int(m.group(10))})
        _cache["fly"] = out
    return _cache["fly"]


def fly_landing(map_name_):
    """Where Fly to `map_name_` (a fly point's flown-to map) lands: (map, x, z), the fly point of the first sSpawnMaps
    entry whose flyPointMapNo is that map (asm/unk_0203BA5C.s: sub_0203BB50 finds the spawn, GetFlyWarpData reads
    flyPointMapNo/X/Y; the fly task sub_02067C30 warps there)."""
    if "spawns" not in _cache:
        txt = _read("asm", "unk_0203BA5C.s").decode()
        out = {}
        for m in re.finditer(r"^\s*spawn\s+\w+,\s*\d,\s*\d,\s*\w+,\s*\w+,\s*\w+,\s*(MAP_\w+),\s*(\w+),\s*(\w+),", txt, re.M):
            out.setdefault(m.group(1), (m.group(1), int(m.group(2), 0), int(m.group(3), 0)))
        _cache["spawns"] = out
    return _cache["spawns"].get(map_name_)


def flypoint_flag(name):
    """A FLYPOINT_* constant's value (include/constants/flypoints.h; some names there are off by one town:
    the Cianwood row of gMapFlypointParams says FLYPOINT_GOLDENROD)."""
    txt = _read("include", "constants", "flypoints.h").decode()
    m = re.search(r"#define\s+%s\s+(\d+)" % re.escape(name), txt)
    return int(m.group(1)) if m else None


def fly_destination(map_name_):
    """The fly point whose flown-to (or named) map is `map_name_`; None if no fly point lands there."""
    pts = fly_points()
    return next((p for p in pts if p["map"] == map_name_), None) or next((p for p in pts if p["name"] == map_name_), None)


def fly_start_block(map_name_, x, z, spawn_warp=None):
    """The fly map's first cursor block (FieldSystem_InitPokegearArgs, src/unk_02092BE8.c:40-61, fly_map.c:111-116):
    on the overworld matrix the player's block (x / 32, z / 32); elsewhere the map header's worldMapX/Y when set, else
    the special spawn warp's block (`spawn_warp` = (x, z), the save's escape point)."""
    h = map_headers().get(map_name_)
    if h is None or h["main"]:
        return x // BLOCK, z // BLOCK
    if h["worldMapX"] or h["worldMapY"]:
        return h["worldMapX"], h["worldMapY"]
    if spawn_warp is None:
        return None
    return spawn_warp[0] // BLOCK, spawn_warp[1] // BLOCK


def region(bx, by):
    """Pokegear_RegionFromCoords (src/application/pokegear/main/overlay_100_021E5900.c:167-181) of an overworld block:
    "johto", "kanto" or "indigo"."""
    if bx > 21:
        if bx == 25 and by == 8:
            return "johto"  # Mt. Silver
        if (bx == 28 and by == 6) or (bx == 28 and 8 < by < 13):
            return "indigo"  # Indigo Plateau, Victory Road
        return "kanto"
    return "johto"


def fly_allowed_from(point, here_region, bx, by):
    """Whether the fly map takes the fly point `point` hovered at block (bx, by) when the player is in `here_region`
    (ov101_021EA804/ov101_021EA7E4, src/application/pokegear/map/overlay_101_021E9270.c:704-719: the Indigo Plateau
    and Route 26 always, else only within the player's region unless the player is at Indigo)."""
    if point["map"] in ("MAP_INDIGO_PLATEAU", "MAP_ROUTE_26"):
        return True
    return here_region == "indigo" or region(bx, by) == here_region


# ---------------------------------------------------------------- the overworld tiles (tools/hg_map.py's readers)
def overworld():
    """(width, height, headers, land data ids) of map matrix 0, the overworld."""
    if "matrix" not in _cache:
        m = hg_map.matrix(0)
        _cache["matrix"] = (m["w"], m["h"], m["headers"], m["models"])
    return _cache["matrix"]


def block_attrs(bx, bz):
    """The 32x32 u16 tile attributes of overworld block (bx, bz), or None outside the matrix / an empty block."""
    w, h, _, models = overworld()
    if not (0 <= bx < w and 0 <= bz < h):
        return None
    key = ("attrs", bx, bz)
    if key not in _cache:
        mid = models[bz * w + bx]
        _cache[key] = hg_map.land_attrs(mid) if mid < len(hg_map.land_members()) else None
    return _cache[key]


def tile(x, z):
    """The u16 tile attribute at world tile (x, z), or None."""
    attrs = block_attrs(x // BLOCK, z // BLOCK)
    return None if attrs is None else attrs[(z % BLOCK) * BLOCK + x % BLOCK]


def block_map(x, z):
    """The map header name of the overworld block holding world tile (x, z)."""
    w, h, headers, _ = overworld()
    bx, bz = x // BLOCK, z // BLOCK
    if headers is None or not (0 <= bx < w and 0 <= bz < h):
        return None
    return map_name(headers[bz * w + bx])


def glyph(t):
    if t is None:
        return " "
    b = t & 0xFF
    if b == WHIRLPOOL:
        return "@"
    if b == WATERFALL:
        return "|"
    if b in SURFABLE:
        return "~"
    if b in JUMPS:
        return {0x38: ">", 0x39: "<", 0x3A: "^", 0x3B: "v"}[b]
    if t & COLLISION:
        return "#"
    if b in (0x02, 0x03):
        return '"'
    return "."


def passable(t, surf):
    if t is None:
        return False
    b = t & 0xFF
    if b == WHIRLPOOL:
        return False
    if b in SURFABLE or b == WATERFALL:
        return surf
    return not t & COLLISION


def route(start, goal, surf=False, avoid=()):
    """A* over the static tiles (4-neighbour, ledges as one-way jumps, land/water both walkable with surf), kept on
    blocks that belong to a map (the matrix's MAP_EVERYWHERE filler blocks, e.g. the open sea west of Route 40, are
    left out); the path as a list of tiles, or None."""
    avoid = set(avoid)

    def h(p):
        return abs(p[0] - goal[0]) + abs(p[1] - goal[1])

    seen = {start: None}
    cost = {start: 0}
    q = [(h(start), 0, start)]
    while q:
        _, c, p = heapq.heappop(q)
        if p == goal:
            path = [p]
            while seen[path[-1]] is not None:
                path.append(seen[path[-1]])
            return path[::-1]
        if c > cost[p]:
            continue
        for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            n = (p[0] + dx, p[1] + dz)
            if n in avoid:
                continue
            t = tile(*n)
            if t is not None and (t & 0xFF) in JUMPS:
                if JUMPS[t & 0xFF] != (dx, dz):
                    continue
                n = (n[0] + dx, n[1] + dz)
                t = tile(*n)
            if (not passable(t, surf) or block_map(*n) in (None, "MAP_EVERYWHERE")) and n != goal:
                continue
            nc = c + 1
            if nc < cost.get(n, 1 << 30):
                cost[n] = nc
                seen[n] = p
                heapq.heappush(q, (nc + h(n), nc, n))
    return None


def corners(path):
    """The path's turning points (and its ends)."""
    out = [path[0]]
    for i in range(1, len(path) - 1):
        a, b, c = path[i - 1], path[i], path[i + 1]
        if (b[0] - a[0], b[1] - a[1]) != (c[0] - b[0], c[1] - b[1]):
            out.append(b)
    out.append(path[-1])
    return out


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    cmd = argv[0]
    if cmd == "fly":
        for p in fly_points():
            flag = flypoint_flag(p["flypoint"])
            sp = fly_landing(p["map"])
            land = "%s (%d,%d)" % sp if sp else "-"
            print("%-24s -> %-38s %-22s flag %2s  cursor x %d..%d y %d..%d  lands %s" % (
                p["name"], p["map"], p["flypoint"], flag, p["x"], p["x"] + p["w"] - 1, p["y"], p["y"] + p["h"] - 1,
                land))
        return 0
    if cmd == "grid":
        x0, z0, x1, z1 = (int(v) for v in argv[1:5])
        print("      " + "".join(str((x // 10) % 10) if x % 10 == 0 else " " for x in range(x0, x1 + 1)))
        for z in range(z0, z1 + 1):
            print("%5d " % z + "".join(glyph(tile(x, z)) for x in range(x0, x1 + 1)) + "  " + (block_map(x0, z) or ""))
        print("legend: . floor  # blocked  \" grass  ~ surfable  @ whirlpool  | waterfall  ><^v ledges")
        return 0
    if cmd == "route":
        x0, z0, x1, z1 = (int(v) for v in argv[1:5])
        surf = "--surf" in argv
        avoid = []
        if "--avoid" in argv:
            for v in argv[argv.index("--avoid") + 1:]:
                if v.startswith("--"):
                    break
                a, b = v.split(",")
                avoid.append((int(a), int(b)))
        path = route((x0, z0), (x1, z1), surf, avoid)
        if path is None:
            print("no route")
            return 1
        print("%d steps; corners: %s" % (len(path) - 1, " ".join("(%d,%d)" % c for c in corners(path))))
        return 0
    print("unknown command %r" % cmd, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
