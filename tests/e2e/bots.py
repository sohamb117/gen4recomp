"""The input bots a milestone's [[step]]s name (tests/e2e/README.md).

Each bot is a function bot(session, step, ctx) that drives the game through a
Session (np_e2e.py) until its goal holds, and raises HarnessError when it
cannot get there within its bound (`max` frames, default per bot). ctx is the
milestone's context: the game, the milestone directory, the name resolver.
"""
import heapq
import os

from np_e2e import (DIR_DELTA, DIR_KEYS, FACINGS, TILE_BEHAVIOR, TILE_COLLISION, TILE_KNOWN, UI_BATTLE_MENU,
                    UI_BATTLE_PARTY, HarnessError, behaviors)

# ---- the battle's touch screen (Platinum src/battle/battle_subscreen.c touch rects; D/P's overlay 11
# tables are byte-identical), as tap points: the centre of each button.
TAP_FIGHT = (128, 80)          # sActionMenuTouchRects[0]: y 0x18-0x90, full width
TAP_MOVES = [(64, 52), (192, 52), (64, 116), (192, 116)]  # sMoveSelectMenuTouchRects[1..4]
TAP_TARGET = (196, 44)         # sTargetSelectMenuTouchRects[1]: the opponent on the right
TAP_YES = (128, 68)            # sYesNoMenuTouchRects[0]: y 0x28-0x60
TAP_NO = (128, 140)            # sYesNoMenuTouchRects[1]: y 0x70-0xA8
# battle_party.c sPartyPokemonScreenTouchRects (slot i) and sSelectPokemonScreenTouchRects[SHIFT]
TAP_PARTY = [(64, 24), (192, 32), (64, 72), (192, 80), (64, 120), (192, 128)]
TAP_SHIFT = (128, 76)

# The battle menu config indices (core/include/np_e2e.h) and what auto_battle answers.
MENU_ACTION = range(1, 11)
MENU_MOVES, MENU_TARGET = 11, 12
MENU_ANSWER = {13: TAP_NO,   # YES/NO: give a nickname? / forfeit? -> NO
               14: TAP_NO,   # make it forget another move? -> NO
               15: TAP_YES,  # give up on learning the move? -> YES
               16: TAP_YES,  # use the next Pokemon? -> YES
               17: TAP_NO}   # switch Pokemon? (trainer about to send the next) -> NO


def _int(step, key, default):
    v = step.get(key, default)
    return int(v)


def _tap(s, xy, hold=4, gap=10):
    s.run(hold, touch=xy)
    s.run(gap)


# ---------------------------------------------------------------- simple input
def bot_press(s, step, ctx):
    keys = step["keys"]
    hold, gap, times = _int(step, "hold", 4), _int(step, "gap", 12), _int(step, "times", 1)
    for _ in range(times):
        s.run(hold, keys)
        s.run(gap)


def bot_tap(s, step, ctx):
    hold, gap, times = _int(step, "hold", 4), _int(step, "gap", 12), _int(step, "times", 1)
    for _ in range(times):
        _tap(s, (int(step["x"]), int(step["y"])), hold, gap)


def bot_wait_frames(s, step, ctx):
    s.run(_int(step, "n", 60))


def _wait(s, step, cond, default, what):
    if not s.run(_int(step, "max", default), until=cond):
        raise HarnessError("%s not reached in %d frames" % (what, _int(step, "max", default)))


def bot_wait_map(s, step, ctx):
    m = ctx.resolve(step["map"])
    if s.map_id != m:
        _wait(s, step, "map_id=%d" % m, 3000, "map %s (%d)" % (step["map"], m))


def bot_wait_field(s, step, ctx):
    if not s.field_ready:
        _wait(s, step, "field_ready=1", 3000, "field_ready=1")


def bot_wait_battle(s, step, ctx):
    if not s.in_battle:
        _wait(s, step, "in_battle=1", 1200, "in_battle=1")


