#!/usr/bin/env python3
"""Solve a Ruby/Sapphire/Emerald gym puzzle offline and print the `steps` bot route through it.

The static model (gba_world.py) and the probe's window show the floor, not the puzzle's moving parts. This searches
(x, y, elevation, puzzle state, the map's people) with the game's own rules, read from the decomp:

  rotating gates (Fortree Gym, Trick House 6; src/rotating_gate.c, the same tables in pokeruby and pokeemerald)
  - a step with no other collision asks the gates (field_player_avatar.c CheckForObjectEventCollision):
    CheckForRotatingGatePuzzleCollision, the first gate whose 4x4 box (gate x-2..x+1, y-2..y+1) holds the tile and
    whose sRotatingGate_RotationInfo<d> entry names an arm the gate has now (RotatingGate_HasArm), decides: it turns
    (the step goes on) when RotatingGate_CanRotate finds no map collision == 1 where its arms sweep
    (sRotatingGate_ArmPositions*Rotation), else the step is refused
  - orientations start at the puzzle config's (RotatingGate_ResetAllGateOrientations on entering the map)

  rotating-tile statues (Emerald's Mossdeep Gym; src/rotating_tile_puzzle.c)
  - a floor switch is a coord event whose script runs `moverotatingtileobjects N`: every object standing on an
    arrow metatile of colour N (METATILE_MossdeepGym_YellowArrow_Right + 8 * N + 0..3: right, down, left, up)
    moves one tile that way, unconditionally (MoveRotatingTileObjects)
  - warp panels (MB_MOSSDEEP_GYM_WARP and the map's other step-on warps to itself) move the player to the paired
    warp; coord events that warp elsewhere (WarpToEntrance) are never stepped on

  Strength boulders and Rock Smash rocks (Seafloor Cavern, Victory Road; field_player_avatar.c)
  - walking into an OBJ_EVENT_GFX_PUSHABLE_BOULDER pushes it one tile when the tile beyond has no collision, no
    object, no elevation mismatch for the boulder and is no non-animated door (TryPushBoulder); the player walks
    in place (PushBoulder_Move). FLAG_SYS_USE_STRENGTH clears on every map load (overworld.c), so the first push
    on a map is preceded by A on the boulder and YES (EventScript_StrengthBoulder)
  - an OBJ_EVENT_GFX_BREAKABLE_ROCK goes with A and YES (EventScript_RockSmash); both come back on a map load
  With boulders or rocks the route prints as milestone [[step]] blocks (steps, the Strength / Rock Smash prompts
  as interact + advance_text, each push as a held press).

The map's trainers are part of the state: one who sees the player (a straight line within its sight range, the
way it faces, clear of collision and people: trainer_see.c CheckTrainer / CheckPathBetweenTrainerAndPlayer)
walks up to the player and stays there after the battle. A battle costs BATTLE_COST tiles.

    python3 tests/e2e/tools/gba_puzzle.py --game emerald MAP_FORTREE_CITY_GYM 15 24 15 3
    python3 tests/e2e/tools/gba_puzzle.py --game emerald MAP_MOSSDEEP_CITY_GYM 6 34 23 8
"""
import argparse
import heapq
import json
import os
import re
import sys
from array import array

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gba_world  # noqa: E402

GATE_PUZZLES = {"MAP_FORTREE_CITY_GYM": "Fortree", "MAP_ROUTE110_TRICK_HOUSE_PUZZLE6": "TrickHouse"}
ARMS = {"GATE_ARM_NORTH": 0, "GATE_ARM_EAST": 1, "GATE_ARM_SOUTH": 2, "GATE_ARM_WEST": 3}
ACW, CW = 1, 2  # ROTATE_ANTICLOCKWISE, ROTATE_CLOCKWISE
DIRS = ((0, -1), (0, 1), (-1, 0), (1, 0))  # up, down, left, right (DIR_NORTH, _SOUTH, _WEST, _EAST)
DIR_NAMES = ("North", "South", "West", "East")
ARROW_DIRS = ((1, 0), (0, 1), (-1, 0), (0, -1))  # puzzle tiles 0..3: right, down, left, up
BATTLE_COST = 25
PUSH_COST, SMASH_COST = 2, 8
FACE = {"FACE_UP": (0,), "FACE_DOWN": (1,), "FACE_LEFT": (2,), "FACE_RIGHT": (3,)}
MB_MOSSDEEP_GYM_WARP = 0x0E


