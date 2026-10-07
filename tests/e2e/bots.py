"""The input bots a milestone's [[step]]s name (tests/e2e/README.md).

Each bot is a function bot(session, step, ctx) that drives the game through a
Session (np_e2e.py) until its goal holds, and raises HarnessError when it
cannot get there within its bound (`max` frames, default per bot). ctx is the
milestone's context: the game, the milestone directory, the name resolver.
"""
import heapq
import json
import os
import re
import subprocess

from np_e2e import (DIR_DELTA, DIR_KEYS, FACINGS, ROOT, TILE_BEHAVIOR, TILE_COLLISION, TILE_KNOWN, UI_BATTLE_MENU,
                    UI_BATTLE_PARTY, HarnessError, behaviors)

# ---- the battle's touch screen (Platinum src/battle/battle_subscreen.c touch rects; D/P's overlay 11
# tables are byte-identical), as tap points: the centre of each button.
TAP_FIGHT = (128, 80)          # sActionMenuTouchRects[0]: y 0x18-0x90, full width
TAP_MOVES = [(64, 52), (192, 52), (64, 116), (192, 116)]  # sMoveSelectMenuTouchRects[1..4]
# sTargetSelectMenuTouchRects[1] and [0]: the opponent on the right, then the one on the left (the right one may
# have fainted in a double battle: its button does nothing, and the target menu stays up)
TAP_TARGETS = [(196, 44), (60, 44)]
TAP_YES = (128, 68)            # sYesNoMenuTouchRects[0]: y 0x28-0x60
TAP_NO = (128, 140)            # sYesNoMenuTouchRects[1]: y 0x70-0xA8
# battle_party.c sPartyPokemonScreenTouchRects (slot i) and sSelectPokemonScreenTouchRects[SHIFT]
TAP_PARTY = [(64, 24), (192, 32), (64, 72), (192, 80), (64, 120), (192, 128)]
TAP_SHIFT = (128, 76)
TAP_RUN = (128, 172)          # sActionMenuTouchRects[3]: y 0x98-0xC0, x 0x58-0xA8
TAP_POKEMON = (216, 168)      # sActionMenuTouchRects[2]: y 0x90-0xC0, x 0xB0-0xFF

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

# Surfable tile behaviors: Platinum src/map_tile_behavior.c sTileBehaviorFlags entries with
# TILE_BEHAVIOR_FLAG_SURFABLE (0x10-0x15, 0x19, 0x22, 0x2A, 0x50-0x53), the bridges over water left out (they are
# walked on). D/P's behaviors are the same numbers. 0x13 is the waterfall, climbed (up) with Waterfall.
SURFABLE = {0x10, 0x11, 0x12, 0x14, 0x15, 0x19, 0x22, 0x2A, 0x50, 0x51, 0x52, 0x53}
WATERFALL = 0x13
ROCK_CLIMB = {0x4B: (0, 1), 0x4C: (2, 3)}  # ROCK_CLIMB_N_S / _E_W and the directions that climb them
# Field-move obstacles among map objects, by graphics id (Platinum generated/object_events_gfx.txt rows 86-87,
# D/P include/constants/sprites.h:80-81 SPRITE_BREAKROCK/TREE: the same ids). Strength boulders (84) are
# puzzles: steps push them.
GFX_ROCK_SMASH, GFX_CUT_TREE = 85, 86
# A* cost of a field-move tile or object: one interaction plus its scene
FIELD_MOVE_COST = 4
# A* cost added per earlier visit of a tile in the same walk (Terrain.visits)
VISIT_COST = 2


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


# ---- auto_battle's move choice: the probe's battle report (np_e2e.py Probe.battlers / party) scored with the ROM's
# move data and type chart (np_save4 gamedata). The constants are the game's (Platinum include/constants/battle.h,
# generated/move_battle_effects.txt, generated/moves.h; D/P number them the same).
BATTLE_TYPE_DOUBLES, BATTLE_TYPE_2VS2, BATTLE_TYPE_TAG = 0x02, 0x08, 0x10
MOVE_CLASS_STATUS = 2
# moves a bot should not pick for damage: the user faints, they fail unless the foe sleeps / the user sleeps / it is
# the first turn / the user is not hit first
AVOID_MOVES = {120, 153, 138, 173, 252, 264}  # Self-Destruct, Explosion, Dream Eater, Snore, Fake Out, Focus Punch
# battle effects that spend two turns on one hit: a charge turn (Razor Wind, Sky Attack, Skull Bash, Solar Beam, Fly,
# Dive, Dig, Bounce, Shadow Force) or a recharge turn after (Hyper Beam)
TWO_TURN_EFFECTS = {39, 75, 145, 151, 155, 255, 256, 263, 272, 80}
# power 1 in the move data: the power is computed in battle (Return, Low Kick, Hidden Power...) or the damage is
# fixed (Seismic Toss, Dragon Rage); scored as an ordinary move
COMPUTED_POWER = 60
# a move that hits every adjacent battler (RANGE_ALL_ADJACENT: Earthquake, Surf) beside a live ally: scored down
RANGE_ALL_ADJACENT, ALLY_HIT_FACTOR = 0x08, 0.25
# abilities that make a move type do nothing (ability ids, generated/abilities.txt): Levitate (Ground), Water
# Absorb and Dry Skin (Water), Volt Absorb and Motor Drive (Electric), Flash Fire (Fire); Wonder Guard lets only
# super-effective moves through
ABILITY_IMMUNE = {26: {4}, 11: {11}, 87: {11}, 10: {13}, 78: {13}, 18: {10}}
WONDER_GUARD = 25

_GAMEDATA = {}