def schedule_length(path):
    """The last frame a press file touches, relative to its start."""
    last, prev = 0, 0
    with open(path) as f:
        for line in f:
            for item in line.split("#", 1)[0].split(";"):
                item = item.strip()
                if not item:
                    continue
                rel = item.startswith("+")
                fields = item.lstrip("+").split(":")
                frame = int(fields[0]) + (prev if rel else 0)
                prev = frame
                rest = [int(v) for v in fields[2:] if v.strip().lstrip("-").isdigit()]
                if fields[1] == "tap":
                    rest = rest[2:]
                n = rest[0] if rest else 6
                end = frame + n
                if len(rest) >= 3:
                    end = frame + rest[1] * (rest[2] - 1) + n
                last = max(last, end)
    return last


def bot_schedule(s, step, ctx):
    path = os.path.join(ctx.dir, step["file"])
    s.sched(path)
    s.run(_int(step, "frames", schedule_length(path) + 1))


def bot_save(s, step, ctx):
    """The in-game save, through the host's quick-save request (np_core's quicksave_seq)."""
    bot_wait_field(s, {"max": step.get("max", 3000)}, ctx)
    seq = s.stat("quicksave_seq") + 1
    s.opt("quicksave_seq", seq)
    if not s.run(240, until="quicksave_seq=%d" % seq):
        raise HarnessError("the quick save request was not handled")
    if s.stat("quicksave_result") != 1:
        raise HarnessError("the quick save was refused or failed (result %d)" % s.stat("quicksave_result"))
    s.run(4)


# ---------------------------------------------------------------- advance_text
def bot_advance_text(s, step, ctx):
    """A with spacing until the player is free (or a battle starts, which auto_battle takes over)."""
    limit = s.frame + _int(step, "max", 3000)
    stop = ["field_ready=1"] + ([] if step.get("through_battle") else ["in_battle=1"])
    while not s.field_ready and (step.get("through_battle") or not s.in_battle):
        if s.frame >= limit:
            raise HarnessError("text did not end in %d frames" % _int(step, "max", 3000))
        if s.run(3, "a", until=stop) or s.run(17, until=stop):
            break
    s.run(2)


# ---------------------------------------------------------------- auto_battle
def bot_auto_battle(s, step, ctx):
    """FIGHT + one move each turn until the battle is over; prompts answered (MENU_ANSWER)."""
    move = _int(step, "move", 0)
    limit = s.frame + _int(step, "max", 30000)
    if not s.in_battle and not s.run(_int(step, "wait", 900), until="in_battle=1"):
        raise HarnessError("no battle started within %d frames" % _int(step, "wait", 900))
    party_try = 0
    turns = 0
    while s.in_battle:
        if s.frame >= limit:
            raise HarnessError("the battle did not end in %d frames" % _int(step, "max", 30000))
        p = s.probe()
        if p is not None and p.ui == UI_BATTLE_MENU:
            idx = p.ui_arg
            if idx in MENU_ACTION:
                _tap(s, TAP_FIGHT)
                turns += 1
                party_try = 0
            elif idx == MENU_MOVES:
                _tap(s, TAP_MOVES[move])
            elif idx == MENU_TARGET:
                _tap(s, TAP_TARGET)
            elif idx in MENU_ANSWER:
                _tap(s, MENU_ANSWER[idx])
            else:
                s.run(2, "b")
                s.run(10)
            continue
        if p is not None and p.ui == UI_BATTLE_PARTY:
            if p.ui_arg == 0:
                # the next slot each time: a fainted one says so and comes back here
                party_try = party_try % 5 + 1
                _tap(s, TAP_PARTY[party_try])
            else:
                _tap(s, TAP_SHIFT)
            continue
        # text, animations, the evolution scene: A advances text (B would cancel an evolution)
        if not s.run(16, until=["ui!=0", "in_battle=0"]):
            s.run(2, "a", until="in_battle=0")
    s.note("auto_battle: battle over after %d turns" % turns)


