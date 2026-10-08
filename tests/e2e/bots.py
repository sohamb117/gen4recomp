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
import struct
import subprocess
import sys

from np_e2e import (BW_GAMES, DIR_DELTA, DIR_KEYS, FACINGS, GBA_GAMES, HGSS_GAMES, ROOT, TILE_BEHAVIOR,
                    TILE_COLLISION, TILE_CONNECTED, TILE_KNOWN, UI_BATTLE_MENU, UI_BATTLE_PARTY, UI_FIELD_MENU,
                    HarnessError, behaviors)

# ---- the battle's touch screen (Platinum src/battle/battle_subscreen.c touch rects; D/P's overlay 11
# tables are byte-identical, and so are HG/SS's: src/battle/battle_input.c sTouchscreenRect*Buttons, overlay 8's
# party tables ov08_02224F1C / ov08_02224E54), as tap points: the centre of each button.
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
# HG/SS (src/metatile_behavior.c sMetatileBehaviorFlags with TILE_BEHAVIOR_FLAG_SURFABLE): Platinum's numbers plus
# 0x73, 0x78 and 0x7C, and 0x11 is WHIRLPOOL, crossed only with the HM Whirlpool, so not planned through.
HGSS_SURFABLE = (SURFABLE - {0x11}) | {0x73, 0x78, 0x7C}
# A* cost of a field-move tile or object: one interaction plus its scene
FIELD_MOVE_COST = 4
# A* cost added per earlier visit of a tile in the same walk (Terrain.visits)
VISIT_COST = 2

# Ruby/Sapphire/Emerald (include/constants/metatile_behaviors.h, the same numbers in both decomps): the behaviors
# with TILE_FLAG_SURFABLE in pokeemerald src/metatile_behavior.c sTileBitAttributes (MB_POND_WATER 0x10 ..
# MB_OCEAN_WATER 0x15, MB_NO_SURFACING 0x19, MB_SEAWEED 0x22, MB_SEAWEED_NO_SURFACING 0x2A, the currents
# 0x50..0x53); MB_WATERFALL 0x13 among them is climbed with Waterfall. Field-move objects by graphics id
# (include/constants/event_objects.h, both decomps): OBJ_EVENT_GFX_CUTTABLE_TREE 82, OBJ_EVENT_GFX_BREAKABLE_ROCK 86.
GBA_SURFABLE = frozenset({0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x19, 0x22, 0x2A, 0x50, 0x51, 0x52, 0x53})
GBA_WATERFALL = 0x13
GBA_GFX_CUTTABLE_TREE, GBA_GFX_BREAKABLE_ROCK = 82, 86
# PLAYER_AVATAR_FLAG_UNDERWATER (both decomps' include/global.fieldmap.h): B under water asks to surface
# (field_control_avatar.c TrySetDiveWarp), so an underwater walk never holds B to run
GBA_AVATAR_UNDERWATER = 1 << 4


def _int(step, key, default):
    v = step.get(key, default)
    return int(v)


def snap(s):
    """Dump the screen now onto the milestone's contact sheet (run.py collects frame_NNNNNN.ppm, labelled by
    frame): proof points inside a bot (a battle's first menu, a fish on the hook)."""
    if getattr(s, "shot_dir", None):
        s.dump(os.path.join(s.shot_dir, "frame_%06d.ppm" % s.frame))


def _tap(s, xy, hold=4, gap=10):
    s.run(hold, touch=xy)
    s.run(gap)


# ---------------------------------------------------------------- simple input
def bot_press(s, step, ctx):
    """Raw buttons. With `until` (np_gp's condition syntax: a status or probe field, e.g. "field=0", "map_id=9"),
    the press repeats (up to `times`, default 20) until the condition holds after a press, and fails if it never
    does: for a screen whose readiness is only seen in the probe (the GBA wall clock taking over the field)."""
    keys = step["keys"]
    until = step.get("until")
    hold, gap, times = _int(step, "hold", 4), _int(step, "gap", 12), _int(step, "times", 20 if until else 1)
    if until and s.run(1, until=until):
        return
    for _ in range(times):
        s.run(hold, keys)
        if s.run(gap, until=until or ()) and until:
            return
    if until:
        raise HarnessError("press %s: %s not reached after %d presses" % (keys, until, times))


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


def bot_wait_reset(s, step, ctx):
    """Run until the game resets the system itself, the end of a step list that plays the game to its end: after the
    credits ClearGame calls OS_ResetSystem (Platinum src/clear_game.c:163). The port reboots the guest as the console
    does (pc/src/pc_os_lite.c, np_host_reset): the backup chip is kept and stored, the runtime counts the reset in the
    `resets` status and logs "np_core: soft reset N". That count going up is this step's success; a stop of the core,
    or no reset in `max` frames, fails it. The game ends there (s.ended): the core runs on at the boot screens, so
    run.py takes no end save or probe, and [expect] reads the save the game wrote before the reset."""
    bound = _int(step, "max", 20000)
    if ctx.game in GBA_GAMES:
        # the GBA games end the credits in SoftReset (pokeemerald src/credits.c Task_CreditsSoftReset), which the port
        # runs in place (gba_main.c gba_soft_reset): the probe's soft_resets counts it
        r0 = s.probe().soft_resets
        end = s.frame + bound
        while s.frame < end:
            s.run(min(60, end - s.frame), step.get("keys"))
            p = s.probe()
            if p is not None and p.soft_resets != r0:
                s.ended = "SoftReset"
                s.note("wait_reset: the game reset itself (soft reset %d)" % p.soft_resets)
                return
        raise HarnessError("no SoftReset in %d frames" % bound)
    resets = s.stat("resets")
    if not s.run(bound, step.get("keys"), until="resets!=%d" % resets):
        raise HarnessError("no OS_ResetSystem in %d frames" % bound)
    s.ended = "OS_ResetSystem"
    s.note("wait_reset: the game reset itself (soft reset %d)" % s.stat("resets"))


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


# Black/White's in-game save through their own menu (they have no host quick save): X opens the start menu on the
# touch screen, SAVE is its left button of the second row, A answers YES and closes the messages. The tap point and
# the timings are the proven proposal run's (docs/BW_PLAN.md: X, then SAVE at (64,94), then A every 40 frames).
BW_TAP_SAVE = (64, 94)


def _bw_menu_save(s):
    s.run(4, "x")
    if s.run(40, until="field_ready=1") or s.field_ready:
        raise HarnessError("save: X did not open the menu")
    _tap(s, BW_TAP_SAVE, 4, 60)
    for _ in range(30):
        s.run(4, "a")
        if s.run(36, until="field_ready=1"):
            s.run(4)
            return
    raise HarnessError("save: the save menu did not close in 1200 frames")


def bot_save(s, step, ctx):
    """The in-game save, through the host's quick-save request (np_core's quicksave_seq); Black/White through the
    game's menu."""
    bot_wait_field(s, {"max": step.get("max", 3000)}, ctx)
    if s.game in BW_GAMES:
        _bw_menu_save(s)
        return
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
    asks something A must not answer) run on. `key = "b"` presses B instead: B advances a message as A does but
    answers a YES/NO as NO; HG/SS's scripted phone calls need it (the call leaves the Pokegear open on its contact
    list, where A would place another call and two B's close it: README, HeartGold and SoulSilver)."""
    key = str(step.get("key", "a")).lower()
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
        if not s.run(2, key, until=stop):
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
        out = subprocess.run(ctx.save4 + ["gamedata", ctx.rom], capture_output=True, text=True)
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
    wild Pokemon) and fights once FLEE_TRIES runs have not ended the battle.

    With snap = true the screen at the first action menu (both battlers on the field) goes on the contact sheet.

    Ruby/Sapphire/Emerald have no touch screen: _gba_auto_battle makes the same choices with buttons."""
    if ctx.game in GBA_GAMES:
        return _gba_auto_battle(s, step, ctx)
    fixed = "move" in step
    move = _int(step, "move", 0)
    send_best = step.get("send") == "best"  # a fainted lead's replacement: the best scorer, not the first able
    flee = FLEE_TRIES if step.get("flee") else 0
    snap_menu = bool(step.get("snap"))
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
            if idx in MENU_ACTION and snap_menu:
                snap_menu = False
                snap(s)
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


# ---------------------------------------------------------------- auto_battle on the GBA
# Ruby/Sapphire/Emerald battles take buttons; the probe (np_e2e.h v4) reports the menu the game waits on with the DS
# numbering and its cursor (games/emerald/pc/src/emerald_e2e.c, ruby_e2e.c). The game's BATTLE_TYPE_* bits
# (include/constants/battle.h): DOUBLE 1 << 0, TRAINER 1 << 3, MULTI 1 << 6.
GBA_BATTLE_DOUBLE, GBA_BATTLE_TRAINER, GBA_BATTLE_MULTI = 0x01, 0x08, 0x40
# the YES/NO cursor (0 YES, 1 NO) each prompt is answered with: nickname NO, "delete a move?" NO, "stop learning?"
# YES, "use the next Pokemon?" YES, "switch Pokemon?" NO
GBA_MENU_ANSWER = {13: 1, 14: 1, 15: 0, 16: 0, 17: 1}
GBA_ACTION_FIGHT, GBA_ACTION_POKEMON, GBA_ACTION_RUN = 0, 2, 3  # the action menu's 2x2 cursor


def _gba_press(s, key, settle=6):
    """One press: the game's menus take a key on the frame it goes down (JOY_NEW)."""
    s.run(2, key)
    s.run(settle)


def _gba_grid_cursor(s, idx, want):
    """Moves the 2x2 cursor of battle menu `idx` (0 top left, 1 top right, 2 bottom left, 3 bottom right:
    battle_controller_player.c HandleInputChooseAction / HandleInputChooseMove) onto `want`; False when the menu
    went away or the cursor would not go there (a move slot past the last move)."""
    for _ in range(4):
        p = s.probe()
        if p is None or p.ui != UI_BATTLE_MENU or p.ui_arg != idx:
            return False
        if p.ui_cursor == want:
            return True
        if (p.ui_cursor ^ want) & 1:
            _gba_press(s, "right" if want & 1 else "left")
        else:
            _gba_press(s, "down" if want & 2 else "up")
    p = s.probe()
    return p is not None and p.ui == UI_BATTLE_MENU and p.ui_arg == idx and p.ui_cursor == want


