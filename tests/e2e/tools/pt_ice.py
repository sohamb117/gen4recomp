#!/usr/bin/env python3
"""Solve Platinum's Snowpoint Gym ice (the slopes and snowballs) offline and print a `slide` step's dirs.

    python3 tests/e2e/tools/pt_ice.py [--heights] [--from X Z] [--to X Z ...]

The rules are the game's (src/player_move.c PlayerAvatar_TileMove_Ice and the ice speed helpers below it,
src/overlay005/ov5_021DFB54.c ov5_021E06A8):
- a press from a standstill walks one tile; arriving on an ICE tile the slide starts at speed 0;
- on an ICE tile the slide continues in the moving direction; the height 4 units ahead of the tile centre against
  the centre (PlayerAvatar_CheckIceHeightChange) says downhill (speed + 1, at most 3) or uphill (speed - 1; below 0
  the slide turns back);
- the next tile blocked (collision bit, a BLOCK_* behaviour, an object, a height step of 20 or more) ends the slide;
  blocked going uphill, it turns back instead;
- a snowball (OBJ_EVENT_GFX_SNOWBALL) met while sliding at speed 1 or more is smashed and the slide goes on;
- a non-ICE tile ends the slide.
Heights are the land data's BDHC plates (src/overlay005/bdhc.c CalculateObjectHeight). Trainers' sight is not
modelled: the slide bot fights whoever spots the player and the route is checked in game.
"""
import argparse
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pt_map  # noqa: E402

F = 4096
ICE, BLOCK_NS, BLOCK_EW = 0x20, 0x49, 0x4A
D = {"UP": (0, -1), "DOWN": (0, 1), "LEFT": (-1, 0), "RIGHT": (1, 0)}
OPP = {"UP": "DOWN", "DOWN": "UP", "LEFT": "RIGHT", "RIGHT": "LEFT"}


