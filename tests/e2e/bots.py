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
TAP_RUN = (128, 172)          # sActionMenuTouchRects[3]: y 0x98-0xC0, x 0x58-0xA8

# The battle menu config indices (core/include/np_e2e.h) and what auto_battle answers.
MENU_ACTION = range(1, 11)
MENU_MOVES, MENU_TARGET = 11, 12
MENU_ANSWER = {13: TAP_NO,   # YES/NO: give a nickname? / forfeit? -> NO
               14: TAP_NO,   # make it forget another move? -> NO
               15: TAP_YES,  # give up on learning the move? -> YES
               16: TAP_YES,  # use the next Pokemon? -> YES
               17: TAP_NO}   # switch Pokemon? (trainer about to send the next) -> NO

# auto_battle flee: RUN taps per battle before it fights instead
FLEE_TRIES = 2

# walk_to's A* cost of a tall-grass tile (a plain tile costs 1)
GRASS_COST = 6


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
# A coord or OnFrame script starts a few frames after the step that triggers it, and a field can be free for
# a frame or two between a script's parts (a camera pan, a walk-in): the field counts as free again only
# after SETTLE frames without a script.
TEXT_START, TEXT_SETTLE = 40, 30


def bot_advance_text(s, step, ctx):
    """A with spacing until the player is free (or a battle starts, which auto_battle takes over).

    With `map`, it stops as soon as that map is loaded, before the next map's own scripts (an OnFrame that
    asks something A must not answer) run on."""
    bound = _int(step, "max", 6000)
    limit = s.frame + bound
    through = bool(step.get("through_battle"))
    stop = ["field_ready=1"] + ([] if through else ["in_battle=1"])
    want_map = ctx.resolve(step["map"]) if "map" in step else None
    if want_map is not None:
        stop.append("map_id=%d" % want_map)
    s.run(TEXT_START, until=["field_ready=0", "in_battle=1"])
    while True:
        if want_map is not None and s.map_id == want_map:
            break
        if s.in_battle and not through:
            break
        if s.field_ready and not s.in_battle:
            if not s.run(TEXT_SETTLE, until=["field_ready=0", "in_battle=1"]):
                break
            continue
        if s.frame >= limit:
            raise HarnessError("text did not end in %d frames" % bound)
        if not s.run(2, "a", until=stop):
            s.run(6, until=stop)


# ---------------------------------------------------------------- auto_battle
def bot_auto_battle(s, step, ctx):
    """FIGHT + one move each turn until the battle is over; prompts answered (MENU_ANSWER).

    With flee = true it taps RUN at the first action menus (a wild battle ends; a trainer refuses, and so may a
    wild Pokemon) and fights once FLEE_TRIES runs have not ended the battle."""
    move = _int(step, "move", 0)
    flee = FLEE_TRIES if step.get("flee") else 0
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
            if idx in MENU_ACTION and flee:
                flee -= 1
                _tap(s, TAP_RUN)
            elif idx in MENU_ACTION:
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
        if not s.run(6, until=["ui!=0", "in_battle=0"]):
            s.run(2, "a", until=["ui!=0", "in_battle=0"])
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
        # Tall grass costs GRASS_COST steps: the planner goes round it where it can, as a player would, so a
        # walk meets fewer wild battles and reaches the route's trainers with more HP.
        self.grass = {b[k] for k in ("TALL_GRASS", "VERY_TALL_GRASS", "MUD_WITH_GRASS", "MUD_DEEP_WITH_GRASS") if k in b}
        self.blocked_edges = {}  # (x, z, d) -> attempts that failed
        self.cells = {}          # (x, z) -> cell, kept across probes of the same map
        self.objects = set()
        # exit mats and the direction that leaves through them (map_tile_behaviors.h)
        self.mats = {}
        for name, d in (("WARP_ENTRANCE_NORTH", 0), ("WARP_ENTRANCE_SOUTH", 1), ("WARP_ENTRANCE_WEST", 2),
                        ("WARP_ENTRANCE_EAST", 3), ("WARP_NORTH", 0), ("WARP_SOUTH", 1), ("WARP_WEST", 2),
                        ("WARP_EAST", 3), ("WARP_STAIRS_WEST", 2), ("WARP_STAIRS_EAST", 3)):
            if name in b:
                self.mats[b[name]] = d

    def mat_exit(self, cell):
        """The direction that leaves through the exit mat `cell`, or None."""
        if cell is None or not cell & TILE_KNOWN:
            return None
        return self.mats.get(cell & TILE_BEHAVIOR)

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
                c = self.cells.get((nx, nz))
                yield d, nx, nz, GRASS_COST if c is not None and (c & TILE_BEHAVIOR) in self.grass else 1

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
    """Back to a free player: battles fought (auto_battle), text advanced, else wait.

    Returns the frames spent in battles, which do not count against walk_to's bound (the milestone's
    budget still does): how many wild battles a walk meets is the game's RNG, not the route."""
    on_battle = step.get("on_battle", "fight")
    on_text = step.get("on_text", "advance")
    waited = 0
    in_battles = 0
    while not s.field_ready:
        if s.frame >= limit + in_battles:
            raise HarnessError("walk_to: the player was not free again before the step's bound")
        if s.in_battle:
            if on_battle not in ("fight", "flee"):
                raise HarnessError("walk_to: a battle started (on_battle = %r)" % on_battle)
            f0 = s.frame
            bot_auto_battle(s, {"flee": on_battle == "flee"}, ctx)
            in_battles += s.frame - f0
            continue
        if s.run(20, until=["field_ready=1", "in_battle=1"]):
            continue
        waited += 20
        if waited >= 60:
            if on_text != "advance":
                raise HarnessError("walk_to: the player is held (text or a cutscene; on_text = %r)" % on_text)
            s.run(2, "a", until=["field_ready=1", "in_battle=1"])
            s.run(6, until=["field_ready=1", "in_battle=1"])
    return in_battles