def _gba_auto_battle(s, step, ctx):
    """bot_auto_battle for the GBA games: the same move and switch choices (best_damage, choose_move, replacement
    over the probe's battle report), made with the D-pad and A on the cursor the probe reports. The party menu
    lists gPlayerParty in slot order with the battler that is out in slot 0 (slots 0 and 1 in a double battle)."""
    fixed = "move" in step
    move = _int(step, "move", 0)
    send_best = step.get("send") == "best"
    flee = FLEE_TRIES if step.get("flee") else 0
    snap_menu = bool(step.get("snap"))
    limit = s.frame + _int(step, "max", 30000)
    if not s.in_battle and not s.run(_int(step, "wait", 900), until="in_battle=1"):
        raise HarnessError("no battle started within %d frames" % _int(step, "wait", 900))
    gd = None if fixed else gamedata(ctx)
    p = s.probe()
    since = p.frame if p is not None else 0
    party_try = 0
    turns = 0
    last = None
    target = 0       # 0 the foe battler 1, 1 battler 3
    again = 0
    menus = {}
    rejected = []
    slot = None
    want = None
    shift_tries = 0
    switched_at = -1
    report = None
    tried = {}
    useless = set()
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
            if fresh and idx in (1, 11, 12):
                me = p.battlers[p.menu_battler]
                foes = [b for b in (1, 3) if b < len(p.battlers) and p.battlers[b].alive]
                if foes and (1 + 2 * target) not in foes:
                    target = (foes[0] - 1) // 2
                foe = p.battlers[1 + 2 * target] if foes else None
                mate = p.menu_battler ^ 2
                ally = bool(p.battle_type & GBA_BATTLE_DOUBLE) and mate < len(p.battlers) and p.battlers[mate].alive
                if idx == 1 and p.menu_battler in tried:
                    mv, fb, sp, hp = tried.pop(p.menu_battler)
                    b = p.battlers[fb] if fb < len(p.battlers) else None
                    if b is not None and b.species == sp and b.hp > 0 and b.hp >= hp:
                        block, _ = blocking_abilities(gd, sp, mv, mon_types(gd, b))
                        if block and (mv, sp) not in useless:
                            useless.add((mv, sp))
                            s.note("auto_battle: move %d did nothing to species %d (ability %s); not using it on it "
                                   "again" % (mv, sp, block))
            if idx == 1 and snap_menu:
                snap_menu = False
                snap(s)
            if idx == 1:
                choice = GBA_ACTION_FIGHT
                if flee:
                    flee -= 1
                    choice = GBA_ACTION_RUN
                else:
                    rejected = []
                    k = None
                    if (fresh and not p.battle_type & GBA_BATTLE_MULTI and me.alive and switched_at != turns
                            and best_damage(gd, me, foe, ally=ally, useless=useless)[0] is None):
                        first = 2 if p.battle_type & GBA_BATTLE_DOUBLE else 1
                        k = replacement(gd, p.party, foe, first, ally, useless, best=send_best)
                        if k is not None and best_damage(gd, p.party[k], foe, ally=ally, useless=useless)[0] is None:
                            k = None
                    if k is not None:
                        s.note("auto_battle: no damaging move left; switching to party slot %d" % k)
                        want, switched_at = k, turns
                        choice = GBA_ACTION_POKEMON
                    else:
                        turns += 1
                    party_try = 0
                if _gba_grid_cursor(s, 1, choice):
                    _gba_press(s, "a")
            elif idx == 11:
                again = again + 1 if last == 11 else 0
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
                if _gba_grid_cursor(s, 11, slot):
                    _gba_press(s, "a")
                    s.run(30, until=["ui_arg!=11", "ui!=%d" % UI_BATTLE_MENU, "in_battle=0"])
                else:
                    again = 2  # the slot cannot be reached (no move there): the next one
            elif idx == 12:
                if last == 12:
                    target = 1 - target  # the target menu again: that opponent is gone, take the other
                tb = 1 + 2 * target
                for _ in range(4):
                    q = s.probe()
                    if q is None or q.ui != UI_BATTLE_MENU or q.ui_arg != 12 or q.ui_cursor == tb:
                        break
                    _gba_press(s, "right")
                _gba_press(s, "a")
            elif idx in GBA_MENU_ANSWER:
                answer = GBA_MENU_ANSWER[idx]
                if p.ui_cursor != answer:
                    _gba_press(s, "down" if answer else "up")
                _gba_press(s, "a")
            else:
                _gba_press(s, "b", 10)
            last = idx
            menus[idx] = menus.get(idx, 0) + 1
            continue
        if p is not None and p.ui == UI_BATTLE_PARTY:
            if p.ui_arg == 0:
                if want is None and report is not None and party_try == 0 and gd is not None:
                    # a fainted lead: the party menu's own list (gPlayerParty, the one out in slot 0) and the foe
                    foe = next((report.battlers[b] for b in (1 + 2 * target, 3 - 2 * target)
                                if b < len(report.battlers) and report.battlers[b].alive), None)
                    first = 2 if report.battle_type & GBA_BATTLE_DOUBLE else 1
                    if not report.battle_type & GBA_BATTLE_MULTI:
                        want = replacement(gd, p.party or report.party, foe, first, best=send_best)
                if want is not None and last != ("party", want):
                    party_try = want
                    last = ("party", want)
                else:
                    want = None
                    party_try = party_try % 5 + 1
                    last = ("party", None)
                shift_tries = 0
                for _ in range(8):  # party_menu.c: DOWN walks the slots in order, then CANCEL, then slot 0
                    q = s.probe()
                    if q is None or q.ui != UI_BATTLE_PARTY or q.ui_arg != 0 or q.ui_cursor == party_try:
                        break
                    _gba_press(s, "down")
                _gba_press(s, "a", 12)
            elif shift_tries >= 3:
                s.note("auto_battle: party slot %d refused; the next one" % party_try)
                _gba_press(s, "b", 20)
                shift_tries = 0
                want = None
            else:
                # SHIFT is the first entry of Task_HandleSelectionMenuInput's list
                _gba_press(s, "a", 12)
                shift_tries += 1
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

    def __init__(self, surf=False, hm=False, avoid=(), game=None):
        self.surf, self.hm = surf, hm
        # tiles the step says are blocked though the probe's grid shows them free (props with their own collision,
        # e.g. D/P Veilstone Gym's sliding bars): never planned through, so never bumped
        self.avoid = {(int(x), int(z)) for x, z in avoid}
        self.gba = game in GBA_GAMES
        self.blocked_edges = {}  # (x, z, d) -> attempts that failed
        # (x, z) -> times the walk stood there: each visit makes the tile cost VISIT_COST more, so plans that
        # flip as the window slides (unknown tiles are hoped passable) stop swinging between two tiles
        self.visits = {}
        self.cells = {}          # (x, z) -> cell, kept across probes of the same map
        self.window = None       # the latest probe window's top-left tile
        self.warp_tiles = set()  # GBA: step-on warp tiles of the map, avoided unless the goal
        # The probe's step layers (D/P, GBA): (x, z) -> {height: {d: (tx, tz, height or None)}}, the steps the
        # game's own movement check allows from each place to stand (a bridge deck and the path under it are two),
        # and the player's height. Tiles without layers (Platinum, or not reached) are planned from the cells alone.
        self.layers = {}
        self.height = None
        self.objects = set()
        self.hm_objects = set()  # cut trees and Rock Smash rocks, when hm
        # the goal is across a GBA map border (a connected map's tile): only then are such tiles planned through
        self.goal_connected = False
        if self.gba:
            self._gba_tables()
            return
        # Black/White: np_e2e.behaviors has none for them, and the Surf/HM tiles and objects below are Platinum/D/P
        # numbers, so their walks go by the collision bit and the game's step layers alone
        bw = game in BW_GAMES
        self.surfable, self.waterfall, self.rock_climb = (set(), None, {}) if bw else (SURFABLE, WATERFALL, ROCK_CLIMB)
        self.hm_gfx = () if bw else (GFX_ROCK_SMASH, GFX_CUT_TREE)
        b = behaviors(game)
        self.jump = {b[k]: d for d, k in enumerate(("JUMP_NORTH", "JUMP_SOUTH", "JUMP_WEST", "JUMP_EAST")) if k in b}
        self.block_into = {}  # behavior -> directions that cannot enter the tile
        for name, dirs in (("BLOCK_EASTWARD", (3,)), ("BLOCK_WESTWARD", (2,)), ("BLOCK_NORTHWARD", (0,)),
                           ("BLOCK_SOUTHWARD", (1,))):
            if name in b:
                self.block_into[b[name]] = dirs
        water = [v for k, v in b.items() if k.startswith("WATER") or k in ("WATERFALL", "DEEP_WATER")]
        self.water = set(water)
        if game in HGSS_GAMES:
            # HG/SS's surfable numbers (and the whirlpool) beyond Platinum's WATER* names
            self.surfable = HGSS_SURFABLE
            self.water |= HGSS_SURFABLE | {0x11}
        # Tall grass costs GRASS_COST steps: the planner goes round it where it can, as a player would, so a
        # walk meets fewer wild battles and reaches the route's trainers with more HP.
        self.grass = {b[k] for k in ("TALL_GRASS", "VERY_TALL_GRASS", "MUD_WITH_GRASS", "MUD_DEEP_WITH_GRASS") if k in b}
        # Bike slopes (e.g. Route 209 (562,691..692)) go uphill only at bicycle speed: on foot the player
        # slides back down forever, so walk_to never plans across one.
        self.slopes = {b[k] for k in ("BIKE_SLOPE_TOP", "BIKE_SLOPE_BOTTOM") if k in b}
        # exit mats and the direction that leaves through them (map_tile_behaviors.h)
        self.mats = {}
        for name, d in (("WARP_ENTRANCE_NORTH", 0), ("WARP_ENTRANCE_SOUTH", 1), ("WARP_ENTRANCE_WEST", 2),
                        ("WARP_ENTRANCE_EAST", 3), ("WARP_NORTH", 0), ("WARP_SOUTH", 1), ("WARP_WEST", 2),
                        ("WARP_EAST", 3), ("WARP_STAIRS_WEST", 2), ("WARP_STAIRS_EAST", 3)):
            if name in b:
                self.mats[b[name]] = d
        if game in HGSS_GAMES:
            # HG/SS ladders (TILE_BEHAVIOR_LADDER_NORTH/_SOUTH 0x3C/0x3D) warp when the player stands on one and
            # pushes north / south (src/field/field_control.c:500-511), as an exit mat does; LADDER_DOWN 0x3E warps
            # when stepped onto (:743-745), as a door does
            self.mats.update({0x3C: 0, 0x3D: 1})

    def _gba_tables(self):
        """Ruby/Sapphire/Emerald metatile behaviors (both decomps number include/constants/metatile_behaviors.h
        alike). The step layers carry the game's own verdict on collision, ledges, one-way tiles and elevation
        (water is elevation 1 to a walker at 3); these sets are what the planner adds to it."""
        self.surfable, self.waterfall, self.rock_climb = GBA_SURFABLE - {GBA_WATERFALL}, GBA_WATERFALL, {}
        self.hm_gfx = (GBA_GFX_BREAKABLE_ROCK, GBA_GFX_CUTTABLE_TREE)
        self.jump = {0x3A: 0, 0x3B: 1, 0x39: 2, 0x38: 3}  # MB_JUMP_NORTH, _SOUTH, _WEST, _EAST
        self.block_into = {}
        self.water = set(GBA_SURFABLE)
        self.grass = {0x02, 0x03, 0x24}  # MB_TALL_GRASS, MB_LONG_GRASS, MB_ASHGRASS
        # forced movement (MB_WALK_* 0x40..0x43, MB_SLIDE_* 0x44..0x47), MB_MUDDY_SLOPE (the Mach Bike's)
        # and the cracked floors (MB_CRACKED_FLOOR 0xD2 gives way under a second visit, MB_CRACKED_FLOOR_HOLE 0x66 drops
        # the player a floor: Granite Cave B1F, Sky Pillar), as tests/e2e/gba_world.py leaves them out
        self.slopes = set(range(0x40, 0x48)) | {0xD0, 0xD2, 0x66}
        # arrow warps (field_control_avatar.c TryArrowWarp): MB_EAST/WEST/NORTH/SOUTH_ARROW_WARP 0x62..0x65,
        # MB_WATER_SOUTH_ARROW_WARP 0x6D
        self.mats = {0x62: 3, 0x63: 2, 0x64: 0, 0x65: 1, 0x6D: 1}

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
        self.window = (p.grid_x0, p.grid_z0)
        lay = p.layers()
        if lay:
            self.layers.update(lay)
        self.height = p.player_height if lay and p.player_height in lay.get((p.x, p.z), {}) else None
        self.objects = {(o[0], o[1]) for o in p.objects}
        self.hm_objects = {(o[0], o[1]) for o in p.objects if self.hm and o[3] in self.hm_gfx}
        if self.gba:
            # warp tiles that fire when stepped on (ladders, holes, cave mouths; not arrow mats, which need a push,
            # nor doors, entered moving north): never walked over unless they are the goal
            self.warp_tiles = set()
            for w in p.warps:
                c = self.cells.get((w[0], w[1]))
                if c is not None and (c & TILE_BEHAVIOR) not in self.mats and (c & TILE_BEHAVIOR) not in (0x69, 0x8D):
                    self.warp_tiles.add((w[0], w[1]))

    def field_move(self, x, z, d):
        """The field move that enters (x, z) moving in direction d ('water', 'waterfall', 'climb', 'object'), if the
        walk may use one there (`surf`, `hm`); else None."""
        if (x, z) in self.hm_objects:
            return "object"
        c = self.cells.get((x, z))
        if c is None:
            return None
        beh = c & TILE_BEHAVIOR
        if self.surf and beh in self.surfable and not (self.gba and c & TILE_COLLISION):
            # (a GBA rock drawn over the sea is ocean water with the collision bit: Route 128 (77,30))
            return "water"
        if self.surf and beh == self.waterfall and d == 0:
            return "waterfall"
        if self.gba and self.surf and beh == self.waterfall and d == 1:
            return "water"  # down the falls: the forced current carries a surfer (gba_world.py, waterfalls)
        if self.hm and beh in self.rock_climb and d in self.rock_climb[beh]:
            return "climb"
        return None

    def goal_ok(self, x, z, d):
        """A goal may be entered from any side, but a GBA animated door only moving north (field_control_avatar.c
        TryDoorWarp: the player faces north into MB_ANIMATED_DOOR 0x69 / MB_PETALBURG_GYM_DOOR 0x8D)."""
        if not self.gba or d == 0:
            return True
        c = self.cells.get((x, z))
        return c is None or (c & TILE_BEHAVIOR) not in (0x69, 0x8D)

    def passable(self, x, z, d, goal, terrain=False):
        """Can the player step into (x, z) moving in direction d? Unknown tiles are hoped passable. With terrain the
        game's own check already allowed the step (step layers), so the collision bit is not asked again."""
        if (x - DIR_DELTA[d][0], z - DIR_DELTA[d][1], d) in self.blocked_edges:
            return False
        if (x, z) == goal:
            return self.goal_ok(x, z, d)
        if (x, z) in self.avoid or (x, z) in self.warp_tiles:
            return False
        if self.field_move(x, z, d):
            return True
        if (x, z) in self.objects:
            return False
        c = self.cells.get((x, z))
        if c is None:
            # a GBA window cell the game knows no tile for is off the map (GetMapBorderIdAt: CONNECTION_INVALID);
            # elsewhere, and beyond the window, an unknown tile is hoped passable
            return not (self.gba and self.window and 0 <= x - self.window[0] < 64 and 0 <= z - self.window[1] < 64)
        if self.gba and c & TILE_CONNECTED:
            return False  # across a GBA map border: only the goal (the walk ends where the player crosses)
        beh = c & TILE_BEHAVIOR
        if (c & TILE_COLLISION and not terrain) or beh in self.water or beh == self.waterfall or beh in self.slopes:
            return False
        if beh in self.block_into and d in self.block_into[beh]:
            return False
        if beh in self.jump:
            return False  # handled as a jump edge
        return True

    def _layer(self, x, z):
        """The height of (x, z)'s only layer; None for no layers or several (the planner can't say which)."""
        lay = self.layers.get((x, z))
        return next(iter(lay)) if lay and len(lay) == 1 else None

    def _step_cost(self, nx, nz, d):
        c = self.cells.get((nx, nz))
        extra = self.visits.get((nx, nz), 0) * VISIT_COST
        if self.field_move(nx, nz, d) in ("object", "climb"):
            return FIELD_MOVE_COST + extra
        return extra + (GRASS_COST if c is not None and (c & TILE_BEHAVIOR) in self.grass else 1)

    def neighbours(self, x, z, h, goal):
        """(d, x, z, height, cost) for each step from (x, z) standing at height h (None: unknown layer)."""
        moves = self.layers.get((x, z), {}).get(h) if h is not None else None
        for d, (dx, dz) in enumerate(DIR_DELTA):
            nx, nz = x + dx, z + dz
            c = self.cells.get((nx, nz))
            if moves is not None:
                # the game's own verdict on the terrain (heights, bridges, collision); behaviors and objects here
                if (x, z, d) in self.blocked_edges:
                    continue
                if d in moves:
                    tx, tz, th = moves[d]
                    if (tx, tz) != (nx, nz):  # a ledge
                        if (nx, nz) != goal and (tx, tz) not in self.objects:
                            yield d, tx, tz, th, 2 + self.visits.get((tx, tz), 0) * VISIT_COST
                        continue
                    if self.passable(nx, nz, d, goal, terrain=True):
                        yield d, nx, nz, th, self._step_cost(nx, nz, d)
                elif ((nx, nz) == goal and self.goal_ok(nx, nz, d)) or self.field_move(nx, nz, d) in ("climb", "waterfall"):
                    # into a goal the terrain refuses (a door in a wall warps), or up a climb / waterfall
                    yield d, nx, nz, self._layer(nx, nz), self._step_cost(nx, nz, d)
                elif (nx, nz) not in self.layers and self.passable(nx, nz, d, goal):
                    # a tile the flood never reached (it stops at some map block edges, e.g. Oreburgh (300..304,
                    # 767|768)): the grid's word, as for a tile without layers; a bump teaches the rest
                    yield d, nx, nz, None, self._step_cost(nx, nz, d)
                continue
            if c is not None and (c & TILE_BEHAVIOR) in self.jump and (nx, nz) != goal:
                if self.jump[c & TILE_BEHAVIOR] == d and (x, z, d) not in self.blocked_edges:
                    lx, lz = nx + dx, nz + dz  # a ledge: over it, landing one tile beyond
                    if (lx, lz) not in self.objects:
                        yield d, lx, lz, self._layer(lx, lz), 2 + self.visits.get((lx, lz), 0) * VISIT_COST
                continue
            if self.passable(nx, nz, d, goal):
                yield d, nx, nz, self._layer(nx, nz), self._step_cost(nx, nz, d)

    def path(self, start, goal, limit=20000):
        """A* over tiles (and their layers, where the probe has them) from start at the player's height;
        returns the list of first-step directions, or None."""
        def h(x, z):
            return abs(x - goal[0]) + abs(z - goal[1])

        s0 = (start[0], start[1], self.height)
        # heap entries order by (f, g, x, z, height) with an unknown height first: states mix None and int heights
        openq = [(h(*start), 0, start[0], start[1], -(1 << 30) if self.height is None else self.height, s0)]
        came = {s0: None}
        cost = {s0: 0}
        n = 0
        while openq and n < limit:
            _, g, _, _, _, cur = heapq.heappop(openq)
            n += 1
            if cur[:2] == goal:
                dirs = []
                while came[cur] is not None:
                    prev, d = came[cur]
                    dirs.append(d)
                    cur = prev
                return dirs[::-1]
            if g > cost.get(cur, 1 << 30):
                continue
            for d, nx, nz, nh, step_cost in self.neighbours(cur[0], cur[1], cur[2], goal):
                ng = g + step_cost
                nxt = (nx, nz, nh)
                if ng < cost.get(nxt, 1 << 30):
                    cost[nxt] = ng
                    came[nxt] = (cur, d)
                    heapq.heappush(openq, (ng + h(nx, nz), ng, nx, nz, -(1 << 30) if nh is None else nh, nxt))
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
            if on_text == "stop":
                # GBA walks stop only for the goal's own scene (the coord event the goal is on): a trainer who
                # spots the player on the way is fought, as with on_text = "advance"
                at = step.get("_stop_at")
                p = s.probe() if at is not None else None
                if p is None or abs(p.x - at[0]) + abs(p.z - at[1]) <= 1:
                    raise _Held()
            if on_text not in ("advance", "stop"):
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
            # a corner off the line is where a warp puts the player (GBA warp panels and step-on warps fire a few
            # dozen frames after the step that reached them)
            s.run(180, until=["x!=%d" % here[0], "z!=%d" % here[1], "in_battle=1"])
            s.run(24, until="field_ready=1")
            p = s.probe()
            if (p.x, p.z) == here:
                raise HarnessError("steps: at (%d,%d), corner %d (%d,%d) is not in a straight line" % (
                    here + (k,) + route[k]))
            continue
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
        # landing on the next corner off the line to this one (a platform ride) reaches it; walking the line onto
        # a tile the route passes again later (a gate puzzle's back and forth) does not
        on_line = (p.x - here[0]) * (route[k][0] - p.x) >= 0 and (p.z - here[1]) * (route[k][1] - p.z) >= 0 and (
            p.x == here[0] or p.z == here[1])
        if k + 1 < len(route) and (p.x, p.z) == route[k + 1] and (p.x, p.z) != route[k] and not on_line:
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