def _array(src, name):
    m = re.search(r"\b%s\s*\[[^\]]*\](?:\s*\[[^\]]*\])?\s*=\s*\{(.*?)\n\};" % re.escape(name), src, re.S)
    if not m:
        raise SystemExit("rotating_gate.c: no %s" % name)
    return re.sub(r"//[^\n]*", "", m.group(1))


def load_gate_rules(decomp_dir, puzzle):
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
    def __init__(self, game, map_name, hidden=()):
        self.world = w = gba_world.World(game)
        self.mid = w.map_id(map_name)
        self.m = m = w._map(self.mid)
        j = w._json[self.mid]
        self.gates = []
        if map_name in GATE_PUZZLES:
            self.gates, self.layout, self.info, self.armpos = load_gate_rules(w.dir, GATE_PUZZLES[map_name])
        # metatile ids (the statues' arrows), from the layout's blockdata as gba_world reads it
        blocks = array("H")
        with open(os.path.join(w.dir, m.layout["blockdata_filepath"]), "rb") as f:
            blocks.frombytes(f.read())
        if sys.byteorder != "little":
            blocks.byteswap()
        self.mt = [v & 0x3FF for v in blocks]
        self.arrow_base = self._metatile_label("METATILE_MossdeepGym_YellowArrow_Right")
        # people: (x, y, facing directions, sight range (0: never battles), kind, elevation); objects whose flag is
        # set (`hidden`, local ids) are not there
        self.people, self.kinds = [], []
        for i, o in enumerate(j.get("object_events", [])):
            r = int(o.get("trainer_sight_or_berry_tree_id") or 0)
            if o.get("trainer_type", "TRAINER_TYPE_NONE") in ("TRAINER_TYPE_NONE", 0, "0"):
                r = 0
            mt = o.get("movement_type", "")
            g = o.get("graphics_id", "")
            kind = "boulder" if g.endswith("PUSHABLE_BOULDER") else "rock" if g.endswith("BREAKABLE_ROCK") else "person"
            x, y = (-1, -1) if i + 1 in hidden else (o["x"], o["y"])
            self.people.append((x, y, next((v for k, v in FACE.items() if mt.endswith(k)), (0, 1, 2, 3)), r))
            self.kinds.append((kind, int(o.get("elevation", 0))))
        # coord events: floor switches (colour), warps away (never stepped on)
        with open(os.path.join(w.dir, "data", "maps", m.folder, "scripts.inc")) as f:
            scripts = f.read()
        self.switches, self.no_step = {}, set()
        for c in j.get("coord_events", []):
            body = re.search(r"^%s::\n(.*?)(?:^\S|\Z)" % re.escape(c.get("script", "")), scripts, re.S | re.M)
            body = body.group(1) if body else ""
            sw = re.search(r"moverotatingtileobjects\s+(\d+)", body)
            if sw:
                self.switches[(c["x"], c["y"])] = int(sw.group(1))
            elif re.search(r"\bwarp", body):
                self.no_step.add((c["x"], c["y"]))
        # step-on warps to this same map: (x, y) -> the paired warp's tile
        self.pads, self.goal = {}, None
        for x, y, _e, dmid, dk, _k in [r for recs in m.warp_at.values() for r in recs]:
            if dmid == self.mid and dk is not None and dk < len(m.warps):
                self.pads[(x, y)] = (m.warps[dk][0], m.warps[dk][1])

    def _metatile_label(self, name):
        try:
            with open(os.path.join(self.world.dir, "include", "constants", "metatile_labels.h")) as f:
                m = re.search(r"#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)" % name, f.read())
            return int(m.group(1), 0) if m else None
        except OSError:
            return None

    def coll(self, x, y):
        m = self.m
        return m.coll[y * m.w + x] if 0 <= x < m.w and 0 <= y < m.h else 1

    # ---------------------------------------------------------------- rotating gates
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

    # ---------------------------------------------------------------- statues
    def switch(self, colour, people):
        """MoveRotatingTileObjects(colour): the people on that colour's arrows move one tile."""
        if self.arrow_base is None:
            return people
        out = list(people)
        for i, (x, y, beaten) in enumerate(people):
            if not (0 <= x < self.m.w and 0 <= y < self.m.h):
                continue
            k = self.mt[y * self.m.w + x] - self.arrow_base
            if k < 0 or k // 8 != colour or k % 8 >= 4:
                continue
            dx, dy = ARROW_DIRS[k % 8]
            out[i] = (x + dx, y + dy, beaten)
        return tuple(out)

    # ---------------------------------------------------------------- trainers
    def spotted(self, x, y, people):
        """The people after the player stops on (x, y), and the battles that took: the first trainer (object
        order) still to fight who sees the player along a clear line walks up to the next tile and stays."""
        for i, (tx, ty, beaten) in enumerate(people):
            _, _, dirs, r = self.people[i]
            if beaten or not r:
                continue
            for d in dirs:
                dx, dy = DIRS[d]
                k = (x - tx) * dx + (y - ty) * dy
                if not (1 <= k <= r) or (tx + dx * k, ty + dy * k) != (x, y):
                    continue
                busy = {(a, b) for a, b, _ in people}
                if any(self.coll(tx + dx * n, ty + dy * n) or (tx + dx * n, ty + dy * n) in busy for n in range(1, k)):
                    continue
                t = list(people)
                t[i] = (tx + dx * (k - 1), ty + dy * (k - 1), True)
                return tuple(t), 1
        return people, 0

    # ---------------------------------------------------------------- search
    def arrive(self, x, y, e, orients, people):
        """What stopping on (x, y) does: a warp panel, a floor switch, a trainer. (state, battles) or None."""
        if (x, y) in self.pads:
            x, y = self.pads[(x, y)]
            return (x, y, e, orients, people), 0
        if (x, y) in self.switches:
            people = self.switch(self.switches[(x, y)], people)
            if (x, y) in {(a, b) for a, b, _ in people}:
                return None  # a statue onto the player: never planned
        people, b = self.spotted(x, y, people)
        return (x, y, e, orients, people), b

    def steps(self, state):
        """(direction, next state, tiles moved, battles) for each step from state."""
        m = self.m
        x, y, e, orients, people = state
        busy = {(a, b) for a, b, _ in people}
        bt = m.beh[y * m.w + x]
        for d, (dx, dy) in enumerate(DIRS):
            nx, ny = x + dx, y + dy
            if not (0 <= nx < m.w and 0 <= ny < m.h) or (nx, ny) in self.no_step:
                continue
            if (nx, ny) in busy:
                i = next(k for k, (a, b, _) in enumerate(people) if (a, b) == (nx, ny))
                kind, be = self.kinds[i]
                if kind == "rock":
                    t = list(people)
                    t[i] = (-1, -1, True)
                    yield d, (x, y, e, orients, tuple(t)), 0, 0, "smash"
                elif kind == "boulder":
                    bx, by = nx + dx, ny + dy
                    if not (0 <= bx < m.w and 0 <= by < m.h) or (bx, by) in busy:
                        continue
                    k = by * m.w + bx
                    te = m.elev[k]
                    if (m.coll[k] or m.beh[k] in gba_world.NONANIM_DOORS
                            or (be not in (0, 15) and te not in (0, 15) and te != be)):
                        continue
                    t = list(people)
                    t[i] = (bx, by, people[i][2])
                    yield d, (x, y, e, orients, tuple(t)), 0, 0, "push"
                continue
            j = ny * m.w + nx
            bn = m.beh[j]
            if gba_world.MB_JUMP.get(bn) == (dx, dy):  # ShouldJumpLedge, before the gates
                lx, ly = nx + dx, ny + dy
                if not (0 <= lx < m.w and 0 <= ly < m.h) or (lx, ly) in busy:
                    continue
                k = ly * m.w + lx
                if m.coll[k] or m.beh[k] in gba_world.FORBIDDEN:
                    continue
                ne = m.elev[k]
                a = self.arrive(lx, ly, e if ne in (0, 15) else ne, orients, people)
                if a:
                    yield d, a[0], 2, a[1], "walk"
                continue
            if (m.coll[j] or bt in gba_world._LEAVE_BLOCKED[d] or bn in gba_world._ENTER_BLOCKED[d]
                    or bn in gba_world.FORBIDDEN):
                continue
            if bn in gba_world.STEP_WARP[self.world.decomp] and (nx, ny) not in self.pads and (nx, ny) != self.goal:
                continue  # a warp off the map, unless it is where the route goes
            ne = m.elev[j]
            if e not in (0, 15) and ne not in (0, 15) and ne != e:
                continue
            ok, no = self.gate_step(d, nx, ny, orients) if self.gates else (True, orients)
            if ok:
                a = self.arrive(nx, ny, e if ne in (0, 15) else ne, no, people)
                if a:
                    yield d, a[0], 1, a[1], "walk"

    def solve(self, sx, sy, tx, ty):
        """Dijkstra from (sx, sy) to (tx, ty): the list of (direction, tiles, landed tile, action) moves, or
        None."""
        m = self.m
        self.goal = (tx, ty)
        e0 = m.elev[sy * m.w + sx]
        start = (sx, sy, 3 if e0 in (0, 15) else e0, tuple(g[3] for g in self.gates),
                 tuple((p[0], p[1], False) for p in self.people))
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
                    prev, d, n, act = parent[st]
                    moves.append((d, n, st[:2], act))
                    st = prev
                return moves[::-1]
            for d, nst, n, b, act in self.steps(st):
                nc = c + n + b * BATTLE_COST + (PUSH_COST if act == "push" else SMASH_COST if act == "smash" else 0)
                if nc < dist.get(nst, 1 << 60):
                    dist[nst] = nc
                    parent[nst] = (st, d, n, act)
                    heapq.heappush(heap, (nc, tick, nst))
                    tick += 1
        return None