def gamedata(ctx):
    """The ROM's battle tables (np_save4 gamedata): species types, moves, type chart; loaded once per ROM."""
    if ctx.rom not in _GAMEDATA:
        out = subprocess.run([ctx.save4, "gamedata", ctx.rom], capture_output=True, text=True)
        if out.returncode != 0:
            raise HarnessError("np_save4 gamedata: %s" % out.stderr.strip())
        _GAMEDATA[ctx.rom] = json.loads(out.stdout)
    return _GAMEDATA[ctx.rom]


def mon_types(gd, mon):
    """A Pokemon's types: the battle's own (battlers) or its species' (party members)."""
    if mon.types:
        return set(mon.types)
    return set(gd["species"][mon.species][:2]) if mon.species < len(gd["species"]) else set()


def blocking_abilities(gd, species, move, foe_types):
    """(abilities the species may have that make `move` do nothing to it, all its abilities)."""
    if species is None or species >= len(gd["species"]) or not move or move >= len(gd["moves"]):
        return [], []
    mtype = gd["moves"][move][3]
    eff = 1.0
    for t in foe_types:
        eff *= gd["type_chart"][mtype][t] / 10.0
    have = [a for a in gd["species"][species][2:4] if a]
    return [a for a in have if mtype in ABILITY_IMMUNE.get(a, ()) or (a == WONDER_GUARD and eff <= 1)], have


def move_value(gd, move, user_types, foe_types, ally=False, foe_species=None):
    """Expected damage of a move, relative: base power x STAB x type effectiveness x accuracy (halved for a move
    that takes two turns, cut to ALLY_HIT_FACTOR when it would hit a live ally too); 0 for a status move, one in
    AVOID_MOVES, or one every ability of the foe's species blocks (Gastly's Levitate)."""
    if not move or move >= len(gd["moves"]) or move in AVOID_MOVES:
        return 0.0
    effect, cls, power, mtype, acc, _pp, _priority, rng = gd["moves"][move][:8]
    if cls == MOVE_CLASS_STATUS or power == 0:
        return 0.0
    block, have = blocking_abilities(gd, foe_species, move, foe_types)
    if have and len(block) == len(have):
        return 0.0
    v = float(COMPUTED_POWER if power == 1 else power)
    if mtype in user_types:
        v *= 1.5
    for t in foe_types:
        v *= gd["type_chart"][mtype][t] / 10.0
    v *= (acc or 100) / 100.0
    if ally and rng & RANGE_ALL_ADJACENT:
        v *= ALLY_HIT_FACTOR
    return v / 2 if effect in TWO_TURN_EFFECTS else v


def usable_slots(mon, rejected=()):
    return [i for i in range(4) if mon.moves[i] and mon.pp[i] > 0 and mon.moves[i] != mon.disabled_move
            and i not in rejected]


def best_damage(gd, mon, foe, rejected=(), ally=False, useless=()):
    """(slot, value) of the mon's best damaging move with PP against foe; (None, 0) when it has none. `useless`
    holds the (move, foe species) pairs this battle has seen blocked by an ability (Bronzor's Levitate or Heatproof)."""
    best, value = None, 0.0
    user, foes = mon_types(gd, mon), mon_types(gd, foe) if foe is not None else set()
    for i in usable_slots(mon, rejected):
        if foe is not None and (mon.moves[i], foe.species) in useless:
            continue
        v = move_value(gd, mon.moves[i], user, foes, ally, foe.species if foe is not None else None)
        if v > value:
            best, value = i, v
    return best, value


def choose_move(gd, mon, foe, rejected=(), ally=False, useless=()):
    """The slot to use: the best damaging move, else the first status move with PP, else None (the game uses
    Struggle by itself when nothing has PP)."""
    slot, _ = best_damage(gd, mon, foe, rejected, ally, useless)
    if slot is not None:
        return slot
    rest = usable_slots(mon, rejected)
    return rest[0] if rest else None


def replacement(gd, party, foe, first, ally=False, useless=(), best=False):
    """The party screen slot to send in: the first healthy member from `first` on with a damaging move against foe,
    else the first healthy one; None when there is none. With best (auto_battle's send = "best"): the healthy member
    whose best move scores highest against foe, weighted by its level."""
    healthy = [k for k in range(first, len(party)) if party[k].alive]
    if best:
        scored = [(best_damage(gd, party[k], foe, ally=ally, useless=useless)[1] * max(party[k].level, 1), -k, k)
                  for k in healthy]
        scored = [t for t in scored if t[0] > 0]
        if scored:
            return max(scored)[2]
    for k in healthy:
        if best_damage(gd, party[k], foe, ally=ally, useless=useless)[0] is not None:
            return k
    return healthy[0] if healthy else None