class Gym:
    def __init__(self, header="MAP_HEADER_SNOWPOINT_CITY_GYM"):
        h = pt_map.header_fields(header)
        m = json.load(open(os.path.join(pt_map.PT, "res", "field", "matrices", "%s.json" % h["mapMatrixID"])))
        self.member = int(m["maps"][0][0].split("_")[1])
        self.land = pt_map.land(self.member)
        self._bdhc()
        ev = json.load(open(os.path.join(pt_map.PT, "res", "field", "events", "%s.json" % h["eventsArchiveID"])))
        self.snowballs = {}
        self.people = set()
        for o in ev["object_events"]:
            if o["graphics_id"] == "OBJ_EVENT_GFX_SNOWBALL":
                self.snowballs[(o["x"], o["z"])] = len(self.snowballs)
            else:
                self.people.add((o["x"], o["z"]))

    def _bdhc(self):
        raw = open(os.path.join(pt_map.PT, "res", "field", "maps", "data", "map_data_%03d.bin" % self.member), "rb").read()
        a, p, mo, b = struct.unpack("<4I", raw[:16])
        bd = raw[16 + a + p + mo:16 + a + p + mo + b]
        pc, nc, cc, plc = struct.unpack("<4H", bd[4:12])
        o = 16
        self.pts = [struct.unpack("<2i", bd[o + 8 * i:o + 8 * i + 8]) for i in range(pc)]
        o += 8 * pc
        self.nor = [struct.unpack("<3i", bd[o + 12 * i:o + 12 * i + 12]) for i in range(nc)]
        o += 12 * nc
        self.con = [struct.unpack("<i", bd[o + 4 * i:o + 4 * i + 4])[0] for i in range(cc)]
        o += 4 * cc
        self.plates = [struct.unpack("<4H", bd[o + 8 * i:o + 8 * i + 8]) for i in range(plc)]

    def height(self, x, z, cur=0):
        """BDHC height (fx32) at block-local fx32 (x, z): the candidate plate nearest the current height."""
        c = []
        for p1, p2, ni, ci in self.plates:
            (x1, z1), (x2, z2) = self.pts[p1], self.pts[p2]
            if min(x1, x2) <= x <= max(x1, x2) and min(z1, z2) <= z <= max(z1, z2):
                nx, ny, nz = self.nor[ni]
                c.append(-(nx * x // F + nz * z // F + self.con[ci]) * F // ny)
        return min(c, key=lambda y: abs(y - cur)) if c else None

    def centre(self, x, z, dx=0, dz=0, ahead=0):
        return (((x % 32) * 16 + 8 - 256) * F + dx * ahead * F, ((z % 32) * 16 + 8 - 256) * F + dz * ahead * F)

    def tile_h(self, x, z, cur=0):
        return self.height(*self.centre(x, z), cur)

    def beh(self, x, z):
        return self.land[z * 32 + x] & 0xFF

    def coll(self, x, z):
        return not (0 <= x < 32 and 0 <= z < 32) or bool(self.land[z * 32 + x] & 0x8000)

    def blocked(self, x, z, d, broken, y):
        """Can the player at (x, z) (height y) not step in direction d? 'snow' when a snowball is in the way."""
        dx, dz = D[d]
        nx, nz = x + dx, z + dz
        if self.coll(nx, nz):
            return True
        cur, nxt = self.beh(x, z), self.beh(nx, nz)
        if dz and (cur == BLOCK_NS or nxt == BLOCK_NS):
            return True
        if dx and (cur == BLOCK_EW or nxt == BLOCK_EW):
            return True
        h = self.tile_h(nx, nz, y)
        if h is None or abs(h - y) >= 20 * F:
            return True
        if (nx, nz) in self.people:
            return True
        if (nx, nz) in self.snowballs and self.snowballs[(nx, nz)] not in broken:
            return "snow"
        return False

    def press(self, x, z, d, broken):
        """One press from a standstill: (x, z, broken) after the walk and any slide, or None if nothing moves."""
        y = self.tile_h(x, z)
        if self.blocked(x, z, d, broken, y):
            return None
        dx, dz = D[d]
        x, z = x + dx, z + dz
        speed = 0
        for _ in range(64):
            if self.beh(x, z) != ICE:
                return x, z, broken
            y = self.tile_h(x, z, y)
            ahead = self.height(*self.centre(x, z, dx, dz, 4), y)
            change = 0 if ahead is None or ahead == y else (1 if ahead > y else -1)
            b = self.blocked(x, z, d, broken, y)
            if b == "snow" and speed >= 1:
                # smashed at the end of the step onto (x, z), before this tile's speed update
                broken = broken | {self.snowballs[(x + dx, z + dz)]}
                b = False
            if b:
                if change != 1:
                    return x, z, broken
                return self._back(x, z, d, broken, y)
            if change == 1:
                speed -= 1
                if speed < 0:
                    return self._back(x, z, d, broken, y)
            elif change == -1:
                speed = min(speed + 1, 3)
            x, z = x + dx, z + dz
        return None

    def _back(self, x, z, d, broken, y):
        """Blocked or out of speed going uphill: one slow step back down, then the tile behaviour is ignored, so
        the slide ends there (PlayerAvatar_SetIgnoreTileBehavior, player_move.c:562-576, :582-593)."""
        d = OPP[d]
        if self.blocked(x, z, d, broken, y):
            return x, z, broken
        dx, dz = D[d]
        return x + dx, z + dz, broken


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--heights", action="store_true", help="print the tile heights and behaviours")
    ap.add_argument("--from", dest="start", type=int, nargs=2, default=[11, 28])
    ap.add_argument("--to", type=int, nargs=2, action="append", help="goal tiles (default: next to Candice)")
    ap.add_argument("--after", default="", help="presses replayed from --from first (the snowballs they smash stay "
                                                 "smashed); the search starts where they end, e.g. the way back out")
    a = ap.parse_args()
    g = Gym()
    if a.heights:
        for z in range(32):
            row = []
            for x in range(23):
                if g.coll(x, z):
                    row.append("   #")
                    continue
                h = g.tile_h(x, z)
                k = {ICE: " ", BLOCK_NS: "|", BLOCK_EW: "-"}.get(g.beh(x, z), ".")
                if (x, z) in g.snowballs:
                    k = "o"
                elif (x, z) in g.people:
                    k = "P"
                row.append("%3d%s" % (h // F if h is not None else -1, k))
            print("%2d %s" % (z, "".join(row)))
        return
    goals = {tuple(t) for t in a.to} if a.to else {(11, 4), (10, 4), (12, 4)}
    start = (a.start[0], a.start[1], frozenset())
    for d in a.after.split():
        n = g.press(start[0], start[1], d, start[2])
        if n is None:
            sys.exit("--after: %s from (%d,%d) does not move" % (d, start[0], start[1]))
        start = (n[0], n[1], frozenset(n[2]))
    if a.after:
        print("# after --after: (%d,%d), snowballs smashed %s" % (start[0], start[1], sorted(start[2])))
    prev = {start: None}
    q = collections.deque([start])
    while q:
        s = q.popleft()
        if (s[0], s[1]) in goals:
            path = []
            while prev[s] is not None:
                s, d = prev[s]
                path.append(d)
            print(" ".join(reversed(path)))
            return
        for d in D:
            n = g.press(s[0], s[1], d, s[2])
            if n is None:
                continue
            n = (n[0], n[1], frozenset(n[2]))
            if n not in prev:
                prev[n] = (s, d)
                q.append(n)
    sys.exit("no route")


if __name__ == "__main__":
    main()