def corners(sx, sy, moves):
    """The route's corners: one tile per straight run (a ledge's jump runs on in its direction); a warp panel's
    landing tile is a corner of its own, as the steps bot counts a tile it is carried to."""
    out, x, y, last = [], sx, sy, None
    for d, n, landed, _act in moves:
        ex, ey = x + DIRS[d][0] * n, y + DIRS[d][1] * n
        if last is not None and d != last:
            out.append((x, y))
        if (ex, ey) != landed:  # warped: the panel stepped on, then where it put the player
            out.append((ex, ey))
            out.append(landed)
            last = None
        else:
            last = d
        x, y = landed
    if not out or out[-1] != (x, y):
        out.append((x, y))
    return out


FACE_NAMES = ("up", "down", "left", "right")


def blocks(sx, sy, moves):
    """The route as milestone [[step]] blocks: walks as `steps` corners, each push a held press (the first on the map
    after A + YES on the boulder), each rock A + YES."""
    out, walk, x, y, strength = [], [], sx, sy, False

    def flush():
        if walk:
            out.append({"do": "steps", "route": [list(t) for t in corners(walk[0][0], walk[0][1], walk[1:])]})
            walk.clear()

    for mv in moves:
        d, n, landed, act = mv
        if act == "walk":
            if not walk:
                walk.append((x, y))
            walk.append(mv)
            x, y = landed
            continue
        flush()
        if act == "smash" or not strength:
            out.append({"do": "steps", "route": [[x, y]], "face": FACE_NAMES[d], "interact": True,
                        "note": "Rock Smash: A + YES" if act == "smash" else "Strength: A on the boulder + YES"})
            out.append({"do": "advance_text"})
            strength = strength or act == "push"
        if act == "push":
            out.append({"do": "press", "keys": FACE_NAMES[d], "hold": 24, "gap": 40, "note": "push the boulder"})
    flush()
    return out