def bot_walk_to(s, step, ctx):
    """Walk to tile (x, z): A* over the probe's terrain, replanning as it learns; warps by walking into them."""
    goal = (int(step["x"]), int(step["z"]))
    bound = _int(step, "max", 6000)
    limit = s.frame + bound
    run_key = "b" if step.get("run", True) else None
    want_map = ctx.resolve(step["map"]) if "map" in step else None
    limit += _field_or_handle(s, step, ctx, limit)
    p = s.probe()
    if p is None:
        raise HarnessError("walk_to: no probe (guest built without the e2e probe?)")
    if want_map is not None and p.map_id != want_map:
        raise HarnessError("walk_to: on map %d, the step expects %s (%d)" % (p.map_id, step["map"], want_map))
    terrain = Terrain()
    start_map = p.map_id
    steps = 0
    warped = False
    while (p.x, p.z) != goal:
        if s.frame >= limit:
            raise HarnessError("walk_to (%d,%d): still at (%d,%d) after %d frames (battles excluded)" % (
                goal + (p.x, p.z, bound)))
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
        # Hold the direction through the turn-in-place (a short press only turns) until the step begins:
        # the probe's tile changes as a step starts. A bump into something solid never changes it.
        moved = s.run(24, keys, until=["x!=%d" % here[0], "z!=%d" % here[1], "map_id!=%d" % start_map,
                                       "in_battle=1"])
        # let the step finish so the next probe sees a settled tile; then whatever the step started (a warp's
        # fade, a coord script, a trainer's sight, a wild battle) runs until the player is free again
        s.run(24, until="field_ready=1")
        if not s.field_ready:
            limit += _field_or_handle(s, step, ctx, limit)
        p = s.probe()
        if p.map_id != start_map:
            # Outdoors the matrix is one coordinate space: a step across a map border changes map_id
            # and moves one tile (two over a ledge). Anything else is a warp.
            nxt = (here[0] + DIR_DELTA[d][0], here[1] + DIR_DELTA[d][1])
            if abs(p.x - here[0]) + abs(p.z - here[1]) <= 2:
                s.note("walk_to: crossed from map %d to %d at (%d,%d)" % (start_map, p.map_id, p.x, p.z))
                start_map = p.map_id
                steps += 1
                continue
            s.note("walk_to: warped from map %d to %d stepping %s from (%d,%d)" % (start_map, p.map_id,
                                                                                  DIR_KEYS[d], *here))
            if nxt == goal or here == goal or len(dirs) == 1:
                warped = True
                break
            raise HarnessError("walk_to (%d,%d): an unexpected warp to map %d at (%d,%d)" % (goal + (p.map_id,) + here))
        if (p.x, p.z) == here:
            key = (here[0], here[1], d)
            terrain.blocked_edges[key] = terrain.blocked_edges.get(key, 0) + 1
            if not moved:
                s.run(4)
        else:
            steps += 1
    if not warped:
        # An exit mat (WARP_ENTRANCE_*, stairs, WARP_<dir>) warps when the player pushes off it in its
        # direction; a goal on one means "leave through it".
        p = s.probe()
        d = terrain.mat_exit(p.cell(p.x, p.z))
        if d is not None:
            if not s.run(150, DIR_KEYS[d], until="map_id!=%d" % start_map):
                raise HarnessError("walk_to (%d,%d): the exit mat did not warp pushing %s" % (goal + (DIR_KEYS[d],)))
            _field_or_handle(s, step, ctx, limit)
            warped = True
            s.note("walk_to: left map %d through the mat at (%d,%d) to map %d" % (start_map, goal[0], goal[1], s.map_id))
    # the probe's tile is the step's target from the step's first frame and the field reads free between the
    # frames of a step: let the last step's walk finish before facing, talking or the next bot (a script the
    # goal tile starts is the next step's to handle)
    s.run(16)
    p = s.probe()
    s.note("walk_to: at (%d,%d) on map %d after %d steps" % (p.x, p.z, s.map_id, steps))
    if "face" in step:
        d = FACINGS[step["face"]]
        p = s.probe()
        if p.facing != d:
            s.run(2, DIR_KEYS[d])
            s.run(10)
    if step.get("interact"):
        s.run(4, "a")
        s.run(12)


