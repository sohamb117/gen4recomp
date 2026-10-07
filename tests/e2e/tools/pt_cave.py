#!/usr/bin/env python3
"""Plan a Platinum walk through a cave of several maps joined by warps (Victory Road) and print walk_to steps.

    python3 tests/e2e/tools/pt_cave.py MAP_HEADER_VICTORY_ROAD_1F 15,77 MAP_HEADER_POKEMON_LEAGUE 853,582 \
        --maps MAP_HEADER_VICTORY_ROAD --climb --smash [--open FLAG ...] [--free X,Z@MAP ...]

pt_route.py's model of one map (collision, ledges, bridge decks entered from their ends, the BDHC plates' heights:
a step of 20+ is a wall, Rock Climb walls along their axis, Rock Smash rocks with --smash, fixed objects and Strength
boulders block) joined across the maps' warps, as pt_warps.py joins them: stepping onto (or into) a warp tile lands on
the destination warp's tile, at the plate there nearest the ground. Only maps whose header starts with one of --maps
are entered, plus the goal map. --free X,Z@MAP lets a tile through (a boulder pushed out of the way, a trainer who
steps aside). Prints one walk_to step per map leg (via = the route's corners, the leg's last tile the warp).
"""
import argparse
import heapq
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import pt_gym  # noqa: E402
import pt_map  # noqa: E402
from np_e2e import behaviors  # noqa: E402

D = [(0, -1), (0, 1), (-1, 0), (1, 0)]
T = pt_map.TILES
CLIMB = {0x4B: (D[0], D[1]), 0x4C: (D[2], D[3])}


