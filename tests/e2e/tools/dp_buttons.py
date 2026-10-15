#!/usr/bin/env python3
"""Find the button presses that open a D/P puzzle room, by playing them (Sunyshore Gym gears, any coord-button room).

    python3 tests/e2e/tools/dp_buttons.py --game diamond SAVE --map MAP X Z --buttons X,Z X,Z ... --goal X,Z
        [--depth N]

Breadth-first over press sequences. Each node boots SAVE placed at (X, Z) of MAP (the save lab), walks onto the
buttons of its sequence in order (walk_to, every other button avoided; a button pressed twice in a row is stepped off
and on again), then reads the probe's step layers (np_e2e.h v3: the game's own movement check, the room's dynamic
collision included) and floods from the player. A node whose flood reaches the goal is the answer; otherwise each
button the flood reaches is a child. Nodes with the same reachable set and the same presses per button (mod 4: a gear
turns a quarter per press) are one state. Prints the walk_to steps
(TOML) of the shortest sequence.
"""
import argparse
import collections
import os
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import bots  # noqa: E402
import run  # noqa: E402
from dp_warps import clear_reach  # noqa: E402
from np_e2e import DIR_DELTA, HarnessError, Session  # noqa: E402


def steps_for(seq, buttons):
    """walk_to steps pressing the buttons of seq in order."""
    out = []
    for i, b in enumerate(seq):
        avoid = [list(o) for o in buttons if o != b]
        if i and seq[i - 1] == b:
            out.append({"do": "_step_off", "x": b[0], "z": b[1], "avoid": avoid})
        out.append({"do": "walk_to", "x": b[0], "z": b[1], "avoid": avoid, "max": 4000})
    return out


def play(game, save, work, place, seq, buttons):
    """Boot, press seq; returns (reach, player tile, layers) or None when a press could not be walked."""
    sav = os.path.join(work, "b.sav")
    with open(save, "rb") as f, open(sav, "wb") as g:
        g.write(f.read())
    recipe = os.path.join(work, "move.recipe")
    with open(recipe, "w") as f:
        f.write("map %s %d %d FACE_UP\n" % place)
    run.mint(game, recipe, sav, sav, work)
    s = Session(game.gp, game.rom, game.name, sav, os.path.join(work, "b.log"), 40000, options=run.DEFAULT_OPTIONS)
    ctx = run.Ctx(game, types.SimpleNamespace(dir=work))
    try:
        run.boot_continue(s)
        for st in steps_for(seq, buttons):
            if st["do"] == "_step_off":
                p = s.probe()
                lay = p.layers() or {}
                for moves in lay.get((p.x, p.z), {}).values():
                    for tx, tz, _ in moves.values():
                        if [tx, tz] not in st["avoid"] and abs(tx - p.x) + abs(tz - p.z) == 1:
                            bots.BOTS["walk_to"](s, {"x": tx, "z": tz, "avoid": st["avoid"], "max": 600}, ctx)
                            break
                    else:
                        continue
                    break
                continue
            bots.BOTS["walk_to"](s, st, ctx)
        s.run(60)
        p = s.probe()
        lay = p.layers() or {}
        objs = {(o[0], o[1]) for o in p.objects}
        return clear_reach(lay, objs, (p.x, p.z)), (p.x, p.z)
    except HarnessError as e:
        print("  %s: %s" % (seq, e))
        return None
    finally:
        s.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", required=True, choices=sorted(run.GAMES))
    ap.add_argument("save")
    ap.add_argument("--map", nargs=3, required=True, metavar=("MAP", "X", "Z"))
    ap.add_argument("--buttons", nargs="+", required=True, metavar="X,Z")
    ap.add_argument("--goal", required=True, metavar="X,Z")
    ap.add_argument("--depth", type=int, default=6)
    args = ap.parse_args()
    game = run.Game(args.game)
    game.tools()
    work = tempfile.mkdtemp(prefix="dp_buttons.")
    place = (args.map[0], int(args.map[1]), int(args.map[2]))
    buttons = [tuple(int(v) for v in b.split(",")) for b in args.buttons]
    goal = tuple(int(v) for v in args.goal.split(","))
    seen = set()
    q = collections.deque([()])
    while q:
        seq = q.popleft()
        res = play(game, args.save, work, place, list(seq), buttons)
        if res is None:
            continue
        reach, at = res
        near_goal = goal in reach or any((goal[0] + dx, goal[1] + dz) in reach for dx, dz in DIR_DELTA)
        print("%-40s at %s: %d tiles, buttons %s%s" % (seq, at, len(reach), [b for b in buttons if b in reach],
                                                     ", GOAL" if near_goal else ""))
        if near_goal:
            for st in steps_for(list(seq), buttons) + [{"do": "walk_to", "x": goal[0], "z": goal[1],
                                                         "avoid": [list(b) for b in buttons]}]:
                print("\n[[step]]")
                for k, v in st.items():
                    print("%s = %s" % (k, '"%s"' % v if isinstance(v, str) else v))
            return
        key = (frozenset(reach), tuple(seq.count(b) % 4 for b in buttons))
        if key in seen or len(seq) >= args.depth:
            continue
        seen.add(key)
        for b in buttons:
            if b in reach:
                q.append(seq + (b,))
    sys.exit("no sequence of %d presses or fewer reaches %s" % (args.depth, goal))


if __name__ == "__main__":
    main()
