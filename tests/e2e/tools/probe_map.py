#!/usr/bin/env python3
"""The probe's 64x64 tile window around the player as ASCII, from a save: for authoring walk_to steps on any game.

    python3 tests/e2e/tools/probe_map.py --game diamond SAVE [--warp MAP WARP | --map MAP X Z] [--keys K ...]
        [--steps JSON] [--shot PNG] [--save-to OUT]

Boots SAVE through CONTINUE (as run.py does) -- first moved by the save lab with --warp/--map --, optionally plays `--keys KEYS:FRAMES` holds, then prints the window the
guest's e2e probe publishes (core/include/np_e2e.h): what walk_to plans over. Coordinates are the probe's (world tiles
outdoors). Legend: '@' player, '#' collision, '.' floor, 'g' tall grass, '~' surfable water, 'F' waterfall,
'r' rock-climb wall, 'v' ledge, 'M' exit mat, 'o' map object (local id listed), '?' not loaded, other behaviours
as two hex digits are listed under the map. --steps runs milestone steps first (a JSON list of step tables, e.g.
'[{"do": "walk_to", "x": 40, "z": 54}]'), --shot writes the screen after them, --save-to quick-saves and writes the save
to OUT: the next exploration boots from there (maps the probe cannot plan alone, e.g. the Distortion World).
"""
import json
import types
import argparse
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import bots  # noqa: E402
import run  # noqa: E402
from np_e2e import TILE_BEHAVIOR, TILE_COLLISION, TILE_KNOWN, Session, behaviors  # noqa: E402


def render(p):
    b = behaviors()
    grass = {b[k] for k in ("TALL_GRASS", "VERY_TALL_GRASS") if k in b}
    ledges = {b[k] for k in ("JUMP_NORTH", "JUMP_SOUTH", "JUMP_WEST", "JUMP_EAST")}
    mats = {v for k, v in b.items() if k.startswith("WARP_")}
    objs = {(o[0], o[1]): o for o in p.objects}
    other = {}
    lines = ["map %d player (%d,%d) facing %d; window x %d..%d z %d..%d" % (
        p.map_id, p.x, p.z, p.facing, p.grid_x0, p.grid_x0 + 63, p.grid_z0, p.grid_z0 + 63)]
    lines.append("      " + "".join(str((p.grid_x0 + i) % 10) for i in range(64)))
    for gz in range(64):
        z = p.grid_z0 + gz
        row = []
        for gx in range(64):
            x = p.grid_x0 + gx
            c = p.grid[gz * 64 + gx]
            beh = c & TILE_BEHAVIOR
            if (x, z) == (p.x, p.z):
                ch = "@"
            elif (x, z) in objs:
                ch = "o"
            elif not c & TILE_KNOWN:
                ch = "?"
            elif beh in bots.SURFABLE:
                ch = "~"
            elif beh == bots.WATERFALL:
                ch = "F"
            elif beh in bots.ROCK_CLIMB:
                ch = "r"
            elif beh in ledges:
                ch = "v"
            elif beh in mats:
                ch = "M"
            elif c & TILE_COLLISION:
                ch = "#"
            elif beh in grass:
                ch = "g"
            elif beh == 0:
                ch = "."
            else:
                ch = "="
                other.setdefault(beh, []).append((x, z))
            row.append(ch)
        lines.append("%5d %s" % (z, "".join(row)))
    for (x, z), o in sorted(objs.items()):
        lines.append("object id %d gfx %d at (%d,%d)" % (o[2], o[3], x, z))
    names = {v: k for k, v in b.items()}
    for beh, tiles in sorted(other.items()):
        lines.append("'=' 0x%02X %s: %d tiles, e.g. %s" % (beh, names.get(beh, "?"), len(tiles), tiles[:4]))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", required=True, choices=sorted(run.GAMES))
    ap.add_argument("save")
    ap.add_argument("--warp", nargs=2, metavar=("MAP", "WARP"), help="lab-warp the save there first")
    ap.add_argument("--map", nargs=3, metavar=("MAP", "X", "Z"), help="lab-place the player there first")
    ap.add_argument("--keys", nargs="*", default=[], help="KEYS:FRAMES holds after the boot, e.g. up:16 a:4")
    ap.add_argument("--steps", help="JSON list of milestone steps to run after the keys")
    ap.add_argument("--shot", help="write the screen after the steps to this .png")
    ap.add_argument("--save-to", help="quick-save after the steps and write the save here")
    ap.add_argument("--frames", type=int, default=20000, help="frame budget (default 20000)")
    args = ap.parse_args()
    game = run.Game(args.game)
    game.tools()
    work = tempfile.mkdtemp(prefix="probe_map.")
    sav = os.path.join(work, "x.sav")
    with open(args.save, "rb") as f, open(sav, "wb") as g:
        g.write(f.read())
    if args.warp or args.map:
        recipe = os.path.join(work, "move.recipe")
        with open(recipe, "w") as f:
            f.write("warp %s %s\n" % tuple(args.warp) if args.warp else "map %s %s %s FACE_DOWN\n" % tuple(args.map))
        run.mint(game, recipe, sav, sav, work)
    s = Session(game.gp, game.rom, game.name, sav, os.path.join(work, "run.log"), args.frames,
                options=run.DEFAULT_OPTIONS)
    saved = False
    try:
        run.boot_continue(s)
        for k in args.keys:
            keys, n = k.rsplit(":", 1)
            s.run(int(n), keys)
            s.run(30)
        try:
            if args.steps:
                ctx = run.Ctx(game, types.SimpleNamespace(dir=work))
                for i, step in enumerate(json.loads(args.steps), 1):
                    bots.BOTS[step["do"]](s, step, ctx)
                    print("step %d %s ok, frame %d, map %d" % (i, step["do"], s.frame, s.map_id))
        except Exception as e:  # noqa: BLE001 -- show where it stopped
            print("steps stopped: %s" % e)
        print(render(s.probe()))
        if args.shot:
            ppm = os.path.join(work, "shot.ppm")
            s.dump(ppm)
            os.replace(run.ppm_to_png(ppm), args.shot)
        if args.save_to:
            bots.BOTS["save"](s, {}, run.Ctx(game, types.SimpleNamespace(dir=work)))
            s.quit()
            saved = True
            with open(sav, "rb") as f, open(args.save_to, "wb") as g:
                g.write(f.read())
    finally:
        if not saved:
            s.kill()
        print("log: %s" % os.path.join(work, "run.log"))


if __name__ == "__main__":
    main()
