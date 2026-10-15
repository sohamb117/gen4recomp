#!/usr/bin/env python3
"""Plan a walk through a Platinum building of warp panels and stairs (Galactic HQ) and print walk_to steps.

    python3 tests/e2e/tools/pt_warps.py MAP_HEADER_GALACTIC_HQ_B2F 3 16 MAP_HEADER_GALACTIC_HQ_4F 8 11 \
        --maps MAP_HEADER_GALACTIC_HQ_ --open FLAG_HIDE_GALACTIC_HQ_B2F_DOOR ...

A breadth-first search over (map, tile): the land data's collision (pt_map), the maps' warps (stepping onto a warp
tile lands on the destination warp's tile; landing does not warp again) and their objects as obstacles where they
start (people, items, doors; walkers and wanderers are left to walk_to's replanning), except objects whose hidden flag is named by --open (a key door the walk opens,
a script's RemoveObject). Only maps whose header starts with one of --maps are entered. Each leg is one map: walk_to
the warp tile (or the goal), with `avoid` naming that map's other warp tiles so the A* never steps on a panel it
is not taking. --recipe takes the hidden flags a milestone's lab.recipe sets. A door crossed (--open) is listed so a talk step can open it first.
"""
import argparse
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pt_map  # noqa: E402

T = pt_map.TILES


ROCK_CLIMB = {0x4B, 0x4C}  # ROCK_CLIMB_N_S / _E_W (bots.ROCK_CLIMB)


class Map:
    def __init__(self, name, opened, hm=False, trainers=False):
        h = pt_map.header_fields(name)
        m = json.load(open(os.path.join(pt_map.PT, "res", "field", "matrices", "%s.json" % h["mapMatrixID"])))
        maps, headers = m["maps"], m.get("headers")
        self.name = name
        self.hm = hm
        self.cells = {}
        for cz, row in enumerate(maps):
            for cx, c in enumerate(row):
                if c != "MAP_NONE" and (not headers or headers[cz][cx] == name):
                    self.cells[(cx, cz)] = pt_map.land(int(c.split("_")[1]))
        ev = json.load(open(os.path.join(pt_map.PT, "res", "field", "events", "%s.json" % h["eventsArchiveID"])))
        self.warps = [(w["x"], w["z"], w["dest_header_id"], w["dest_warp_id"]) for w in ev.get("warp_events", [])]
        self.warp_at = {}
        for i, (x, z, dm, dw) in enumerate(self.warps):
            self.warp_at.setdefault((x, z), (dm, dw))
        self.blocks = {}
        for o in ev.get("object_events", []):
            if o.get("hidden_flag") in opened or "WANDER" in o.get("movement_type", "") \
                    or "WALK" in o.get("movement_type", ""):
                continue
            if trainers and o.get("trainer_type", "TRAINER_TYPE_NONE") != "TRAINER_TYPE_NONE":
                continue
            if hm and o.get("graphics_id") in ("OBJ_EVENT_GFX_ROCK_SMASH", "OBJ_EVENT_GFX_CUT_TREE"):
                continue
            self.blocks[(o["x"], o["z"])] = (o.get("id"), o.get("hidden_flag"))
        self.doors = {(o["x"], o["z"]): o.get("hidden_flag") for o in ev.get("object_events", [])
                      if o.get("hidden_flag") in opened and "DOOR" in o.get("graphics_id", "")}

    def free(self, x, z):
        land = self.cells.get((x // T, z // T))
        if land is None or x < 0 or z < 0:
            return False
        t = land[(z % T) * T + x % T]
        if self.hm and (t & 0xFF) in ROCK_CLIMB:
            return (x, z) not in self.blocks
        return not t & 0x8000 and (x, z) not in self.blocks


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("start_map")
    ap.add_argument("sx", type=int)
    ap.add_argument("sz", type=int)
    ap.add_argument("goal_map")
    ap.add_argument("gx", type=int)
    ap.add_argument("gz", type=int)
    ap.add_argument("--maps", nargs="+", required=True, help="header prefixes the walk may enter")
    ap.add_argument("--open", nargs="*", default=[], help="hidden flags of objects that are gone (opened doors)")
    ap.add_argument("--hm", action="store_true", help="Rock Climb walls and Rock Smash rocks / Cut trees pass "
                                                       "(walk_to hm = true); Strength boulders still block")
    ap.add_argument("--recipe", help="a lab.recipe: the flags it sets hide their objects too (the story so far)")
    ap.add_argument("--trainers", action="store_true", help="trainers do not block (fought where they stand: a walk "
                                                            "into one is a battle, then they stay beside the path)")
    a = ap.parse_args()
    if a.recipe:
        for line in open(a.recipe):
            w = line.split("#", 1)[0].split()
            if len(w) == 2 and w[0] == "flag":
                a.open.append(w[1])
            elif len(w) == 2 and w[0] == "clear-flag" and w[1] in a.open:
                a.open.remove(w[1])
    loaded = {}

    def get(name):
        if name not in loaded:
            loaded[name] = Map(name, set(a.open), a.hm, a.trainers)
        return loaded[name]

    start = (a.start_map, a.sx, a.sz)
    goal = (a.goal_map, a.gx, a.gz)
    prev = {start: None}
    q = collections.deque([start])
    while q:
        cur = q.popleft()
        if cur == goal:
            break
        name, x, z = cur
        m = get(name)
        for dx, dz in ((0, -1), (0, 1), (-1, 0), (1, 0)):
            nx, nz = x + dx, z + dz
            if (nx, nz) not in m.warp_at and not m.free(nx, nz):
                continue  # a door's warp tile has the collision bit: walking into it warps
            nxt = (name, nx, nz)
            if (nx, nz) in m.warp_at:
                dm, dw = m.warp_at[(nx, nz)]
                if not any(dm.startswith(p) for p in a.maps):
                    continue
                d = get(dm)
                if dw >= len(d.warps):
                    continue
                nxt = (dm, d.warps[dw][0], d.warps[dw][1])
                if nxt not in prev:
                    prev[nxt] = (cur, (nx, nz))
                    q.append(nxt)
                continue
            if nxt not in prev:
                prev[nxt] = (cur, None)
                q.append(nxt)
    if goal not in prev:
        sys.exit("no way from %s to %s" % (start, goal))
    path = []
    n = goal
    while prev[n] is not None:
        p, w = prev[n]
        path.append((p, n, w))
        n = p
    path.reverse()
    legs = []
    tiles = []
    for p, n, w in path:
        tiles.append((p[0], p[1], p[2]))
        if w is not None:
            legs.append((p[0], w, n[0], tiles))
            tiles = []
    legs.append((goal[0], (goal[1], goal[2]), None, tiles + [goal]))
    for name, (x, z), dest, walked in legs:
        m = get(name)
        doors = sorted({m.doors[(tx, tz)] for _, tx, tz in walked if (tx, tz) in m.doors})
        avoid = sorted({(wx, wz) for wx, wz, _, _ in m.warps} - {(x, z)} - {(walked[0][1], walked[0][2])})
        print("# %s -> (%d,%d)%s%s" % (name, x, z, " -> " + dest if dest else "",
                                      "; crosses the door " + ", ".join(doors) if doors else ""))
        print("[[step]]\ndo = \"walk_to\"\nx = %d\nz = %d%s" % (x, z, "\nhm = true" if a.hm else ""))
        if avoid:
            print("avoid = [%s]" % ", ".join("[%d, %d]" % t for t in avoid))
        if dest:
            print("\n[[step]]\ndo = \"wait_map\"\nmap = \"%s\"" % dest)
        print()


if __name__ == "__main__":
    main()