class Map:
    def __init__(self, name, a, b):
        h = pt_map.header_fields(name)
        m = json.load(open(os.path.join(pt_map.PT, "res", "field", "matrices", "%s.json" % h["mapMatrixID"])))
        self.name, self.a = name, a
        self.maps, self.headers = m["maps"], m.get("headers")
        self.land, self.plates = {}, {}
        ev = json.load(open(os.path.join(pt_map.PT, "res", "field", "events", "%s.json" % h["eventsArchiveID"])))
        self.warps = [(w["x"], w["z"], w["dest_header_id"], w["dest_warp_id"]) for w in ev.get("warp_events", [])]
        self.warp_at = {}
        for x, z, dm, dw in self.warps:
            self.warp_at.setdefault((x, z), (dm, dw))
        self.fixed = set()
        for o in ev.get("object_events", []):
            g = o.get("graphics_id", "")
            if o.get("hidden_flag") in a.open or "ROCK_SMASH" in g and a.smash or "CUT_TREE" in g and a.cut:
                continue
            if (o["x"], o["z"]) in a.free.get(name, ()):
                continue
            if "WANDER" in o.get("movement_type", "") or "WALK" in o.get("movement_type", ""):
                continue
            self.fixed.add((o["x"], o["z"]))
        self.fixed |= a.block.get(name, set())
        self.bridge = {v for k, v in b.items() if "BRIDGE" in k}
        self.bridge_start = b.get("BRIDGE_START")
        self.waterfall = b["WATERFALL"]
        self.water = {v for k, v in b.items() if k.startswith("WATER") or k in ("WATERFALL", "DEEP_WATER")}
        self.slopes = {b.get("BIKE_SLOPE_TOP"), b.get("BIKE_SLOPE_BOTTOM")}
        self.jump = {b["JUMP_NORTH"]: D[0], b["JUMP_SOUTH"]: D[1], b["JUMP_WEST"]: D[2], b["JUMP_EAST"]: D[3]}
        self.vertical = {}

    def cell(self, x, z):
        if x < 0 or z < 0 or z // T >= len(self.maps) or x // T >= len(self.maps[0]):
            return None
        c = self.maps[z // T][x // T]
        if c == "MAP_NONE" or (self.headers and self.headers[z // T][x // T] not in (self.name, "MAP_HEADER_NONE")
                               and self.a.strict):
            return None
        return c

    def tile(self, x, z):
        c = self.cell(x, z)
        if c is None:
            return 0x8000
        if c not in self.land:
            self.land[c] = pt_map.land(int(c.split("_")[1]))
        return self.land[c][(z % T) * T + x % T]

    def heights(self, x, z):
        c = self.cell(x, z)
        if c is None:
            return []
        if c not in self.plates:
            self.plates[c] = pt_gym.bdhc_heights(int(c.split("_")[1]))
        return self.plates[c][(x % T, z % T)]

    def height(self, x, z, h):
        hs = self.heights(x, z)
        return min(hs, key=lambda v: abs(v - h)) if hs else None

    def beh(self, x, z):
        return self.tile(x, z) & 0xFF

    def solid(self, x, z):
        if self.a.waterfall and self.beh(x, z) == self.waterfall:
            return False
        if self.a.climb and self.beh(x, z) in CLIMB and (x, z) not in self.fixed:
            return False
        return (self.tile(x, z) & 0x8000 or (x, z) in self.fixed or self.beh(x, z) in self.slopes
                or (self.beh(x, z) in self.water and not self.a.surf))

    def deck(self, x, z):
        return self.beh(x, z) in self.bridge and not self.tile(x, z) & 0x8000

    def runs_ns(self, x, z):
        if (x, z) not in self.vertical:
            n = [0, 0]
            for i, (dx, dz) in enumerate(D):
                k = 1
                while self.deck(x + dx * k, z + dz * k):
                    k += 1
                n[i // 2] += k - 1
            self.vertical[(x, z)] = n[0] >= n[1]
        return self.vertical[(x, z)]

    def moves(self, x, z, high):
        """(dir, x, z, on a deck) of each step from (x, z); a warp tile is yielded whatever its collision."""
        for d, (dx, dz) in enumerate(D):
            nx, nz = x + dx, z + dz
            if (nx, nz) in self.warp_at:
                yield d, nx, nz, False
                continue
            if self.solid(nx, nz):
                continue
            b0, b1 = self.beh(x, z), self.beh(nx, nz)
            if self.a.climb and (b1 in CLIMB or b0 in CLIMB):
                if b1 in CLIMB and (dx, dz) not in CLIMB[b1] or b0 in CLIMB and (dx, dz) not in CLIMB[b0]:
                    continue
            if b1 in self.jump:
                if self.jump[b1] == (dx, dz) and not self.solid(nx + dx, nz + dz):
                    yield d, nx + dx, nz + dz, False
                continue
            if high:
                if (dx == 0) == self.runs_ns(x, z):
                    yield d, nx, nz, self.deck(nx, nz)
                elif b0 == self.bridge_start and not self.deck(nx, nz):
                    yield d, nx, nz, False  # off a deck's end tile sideways (Victory Road 1F (22,48) onto Rock Climb)
            elif self.deck(nx, nz) and (dx == 0) == self.runs_ns(nx, nz):
                if not self.deck(x, z):
                    yield d, nx, nz, True
            else:
                yield d, nx, nz, False


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("start_map")
    ap.add_argument("start")
    ap.add_argument("goal_map")
    ap.add_argument("goal")
    ap.add_argument("--maps", nargs="+", required=True, help="header prefixes the walk may enter")
    ap.add_argument("--open", nargs="*", default=[], help="hidden flags of objects that are gone")
    ap.add_argument("--free", nargs="*", default=[], help="X,Z@MAP tiles that do not block")
    ap.add_argument("--block", nargs="*", default=[], help="X,Z@MAP tiles that block (where a boulder was pushed)")
    ap.add_argument("--cut", action="store_true")
    ap.add_argument("--surf", action="store_true")
    ap.add_argument("--waterfall", action="store_true", help="waterfalls climb and descend (HM07)")
    ap.add_argument("--climb", action="store_true")
    ap.add_argument("--smash", action="store_true")
    ap.add_argument("--no-heights", dest="heights", action="store_false")
    ap.add_argument("--strict", action="store_true", help="matrix cells of other headers are walls")
    ap.add_argument("--show", nargs="*", default=[], help="maps to draw with the tiles reached ('o'), when no path")
    a = ap.parse_args()
    for k in ("free", "block"):
        tiles = {}
        for f in getattr(a, k):
            xz, m = f.split("@")
            tiles.setdefault(m, set()).add(tuple(map(int, xz.split(","))))
        setattr(a, k, tiles)
    a.open = set(a.open)
    b = behaviors()
    loaded = {}

    def get(name):
        if name not in loaded:
            loaded[name] = Map(name, a, b)
        return loaded[name]

    def ground(m, x, z):
        hs = m.heights(x, z)
        return min(hs, key=abs) if hs else 0

    sm = get(a.start_map)
    sx, sz = map(int, a.start.split(","))
    gx, gz = map(int, a.goal.split(","))
    first = (a.start_map, sx, sz, False, -1, ground(sm, sx, sz) if a.heights else 0)
    dist, prev, q, end = {first: 0}, {first: None}, [(0, first)], None
    while q:
        c, u = heapq.heappop(q)
        if c > dist[u]:
            continue
        name, x, z, hi, ld, h = u
        if name == a.goal_map and (x, z) == (gx, gz):
            end = u
            break
        m = get(name)
        for d, nx, nz, nhi in m.moves(x, z, hi):
            if (nx, nz) in m.warp_at:
                dm, dw = m.warp_at[(nx, nz)]
                if not (dm == a.goal_map or any(dm.startswith(p) for p in a.maps)):
                    continue
                t = get(dm)
                if dw >= len(t.warps):
                    continue
                wx, wz = t.warps[dw][:2]
                v = (dm, wx, wz, False, -2 - d, ground(t, wx, wz) if a.heights else 0)
                w = c + 1
                if w < dist.get(v, 1e9):
                    dist[v], prev[v] = w, (u, (nx, nz))
                    heapq.heappush(q, (w, v))
                continue
            nh = 0
            if a.heights:
                nh = m.height(nx, nz, h)
                jumped = abs(nx - x) + abs(nz - z) > 1
                climbing = (a.climb and (m.beh(nx, nz) in CLIMB or m.beh(x, z) in CLIMB)
                            or a.waterfall and m.waterfall in (m.beh(nx, nz), m.beh(x, z)))
                if nh is None and not climbing:
                    continue
                if not jumped and not climbing and abs(nh - h) >= 20:
                    continue
                if nh is None:
                    nh = h
            v = (name, nx, nz, nhi, d, nh)
            w = c + 1 + (0.3 if d != ld else 0)
            if w < dist.get(v, 1e9):
                dist[v], prev[v] = w, (u, None)
                heapq.heappush(q, (w, v))
    if end is None:
        seen = {}
        for v in dist:
            seen.setdefault(v[0], set()).add((v[1], v[2]))
        for name in a.show:
            m, s = get(name), seen.get(name, set())
            xs = [p[0] for p in s] or [0]
            zs = [p[1] for p in s] or [0]
            print(name)
            x1 = max(xs) + 4
            print("    " + "".join(str(x // 10 % 10) for x in range(x1)))
            print("    " + "".join(str(x % 10) for x in range(x1)))
            for z in range(max(0, min(zs) - 3), max(zs) + 4):
                print("%3d " % z + "".join(
                    "o" if (x, z) in s else "W" if (x, z) in m.warp_at else "B" if (x, z) in m.fixed else
                    "r" if m.beh(x, z) in CLIMB else "#" if m.solid(x, z) else "." for x in range(x1)))
        sys.exit("no path; reached: %s" % {k: len(s) for k, s in seen.items()})
    path, warps = [], {}
    n = end
    while n is not None:
        path.append(n)
        p = prev[n]
        if p is None:
            break
        n, w = p
        if w is not None:
            warps[len(path) - 1] = w
    path.reverse()
    k = len(path)
    wpos = {k - 1 - i: w for i, w in warps.items()}  # index of the tile the warp was taken from
    leg = []
    for i, p in enumerate(path):
        leg.append(p)
        if i + 1 in wpos or i == len(path) - 1:
            # leg ends: through a warp at path[i] -> path[i+1] (the warp tile is wpos[i+1]), or at the goal
            corners = [q[1:3] for j, q in enumerate(leg[1:-1], 1) if leg[j + 1][4] != q[4] or leg[j + 1][3] != q[3]]
            last = wpos[i + 1] if i + 1 in wpos else p[1:3]
            if i + 1 in wpos:
                corners.append(p[1:3])
            print("# %s: %d steps%s" % (p[0], len(leg), (", warp (%d,%d) -> %s" % (last + (path[i + 1][0],)))
                                         if i + 1 in wpos else ""))
            print("via = [%s]" % ", ".join("[%d, %d]" % c for c in corners))
            print("x = %d\nz = %d\n" % last)
            leg = []


if __name__ == "__main__":
    main()