def bot_moves(s, step, ctx):
    """Replay a direction route: `dirs` = [[KEY, N], ...] (tools/pt_explore.py prints one). KEY "U"|"D"|"L"|"R": N
    moves, each held until the probe's (map, x, y, z) changes, then the player is free again (text a move starts is
    advanced); N may instead be a target "x12", "z40" or "y233" (the overlay's tile y, the probe's y halved): moves
    in that direction until the coordinate is reached, so jump tiles (two tiles a move) and a platform ride (an
    event that carries the player once a step ends) need no counting. KEY "A": talk, N the direction to face first
    ("R": a bump into the person, which on a Distortion World wall is what turns the player toward them), A until a
    script starts, three tries. KEY "F": a field move, N the direction to bump first (water: Surf; a boulder:
    Strength), A answering YES. KEY "P": push a Strength boulder N ("R"): the direction held 40 frames (the player
    stays), then until free (a boulder falling through a Distortion World hole plays a scene). KEY "W": wait N
    frames, then until the player is free (an elevator ride).
    For maps whose walkable surface the probe's land grid does not show (Platinum's Distortion World: floating
    platforms, walls walked on, elevators). A move that changes nothing is tried again, three times at most."""
    bound = _int(step, "max", 9000)
    limit = s.frame + bound
    done = 0

    def one(name, d):
        nonlocal limit
        p = s.probe()
        here = (p.map_id, p.x, p.y, p.z)
        for attempt in range(4):
            if s.frame >= limit:
                raise HarnessError("moves: %d done, out of %d frames at %s going %s" % (done, bound, here, name))
            # held without a gap: on a Distortion World wall a released key turns the player again
            s.run(48, DIR_KEYS[d], until=["x!=%d" % here[1], "z!=%d" % here[3], "y!=%d" % here[2],
                                         "map_id!=%d" % here[0], "in_battle=1"])
            s.run(20, until="field_ready=1")
            limit += _field_or_handle(s, step, ctx, limit)
            p = s.probe()
            if (p.map_id, p.x, p.y, p.z) != here:
                return p
        raise HarnessError("moves: stuck at %s going %s (move %d)" % (here, name, done + 1))

    for name, n in step["dirs"]:
        if name == "W":
            s.run(int(n))
            limit += _field_or_handle(s, step, ctx, limit)
            continue
        if name == "F":
            s.run(16, DIR_KEYS["UDLR".index(n)])
            s.run(40, until="field_ready=1")
            limit += _use_field_move(s, step, ctx, "a field move %s" % n, limit)
            continue
        if name == "P":
            s.run(40, DIR_KEYS["UDLR".index(n)])
            s.run(20, until="field_ready=1")
            limit += _field_or_handle(s, step, ctx, limit)
            continue
        if name == "A":
            for _ in range(3):
                s.run(16, DIR_KEYS["UDLR".index(n)])
                s.run(40, until="field_ready=1")
                s.run(4, "a")
                if s.run(40, until="field_ready=0"):
                    break
            else:
                p = s.probe()
                raise HarnessError("moves: A facing %s started nothing at (%d,%d) y %d" % (n, p.x, p.z, p.y))
            limit += _field_or_handle(s, step, ctx, limit)
            continue
        d = "UDLR".index(name)
        if isinstance(n, str):
            axis, want = n[0], int(n[1:])
            for _ in range(80):
                p = s.probe()
                if {"x": p.x, "z": p.z, "y": p.y // 2}[axis] == want:
                    break
                one(name, d)
                done += 1
            else:
                raise HarnessError("moves: %s never reached %s" % (name, n))
            continue
        for _ in range(int(n)):
            one(name, d)
            done += 1
    s.run(16)
    p = s.probe()
    s.note("moves: %d moves, at (%d,%d) y %d on map %d" % (done, p.x, p.z, p.y, s.map_id))
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
    if what == "waterfall" and ctx.game in GBA_GAMES:
        # a GBA surfer who bumps into the falls is turned to face downstream; A asks only facing north
        # (field_control_avatar.c GetInteractedWaterScript: IsPlayerSurfingNorth)
        s.run(2, "up")
        s.run(12)
    s.run(4, "a")
    bot_advance_text(s, {"max": 2400}, ctx)
    return _field_or_handle(s, step, ctx, limit)


class _Held(Exception):
    """walk_to with on_text = "stop": a script holds the player (a coord trigger's scene); the walk ends there."""


def bot_walk_to(s, step, ctx):
    """walk_to; with on_text = "stop" a script that holds the player on the way (a coordinate trigger) ends the walk
    where it stands and the scene is left to the next steps (advance_text answers its menus with A, where the walk's
    B would say NO or cancel)."""
    try:
        _walk_to(s, step, ctx)
    except _Held:
        p = s.probe()
        s.note("walk_to: a script holds the player at (%d,%d) on map %d; on_text = stop ends the walk" % (
            p.x, p.z, p.map_id))


# re-plans of a GBA cross-map walk (a leg that ended on another map than the route said, a door that did not open)
GBA_ROUTE_TRIES = 10


def _gba_vars(s, p, ctx):
    """name -> the game's value of that VAR_ now (the probe's vars_addr/vars_count: gSaveBlock1's vars from 0x4000),
    or None: what decides the maps' ON_TRANSITION layout switches for the static route (gba_world._transition_layout:
    Sky Pillar's floors are whole until VAR_SKY_PILLAR_STATE 2)."""
    cache = {}

    def var(name):
        if name not in cache:
            cache[name] = None
            try:
                i = ctx.resolve(name) - 0x4000
            except HarnessError:
                return None
            if p.vars_addr and 0 <= i < p.vars_count:
                cache[name] = int.from_bytes(s.peek(p.vars_addr + 2 * i, 2), "little")
        return cache[name]
    return var


def _gba_route_to(s, step, ctx):
    """A GBA walk_to whose `map` is not the current map: the static world route (gba_world.py: the decomp's map
    layouts, connections and warps under the game's own step rules) gives the maps to cross and the exit tile on
    each; each leg is an ordinary walk on the probe, which sees the people and scripts the static model does not.
    A leg that lands on another map than planned re-routes from there; a warp that did not fire (a locked door) is
    avoided from then on. Returns once the player is on `map`."""
    import gba_world
    want = ctx.resolve(step["map"])
    world = gba_world.World(ctx.game)
    avoid_warps = []
    # on_text = "stop" is for the goal map's own scene: on the way, trainers' and other text is advanced
    keys = {k: step[k] for k in ("on_battle", "run", "hold", "surf", "hm", "move", "max") if k in step}
    dive = bool(step.get("dive"))
    if dive:
        keys["surf"] = True  # the dive tiles are deep water, and one surfaces surfing
    force = bool(step.get("_force"))  # route even from the goal's own map (walk_to found no path on it)
    for _ in range(GBA_ROUTE_TRIES):
        _field_or_handle(s, keys, ctx, s.frame + _int(step, "max", 6000))
        p = s.probe()
        if p.map_id == want and not force:
            return
        tx, tz = int(step["x"]), int(step["z"])
        # a goal the static model cannot stand on (a door in a wall, a counter): the route only has to reach its
        # map, so a tile beside it will do; the walk that follows goes into the goal itself
        legs, why = None, None
        var = _gba_vars(s, p, ctx)
        for gx, gz in ((tx, tz), (tx, tz + 1), (tx, tz - 1), (tx - 1, tz), (tx + 1, tz)):
            try:
                legs = world.route(p.map_id, p.x, p.z, want, gx, gz, elevation=p.y, surf=bool(step.get("surf")),
                                   avoid_warps=avoid_warps, dive=dive, var=var)
                break
            except gba_world.NoRoute as e:
                why = why or e
        if legs is None:
            raise HarnessError("walk_to: %s" % why)
        if len(legs) == 1 and p.map_id == want:
            return  # forced: the static model goes straight there now
        s.note("walk_to: route to %s (%d,%d): %s" % (step["map"], int(step["x"]), int(step["z"]), "; ".join(
            "%s %s (%d,%d)" % (l.kind, world.map_name(l.map), l.x, l.z) for l in legs)))
        for leg in legs[:-1]:
            if s.map_id != leg.map:
                break
            p = s.probe()
            if leg.kind == "warp" and (leg.x, leg.z) in {(o[0], o[1]) for o in p.objects}:
                # someone stands on the warp (Lavaridge Gym's trainers wait in its sand holes): another way
                avoid_warps.append((leg.map, leg.x, leg.z))
                s.note("walk_to: the warp at (%d,%d) on map %d is occupied; re-routing" % (leg.x, leg.z, leg.map))
                break
            try:
                if not (leg.kind in ("dive", "emerge") and (p.x, p.z) == (leg.x, leg.z)):
                    # (on the spot already, e.g. arrived there through a water door: walking "to" it would push
                    # through the door it is)
                    _walk_to(s, dict(keys, x=leg.x, z=leg.z, max=_int(step, "max", 6000)), ctx)
            except HarnessError as e:
                if s.map_id == leg.map and leg.kind == "warp":
                    avoid_warps.append((leg.map, leg.x, leg.z))
                    s.note("walk_to: %s; avoiding that warp, re-routing" % e)
                    break
                if s.map_id == leg.map:
                    raise
                # an unplanned warp (a floor that gave way, a hole): the route goes on from where it landed
                s.note("walk_to: %s; re-routing" % e)
                break
            if leg.kind == "warp" and s.map_id == leg.map:
                # a step-on warp (a cave mouth, stairs) fires after the step ends: its fade takes a few dozen frames
                s.run(120, until="map_id!=%d" % leg.map)
                _field_or_handle(s, keys, ctx, s.frame + _int(step, "max", 6000))
            if leg.kind in ("dive", "emerge") and s.map_id == leg.map:
                _gba_dive(s, leg, ctx)
                _field_or_handle(s, keys, ctx, s.frame + _int(step, "max", 6000))
            if s.map_id != leg.next_map:
                if leg.kind == "warp" and s.map_id == leg.map:
                    avoid_warps.append((leg.map, leg.x, leg.z))
                s.note("walk_to: the %s at (%d,%d) left map %d for %d, not %d; re-routing" % (
                    leg.kind, leg.x, leg.z, leg.map, s.map_id, leg.next_map))
                break
    if s.map_id != want:
        raise HarnessError("walk_to: not on %s after %d routes (on map %d)" % (step["map"], GBA_ROUTE_TRIES, s.map_id))


def _gba_dive(s, leg, ctx):
    """Dive (A) or surface (B) where the player stands, as a player does (field_control_avatar.c [519/529]
    TrySetupDiveDownScript / TrySetupDiveEmergeScript -> UseDiveScript / S_UseDiveUnderwater,
    data/field_move_scripts.inc [220-270]): the key starts the script, its YES/NO opens on YES, A answers it and
    then advances "used DIVE" until the field effect warps (only up to the map change: one more A on deep water
    would offer the dive again)."""
    key = "a" if leg.kind == "dive" else "b"
    start = s.map_id
    # a wild battle the last step met starts a few frames after it: fought (fled) first, and again if one cuts in
    s.run(30)
    _field_or_handle(s, {"on_battle": "flee"}, ctx, s.frame + 30000)
    for _ in range(4):
        s.run(2, key)
        if s.run(60, until="field_ready=0") and not s.in_battle:
            break
        if s.in_battle:
            _field_or_handle(s, {"on_battle": "flee"}, ctx, s.frame + 30000)
    else:
        p = s.probe()
        raise HarnessError("walk_to: no %s prompt at (%d,%d) on map %d (FLAG_BADGE07_GET and a party member with "
                           "Dive are needed)" % (leg.kind, p.x, p.z, p.map_id))
    for _ in range(12):
        s.run(30)
        if s.map_id != start:
            break
        s.run(2, "a")
        if s.run(60, until="map_id!=%d" % start):
            break
    if s.map_id == start:
        raise HarnessError("walk_to: the %s at (%d,%d) on map %d did not leave the map" % (
            leg.kind, leg.x, leg.z, start))
    s.run(60, until="field_ready=1")
    s.note("walk_to: %s from map %d to %d" % (leg.kind, start, s.map_id))


def _walk_to(s, step, ctx):
    """Walk to tile (x, z): A* over the probe's terrain, replanning as it learns; warps by walking into them.

    With via = [[x, z], ...] it walks to each waypoint first (each under its own `max`): routes longer than the
    probe's 64x64 window, or past what A* cannot know (a bridge's deck vs the path under it). A waypoint someone
    stands on (a trainer who walked up to the player there) counts as reached from the tile next to it.

    GBA: `map` naming another map than the current one walks there first (_gba_route_to)."""
    if step.get("dive") and not step.get("surf"):
        step = dict(step, surf=True)  # a dive route arrives surfing (the cavern's pool): its last leg surfs too
    if ctx.game in GBA_GAMES and "map" in step and s.map_id != ctx.resolve(step["map"]):
        _gba_route_to(s, step, ctx)
    if step.get("via"):
        leg = {k: v for k, v in step.items() if k not in ("via", "face", "interact", "map")}
        for i, (wx, wz) in enumerate(step["via"]):
            _walk_to(s, dict(leg, x=wx, z=wz, _corner=True,
                             **({"map": step["map"]} if i == 0 and "map" in step else {})), ctx)
        step = {k: v for k, v in step.items() if k not in ("via", "map")}
    goal = (int(step["x"]), int(step["z"]))
    if ctx.game in GBA_GAMES and step.get("on_text") == "stop":
        step = dict(step, _stop_at=goal)  # _field_or_handle: stop only for a scene at the goal
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
    terrain = Terrain(surf=bool(step.get("surf")), hm=bool(step.get("hm")), avoid=step.get("avoid", ()), game=ctx.game)
    field_tries = {}  # tile -> field-move attempts
    start_map = p.map_id
    conn0 = p.connection_seq
    steps = 0
    warped = False
    if terrain.gba:
        # a GBA goal across the map's border (a connected map's tile, as the probe's window shows it in this map's
        # coordinates): the walk ends where the player crosses into that map
        c = p.cell(*goal)
        terrain.goal_connected = c is not None and bool(c & TILE_CONNECTED)
    while (p.x, p.z) != goal:
        if s.frame >= limit:
            near = sorted((o[0], o[1], o[2]) for o in p.objects if abs(o[0] - p.x) + abs(o[1] - p.z) <= 4)
            raise HarnessError("walk_to (%d,%d): still at (%d,%d) after %d frames (battles excluded); facing %d, "
                               "objects near (x, z, id) %s, %d edges learned blocked %s" % (
                                   goal + (p.x, p.z, bound, p.facing, near, len(terrain.blocked_edges),
                                           sorted(terrain.blocked_edges)[:8])))
        terrain.update(p)
        if terrain.gba and not terrain.goal_connected:
            # a far goal across the border comes into the window on the way
            terrain.goal_connected = bool(terrain.cells.get(goal, 0) & TILE_CONNECTED)
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
            terrain.layers.clear()
            terrain.update(p)
            dirs = terrain.path((p.x, p.z), goal)
        if not dirs and terrain.gba and not step.get("_rerouted"):
            # a goal on this map reached only through another (Lavaridge Gym's floors, a cave's upper level): the
            # static world route goes round, then the walk ends on this map as asked
            s.note("walk_to (%d,%d): no path on map %d from (%d,%d); routing through the world" % (
                goal + (p.map_id, p.x, p.z)))
            _gba_route_to(s, dict(step, map=p.map_id, _force=True), ctx)
            return _walk_to(s, dict(step, map=p.map_id, _rerouted=True), ctx)
        if not dirs:
            raise HarnessError("walk_to (%d,%d): no path from (%d,%d) on map %d" % (goal + (p.x, p.z, p.map_id)))
        d = dirs[0]
        here = (p.x, p.z)
        terrain.visits[here] = terrain.visits.get(here, 0) + 1
        run = run_key and not (terrain.gba and p.avatar_flags & GBA_AVATAR_UNDERWATER)
        keys = DIR_KEYS[d] + ("+" + run_key if run else "")
        # Hold the direction through the turn-in-place (a short press only turns) until the step begins:
        # the probe's tile changes as a step starts. A bump into something solid never changes it. `hold`: deep
        # snow's slow steps need longer before the next one starts (Platinum 35, Acuity Lakefront)
        moved = s.run(_int(step, "hold", 24), keys, until=["x!=%d" % here[0], "z!=%d" % here[1],
                                                         "map_id!=%d" % start_map, "in_battle=1"])
        # let the step finish so the next probe sees a settled tile; then whatever the step started (a warp's
        # fade, a coord script, a trainer's sight, a wild battle) runs until the player is free again
        s.run(24, until="field_ready=1")
        if not s.field_ready:
            limit += _field_or_handle(s, step, ctx, limit)
        p = s.probe()
        if terrain.gba and p.map_id != start_map and p.connection_seq != conn0:
            # a GBA map's coordinates are its own: across a connection the map and the coordinates change at once
            s.note("walk_to: crossed from map %d into map %d, now at (%d,%d)" % (start_map, p.map_id, p.x, p.z))
            if terrain.goal_connected or (here[0] + DIR_DELTA[d][0], here[1] + DIR_DELTA[d][1]) == goal:
                warped = True
                break
            raise HarnessError("walk_to (%d,%d): crossed into map %d at (%d,%d) on the way; give this walk a goal across "
                               "the border and walk on in the next map's coordinates" % (goal + (p.map_id, p.x, p.z)))
        if p.map_id != start_map:
            # Outdoors the matrix is one coordinate space: a step across a map border changes map_id
            # and moves one tile (two over a ledge). Anything else is a warp.
            nxt = (here[0] + DIR_DELTA[d][0], here[1] + DIR_DELTA[d][1])
            if not terrain.gba and abs(p.x - here[0]) + abs(p.z - here[1]) <= 2:
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
            surfing = cell is not None and (cell & TILE_BEHAVIOR) in terrain.surfable
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
            # a mat may warp within the map (D/P Mt. Coronet South 2F (7,23) -> (7,12)): a jump counts too
            here, left = (p.x, p.z), False
            for _ in range(19):
                if s.run(8, DIR_KEYS[d], until="map_id!=%d" % start_map):
                    left = True
                    break
                q = s.probe()
                if abs(q.x - here[0]) + abs(q.z - here[1]) > 2:
                    left = True
                    break
            if not left:
                raise HarnessError("walk_to (%d,%d): the exit mat did not warp pushing %s" % (goal + (DIR_KEYS[d],)))
            _field_or_handle(s, step, ctx, limit)
            warped = True
            p = s.probe()
            s.note("walk_to: left map %d through the mat at (%d,%d) to map %d (%d,%d)" % (
                start_map, goal[0], goal[1], s.map_id, p.x, p.z))
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
# Ruby/Sapphire/Emerald: the nurse at (7,2) behind the counter (7,3), the exit arrow mats at (6,8)/(7,8) (every
# data/maps/*_PokemonCenter_1F/map.json of both decomps, e.g. OldaleTown_PokemonCenter_1F)
GBA_PC_COUNTER, GBA_PC_EXIT = (7, 4), (7, 8)
# HG/SS: every *PC0101 (Pokemon Center 1F) zone_event has the nurse (SPRITE_PCWOMAN1) at (8,11) behind the counter
# and the town exit warp at (8,19) (games/heartgold/files/fielddata/eventdata/zone_event/*PC0101.json, e.g.
# 066_T21PC0101 Cherrygrove)
HGSS_PC_COUNTER, HGSS_PC_EXIT = (8, 13), (8, 19)


def bot_heal(s, step, ctx):
    """Heal the party at the Pokemon Center whose door is (x, z) on this map (GBA: or on `map`, walked to first,
    with walk_to's `surf` and `hm`), and come back out of it."""
    town = ctx.resolve(step["map"]) if "map" in step else s.map_id
    walk = {"x": step["x"], "z": step["z"], "on_battle": step.get("on_battle", "flee"), "max": _int(step, "max", 6000)}
    for k in ("map", "surf", "hm"):
        if k in step:
            walk[k] = step[k]
    bot_walk_to(s, walk, ctx)
    if s.map_id == town:
        raise HarnessError("heal: (%d,%d) is not a door on map %d" % (int(step["x"]), int(step["z"]), town))
    center = s.map_id
    counter, exit_ = ((GBA_PC_COUNTER, GBA_PC_EXIT) if ctx.game in GBA_GAMES
                      else (HGSS_PC_COUNTER, HGSS_PC_EXIT) if ctx.game in HGSS_GAMES else (PC_COUNTER, PC_EXIT))
    bot_walk_to(s, {"x": counter[0], "z": counter[1], "face": "up", "interact": True}, ctx)
    bot_advance_text(s, {}, ctx)  # A answers YES to resting the Pokemon
    bot_walk_to(s, {"x": exit_[0], "z": exit_[1]}, ctx)
    if s.map_id == center:
        raise HarnessError("heal: did not leave the Pokemon Center (map %d)" % center)
    s.note("heal: healed in map %d, back on map %d" % (center, s.map_id))


# ---------------------------------------------------------------- party / grind
def party(s, ctx):
    """The party as the game has it now: an in-game save, then np_save4's dump of the save file np_gp wrote."""
    return save_dump(s, ctx)["party"]


def save_dump(s, ctx):
    """np_save4's dump of an in-game save made now (the whole save, as [expect] save expressions see it)."""
    before = _stores(s)
    bot_save(s, {}, ctx)
    if ctx.game in GBA_GAMES:
        s.flush()  # the GBA flash chip is stored by the host, when asked
    for _ in range(60):  # np_gp writes the chip file when the game's card write completes
        if _stores(s) > before:
            break
        s.run(2)
    else:
        raise HarnessError("party: the save was not written to %s" % s.save_path)
    out = subprocess.run(ctx.save4 + ["dump", ctx.rom, s.save_path], capture_output=True, text=True)
    if out.returncode != 0:
        raise HarnessError("party: np_save4 cannot read %s" % s.save_path)
    return json.loads(out.stdout)


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


def bot_pace(s, step, ctx):
    """Run back and forth between (x, z) and (x+1, z) until `until` (a Python expression over the save dump `s`,
    as [expect] save expressions) holds, checked by an in-game save every `every` steps (default 128): the steps
    the Day Care's egg roll counts (daycare.c Daycare_Update: one roll each 256 steps of the second parent).
    Battles on the way are fled, scenes advanced."""
    bound = _int(step, "max", 60000)
    limit = s.frame + bound
    every = _int(step, "every", 128)
    spot = (int(step["x"]), int(step["z"]))
    bot_walk_to(s, {"x": spot[0], "z": spot[1]}, ctx)
    k = 0
    while True:
        if eval(step["until"], {}, {"s": save_dump(s, ctx)}):
            s.note("pace: %s after %d steps" % (step["until"], k))
            return
        for _ in range(every):
            if s.frame >= limit:
                raise HarnessError("pace: %s not true after %d steps (%d frames)" % (step["until"], k, bound))
            k += 1
            p = s.probe()
            d = "right" if p.x == spot[0] else "left"
            s.run(24, d + "+b", until=["x!=%d" % p.x, "in_battle=1"])
            s.run(24, until="field_ready=1")
            if not s.field_ready:
                _field_or_handle(s, {"on_battle": "flee"}, ctx, limit)


def bot_dump(s, step, ctx):
    """Note the value of `expr` (a Python expression over the dump `s` of an in-game save made now, as [expect]
    save expressions) in the run log as `dump: EXPR = VALUE`: a mid-run state for [expect] log patterns (a roamer's
    map before and after a map change)."""
    s.note("dump: %s = %r" % (step["expr"], eval(step["expr"], {}, {"s": save_dump(s, ctx)})))


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
        t = Terrain(game=ctx.game)
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
    tile next to it, face it, A until a script starts. Wandering people are chased (re-planned) as they move. `surf`,
    `hm` and `avoid` go to the walks as walk_to takes them (an object on the water is reached surfing)."""
    oid = int(step["id"])
    bound = _int(step, "max", 6000)
    limit = s.frame + bound
    while s.frame < limit:
        limit += _field_or_handle(s, step, ctx, limit)
        p = s.probe()
        obj = next(((o[0], o[1]) for o in p.objects if o[2] == oid), None)
        if obj is None and ctx.game in GBA_GAMES:
            # a GBA map spawns its people only near the camera: until then, head for where map.json puts it
            import gba_world
            home = next(((x, z) for i, x, z in gba_world.World(ctx.game).objects(p.map_id) if i == oid), None)
            if home is not None and abs(p.x - home[0]) + abs(p.z - home[1]) > 6:
                obj = home
        if obj is None:
            raise HarnessError("talk_to: no object with local id %d on map %d" % (oid, p.map_id))
        if ctx.game in GBA_GAMES and abs(p.x - obj[0]) + abs(p.z - obj[1]) > 1 and abs(p.x - obj[0]) + abs(p.z - obj[1]) <= 3:
            # GBA people walking a loop (Mr. Briney round his table) or wandering pass by: near one, wait a little
            # for it to come next to the player before chasing it
            for _ in range(24):
                s.run(4)
                p = s.probe()
                obj = next(((o[0], o[1]) for o in p.objects if o[2] == oid), obj)
                if abs(p.x - obj[0]) + abs(p.z - obj[1]) == 1 or not s.field_ready:
                    break
        if abs(p.x - obj[0]) + abs(p.z - obj[1]) == 1:
            d = _toward((p.x, p.z), obj)
            if p.facing != d:
                s.run(2, DIR_KEYS[d])
                if ctx.game not in GBA_GAMES:
                    s.run(8)
                    continue  # it may have moved meanwhile
                # a GBA walker moves on within a few frames: the turn (a 2-frame press turns in place) and A at once
                s.run(4)
            s.run(2, "a")
            if s.run(30, until=["field_ready=0", "in_battle=1"]):
                s.note("talk_to: talking to object %d at (%d,%d)" % (oid, obj[0], obj[1]))
                return
            continue
        t = Terrain(game=ctx.game)
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
            # the walk fights what it meets with this step's move and on_battle (a sight trainer on the way), and
            # crosses water / field-move obstacles with its surf / hm (an object on the water: HG/SS's red Gyarados)
            sub = {k: step[k] for k in ("move", "on_battle", "on_text", "surf", "hm", "avoid") if k in step}
            # a GBA wanderer moves every second or so: short legs, re-aimed at where it stands now
            leg = 90 if ctx.game in GBA_GAMES else 900
            sub.update(x=c[0], z=c[1], max=min(leg, max(limit - s.frame, 1)))
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


def overworld_headers(game):
    """The overworld matrix's header names by [z][x] block: Platinum's map_matrix_000 (MainMapMatrixData_Load,
    src/map_matrix.c:149-161), D/P's fielddata/mapmatrix narc 0 (arm9/src/map_matrix.c: u8 width, height, has
    headers, has altitudes, name length, the name, then width*height u16 header ids)."""
    if game == "platinum":
        path = os.path.join(ROOT, "games", "platinum", "res", "field", "matrices", "map_matrix_000.json")
        return json.load(open(path))["headers"]
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools"))
    import dp_script
    d = open(os.path.join(ROOT, "games", "diamond", "files", "fielddata", "mapmatrix", "map_matrix",
                          "narc_0000.bin"), "rb").read()
    w, h, p = d[0], d[1], 5 + d[4]
    ids = struct.unpack_from("<%dH" % (w * h), d, p)
    return [[dp_script.map_name(i) if i != 0xFFFF else "MAP_NONE" for i in ids[z * w:(z + 1) * w]] for z in range(h)]


def fly_blocks(name, game="platinum"):
    """The town-map blocks of a fly destination: the cells of the overworld matrix whose header is `name`
    (CanFlyToHoveredLocation: the hovered cell's header must be the fly location's, town_map/graphics.c:1125-1140,
    fly_locations.c:291-304)."""
    headers = overworld_headers(game)
    return [(x, z) for z, row in enumerate(headers) for x, h in enumerate(row) if h == name]


def _menu_key(s, key, n=1, gap=8):
    for _ in range(n):
        s.run(2, key)
        s.run(gap)


def _field_move_entry(s, step, ctx, move, what, mons=None, field_moves=FIELD_MOVES):
    """(party slot, context-menu entry) of a field move: `slot` names the slot, else the first party member that
    knows `move` (`mons`, else an in-game save's dump). The context menu lists SUMMARY, then the field moves
    (`field_moves`) in move-slot order up to the first empty slot, SWITCH, ITEM, CANCEL (party_menu/main.c:1791-1839)."""
    if mons is None:
        mons = party(s, ctx)
    if "slot" in step:
        slot = _int(step, "slot", 0)
    else:
        slot = next((i for i, m in enumerate(mons) if any(mv["id"] == move for mv in m["moves"])), None)
        if slot is None:
            raise HarnessError("%s: no party member knows move %d" % (what, move))
    known = []
    for mv in (mv["id"] for mv in mons[slot]["moves"]):
        if not mv:
            break
        if mv in field_moves:
            known.append(mv)
    if move not in known:
        raise HarnessError("%s: party slot %d does not know move %d" % (what, slot, move))
    return slot, 1 + known.index(move)


def _open_field_move(s, slot, entry):
    """X, POKEMON (the start menu's remembered cursor tracked per run), the party slot, the context-menu entry, A."""
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


def bot_field_move(s, step, ctx):
    """Use a field move from the party menu as a player does (Defog, Flash, Teleport, Dig, Sweet Scent, Softboiled):
    X, POKEMON, the party member that knows `move` (a MOVE_* name or id; `slot` names the slot), the move in its
    context menu, then the text it starts is advanced (advance_text) unless `text = false`. The start menu needs the
    Pokedex (START_MENU's order). HG/SS: _hgss_open_party_move's menus."""
    move = ctx.resolve(step["move"])
    bot_wait_field(s, step, ctx)
    if ctx.game in HGSS_GAMES:
        _hgss_field_move(s, step, ctx, move)
    else:
        slot, entry = _field_move_entry(s, step, ctx, move, "field_move")
        s.note("field_move: move %d, party slot %d, menu entry %d" % (move, slot, entry))
        _open_field_move(s, slot, entry)
    if step.get("text", True):
        bot_advance_text(s, {"max": step["max"]} if "max" in step else {}, ctx)


def bot_fly(s, step, ctx):
    """Fly to `map` (a town's header) the way a player does: X, POKEMON, the party member that knows Fly, FLY, the
    town map's cursor moved block by block to the destination, A. `slot` names the party slot (default: the first
    that knows Fly, from an in-game save's dump); `block = [x, z]` the town-map block (default: the destination's
    block on the overworld matrix nearest the player's). Platinum menus (start_menu.c, party_menu/main.c,
    town_map/graphics.c); D/P's start menu, party menu and fly map are laid out the same way. HG/SS: _hgss_fly."""
    if ctx.game in HGSS_GAMES:
        _hgss_fly(s, step, ctx)
        return
    dest = ctx.resolve(step["map"])
    bot_wait_field(s, step, ctx)
    p = s.probe()
    here = (p.x // 32, p.z // 32)
    if "start" in step:
        # off the overworld matrix (caves, lakes, buildings) the town map opens on the exit location, the overworld
        # tile the player last left it from (town_map/context.c:108-116), which the save dump does not show
        here = tuple(int(v) for v in step["start"])
    else:
        headers = overworld_headers(ctx.game)
        on = here[1] < len(headers) and here[0] < len(headers[here[1]]) and headers[here[1]][here[0]] != "MAP_NONE" \
            and ctx.resolve(headers[here[1]][here[0]]) == s.map_id
        if not on:
            raise HarnessError("fly: map %d is off the overworld matrix: walk out first, or give start = [x, z] (the "
                               "town-map block of the exit location)" % s.map_id)
    if "block" in step:
        goal = tuple(int(v) for v in step["block"])
    else:
        blocks = fly_blocks(step["map"], ctx.game)
        if not blocks:
            raise HarnessError("fly: %s is on no overworld block" % step["map"])
        goal = min(blocks, key=lambda b: abs(b[0] - here[0]) + abs(b[1] - here[1]))
    slot, entry = _field_move_entry(s, step, ctx, MOVE_FLY, "fly")
    s.note("fly: to %s (%d), block %s from %s, party slot %d, menu entry %d" % (step["map"], dest, goal, here, slot,
                                                                                entry))
    _open_field_move(s, slot, entry)
    s.run(120)
    # the fly map opens with the cursor on the player's block (town_map/main.c:218-227)
    gx = min(max(goal[0], FLY_X[0]), FLY_X[1])
    gz = min(max(goal[1], FLY_Z[0]), FLY_Z[1])
    _menu_key(s, "right" if gx > here[0] else "left", abs(gx - here[0]), gap=14)
    _menu_key(s, "down" if gz > here[1] else "up", abs(gz - here[1]), gap=14)
    s.run(20)
    if s.shot_dir:
        s.dump(os.path.join(s.shot_dir, "fly-map.ppm"))
    # A picks the hovered town; D/P's fly map can let the first press go (seen: Veilstone hovered, no flight), so
    # press again while the map is still up
    flown = False
    for _ in range(5):
        s.run(4, "a")
        if s.run(120, until="map_id=%d" % dest):
            flown = True
            break
    if not flown and not s.run(_int(step, "max", 1500), until="map_id=%d" % dest):
        raise HarnessError("fly: still on map %d, not %s (%d)" % (s.map_id, step["map"], dest))
    bot_wait_field(s, {}, ctx)
    p = s.probe()
    s.note("fly: landed on map %d at (%d,%d)" % (s.map_id, p.x, p.z))


# ---------------------------------------------------------------- HG/SS: start menu, party menu, fly map
# pokeheartgold's screens, driven with keys only. The probe sees none of them, so every count below is the decomp's
# and unverified at runtime; each bot notes the presses it makes.
#
# X opens the start menu (src/field/field_control.c:151-157 -> StartMenu_Init). Overlay 27 draws it on the bottom
# screen; outside the Safari Zone, Bug Contest, Pal Park and link rooms in layout 0 (ov27_0225BD50 returns 0;
# asm/overlay_27.s ov27_0225CFC8 row 0): slot i holds icon i, START_MENU_ICON_POKEDEX..OPTIONS (include/start_menu.h:
# 9-15), slots 0-3 the left column top to bottom, 4-6 the right one (touch rects ov27_0225CF68 entries 1-7). A slot
# is drawn when FieldSystem_ShouldDrawStartMenuIcon says so (src/start_menu.c:535-556): its flag here
# (src/sys_flags.c:273-288; CheckGotMenuIconI is FLAG_GOT_BAG + 0..3).
HGSS_START_MENU = [("pokedex", "FLAG_GOT_POKEDEX"), ("pokemon", "FLAG_GOT_STARTER"), ("bag", "FLAG_GOT_BAG"),
                   ("pokegear", "FLAG_GOT_POKEGEAR"), ("trainer_card", "FLAG_GOT_TRAINER_CARD"),
                   ("save", "FLAG_GOT_SAVE_BUTTON"), ("options", "FLAG_GOT_OPTIONS_BUTTON")]
# The D-pad (newKeys, ov27_0225B404) moves the cursor with ov27_0225B360: per slot and direction (up, down, left,
# right) three candidates (ov27_0225D0B4), the first drawn one is the new slot (the slot itself: no move).
HGSS_START_MENU_MOVES = [
    ((3, 2, 1), (1, 2, 3), (4, 0, 0), (4, 0, 0)),
    ((0, 3, 2), (2, 3, 0), (5, 1, 0), (5, 1, 0)),
    ((1, 0, 3), (3, 0, 1), (6, 2, 0), (6, 2, 0)),
    ((2, 1, 0), (0, 1, 2), (6, 3, 0), (6, 3, 0)),
    ((6, 5, 4), (5, 6, 4), (0, 4, 0), (0, 4, 0)),
    ((4, 6, 5), (6, 4, 5), (1, 5, 0), (1, 5, 0)),
    ((5, 4, 6), (4, 5, 6), (2, 6, 0), (2, 6, 0)),
]
HGSS_DIRS = ("up", "down", "left", "right")
# A takes the button FieldSystem.unkD3 names (StartMenu_HandleKeyInput, src/start_menu.c:591-605): the cursor's
# index among the drawn slots, which ov27 writes on every move (ov27_0225C170) and reopens the menu on
# (ov27_0225C1AC; a slot not drawn falls back to the first drawn, ov27_0225C1EC), so the menu remembers it. The
# FieldSystem starts zeroed (src/field_system.c:143-144): index 0 after a boot. Tracked per run in s.hgss_menu_index
# (as Platinum's s.start_menu_option), so steps that open the menu by hand (press) must leave it on POKEMON.
# The party menu's field moves, sFieldMoves (src/party_menu.c:202-219): Cut, Fly, Surf, Strength, Rock Smash,
# Waterfall, Rock Climb, Whirlpool, Flash, Teleport, Dig, Sweet Scent, Chatter, Headbutt, Milk Drink, Softboiled.
HGSS_FIELD_MOVES = {15, 19, 57, 70, 249, 127, 431, 250, 148, 100, 91, 230, 448, 29, 208, 135}
MOVE_DIG = 91
# The fly map's cursor (fly_map.c:111-120, ov101_021EB654): x 2 .. maxXscroll - 1 (sMapXScrollLimits 26 Johto, 29
# with the Indigo Plateau, 45 with Kanto), the fly points' y (playerY - 2) 0 .. 15
HGSS_FLY_X, HGSS_FLY_Y = (2, 44), (0, 15)


def _hg_world():
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools"))
    import hg_world
    return hg_world


def _hgss_menu_path(shown, cur, want):
    """The D-pad presses that take the start menu's cursor from slot `cur` to slot `want` over the drawn slots."""
    prev = {cur: None}
    queue = [cur]
    for c in queue:
        for d, cands in enumerate(HGSS_START_MENU_MOVES[c]):
            n = next((k for k in cands if k in shown), c)
            if n not in prev:
                prev[n] = (c, d)
                queue.append(n)
    if want not in prev:
        raise HarnessError("start menu: slot %d not reachable from %d over %s" % (want, cur, shown))
    keys = []
    while prev[want] is not None:
        want, d = prev[want]
        keys.append(HGSS_DIRS[d])
    return keys[::-1]


def _hgss_open_party_move(s, ctx, dump, slot, k):
    """X, POKEMON, party slot `slot`, its k-th field move, A (HG/SS, keys only):
    - start menu: the cursor from the remembered slot to POKEMON (_hgss_menu_path), A;
    - party menu: opened on slot 0 (Task_StartMenu_Pokemon -> PartyMenu_LaunchApp_Unk1(.., 0), src/start_menu.c:808,
      src/launch_application.c:252-257); RIGHT steps through the slots in order (_0210140C buttonRight 0->1->..->5,
      src/party_menu.c:158-167), A opens the member's context menu (sub_0207AC70, :1476-1481);
    - context menu (sub_0207B0B0, src/party_menu.c:1613-1659): SUMMARY, SWITCH, ITEM (MAIL), CANCEL, then the field
      moves in move-slot order; it opens on SUMMARY (PartyMenu_CreateContextMenuCursor selection 0,
      src/party_context_menu.c:470-498); LEFT or RIGHT goes to the first field move, DOWN to the next
      (sDpadNavParam_PartyMenu rows numItems - 2, party_context_menu.c:217-288; PartyMenu_HandleInput_ContextMenu
      :1196-1215), A uses it."""
    flags = set(dump.get("flags", []))
    shown = [i for i, (_, flag) in enumerate(HGSS_START_MENU) if ctx.resolve(flag) in flags]
    pokemon = 1
    if pokemon not in shown:
        raise HarnessError("start menu: no POKEMON button (FLAG_GOT_STARTER clear)")
    index = getattr(s, "hgss_menu_index", 0)
    cur = shown[index] if index < len(shown) else 0
    if cur not in shown:
        cur = shown[0]
    keys = _hgss_menu_path(shown, cur, pokemon)
    s.note("start menu: slots %s, cursor %d -> POKEMON: %s; party slot %d (RIGHT x%d); context menu: LEFT, DOWN x%d"
           % (shown, cur, " ".join(keys) or "-", slot, slot, k))
    s.run(2, "x")
    s.run(30)
    for key in keys:
        _menu_key(s, key)
    s.run(2, "a")
    s.hgss_menu_index = shown.index(pokemon)
    s.run(90)
    _menu_key(s, "right", slot)
    s.run(2, "a")
    s.run(20)
    _menu_key(s, "left")
    _menu_key(s, "down", k)
    s.run(2, "a")


def _hgss_party_field_move(s, step, ctx, move, what, dump=None):
    """(dump, party slot, index among the member's field moves) for `move`."""
    if dump is None:
        dump = save_dump(s, ctx)
    slot, entry = _field_move_entry(s, step, ctx, move, what, dump["party"], HGSS_FIELD_MOVES)
    return dump, slot, entry - 1


def _hgss_field_move(s, step, ctx, move):
    dump, slot, k = _hgss_party_field_move(s, step, ctx, move, "field_move")
    s.note("field_move: move %d, party slot %d, field move %d" % (move, slot, k))
    _hgss_open_party_move(s, ctx, dump, slot, k)


def _hgss_dig_out(s, step, ctx):
    """Dig from a cave that allows it (FieldMove_CheckDig, src/field_move.c:510-525) back to the special spawn warp,
    the dungeon's entrance outside; done when the map has changed and the player is free."""
    before = s.map_id
    _hgss_field_move(s, step, ctx, MOVE_DIG)
    if not s.run(_int(step, "max", 1500), until="map_id!=%d" % before) and s.map_id == before:
        raise HarnessError("fly: Dig did not leave map %d" % before)
    bot_wait_field(s, {}, ctx)
    p = s.probe()
    s.note("fly: dug out to map %d (%d,%d)" % (s.map_id, p.x, p.z))


def _hgss_fly(s, step, ctx):
    """HG/SS Fly (bot_fly): the field move FLY (_hgss_open_party_move), then the fly map, the Pokegear map app in fly
    mode (FieldMove_UseFly -> PokegearTownMap_LaunchApp(.., 0), src/field_move.c:240-248; FlyMap_*, fly_map.c):
    - the cursor opens on the player's overworld block (x / 32, z / 32), off the overworld on the map header's
      worldMapX/Y, else the special spawn warp's block (FieldSystem_InitPokegearArgs, src/unk_02092BE8.c:40-61;
      fly_map.c:111-116); `start = [x, y]` overrides it;
    - a held direction moves it one block per two frames (ov101_021EB654, ov101_021EC304), so each 2-frame press is
      one block;
    - A over a fly point whose flag is set (gMapFlypointParams rects, PokegearMap_GetFlyDestinationAtCoord,
      overlay_101_021E9270.c:721-747; only within the player's region but for the Indigo Plateau / Route 26) opens
      FLY / CANCEL with the cursor on FLY (PokegearMap_SpawnFlyContextMenu; TouchscreenListMenu_Create selection 0),
      A flies (FlyMap_HandleContextMenu, overlay_101_021EDCE0.c:311-330; Task_UseFlyInField, src/start_menu.c:
      1368-1390), landing on the fly point's spawn (tools/hg_world.py fly).
    A map that does not allow Fly (FieldMove_CheckFly, src/field_move.c:217-238) fails, unless `dig = true` and it
    allows Dig: then Dig first."""
    hw = _hg_world()
    want = step["map"]
    point = hw.fly_destination(want)
    if point is None:
        raise HarnessError("fly: %s is no fly destination (tools/hg_world.py fly)" % want)
    landing = hw.fly_landing(point["map"])
    dest = ctx.resolve(landing[0] if landing else point["map"])
    bot_wait_field(s, step, ctx)
    if s.map_id == dest:
        raise HarnessError("fly: already on %s" % want)
    header = hw.map_headers().get(hw.map_name(s.map_id), {})
    if not header.get("fly"):
        if step.get("dig") and header.get("dig"):
            _hgss_dig_out(s, step, ctx)
            header = hw.map_headers().get(hw.map_name(s.map_id), {})
        if not header.get("fly"):
            raise HarnessError("fly: map %s does not allow Fly%s" % (
                hw.map_name(s.map_id), " (it allows Dig: dig = true)" if header.get("dig") else ""))
    p = s.probe()
    if "start" in step:
        here = tuple(int(v) for v in step["start"])
    else:
        here = hw.fly_start_block(hw.map_name(s.map_id), p.x, p.z)
        if here is None:
            raise HarnessError("fly: map %s has no world-map block: give start = [x, y] (its exit's block)"
                               % hw.map_name(s.map_id))
    # Pokegear_GetCurrentRegion reads the same tile the cursor starts from [INFERENCE: one block, one region]
    region = hw.region(*here)
    if "block" in step:
        goal = tuple(int(v) for v in step["block"])
    else:
        cells = [(x, y) for x in range(point["x"], point["x"] + point["w"])
                 for y in range(point["y"], point["y"] + point["h"]) if hw.fly_allowed_from(point, region, x, y)]
        if not cells:
            raise HarnessError("fly: %s is outside the region (%s) the fly map lets the player reach" % (want, region))
        goal = min(cells, key=lambda c: abs(c[0] - here[0]) + abs(c[1] - here[1]))
    gx = min(max(goal[0], HGSS_FLY_X[0]), HGSS_FLY_X[1])
    gy = min(max(goal[1], HGSS_FLY_Y[0]), HGSS_FLY_Y[1])
    dump, slot, k = _hgss_party_field_move(s, step, ctx, MOVE_FLY, "fly")
    s.note("fly: to %s (lands on %s %s), cursor %s -> %s, party slot %d, field move %d" % (
        want, landing[0] if landing else point["map"], landing[1:] if landing else "?", here, (gx, gy), slot, k))
    _hgss_open_party_move(s, ctx, dump, slot, k)
    s.run(150)
    _menu_key(s, "right" if gx > here[0] else "left", abs(gx - here[0]), gap=6)
    _menu_key(s, "down" if gy > here[1] else "up", abs(gy - here[1]), gap=6)
    s.run(20)
    if s.shot_dir:
        s.dump(os.path.join(s.shot_dir, "fly-map.ppm"))
    flown = False
    for _ in range(3):
        s.run(2, "a")   # the fly point: FLY / CANCEL
        s.run(30)
        s.run(2, "a")   # FLY
        if s.run(240, until="map_id=%d" % dest):
            flown = True
            break
    if not flown and not s.run(_int(step, "max", 1500), until="map_id=%d" % dest):
        raise HarnessError("fly: still on map %d, not %s (%d)" % (s.map_id, want, dest))
    bot_wait_field(s, {}, ctx)
    p = s.probe()
    s.note("fly: landed on map %d at (%d,%d)" % (s.map_id, p.x, p.z))


FISH_ENDS = ("caught", "early", "away", "none")


def _log_since(s, pos):
    """The run log's text from byte `pos` on, and the new end."""
    s._log.flush()
    with open(s.log_path, "rb") as f:
        f.seek(pos)
        data = f.read()
    return data.decode("utf-8", "replace"), pos + len(data)


def bot_fish(s, step, ctx):
    """Fish with the registered rod as a player does: Y casts; the guest's fishing trace (PC_TRACE_FISH=1 in [run]
    env; pc_fish_trace, pc_probe2d.c: one line per state change) says when the bite window opens, and A hooks
    then (a press before the bite reels in early, fishing.c). A cast that ends without a catch ("none": not even a
    nibble, "away", "early") has its message closed with B and the rod is cast again, up to `casts` (default 12).
    Done when the hooked Pokemon's battle starts; the screen at the bite goes on the contact sheet."""
    casts = _int(step, "casts", 12)
    limit = s.frame + _int(step, "max", 9000)
    bot_wait_field(s, {}, ctx)
    _, pos = _log_since(s, 0)
    seen = False
    for n in range(1, casts + 1):
        s.run(2, "y")
        outcome = None
        while outcome is None:
            if s.frame >= limit:
                raise HarnessError("fish: cast %d did not end in %d frames%s" % (
                    n, _int(step, "max", 9000), "" if seen else " (no pc-fish trace: set PC_TRACE_FISH=1)"))
            s.run(2)
            txt, pos = _log_since(s, pos)
            for word in re.findall(r"pc-fish: f=\d+ rod=\d+ (\w+)", txt):
                seen = True
                if word == "bite":
                    snap(s)
                    s.run(2, "a")
                elif word in FISH_ENDS:
                    outcome = word
        s.note("fish: cast %d: %s" % (n, outcome))
        if outcome == "caught":
            # "Landed a Pokemon!" waits for A, then the battle starts
            for _ in range(40):
                if s.run(20, until="in_battle=1"):
                    return
                s.run(2, "a")
            raise HarnessError("fish: hooked, but no battle started")
        for _ in range(30):
            if s.run(20, until="field_ready=1"):
                break
            s.run(2, "b")
        s.run(20)
    raise HarnessError("fish: nothing hooked in %d casts" % casts)



def bot_menu(s, step, ctx):
    """GBA: answer the field menu a script is about to show (np_e2e.h NP_E2E_UI_FIELD_MENU: a multichoice or a
    YES/NO): A advances the text before it (a press on the frame the menu appears is not a new press there), then
    the cursor goes to entry `choose` (0 the first; a YES/NO's YES) and A picks it. With `count`, the menu must have
    that many entries (a check that it is the menu meant)."""
    want = _int(step, "choose", 0)
    limit = s.frame + _int(step, "max", 3000)
    while True:
        p = s.probe()
        if p is not None and p.ui == UI_FIELD_MENU:
            break
        if s.frame >= limit:
            raise HarnessError("menu: no field menu within %d frames" % _int(step, "max", 3000))
        if not s.run(2, "a", until="ui=%d" % UI_FIELD_MENU):
            s.run(8, until="ui=%d" % UI_FIELD_MENU)
    s.run(4)  # the menu's own input delay (script_menu.c sProcessInputDelay)
    p = s.probe()
    if "count" in step and p.ui_arg != _int(step, "count", 0):
        raise HarnessError("menu: %d entries, the step expects %d" % (p.ui_arg, _int(step, "count", 0)))
    for _ in range(16):
        p = s.probe()
        if p.ui != UI_FIELD_MENU or p.ui_cursor == want:
            break
        _gba_press(s, "down" if p.ui_cursor < want else "up")
    s.note("menu: entry %d of %d" % (want, p.ui_arg))
    _gba_press(s, "a", 10)


def _gba_flag(s, p, ctx, name):
    """A GBA flag's value now (the probe's flags_addr: gSaveBlock1's flags bitfield), or None."""
    try:
        fid = ctx.resolve(name)
    except HarnessError:
        return None
    if not p.flags_addr or fid // 8 >= p.flags_bytes:
        return None
    return bool(s.peek(p.flags_addr + fid // 8, 1)[0] >> (fid % 8) & 1)


def _gba_boulder_ahead(p, d):
    """The object on the tile in direction d from the player, as (x, z), or None."""
    front = (p.x + DIR_DELTA[d][0], p.z + DIR_DELTA[d][1])
    return front if front in {(o[0], o[1]) for o in p.objects} else None


def bot_push(s, step, ctx):
    """GBA Strength: push the boulder in direction `dir` one tile (field_player_avatar.c TryPushBoulder: the player
    walks in place, the boulder moves). Strength is first switched on when FLAG_SYS_USE_STRENGTH is clear (map loads
    clear it): A on the boulder, YES (EventScript_StrengthBoulder). A wild battle that cuts in is fled and the push
    tried again; done when the boulder has left the tile ahead."""
    d = FACINGS[step["dir"]]
    key = DIR_KEYS[d]
    for _ in range(4):
        _field_or_handle(s, {"on_battle": step.get("on_battle", "flee")}, ctx, s.frame + 30000)
        p = s.probe()
        front = _gba_boulder_ahead(p, d)
        if front is None:
            raise HarnessError("push: nothing to push %s of (%d,%d)" % (step["dir"], p.x, p.z))
        if not _gba_flag(s, p, ctx, "FLAG_SYS_USE_STRENGTH"):
            if p.facing != d:
                s.run(2, key)
                s.run(10)
            s.run(4, "a")
            bot_advance_text(s, {"max": 2400}, ctx)
            continue
        s.run(24, key)
        s.run(40, until="field_ready=1")
        q = s.probe()
        if (q.x, q.z) == (p.x, p.z) and front not in {(o[0], o[1]) for o in q.objects}:
            s.note("push: the boulder at (%d,%d) went %s" % (front + (step["dir"],)))
            return
    raise HarnessError("push: the boulder %s of (%d,%d) did not move" % (step["dir"], p.x, p.z))


def bot_smash(s, step, ctx):
    """GBA Rock Smash: the rock in direction `dir` (A, YES: EventScript_RockSmash; a wild battle it starts is
    fought), tried again after a battle that cut in; done when the rock has gone."""
    d = FACINGS[step["dir"]]
    for _ in range(4):
        _field_or_handle(s, {"on_battle": step.get("on_battle", "fight")}, ctx, s.frame + 30000)
        p = s.probe()
        front = _gba_boulder_ahead(p, d)
        if front is None:
            s.note("smash: no rock %s of (%d,%d) any more" % (step["dir"], p.x, p.z))
            return
        if p.facing != d:
            s.run(2, DIR_KEYS[d])
            s.run(10)
        s.run(4, "a")
        bot_advance_text(s, {"max": 2400}, ctx)
    p = s.probe()
    if _gba_boulder_ahead(p, d) is not None:
        raise HarnessError("smash: the rock %s of (%d,%d) is still there" % (step["dir"], p.x, p.z))


def bot_repeat(s, step, ctx):
    """Run `steps` (step tables, each with an optional `if`: an np_gp condition that must hold for it to run now)
    over and over until `until` (an np_gp condition) holds, checked before each round and after each step; at most
    `max_rounds` rounds (default 50). For a run of rooms whose kinds the game draws at random (the Battle Pike's doors:
    a three-path room, then a room of one of several maps), where a fixed list of steps cannot know which comes."""
    until = step["until"]
    rounds = _int(step, "max_rounds", 50)
    for r in range(rounds):
        if s.run(1, until=until):
            s.note("repeat: %s after %d rounds" % (until, r))
            return
        for sub in step["steps"]:
            cond = sub.get("if")
            if cond and not s.run(1, until=cond):
                continue
            BOTS[sub["do"]](s, {k: v for k, v in sub.items() if k != "if"}, ctx)
            if s.run(1, until=until):
                s.note("repeat: %s after %d rounds" % (until, r + 1))
                return
    raise HarnessError("repeat: %s not reached in %d rounds" % (until, rounds))


BOTS = {
    "press": bot_press,
    "tap": bot_tap,
    "wait_frames": bot_wait_frames,
    "wait_map": bot_wait_map,
    "wait_field": bot_wait_field,
    "wait_battle": bot_wait_battle,
    "wait_reset": bot_wait_reset,
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
    "moves": bot_moves,
    "field_move": bot_field_move,
    "fish": bot_fish,
    "hatch": bot_hatch,
    "pace": bot_pace,
    "dump": bot_dump,
    "menu": bot_menu,
    "push": bot_push,
    "smash": bot_smash,
    "repeat": bot_repeat,
}
