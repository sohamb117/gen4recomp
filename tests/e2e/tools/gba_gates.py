#!/usr/bin/env python3
"""Solve a Ruby/Sapphire/Emerald rotating-gate puzzle offline and print the `steps` bot route through it.

The Fortree Gym (and Trick House puzzle 6) turnstiles are sprites whose arms block the edges between tiles; the
static model (gba_world.py) and the probe's window show only the floor under them. This searches (x, y, gate
orientations) with the game's own rules, read from the decomp's src/rotating_gate.c (the same tables in pokeruby
and pokeemerald):

  - a step into (x, y) moving d is first the ordinary step (collision, elevation, ledges, the map's people:
    field_player_avatar.c CheckForObjectEventCollision); only a step with no collision asks the gates
  - CheckForRotatingGatePuzzleCollision: the first gate whose 4x4 box (gate x-2..x+1, y-2..y+1) holds (x, y) and
    whose sRotatingGate_RotationInfo<d> entry names an arm the gate has now (RotatingGate_HasArm) decides: it turns
    (clockwise or anticlockwise, the step goes on) when RotatingGate_CanRotate finds no map collision == 1 on the
    tiles its arms sweep (sRotatingGate_ArmPositions*Rotation), else the step is refused
  - orientations start at the puzzle config's (RotatingGate_ResetAllGateOrientations on entering the map)

The gym's trainers are part of the state: one who sees the player (a straight clear line within its sight
range, the way it faces: trainer_see.c CheckTrainer / CheckPathBetweenTrainerAndPlayer) walks up to the player and
stays there after the battle, so the route walks round where they end up. A battle costs BATTLE_COST tiles, so the
route avoids the ones the gates allow.

    python3 tests/e2e/tools/gba_gates.py --game emerald MAP_FORTREE_CITY_GYM 15 24 15 3
"""
import argparse
import heapq
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gba_world  # noqa: E402

PUZZLES = {"MAP_FORTREE_CITY_GYM": "Fortree", "MAP_ROUTE110_TRICK_HOUSE_PUZZLE6": "TrickHouse"}
ARMS = {"GATE_ARM_NORTH": 0, "GATE_ARM_EAST": 1, "GATE_ARM_SOUTH": 2, "GATE_ARM_WEST": 3}
ACW, CW = 1, 2  # ROTATE_ANTICLOCKWISE, ROTATE_CLOCKWISE
DIRS = ((0, -1), (0, 1), (-1, 0), (1, 0))  # up, down, left, right (DIR_NORTH, _SOUTH, _WEST, _EAST)
DIR_NAMES = ("North", "South", "West", "East")
BATTLE_COST = 25
FACE = {"FACE_UP": (0,), "FACE_DOWN": (1,), "FACE_LEFT": (2,), "FACE_RIGHT": (3,)}


def _array(src, name):
    m = re.search(r"\b%s\s*\[[^\]]*\](?:\s*\[[^\]]*\])?\s*=\s*\{(.*?)\n\};" % re.escape(name), src, re.S)
    if not m:
        raise SystemExit("rotating_gate.c: no %s" % name)
    return re.sub(r"//[^\n]*", "", m.group(1))