# ---------------------------------------------------------------- walk_to
class Terrain:
    """What walk_to knows about the map: the probe's window plus what walking taught it."""

    def __init__(self):
        b = behaviors()
        self.jump = {b["JUMP_NORTH"]: 0, b["JUMP_SOUTH"]: 1, b["JUMP_WEST"]: 2, b["JUMP_EAST"]: 3}
        self.block_into = {}  # behavior -> directions that cannot enter the tile
        for name, dirs in (("BLOCK_EASTWARD", (3,)), ("BLOCK_WESTWARD", (2,)), ("BLOCK_NORTHWARD", (0,)),
                           ("BLOCK_SOUTHWARD", (1,))):
            if name in b:
                self.block_into[b[name]] = dirs
        water = [v for k, v in b.items() if k.startswith("WATER") or k in ("WATERFALL", "DEEP_WATER")]
        self.water = set(water)
        self.blocked_edges = {}  # (x, z, d) -> attempts that failed
        self.cells = {}          # (x, z) -> cell, kept across probes of the same map
        self.objects = set()

    def update(self, p):
        for gz in range(64):
            row = gz * 64
            for gx in range(64):
                c = p.grid[row + gx]
                if c & TILE_KNOWN:
                    self.cells[(p.grid_x0 + gx, p.grid_z0 + gz)] = c
        self.objects = {(o[0], o[1]) for o in p.objects}

    def passable(self, x, z, d, goal):
        """Can the player step into (x, z) moving in direction d? Unknown tiles are hoped passable."""
        if (x - DIR_DELTA[d][0], z - DIR_DELTA[d][1], d) in self.blocked_edges:
            return False
        if (x, z) == goal:
            return True
        if (x, z) in self.objects:
            return False
        c = self.cells.get((x, z))
        if c is None:
            return True
        beh = c & TILE_BEHAVIOR
        if c & TILE_COLLISION or beh in self.water:
            return False
        if beh in self.block_into and d in self.block_into[beh]:
            return False
        if beh in self.jump:
            return False  # handled as a jump edge
        return True

    def neighbours(self, x, z, goal):
        for d, (dx, dz) in enumerate(DIR_DELTA):
            nx, nz = x + dx, z + dz
            c = self.cells.get((nx, nz))
            if c is not None and (c & TILE_BEHAVIOR) in self.jump and (nx, nz) != goal:
                if self.jump[c & TILE_BEHAVIOR] == d and (x, z, d) not in self.blocked_edges:
                    lx, lz = nx + dx, nz + dz  # a ledge: over it, landing one tile beyond
                    if (lx, lz) not in self.objects:
                        yield d, lx, lz, 2
                continue
            if self.passable(nx, nz, d, goal):
                yield d, nx, nz, 1

    def path(self, start, goal, limit=20000):
        """A* over tiles; returns the list of first-step directions, or None."""
        def h(x, z):
            return abs(x - goal[0]) + abs(z - goal[1])

        openq = [(h(*start), 0, start, None)]
        came = {start: None}
        cost = {start: 0}
        n = 0
        while openq and n < limit:
            _, g, cur, _ = heapq.heappop(openq)
            n += 1
            if cur == goal:
                dirs = []
                while came[cur] is not None:
                    prev, d = came[cur]
                    dirs.append(d)
                    cur = prev
                return dirs[::-1]
            if g > cost.get(cur, 1 << 30):
                continue
            for d, nx, nz, step_cost in self.neighbours(cur[0], cur[1], goal):
                ng = g + step_cost
                if ng < cost.get((nx, nz), 1 << 30):
                    cost[(nx, nz)] = ng
                    came[(nx, nz)] = (cur, d)
                    heapq.heappush(openq, (ng + h(nx, nz), ng, (nx, nz), d))
        return None


def _field_or_handle(s, step, ctx, limit):
    """Back to a free player: battles fought (auto_battle), text advanced, else wait."""
    on_battle = step.get("on_battle", "fight")
    on_text = step.get("on_text", "advance")
    waited = 0
    while not s.field_ready:
        if s.frame >= limit:
            raise HarnessError("walk_to: the player was not free again before the step's bound")
        if s.in_battle:
            if on_battle != "fight":
                raise HarnessError("walk_to: a battle started (on_battle = %r)" % on_battle)
            bot_auto_battle(s, {"max": limit - s.frame}, ctx)
            continue
        if s.run(20, until=["field_ready=1", "in_battle=1"]):
            continue
        waited += 20
        if waited >= 60:
            if on_text != "advance":
                raise HarnessError("walk_to: the player is held (text or a cutscene; on_text = %r)" % on_text)
            s.run(3, "a", until=["field_ready=1", "in_battle=1"])


