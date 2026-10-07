#!/usr/bin/env python3
"""Plan a Platinum walk over a map matrix and print walk_to `via` waypoints (the route's corners).

walk_to's A* sees the probe's 64x64 window and no elevation, so it wanders on long routes and cannot tell a
bridge deck from the path under it (Route 215). This plans offline over the land data the way the game moves:
collision, ledges (one-way jumps), water (unless --surf), bike slopes (never on foot), and bridges -- a deck is
entered from its ends and walked along, the path under it crosses it sideways. Objects that never move (cut
trees, rocks, signs, item balls, berry soil, NONE-movement people) of the named headers block; --cut lets cut
trees through. Usage, from the repo root:

    python3 tests/e2e/tools/pt_route.py 560,669 671,598 MAP_HEADER_ROUTE_210_SOUTH MAP_HEADER_ROUTE_215
"""
import argparse
import heapq
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import pt_map  # noqa: E402
from np_e2e import behaviors  # noqa: E402

D = [(0, -1), (0, 1), (-1, 0), (1, 0)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("start")
    ap.add_argument("goal")
    ap.add_argument("headers", nargs="*", help="maps whose fixed objects block")
    ap.add_argument("--matrix", default="map_matrix_000")
    ap.add_argument("--cut", action="store_true", help="cut trees do not block")
    ap.add_argument("--surf", action="store_true", help="water is passable")
    a = ap.parse_args()
    b = behaviors()
    T = pt_map.TILES
    maps = json.load(open(os.path.join(pt_map.PT, "res", "field", "matrices", "%s.json" % a.matrix)))["maps"]
    land = {}

    def tile(x, z):
        if x < 0 or z < 0 or z // T >= len(maps) or x // T >= len(maps[0]):
            return 0x8000
        c = maps[z // T][x // T]
        if c == "MAP_NONE":
            return 0x8000
        if c not in land:
            land[c] = pt_map.land(int(c.split("_")[1]))
        return land[c][(z % T) * T + x % T]

    fixed = set()
    for h in a.headers:
        ev = json.load(open(os.path.join(pt_map.PT, "res", "field", "events",
                                         "%s.json" % pt_map.header_fields(h)["eventsArchiveID"])))
        for o in ev.get("object_events", []):
            g = o.get("graphics_id", "")
            if "CUT_TREE" in g and a.cut:
                continue
            if any(k in g for k in ("CUT_TREE", "ROCK", "BERRY", "SIGN", "POKEBALL", "BOULDER")) \
                    or o.get("movement_type") == "MOVEMENT_TYPE_NONE":
                fixed.add((o["x"], o["z"]))
    bridge = {v for k, v in b.items() if "BRIDGE" in k}
    slopes = {b.get("BIKE_SLOPE_TOP"), b.get("BIKE_SLOPE_BOTTOM")}
    water = {v for k, v in b.items() if k.startswith("WATER") or k in ("WATERFALL", "DEEP_WATER")}
    jump = {b["JUMP_NORTH"]: D[0], b["JUMP_SOUTH"]: D[1], b["JUMP_WEST"]: D[2], b["JUMP_EAST"]: D[3]}
    grass = {b["TALL_GRASS"], b["VERY_TALL_GRASS"]}

    def beh(x, z):
        return tile(x, z) & 0xFF

    def solid(x, z):
        return tile(x, z) & 0x8000 or (x, z) in fixed or beh(x, z) in slopes or (beh(x, z) in water and not a.surf)

    def deck(x, z):
        return beh(x, z) in bridge and not tile(x, z) & 0x8000

    vertical = {}

    def runs_ns(x, z):
        """A bridge tile's axis: the longer of its N-S and E-W runs of bridge tiles."""
        if (x, z) not in vertical:
            n = [0, 0]
            for i, (dx, dz) in enumerate(D):
                k = 1
                while deck(x + dx * k, z + dz * k):
                    k += 1
                n[i // 2] += k - 1
            vertical[(x, z)] = n[0] >= n[1]
        return vertical[(x, z)]

    def moves(x, z, high):
        for d, (dx, dz) in enumerate(D):
            nx, nz = x + dx, z + dz
            if solid(nx, nz):
                continue
            if beh(nx, nz) in jump:
                if jump[beh(nx, nz)] == (dx, dz) and not solid(nx + dx, nz + dz):
                    yield d, nx + dx, nz + dz, False
                continue
            if high:  # on a deck: along it, off its end onto the ground
                if (dx == 0) == runs_ns(x, z):
                    yield d, nx, nz, deck(nx, nz)
            elif deck(nx, nz) and (dx == 0) == runs_ns(nx, nz):
                if not deck(x, z):
                    yield d, nx, nz, True  # up onto a deck from its end
                # under a deck, along its axis: no path there
            else:
                yield d, nx, nz, False  # ground, or across under a deck

    start = tuple(map(int, a.start.split(",")))
    goal = tuple(map(int, a.goal.split(",")))
    first = (start[0], start[1], False, -1)
    dist, prev, q, end = {first: 0}, {first: None}, [(0, first)], None
    while q:
        c, u = heapq.heappop(q)
        if c > dist[u]:
            continue
        if u[:2] == goal:
            end = u
            break
        for d, nx, nz, hi in moves(u[0], u[1], u[2]):
            v = (nx, nz, hi, d)
            # a turn costs a little (fewer corners, fewer waypoints); grass a little more (fewer wild battles)
            w = c + 1 + (0.3 if d != u[3] else 0) + (0.5 if beh(nx, nz) in grass else 0)
            if w < dist.get(v, 1e9):
                dist[v], prev[v] = w, u
                heapq.heappush(q, (w, v))
    if end is None:
        sys.exit("no path from %s to %s" % (a.start, a.goal))
    path = []
    while end:
        path.append(end)
        end = prev[end]
    path.reverse()
    corners = [p[:2] for i, p in enumerate(path[1:-1], 1) if path[i + 1][3] != p[3] or path[i + 1][2] != p[2]]
    print("# %d steps, %d on bridge decks" % (len(path) - 1, sum(1 for p in path if p[2])))
    print("via = [%s]" % ", ".join("[%d, %d]" % c for c in corners))
    print("x = %d\nz = %d" % goal)


if __name__ == "__main__":
    main()