def load_rules(decomp_dir, puzzle):
    """The puzzle config and the gate tables of src/rotating_gate.c."""
    with open(os.path.join(decomp_dir, "src", "rotating_gate.c")) as f:
        src = f.read()
    shape_enum = re.search(r"enum\s*\{(.*?)\};", src[src.index("GATE_SHAPE_L1") - 400:], re.S).group(1)
    shapes = {n: i for i, n in enumerate(re.findall(r"\b(GATE_SHAPE_\w+)", re.sub(r"/\*.*?\*/", "", shape_enum, flags=re.S)))}
    gates = []
    for x, y, shape, orient in re.findall(r"\{\s*(\d+),\s*(\d+),\s*(GATE_SHAPE_\w+),\s*GATE_ORIENTATION_(\d+)\s*\}",
                                          _array(src, "sRotatingGate_%sPuzzleConfig" % puzzle)):
        gates.append((int(x), int(y), shapes[shape], int(orient) // 90))
    layout = [[int(v) for v in re.findall(r"\d", row)]
              for row in re.findall(r"\{([^{}]*)\}", _array(src, "sRotatingGate_ArmLayout"))]
    info = {}
    for d, name in enumerate(DIR_NAMES):
        cells = re.findall(r"GATE_ROT_NONE|GATE_ROT_(A?CW)\((GATE_ARM_\w+),\s*(\d)\)",
                           _array(src, "sRotatingGate_RotationInfo%s" % name))
        info[d] = [None if not rot else ((ACW if rot == "ACW" else CW), ARMS[arm] * 2 + int(long_))
                   for rot, arm, long_ in cells]
    pos = {}
    for rot, name in ((CW, "Clockwise"), (ACW, "AntiClockwise")):
        pos[rot] = [(int(a), int(b)) for a, b in
                    re.findall(r"\{\s*(-?\d+),\s*(-?\d+)\s*\}", _array(src, "sRotatingGate_ArmPositions%sRotation" % name))]
    return gates, layout, info, pos


class Puzzle:
    def __init__(self, game, map_name):
        self.world = gba_world.World(game)
        self.mid = self.world.map_id(map_name)
        self.m = self.world._map(self.mid)
        self.gates, self.layout, self.info, self.armpos = load_rules(self.world.dir, PUZZLES[map_name])
        ev = self.world._json[self.mid].get("object_events", [])
        self.people = set()   # people who never move
        self.trainers = []    # (x, y, facing directions, sight range)
        for o in ev:
            r = int(o.get("trainer_sight_or_berry_tree_id") or 0)
            if o.get("trainer_type", "TRAINER_TYPE_NONE") in ("TRAINER_TYPE_NONE", 0, "0") or r <= 0:
                self.people.add((o["x"], o["y"]))
                continue
            mt = o.get("movement_type", "")
            self.trainers.append((o["x"], o["y"], next((v for k, v in FACE.items() if mt.endswith(k)), (0, 1, 2, 3)), r))

    def coll(self, x, y):
        m = self.m
        return m.coll[y * m.w + x] if 0 <= x < m.w and 0 <= y < m.h else 1

    def has_arm(self, g, orients, arm_info):
        """RotatingGate_HasArm."""
        arm, long_ = arm_info // 2, arm_info % 2
        return self.layout[self.gates[g][2]][((arm - orients[g] + 4) % 4) * 2 + long_]

    def can_rotate(self, g, orients, rot):
        """RotatingGate_CanRotate (the non-BUGFIX test: collision == 1)."""
        gx, gy, shape, _ = self.gates[g]
        for i in range(4):
            for j in range(2):
                if self.layout[shape][2 * i + j]:
                    dx, dy = self.armpos[rot][2 * ((orients[g] + i) % 4) + j]
                    if self.coll(gx + dx, gy + dy) == 1:
                        return False
        return True

    def gate_step(self, d, x, y, orients):
        """CheckForRotatingGatePuzzleCollision: (allowed, new orientations)."""
        for g, (gx, gy, _, _) in enumerate(self.gates):
            if gx - 2 <= x <= gx + 1 and gy - 2 <= y <= gy + 1:
                e = self.info[d][(y - gy + 2) * 4 + (x - gx + 2)]
                if e is None:
                    continue
                rot, arm_info = e
                if self.has_arm(g, orients, arm_info):
                    if not self.can_rotate(g, orients, rot):
                        return False, orients
                    o = list(orients)
                    o[g] = (o[g] + (1 if rot == CW else -1)) % 4
                    return True, tuple(o)
        return True, orients

    def spotted(self, x, y, tpos):
        """The trainers' positions after the player stops on (x, y), and the battles that took: the first trainer
        (object order) still to fight who sees the player along a clear line walks up to the next tile and stays."""
        for i, (tx, ty, beaten) in enumerate(tpos):
            if beaten:
                continue
            _, _, dirs, r = self.trainers[i]
            for d in dirs:
                dx, dy = DIRS[d]
                k = (x - tx) * dx + (y - ty) * dy
                if not (1 <= k <= r) or (tx + dx * k, ty + dy * k) != (x, y):
                    continue
                busy = self.people | {(a, b) for a, b, _ in tpos}
                if any(self.coll(tx + dx * n, ty + dy * n) or (tx + dx * n, ty + dy * n) in busy for n in range(1, k)):
                    continue
                t = list(tpos)
                t[i] = (tx + dx * (k - 1), ty + dy * (k - 1), True)
                return tuple(t), 1
        return tpos, 0

    def steps(self, state):
        """(direction, next state, tiles moved, battles) for each step from state = (x, y, e, orientations,
        trainer positions)."""
        m = self.m
        x, y, e, orients, tpos = state
        people = self.people | {(a, b) for a, b, _ in tpos}
        bt = m.beh[y * m.w + x]
        for d, (dx, dy) in enumerate(DIRS):
            nx, ny = x + dx, y + dy
            if not (0 <= nx < m.w and 0 <= ny < m.h) or (nx, ny) in people:
                continue
            j = ny * m.w + nx
            bn = m.beh[j]
            if gba_world.MB_JUMP.get(bn) == (dx, dy):  # ShouldJumpLedge, before the gates
                lx, ly = nx + dx, ny + dy
                if not (0 <= lx < m.w and 0 <= ly < m.h) or (lx, ly) in people:
                    continue
                k = ly * m.w + lx
                if m.coll[k] or m.beh[k] in gba_world.FORBIDDEN:
                    continue
                ne = m.elev[k]
                nt, b = self.spotted(lx, ly, tpos)
                yield d, (lx, ly, e if ne in (0, 15) else ne, orients, nt), 2, b
                continue
            if (m.coll[j] or bt in gba_world._LEAVE_BLOCKED[d] or bn in gba_world._ENTER_BLOCKED[d]
                    or bn in gba_world.FORBIDDEN or bn in gba_world.STEP_WARP[self.world.decomp]):
                continue
            ne = m.elev[j]
            if e not in (0, 15) and ne not in (0, 15) and ne != e:
                continue
            ok, no = self.gate_step(d, nx, ny, orients)
            if ok:
                nt, b = self.spotted(nx, ny, tpos)
                yield d, (nx, ny, e if ne in (0, 15) else ne, no, nt), 1, b

    def solve(self, sx, sy, tx, ty):
        """Dijkstra from (sx, sy) to (tx, ty): the list of (direction, tiles) moves, or None."""
        m = self.m
        e0 = m.elev[sy * m.w + sx]
        start = (sx, sy, 3 if e0 in (0, 15) else e0, tuple(g[3] for g in self.gates),
                 tuple((t[0], t[1], False) for t in self.trainers))
        dist, parent = {start: 0}, {start: None}
        heap = [(0, 0, start)]
        tick = 1
        while heap:
            c, _, st = heapq.heappop(heap)
            if c > dist[st]:
                continue
            if st[:2] == (tx, ty):
                moves = []
                while parent[st] is not None:
                    st, d, n = parent[st]
                    moves.append((d, n))
                return moves[::-1]
            for d, nst, n, b in self.steps(st):
                nc = c + n + b * BATTLE_COST
                if nc < dist.get(nst, 1 << 60):
                    dist[nst] = nc
                    parent[nst] = (st, d, n)
                    heapq.heappush(heap, (nc, tick, nst))
                    tick += 1
        return None


def corners(sx, sy, moves):
    """The route's corners: one tile per straight run (a ledge's jump runs on in its direction)."""
    out, x, y, last = [], sx, sy, None
    for d, n in moves:
        if last is not None and d != last:
            out.append((x, y))
        x, y = x + DIRS[d][0] * n, y + DIRS[d][1] * n
        last = d
    out.append((x, y))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", default="emerald", choices=sorted(gba_world.DECOMPS))
    ap.add_argument("map", choices=sorted(PUZZLES))
    ap.add_argument("sx", type=int)
    ap.add_argument("sy", type=int)
    ap.add_argument("tx", type=int)
    ap.add_argument("ty", type=int)
    a = ap.parse_args()
    p = Puzzle(a.game, a.map)
    moves = p.solve(a.sx, a.sy, a.tx, a.ty)
    if moves is None:
        raise SystemExit("no route from (%d,%d) to (%d,%d) through the gates" % (a.sx, a.sy, a.tx, a.ty))
    route = corners(a.sx, a.sy, moves)
    print("# %d tiles, %d corners; gates (x, y, shape, orientation) %s" % (sum(n for _, n in moves), len(route), p.gates))
    print("route = %s" % json.dumps([list(t) for t in route]))


if __name__ == "__main__":
    main()