def bot_walk_to(s, step, ctx):
    """Walk to tile (x, z): A* over the probe's terrain, replanning as it learns; warps by walking into them."""
    goal = (int(step["x"]), int(step["z"]))
    limit = s.frame + _int(step, "max", 6000)
    run_key = "b" if step.get("run", True) else None
    want_map = ctx.resolve(step["map"]) if "map" in step else None
    _field_or_handle(s, step, ctx, limit)
    p = s.probe()
    if p is None:
        raise HarnessError("walk_to: no probe (guest built without the e2e probe?)")
    if want_map is not None and p.map_id != want_map:
        raise HarnessError("walk_to: on map %d, the step expects %s (%d)" % (p.map_id, step["map"], want_map))
    terrain = Terrain()
    start_map = p.map_id
    steps = 0
    while (p.x, p.z) != goal:
        if s.frame >= limit:
            raise HarnessError("walk_to (%d,%d): still at (%d,%d) after %d frames" % (
                goal + (p.x, p.z, _int(step, "max", 6000))))
        terrain.update(p)
        dirs = terrain.path((p.x, p.z), goal)
        if not dirs and terrain.blocked_edges:
            # what bumping taught may have been a person in the way: forget it once
            terrain.blocked_edges.clear()
            dirs = terrain.path((p.x, p.z), goal)
        if not dirs:
            raise HarnessError("walk_to (%d,%d): no path from (%d,%d) on map %d" % (goal + (p.x, p.z, p.map_id)))
        d = dirs[0]
        here = (p.x, p.z)
        keys = DIR_KEYS[d] + ("+" + run_key if run_key else "")
        moved = s.run(24, keys, until=["x!=%d" % here[0], "z!=%d" % here[1], "field_ready=0"])
        # let the step finish so the next probe sees a settled tile
        s.run(24, until="field_ready=1")
        if s.map_id != start_map:
            # Outdoors the matrix is one coordinate space: a step across a map border changes map_id
            # and moves one tile (two over a ledge). Anything else is a warp.
            nxt = (here[0] + DIR_DELTA[d][0], here[1] + DIR_DELTA[d][1])
            _field_or_handle(s, step, ctx, limit)
            p = s.probe()
            if abs(p.x - here[0]) + abs(p.z - here[1]) <= 2:
                s.note("walk_to: crossed from map %d to %d at (%d,%d)" % (start_map, s.map_id, p.x, p.z))
                start_map = s.map_id
                steps += 1
                continue
            s.note("walk_to: warped from map %d to %d stepping %s from (%d,%d)" % (start_map, s.map_id,
                                                                                  DIR_KEYS[d], *here))
            if nxt == goal or len(dirs) == 1:
                break
            raise HarnessError("walk_to (%d,%d): an unexpected warp to map %d at (%d,%d)" % (goal + (s.map_id,) + here))
        if not s.field_ready:
            _field_or_handle(s, step, ctx, limit)
        p = s.probe()
        if (p.x, p.z) == here:
            key = (here[0], here[1], d)
            terrain.blocked_edges[key] = terrain.blocked_edges.get(key, 0) + 1
            if not moved:
                s.run(8)
        else:
            steps += 1
    s.note("walk_to: at (%d,%d) on map %d after %d steps" % (s.probe().x, s.probe().z, s.map_id, steps))
    if "face" in step:
        d = FACINGS[step["face"]]
        p = s.probe()
        if p.facing != d:
            s.run(2, DIR_KEYS[d])
            s.run(10)
    if step.get("interact"):
        s.run(4, "a")
        s.run(12)


BOTS = {
    "press": bot_press,
    "tap": bot_tap,
    "wait_frames": bot_wait_frames,
    "wait_map": bot_wait_map,
    "wait_field": bot_wait_field,
    "wait_battle": bot_wait_battle,
    "schedule": bot_schedule,
    "save": bot_save,
    "advance_text": bot_advance_text,
    "auto_battle": bot_auto_battle,
    "walk_to": bot_walk_to,
}