# ---------------------------------------------------------------- heal
# Every Pokemon Center 1F of the three games shares one layout: the nurse at (8,4) behind the counter, the
# exit mat at (8,12) (e.g. Platinum events_sandgem_town_pokecenter_1f / events_jubilife_city_pokecenter_1f,
# D/P zone_event 0398 / 0005).
PC_COUNTER, PC_EXIT = (8, 6), (8, 12)


def bot_heal(s, step, ctx):
    """Heal the party at the Pokemon Center whose door is (x, z) on this map, and come back out of it."""
    town = s.map_id
    bot_walk_to(s, {"x": step["x"], "z": step["z"], "on_battle": step.get("on_battle", "flee"),
                    "max": _int(step, "max", 6000)}, ctx)
    if s.map_id == town:
        raise HarnessError("heal: (%d,%d) is not a door on map %d" % (int(step["x"]), int(step["z"]), town))
    center = s.map_id
    bot_walk_to(s, {"x": PC_COUNTER[0], "z": PC_COUNTER[1], "face": "up", "interact": True}, ctx)
    bot_advance_text(s, {}, ctx)  # A answers YES to resting the Pokemon
    bot_walk_to(s, {"x": PC_EXIT[0], "z": PC_EXIT[1]}, ctx)
    if s.map_id == center:
        raise HarnessError("heal: did not leave the Pokemon Center (map %d)" % center)
    s.note("heal: healed in map %d, back on map %d" % (center, s.map_id))


# ---------------------------------------------------------------- talk_to
def _toward(frm, to):
    """The direction index from tile frm to the adjacent tile to."""
    return DIR_DELTA.index((to[0] - frm[0], to[1] - frm[1]))


def bot_talk_to(s, step, ctx):
    """Talk to map object `id` (its local id in the probe's object list) wherever it stands now: walk to a free
    tile next to it, face it, A until a script starts. Wandering people are chased (re-planned) as they move."""
    oid = int(step["id"])
    bound = _int(step, "max", 6000)
    limit = s.frame + bound
    while s.frame < limit:
        limit += _field_or_handle(s, step, ctx, limit)
        p = s.probe()
        obj = next(((o[0], o[1]) for o in p.objects if o[2] == oid), None)
        if obj is None:
            raise HarnessError("talk_to: no object with local id %d on map %d" % (oid, p.map_id))
        if abs(p.x - obj[0]) + abs(p.z - obj[1]) == 1:
            d = _toward((p.x, p.z), obj)
            if p.facing != d:
                s.run(2, DIR_KEYS[d])
                s.run(8)
                continue  # it may have moved meanwhile
            s.run(2, "a")
            if s.run(30, until=["field_ready=0", "in_battle=1"]):
                s.note("talk_to: talking to object %d at (%d,%d)" % (oid, obj[0], obj[1]))
                return
            continue
        t = Terrain()
        t.update(p)
        cands = []
        for dx, dz in DIR_DELTA:
            c = (obj[0] + dx, obj[1] + dz)
            cell = t.cells.get(c)  # None: beyond the probe's window, hoped passable like walk_to does
            if (cell is not None and cell & TILE_COLLISION) or c in t.objects:
                continue
            cands.append((abs(c[0] - p.x) + abs(c[1] - p.z), c))
        if not cands:
            s.run(16)  # boxed in for now; it wanders
            continue
        c = min(cands)[1]
        try:
            bot_walk_to(s, {"x": c[0], "z": c[1], "max": min(900, max(limit - s.frame, 1))}, ctx)
        except HarnessError as e:
            s.note("talk_to: %s; re-planning" % e)
    raise HarnessError("talk_to: object %d not reached in %d frames" % (oid, bound))


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
    "talk_to": bot_talk_to,
    "heal": bot_heal,
}