# ---------------------------------------------------------------- auto_battle
def bot_auto_battle(s, step, ctx):
    """FIGHT + one move each turn until the battle is over; prompts answered (MENU_ANSWER).

    The move is the usable one (PP left, not disabled) with the highest expected damage against the targeted foe
    (move_value: base power x STAB x type effectiveness, from the ROM); status moves only when no damaging move has
    PP. A species whose every ability blocks a move (Levitate, Water/Volt Absorb, Dry Skin, Motor Drive, Flash Fire,
    Wonder Guard) scores it 0; when only one of its two may, a use that left the foe's HP unchanged shows which it
    has, and the move is not used on that species again in the battle. A lead with no
    damaging move left is switched for the next healthy party member that has one, and a fainted
    lead is replaced the same way. `move = N` overrides the choice with slot N (the next slot whenever the game
    refuses one), for scripted fights.

    With flee = true it taps RUN at the first action menus (a wild battle ends; a trainer refuses, and so may a
    wild Pokemon) and fights once FLEE_TRIES runs have not ended the battle."""
    fixed = "move" in step
    move = _int(step, "move", 0)
    send_best = step.get("send") == "best"  # a fainted lead's replacement: the best scorer, not the first able
    flee = FLEE_TRIES if step.get("flee") else 0
    limit = s.frame + _int(step, "max", 30000)
    if not s.in_battle and not s.run(_int(step, "wait", 900), until="in_battle=1"):
        raise HarnessError("no battle started within %d frames" % _int(step, "wait", 900))
    gd = None if fixed else gamedata(ctx)
    p = s.probe()
    since = p.frame if p is not None else 0  # battle reports older than this are another battle's
    party_try = 0
    turns = 0
    last = None      # the menu answered last
    target = 0       # TAP_TARGETS index: 0 the foe battler 1, 1 battler 3
    again = 0        # move-menu returns in a row
    menus = {}       # battle menu config index -> times answered (the run log's trace of the battle)
    rejected = []    # move slots the game sent back this turn
    slot = None      # the move slot tapped last
    want = None      # the party screen slot auto_battle chose
    shift_taps = 0   # SHIFT taps on the current visit of a party member's page
    switched_at = -1  # `turns` of the last switch
    report = None    # the last battle report of this battle (a Probe)
    tried = {}       # menu battler -> (move, foe battler, foe species, foe HP) of its last move
    useless = set()  # (move, foe species) pairs an ability of the foe was seen to block
    while s.in_battle:
        if s.frame >= limit:
            raise HarnessError("the battle did not end in %d frames" % _int(step, "max", 30000))
        p = s.probe()
        if p is not None and p.battle_frame >= since and p.battlers:
            report = p
        fresh = gd is not None and p is not None and p.battle_fresh and p.battle_frame >= since
        if p is not None and p.ui == UI_BATTLE_MENU:
            idx = p.ui_arg
            me = foe = None
            ally = False
            if fresh:
                me = p.battlers[p.menu_battler]
                foes = [b for b in (1, 3) if b < len(p.battlers) and p.battlers[b].alive]
                if foes and (1 + 2 * target) not in foes:
                    target = (foes[0] - 1) // 2
                foe = p.battlers[1 + 2 * target] if foes else None
                mate = p.menu_battler ^ 2
                ally = bool(p.battle_type & BATTLE_TYPE_DOUBLES) and mate < len(p.battlers) and p.battlers[mate].alive
                if idx in MENU_ACTION and p.menu_battler in tried:
                    mv, fb, sp, hp = tried.pop(p.menu_battler)
                    b = p.battlers[fb] if fb < len(p.battlers) else None
                    if b is not None and b.species == sp and b.hp > 0 and b.hp >= hp:
                        # unchanged HP: a miss, a potion back to full, or the ability; only the last when it may be
                        block, _ = blocking_abilities(gd, sp, mv, mon_types(gd, b))
                        if block and (mv, sp) not in useless:
                            useless.add((mv, sp))
                            s.note("auto_battle: move %d did nothing to species %d (ability %s); not using it on it "
                                   "again" % (mv, sp, block))
            if idx in MENU_ACTION and flee:
                flee -= 1
                _tap(s, TAP_RUN)
            elif idx in MENU_ACTION:
                rejected = []
                k = None
                shared = fresh and p.battle_type & (BATTLE_TYPE_2VS2 | BATTLE_TYPE_TAG)  # party screen interleaved
                if (fresh and not shared and me.alive and switched_at != turns
                        and best_damage(gd, me, foe, ally=ally, useless=useless)[0] is None):
                    first = 2 if p.battle_type & BATTLE_TYPE_DOUBLES else 1
                    k = replacement(gd, p.party, foe, first, ally, useless, best=send_best)
                    if k is not None and best_damage(gd, p.party[k], foe, ally=ally, useless=useless)[0] is None:
                        k = None
                if k is not None:
                    s.note("auto_battle: no damaging move left; switching to party slot %d" % k)
                    want, switched_at = k, turns
                    _tap(s, TAP_POKEMON)
                else:
                    _tap(s, TAP_FIGHT)
                    turns += 1
                party_try = 0
            elif idx == MENU_MOVES:
                # the move menu again straight after a move: the tap may have fallen in the menu's slide-in, so
                # the same move once more; a second time it cannot be used (no PP left, disabled, Taunt, Torment, a
                # choice lock): the next slot, or the next best move
                again = again + 1 if last == MENU_MOVES else 0
                refused = again >= 2
                if refused:
                    again = 0
                if fixed or not fresh:
                    if refused:
                        move = (move + 1) % 4
                        s.note("auto_battle: move slot %d" % move)
                    slot = move
                else:
                    if refused and slot is not None:
                        rejected.append(slot)
                    slot = choose_move(gd, me, foe, rejected, ally, useless)
                    if slot is None:
                        rejected = []
                        slot = choose_move(gd, me, foe, ally=ally, useless=useless) or 0
                    s.note("auto_battle: battler %d slot %d (move %d, %d PP) on species %d" % (
                        p.menu_battler, slot, me.moves[slot], me.pp[slot], foe.species if foe else 0))
                    if foe is not None:
                        tried[p.menu_battler] = (me.moves[slot], 1 + 2 * target, foe.species, foe.hp)
                _tap(s, TAP_MOVES[slot])
                # the move menu stays reported for a few frames while it slides out: not a refusal
                s.run(30, until=["ui_arg!=%d" % MENU_MOVES, "ui!=%d" % UI_BATTLE_MENU, "in_battle=0"])
            elif idx == MENU_TARGET:
                if last == MENU_TARGET:
                    target = 1 - target  # the target menu again: that opponent is gone, take the other
                _tap(s, TAP_TARGETS[target])
            elif idx in MENU_ANSWER:
                _tap(s, MENU_ANSWER[idx])
            else:
                s.run(2, "b")
                s.run(10)
            last = idx
            menus[idx] = menus.get(idx, 0) + 1
            continue
        if p is not None and p.ui == UI_BATTLE_PARTY:
            if p.ui_arg == 0:
                shift_taps = 0
                if want is None and report is not None and party_try == 0 and gd is not None:
                    # a fainted lead: the battle's last report has the party (the screen's order) and the foe
                    foe = next((report.battlers[b] for b in (1 + 2 * target, 3 - 2 * target)
                                if b < len(report.battlers) and report.battlers[b].alive), None)
                    first = 2 if report.battle_type & BATTLE_TYPE_DOUBLES else 1
                    if not report.battle_type & (BATTLE_TYPE_2VS2 | BATTLE_TYPE_TAG):
                        want = replacement(gd, report.party, foe, first, best=send_best)
                if want is not None and last != ("party", want):
                    party_try = want
                    last = ("party", want)
                else:
                    # the next slot each time: a fainted one says so and comes back here
                    want = None
                    party_try = party_try % 5 + 1
                    last = ("party", None)
                _tap(s, TAP_PARTY[party_try])
            elif shift_taps >= 3:
                # the page stayed up through three SHIFT taps (a member that cannot come in: fainted, or already
                # out in a double battle): B back to the party list and the next slot at once (the probe keeps
                # reporting the SHIFT page after B, so the list branch above would not run)
                s.note("auto_battle: party slot %d refused; the next one" % party_try)
                s.run(2, "b")
                s.run(20)
                shift_taps = 0
                want = None
                party_try = party_try % 5 + 1
                last = ("party", None)
                _tap(s, TAP_PARTY[party_try])
            else:
                if shift_taps == 0:
                    _tap(s, TAP_SHIFT)
                else:
                    # the page in key mode (the cursor's red corners on SHIFT): A selects it
                    s.run(2, "a")
                    s.run(12)
                shift_taps += 1
                want = None
            continue
        # text, animations, the evolution scene: A advances text (B would cancel an evolution)
        if not s.run(6, until=["ui!=0", "in_battle=0"]):
            s.run(2, "a", until=["ui!=0", "in_battle=0"])
    s.note("auto_battle: battle over after %d turns (menus answered %s)" % (
        turns, ", ".join("%d x%d" % kv for kv in sorted(menus.items()))))