def toml_blocks(bl):
    lines = []
    for b in bl:
        lines.append("[[step]]")
        for k, v in b.items():
            lines.append("%s = %s" % (k, json.dumps(v) if not isinstance(v, bool) else ("true" if v else "false")))
        lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", default="emerald", choices=sorted(gba_world.DECOMPS))
    ap.add_argument("map")
    ap.add_argument("sx", type=int)
    ap.add_argument("sy", type=int)
    ap.add_argument("tx", type=int)
    ap.add_argument("ty", type=int)
    ap.add_argument("--hide", type=int, nargs="*", default=[], metavar="LOCAL_ID",
                    help="objects the save has hidden (their map.json flag set), by 1-based local id")
    a = ap.parse_args()
    p = Puzzle(a.game, a.map, hidden=set(a.hide))
    moves = p.solve(a.sx, a.sy, a.tx, a.ty)
    if moves is None:
        raise SystemExit("no route from (%d,%d) to (%d,%d) on %s" % (a.sx, a.sy, a.tx, a.ty, a.map))
    if any(act != "walk" for *_, act in moves):
        print("# %d moves (%d pushes, %d rocks)" % (len(moves), sum(m[3] == "push" for m in moves),
                                                  sum(m[3] == "smash" for m in moves)))
        print(toml_blocks(blocks(a.sx, a.sy, moves)))
        return
    route = corners(a.sx, a.sy, moves)
    print("# %d tiles, %d corners; gates (x, y, shape, orientation) %s; switches %s; panels %s" % (
        sum(m[1] for m in moves), len(route), p.gates, p.switches, p.pads))
    print("route = %s" % json.dumps([list(t) for t in route]))


if __name__ == "__main__":
    main()