# ---------------------------------------------------------------- walk_to
class Terrain:
    """What walk_to knows about the map: the probe's window plus what walking taught it."""

    def __init__(self, surf=False, hm=False, avoid=()):
        self.surf, self.hm = surf, hm
        # tiles the step says are blocked though the probe's grid shows them free (props with their own collision,
        # e.g. D/P Veilstone Gym's sliding bars): never planned through, so never bumped
        self.avoid = {(int(x), int(z)) for x, z in avoid}
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
        # Bike slopes (e.g. Route 209 (562,691..692)) go uphill only at bicycle speed: on foot the player
        # slides back down forever, so walk_to never plans across one.
        self.slopes = {b[k] for k in ("BIKE_SLOPE_TOP", "BIKE_SLOPE_BOTTOM") if k in b}
        self.blocked_edges = {}  # (x, z, d) -> attempts that failed
        # (x, z) -> times the walk stood there: each visit makes the tile cost VISIT_COST more, so plans that
        # flip as the window slides (unknown tiles are hoped passable) stop swinging between two tiles
        self.visits = {}
        self.cells = {}          # (x, z) -> cell, kept across probes of the same map
        self.objects = set()
        self.hm_objects = set()  # cut trees and Rock Smash rocks, when hm
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
        self.hm_objects = {(o[0], o[1]) for o in p.objects if self.hm and o[3] in (GFX_ROCK_SMASH, GFX_CUT_TREE)}

    def field_move(self, x, z, d):
        """The field move that enters (x, z) moving in direction d ('water', 'waterfall', 'climb', 'object'), if the
        walk may use one there (`surf`, `hm`); else None."""
        if (x, z) in self.hm_objects:
            return "object"
        c = self.cells.get((x, z))
        if c is None:
            return None
        beh = c & TILE_BEHAVIOR
        if self.surf and beh in SURFABLE:
            return "water"
        if self.surf and beh == WATERFALL and d == 0:
            return "waterfall"
        if self.hm and beh in ROCK_CLIMB and d in ROCK_CLIMB[beh]:
            return "climb"
        return None

    def passable(self, x, z, d, goal):
        """Can the player step into (x, z) moving in direction d? Unknown tiles are hoped passable."""
        if (x - DIR_DELTA[d][0], z - DIR_DELTA[d][1], d) in self.blocked_edges:
            return False
        if (x, z) == goal:
            return True
        if (x, z) in self.avoid:
            return False
        if self.field_move(x, z, d):
            return True
        if (x, z) in self.objects:
            return False
        c = self.cells.get((x, z))
        if c is None:
            return True
        beh = c & TILE_BEHAVIOR
        if c & TILE_COLLISION or beh in self.water or beh == WATERFALL or beh in self.slopes:
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
                        yield d, lx, lz, 2 + self.visits.get((lx, lz), 0) * VISIT_COST
                continue
            if self.passable(nx, nz, d, goal):
                c = self.cells.get((nx, nz))
                fm = self.field_move(nx, nz, d)
                extra = self.visits.get((nx, nz), 0) * VISIT_COST
                if fm in ("object", "climb"):
                    yield d, nx, nz, FIELD_MOVE_COST + extra
                else:
                    yield d, nx, nz, extra + (GRASS_COST if c is not None and (c & TILE_BEHAVIOR) in self.grass else 1)

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
    """Back to a free player: battles fought (auto_battle), text advanced with B, else wait. B advances a message
    as A does (render_text.c:61, :382) but answers a YES/NO as NO: an egg that hatches on a walk's step asks for a
    nickname, and A would open the naming screen (cutscenes/egg_hatch/main.c:155-187, the menu's B cancel,
    egg_hatch/graphics.c:319).

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
            sub = {"flee": on_battle == "flee"}
            if "move" in step:
                sub["move"] = step["move"]
            bot_auto_battle(s, sub, ctx)
            in_battles += s.frame - f0
            continue
        if s.run(20, until=["field_ready=1", "in_battle=1"]):
            continue
        waited += 20
        if waited >= 60:
            if on_text != "advance":
                raise HarnessError("walk_to: the player is held (text or a cutscene; on_text = %r)" % on_text)
            s.run(2, "b", until=["field_ready=1", "in_battle=1"])
            s.run(6, until=["field_ready=1", "in_battle=1"])
    return in_battles


def bot_steps(s, step, ctx):
    """Walk a fixed route: `route` = [[x, z], ...], each a straight line from the one before (tools/pt_gym.py canalave
    prints one), for maps whose collision the probe cannot show (the Canalave Gym's floors and platforms are the gym's
    own tables, not the land data walk_to plans over). Each tile: the direction held until the player's tile changes;
    whatever the step started (a platform ride, a trainer's sight, text) runs until the player is free. A step that
    lands on the next corner instead (a platform carried the player there) counts it reached. A step that moves
    nothing is tried again, three times at most."""
    bound = _int(step, "max", 9000)
    limit = s.frame + bound
    run_key = "+b" if step.get("run", True) else ""
    route = [tuple(int(v) for v in t) for t in step["route"]]
    k, tiles, tries = 0, 0, 0
    while k < len(route):
        if s.frame >= limit:
            raise HarnessError("steps: corner %d of %d after %d tiles, out of %d frames (battles excluded)" % (
                k, len(route), tiles, bound))
        limit += _field_or_handle(s, step, ctx, limit)
        p = s.probe()
        here = (p.x, p.z)
        if here == route[k]:
            k += 1
            continue
        dx, dz = route[k][0] - here[0], route[k][1] - here[1]
        if dx and dz:
            raise HarnessError("steps: at (%d,%d), corner %d (%d,%d) is not in a straight line" % (here + (k,) + route[k]))
        d = DIR_DELTA.index(((dx > 0) - (dx < 0), (dz > 0) - (dz < 0)))
        s.run(24, DIR_KEYS[d] + run_key, until=["x!=%d" % here[0], "z!=%d" % here[1], "in_battle=1"])
        s.run(24, until="field_ready=1")
        limit += _field_or_handle(s, step, ctx, limit)
        p = s.probe()
        if (p.x, p.z) == here:
            tries += 1
            if tries > 3:
                raise HarnessError("steps: stuck at (%d,%d) going %s to corner %d (%d,%d)" % (
                    here + (DIR_KEYS[d], k) + route[k]))
            continue
        tries = 0
        tiles += 1
        if k + 1 < len(route) and (p.x, p.z) == route[k + 1] and (p.x, p.z) != route[k]:
            s.note("steps: carried to (%d,%d)" % (p.x, p.z))
            k += 1
    s.run(16)
    p = s.probe()
    s.note("steps: %d tiles, at (%d,%d) on map %d" % (tiles, p.x, p.z, s.map_id))
    if "face" in step:
        f = FACINGS[step["face"]]
        if p.facing != f:
            s.run(2, DIR_KEYS[f])
            s.run(10)
    if step.get("interact"):
        s.run(4, "a")
        s.run(12)


def _use_field_move(s, step, ctx, what, limit):
    """Facing a field-move tile or object after a bump: A asks (Surf, Waterfall, Rock Climb, Cut, Rock Smash: 'Would
    you like to use ...?', YES is the cursor's default), A answers YES, then the scene plays until the player is
    free; a Rock Smash wild battle is fought. Returns the frames spent in battles."""
    s.note("walk_to: %s ahead, using the field move" % what)
    s.run(4, "a")
    bot_advance_text(s, {"max": 2400}, ctx)
    return _field_or_handle(s, step, ctx, limit)


def bot_walk_to(s, step, ctx):
    """Walk to tile (x, z): A* over the probe's terrain, replanning as it learns; warps by walking into them.

    With via = [[x, z], ...] it walks to each waypoint first (each under its own `max`): routes longer than the
    probe's 64x64 window, or past what A* cannot know (a bridge's deck vs the path under it). A waypoint someone
    stands on (a trainer who walked up to the player there) counts as reached from the tile next to it."""
    if step.get("via"):
        leg = {k: v for k, v in step.items() if k not in ("via", "face", "interact", "map")}
        for i, (wx, wz) in enumerate(step["via"]):
            bot_walk_to(s, dict(leg, x=wx, z=wz, _corner=True,
                                **({"map": step["map"]} if i == 0 and "map" in step else {})), ctx)
        step = {k: v for k, v in step.items() if k not in ("via", "map")}
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
    terrain = Terrain(surf=bool(step.get("surf")), hm=bool(step.get("hm")), avoid=step.get("avoid", ()))
    field_tries = {}  # tile -> field-move attempts
    start_map = p.map_id
    steps = 0
    warped = False
    while (p.x, p.z) != goal:
        if s.frame >= limit:
            near = sorted((o[0], o[1], o[2]) for o in p.objects if abs(o[0] - p.x) + abs(o[1] - p.z) <= 4)
            raise HarnessError("walk_to (%d,%d): still at (%d,%d) after %d frames (battles excluded); facing %d, "
                               "objects near (x, z, id) %s, %d edges learned blocked %s" % (
                                   goal + (p.x, p.z, bound, p.facing, near, len(terrain.blocked_edges),
                                           sorted(terrain.blocked_edges)[:8])))
        terrain.update(p)
        if step.get("_corner") and goal in terrain.objects and abs(p.x - goal[0]) + abs(p.z - goal[1]) == 1:
            s.note("walk_to: waypoint (%d,%d) is occupied; passing it from (%d,%d)" % (goal + (p.x, p.z)))
            break
        dirs = terrain.path((p.x, p.z), goal)
        if not dirs and terrain.blocked_edges:
            # what bumping taught may have been a person in the way: forget it once
            terrain.blocked_edges.clear()
            dirs = terrain.path((p.x, p.z), goal)
        if not dirs:
            # just after a warp the probe can still hold the last map's window: settle, re-read, plan again
            s.run(30)
            p = s.probe()
            terrain.cells.clear()
            terrain.update(p)
            dirs = terrain.path((p.x, p.z), goal)
        if not dirs:
            raise HarnessError("walk_to (%d,%d): no path from (%d,%d) on map %d" % (goal + (p.x, p.z, p.map_id)))
        d = dirs[0]
        here = (p.x, p.z)
        terrain.visits[here] = terrain.visits.get(here, 0) + 1
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
        if abs(p.x - here[0]) + abs(p.z - here[1]) > 2:
            # a warp panel to another spot of the same map (Platinum's Galactic HQ): the map id stays
            nxt = (here[0] + DIR_DELTA[d][0], here[1] + DIR_DELTA[d][1])
            s.note("walk_to: warped within map %d to (%d,%d) stepping %s from (%d,%d)" % (start_map, p.x, p.z,
                                                                                         DIR_KEYS[d], *here))
            if nxt == goal or here == goal or len(dirs) == 1:
                warped = True
                break
            raise HarnessError("walk_to (%d,%d): an unexpected warp panel to (%d,%d) at (%d,%d)" % (
                goal + (p.x, p.z) + here))
        if (p.x, p.z) == here:
            nxt = (here[0] + DIR_DELTA[d][0], here[1] + DIR_DELTA[d][1])
            fm = terrain.field_move(nxt[0], nxt[1], d)
            cell = p.cell(*here)
            surfing = cell is not None and (cell & TILE_BEHAVIOR) in SURFABLE
            if fm and not (fm == "water" and surfing) and field_tries.get(nxt, 0) < 2:
                field_tries[nxt] = field_tries.get(nxt, 0) + 1
                limit += _use_field_move(s, step, ctx, fm, limit)
                p = s.probe()
                continue
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
    if "face" not in step and not step.get("interact"):
        return
    for attempt in range(3):
        if step.get("interact") and not warped and (not s.field_ready or s.in_battle):
            # a trainer who spotted the arrival, or someone's text: not the talk this step is for
            _field_or_handle(s, step, ctx, s.frame + _int(step, "max", 6000))
            p = s.probe()
            if (p.x, p.z) != goal:
                bot_walk_to(s, {k: v for k, v in step.items() if k not in ("face", "interact")}, ctx)
        if "face" in step:
            d = FACINGS[step["face"]]
            if s.probe().facing != d:
                s.run(2, DIR_KEYS[d])
                s.run(10)
        if not step.get("interact"):
            return
        s.run(4, "a")
        if not s.run(12, until="in_battle=1"):
            return
        s.note("walk_to: a battle started as the talk began; fought, then talking again")


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


# ---------------------------------------------------------------- party / grind
def party(s, ctx):
    """The party as the game has it now: an in-game save, then np_save4's dump of the save file np_gp wrote."""
    before = _stores(s)
    bot_save(s, {}, ctx)
    for _ in range(60):  # np_gp writes the chip file when the game's card write completes
        if _stores(s) > before:
            break
        s.run(2)
    else:
        raise HarnessError("party: the save was not written to %s" % s.save_path)
    out = subprocess.run([ctx.save4, "dump", ctx.rom, s.save_path], capture_output=True, text=True)
    if out.returncode != 0:
        raise HarnessError("party: np_save4 cannot read %s" % s.save_path)
    return json.loads(out.stdout)["party"]


def _stores(s):
    s._log.flush()
    with open(s.log_path, errors="replace") as f:
        return f.read().count("-byte save to ")


def bot_walk_to_door(s, step, ctx):
    """walk_to the door the guest's log names: the last match of `pattern` (one group) in the run log keys
    `doors` (group -> [x, z]); the other keys go to walk_to. For a door the game rolls at random and shows only
    as a clue (Platinum's Hearthome Gym: pc_np_field.c e2e_gym_log)."""
    rx = re.compile(step["pattern"])
    hits = []
    for _ in range(_int(step, "wait", 120) // 10 + 1):
        s._log.flush()
        with open(s.log_path, errors="replace") as f:
            hits = rx.findall(f.read())
        if hits:
            break
        s.run(10)
    if not hits:
        raise HarnessError("walk_to_door: the log has no /%s/" % step["pattern"])
    door = step["doors"].get(str(hits[-1]))
    if door is None:
        raise HarnessError("walk_to_door: no door %r in %s" % (hits[-1], sorted(step["doors"])))
    s.note("walk_to_door: %s -> (%d,%d)" % (hits[-1], door[0], door[1]))
    sub = {k: v for k, v in step.items() if k not in ("do", "pattern", "doors", "wait")}
    sub.update(x=int(door[0]), z=int(door[1]))
    bot_walk_to(s, sub, ctx)


def bot_hatch(s, step, ctx):
    """Pace between (x, z) and (x+1, z) until every egg in the party has hatched (a no-op without one): an egg hatches
    on whichever step its cycles run out on (src/egg_hatch.c), so a chain hatches it here, on safe ground, rather than in
    a later puzzle room's walk. The hatch scene's text runs with B (its nickname question: NO)."""
    bound = _int(step, "max", 60000)
    limit = s.frame + bound
    eggs = sum(1 for m in party(s, ctx) if m.get("is_egg"))
    if not eggs:
        s.note("hatch: no egg in the party")
        return
    spot = (int(step["x"]), int(step["z"]))
    bot_walk_to(s, {"x": spot[0], "z": spot[1]}, ctx)
    k = 0
    while s.frame < limit:
        k += 1
        p = s.probe()
        d = "right" if p.x == spot[0] else "left"
        s.run(24, d + "+b", until=["x!=%d" % p.x, "in_battle=1"])
        held = s.frame
        s.run(24, until="field_ready=1")
        if s.field_ready:
            continue
        _field_or_handle(s, {"on_battle": "flee"}, ctx, limit)
        if s.frame - held < 200:
            continue
        left = sum(1 for m in party(s, ctx) if m.get("is_egg"))
        s.note("hatch: a scene of %d frames after %d steps; %d egg(s) left" % (s.frame - held, k, left))
        if not left:
            return
    raise HarnessError("hatch: %d egg(s) still unhatched after %d frames" % (eggs, bound))


def bot_grind(s, step, ctx):
    """Fight wild battles in the tall grass at (x, z)/(x+1, z) until the lead reaches `level`, healing at the
    Pokemon Center door `heal` = [x, z] (same coordinate space) whenever the lead is below half HP or down to 4 PP
    (its `move`, else all its damaging moves together)."""
    level = _int(step, "level", 0)
    bound = _int(step, "max", 60000)
    limit = s.frame + bound
    spot = (int(step["x"]), int(step["z"]))
    door = step.get("heal")
    battles = 0
    while True:
        mons = party(s, ctx)
        lead = next((m for m in mons if not m.get("is_egg")), None)
        if lead is None:
            raise HarnessError("grind: no party")
        if lead["level"] >= level:
            break
        if s.frame >= limit:
            raise HarnessError("grind: lead at level %d after %d battles, wanted %d" % (lead["level"], battles, level))
        if "move" in step:
            mv = lead["moves"][_int(step, "move", 0)] if len(lead["moves"]) > _int(step, "move", 0) else {"pp": 0}
            pp = mv["pp"]
        else:
            gd = gamedata(ctx)
            pp = sum(m["pp"] for m in lead["moves"] if m["id"] < len(gd["moves"])
                     and gd["moves"][m["id"]][1] != MOVE_CLASS_STATUS and gd["moves"][m["id"]][2] > 0)
        if door and (lead["hp"] * 2 < lead["stats"][0] or pp < 5):
            bot_heal(s, {"x": door[0], "z": door[1]}, ctx)
        bot_walk_to(s, {"x": spot[0], "z": spot[1]}, ctx)
        p = s.probe()
        t = Terrain()
        for c in (spot, (spot[0] + 1, spot[1])):
            cell = p.cell(*c)
            if cell is None or (cell & TILE_BEHAVIOR) not in t.grass:
                raise HarnessError("grind: (%d,%d) is not tall grass" % c)
        k = 0
        while not s.in_battle and s.frame < limit:
            k += 1
            s.run(24, "right" if k % 2 else "left", until=["in_battle=1", "x!=%d" % (spot[0] + (0 if k % 2 else 1))])
            s.run(12, until="in_battle=1")
        if s.in_battle:
            bot_auto_battle(s, {"move": step["move"]} if "move" in step else {}, ctx)
            battles += 1
            bot_wait_field(s, {}, ctx)
    s.note("grind: lead at level %d after %d battles" % (lead["level"], battles))


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
            # the walk fights what it meets with this step's move and on_battle (a sight trainer on the way)
            sub = {k: step[k] for k in ("move", "on_battle", "on_text") if k in step}
            sub.update(x=c[0], z=c[1], max=min(900, max(limit - s.frame, 1)))
            bot_walk_to(s, sub, ctx)
        except HarnessError as e:
            s.note("talk_to: %s; re-planning" % e)
    raise HarnessError("talk_to: object %d not reached in %d frames" % (oid, bound))


def bot_slide(s, step, ctx):
    """Ice: each direction in `dirs` (space separated) is a press, then a wait until the slide (or the step) has
    stopped -- the tile unchanged over six probes with the player free; battles and text met on the way are handled
    as walk_to does. A press made mid-slide would be ignored, so a plain press list cannot replay an ice route."""
    dirs = step["dirs"].split()
    limit = s.frame + _int(step, "max", 300 * len(dirs))
    for i, d in enumerate(dirs):
        s.run(8, d)
        last, still = None, 0
        while still < 6:
            if s.frame >= limit:
                raise HarnessError("slide: press %d (%s) has not settled by the step's bound" % (i + 1, d))
            if s.in_battle or not s.field_ready:
                limit += _field_or_handle(s, step, ctx, limit)
                last, still = None, 0
                continue
            s.run(4)
            p = s.probe()
            still = still + 1 if (p.x, p.z) == last else 0
            last = (p.x, p.z)
    p = s.probe()
    s.note("slide: %d presses, at (%d,%d)" % (len(dirs), p.x, p.z))
# ---------------------------------------------------------------- fly
# Platinum's start menu, top to bottom once the Pokedex and a party are owned (src/start_menu.c:593-601, the hidden
# RETIRE/CHAT left out). FieldSystem.menuCursorPos keeps the last chosen option and starts zeroed
# (field_system.c:153-154): START_MENU_OPTION_POKEDEX.
START_MENU = ["pokedex", "pokemon", "bag", "trainer_case", "save", "options", "exit"]
# Moves the party menu lists as field moves, in sFieldMoves order (src/applications/party_menu/main.c:247-263):
# Cut, Fly, Surf, Strength, Defog, Rock Smash, Waterfall, Rock Climb, Flash, Teleport, Dig, Sweet Scent, Chatter,
# Milk Drink, Softboiled
FIELD_MOVES = {15, 19, 57, 70, 432, 249, 127, 431, 148, 100, 91, 230, 448, 208, 135}
MOVE_FLY = 19
# The fly map's cursor stays within x 1..28, z 6..28 (town_map/graphics.c:399-425)
FLY_X, FLY_Z = (1, 28), (6, 28)


def fly_blocks(name):
    """The town-map blocks of a fly destination: the cells of the overworld matrix (map_matrix_000, which
    MainMapMatrixData_Load reads, src/map_matrix.c:149-161) whose header is `name` (CanFlyToHoveredLocation: the
    hovered cell's header must be the fly location's, town_map/graphics.c:1125-1140, fly_locations.c:291-304)."""
    path = os.path.join(ROOT, "games", "platinum", "res", "field", "matrices", "map_matrix_000.json")
    headers = json.load(open(path))["headers"]
    return [(x, z) for z, row in enumerate(headers) for x, h in enumerate(row) if h == name]


def _menu_key(s, key, n=1, gap=8):
    for _ in range(n):
        s.run(2, key)
        s.run(gap)


def bot_fly(s, step, ctx):
    """Fly to `map` (a town's header) the way a player does: X, POKEMON, the party member that knows Fly, FLY, the
    town map's cursor moved block by block to the destination, A. `slot` names the party slot (default: the first
    that knows Fly, from an in-game save's dump); `block = [x, z]` the town-map block (default: the destination's
    block on the overworld matrix nearest the player's). Platinum menus (start_menu.c, party_menu/main.c,
    town_map/graphics.c)."""
    if ctx.game != "platinum" and "block" not in step:
        raise HarnessError("fly: only Platinum's town map is known; give block = [x, z]")
    dest = ctx.resolve(step["map"])
    bot_wait_field(s, step, ctx)
    p = s.probe()
    here = (p.x // 32, p.z // 32)
    if "start" in step:
        # off the overworld matrix (caves, lakes, buildings) the town map opens on the exit location, the overworld
        # tile the player last left it from (town_map/context.c:108-116), which the save dump does not show
        here = tuple(int(v) for v in step["start"])
    elif ctx.game == "platinum":
        path = os.path.join(ROOT, "games", "platinum", "res", "field", "matrices", "map_matrix_000.json")
        headers = json.load(open(path))["headers"]
        on = here[1] < len(headers) and here[0] < len(headers[here[1]]) and headers[here[1]][here[0]] != "MAP_NONE" \
            and ctx.resolve(headers[here[1]][here[0]]) == s.map_id
        if not on:
            raise HarnessError("fly: map %d is off the overworld matrix: walk out first, or give start = [x, z] (the "
                               "town-map block of the exit location)" % s.map_id)
    if "block" in step:
        goal = tuple(int(v) for v in step["block"])
    else:
        blocks = fly_blocks(step["map"])
        if not blocks:
            raise HarnessError("fly: %s is on no overworld block" % step["map"])
        goal = min(blocks, key=lambda b: abs(b[0] - here[0]) + abs(b[1] - here[1]))
    if "slot" in step:
        slot = _int(step, "slot", 0)
        moves = None
    else:
        mons = party(s, ctx)
        k = next((i for i, m in enumerate(mons) if any(mv["id"] == MOVE_FLY for mv in m["moves"])), None)
        if k is None:
            raise HarnessError("fly: no party member knows Fly")
        slot, moves = k, [mv["id"] for mv in mons[k]["moves"]]
    if moves is None:
        mons = party(s, ctx)
        moves = [mv["id"] for mv in mons[slot]["moves"]]
    # the context menu: SUMMARY, the field moves in move-slot order (up to the first empty slot), SWITCH, ITEM,
    # CANCEL (party_menu/main.c:1791-1839)
    known = []
    for mv in moves:
        if not mv:
            break
        if mv in FIELD_MOVES:
            known.append(mv)
    if MOVE_FLY not in known:
        raise HarnessError("fly: party slot %d does not know Fly" % slot)
    entry = 1 + known.index(MOVE_FLY)
    s.note("fly: to %s (%d), block %s from %s, party slot %d, menu entry %d" % (step["map"], dest, goal, here, slot,
                                                                                entry))
    # start menu -> POKEMON
    s.run(2, "x")
    s.run(30)
    cur = START_MENU.index(getattr(s, "start_menu_option", "pokedex"))
    want = START_MENU.index("pokemon")
    _menu_key(s, "down" if want > cur else "up", abs(want - cur))
    s.run(2, "a")
    s.start_menu_option = "pokemon"
    s.run(90)
    # the party grid: two columns, slot 0 at the top left (GridMenuCursorPosition moves)
    _menu_key(s, "right", slot % 2)
    _menu_key(s, "down", slot // 2)
    s.run(2, "a")
    s.run(20)
    _menu_key(s, "down", entry)
    s.run(2, "a")
    s.run(120)
    # the fly map opens with the cursor on the player's block (town_map/main.c:218-227)
    gx = min(max(goal[0], FLY_X[0]), FLY_X[1])
    gz = min(max(goal[1], FLY_Z[0]), FLY_Z[1])
    _menu_key(s, "right" if gx > here[0] else "left", abs(gx - here[0]), gap=14)
    _menu_key(s, "down" if gz > here[1] else "up", abs(gz - here[1]), gap=14)
    s.run(20)
    if s.shot_dir:
        s.dump(os.path.join(s.shot_dir, "fly-map.ppm"))
    s.run(2, "a")
    if not s.run(_int(step, "max", 1500), until="map_id=%d" % dest):
        raise HarnessError("fly: still on map %d, not %s (%d)" % (s.map_id, step["map"], dest))
    bot_wait_field(s, {}, ctx)
    p = s.probe()
    s.note("fly: landed on map %d at (%d,%d)" % (s.map_id, p.x, p.z))


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
    "walk_to_door": bot_walk_to_door,
    "talk_to": bot_talk_to,
    "heal": bot_heal,
    "grind": bot_grind,
    "slide": bot_slide,
    "fly": bot_fly,
    "steps": bot_steps,
    "hatch": bot_hatch,
}
