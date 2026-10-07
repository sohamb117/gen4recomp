#!/usr/bin/env python3
"""Boot a save and play a short key script, dumping screens: for recording menu layouts (start menu, party menu,
slot machines, the Underground menus) before writing `press`/`tap` steps.

    python3 tests/e2e/tools/pt_poke.py SAVE "x/2 w30 shot:menu down/2 w8 a/2 w60 shot:party" [--out DIR]
        [--game platinum] [--env K=V ...] [--steps JSON] [--clock "2009-03-22 21:00:00"]

Script tokens: KEY/N holds KEY (a, b, x, y, start, select, up, down, left, right, l, r; joined with +) N frames;
wN waits N frames; tX,Y/N touches the bottom screen at (X, Y) N frames; shot:NAME dumps DIR/NAME.png;
until:COND runs up to 600 frames until COND (np_gp's until syntax, e.g. field_ready=1, map_id=188);
probe prints the probe's map/tile/state; save makes an in-game save (DIR/poke.sav, the next poke's start). --steps runs milestone steps (JSON list, tools/ms_steps.py) first.
The final save is written to DIR/poke.sav, the shots' contact sheet to DIR/poke.png.
"""
import argparse
import json
import os
import shutil
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import bots  # noqa: E402
import run  # noqa: E402
from np_e2e import Session  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("save")
    ap.add_argument("script")
    ap.add_argument("--out", default="build/e2e/poke")
    ap.add_argument("--game", default="platinum")
    ap.add_argument("--env", nargs="*", default=[])
    ap.add_argument("--steps")
    ap.add_argument("--clock")
    a = ap.parse_args()
    game = run.Game(a.game)
    game.tools()
    os.makedirs(a.out, exist_ok=True)
    sav = os.path.join(a.out, "poke.sav")
    shutil.copyfile(a.save, sav)
    env = {"PC_E2E": "1"}
    env.update(dict(e.split("=", 1) for e in a.env))
    if a.clock:
        env["PC_RTC"] = a.clock
    s = Session(game.gp, game.rom, game.name, sav, os.path.join(a.out, "poke.log"), 400000,
                options=run.DEFAULT_OPTIONS, env=env)
    s.shot_dir = a.out
    run.boot_continue(s)
    ctx = run.Ctx(game, types.SimpleNamespace(dir=a.out))
    if a.steps:
        for st in json.load(open(a.steps)):
            bots.BOTS[st["do"]](s, st, ctx)

    shots = []

    def png(name):
        ppm = os.path.join(a.out, name + ".ppm")
        s.dump(ppm)
        shots.append(("%s f%d" % (name, s.frame), run.ppm_to_png(ppm)))
        print("shot %s f%d" % (name, s.frame))

    for tok in a.script.split():
        if tok.startswith("shot:"):
            png(tok[5:])
        elif tok.startswith("until:"):
            ok = s.run(600, until=tok[6:])
            print("until %s: %s f%d" % (tok[6:], ok, s.frame))
        elif tok == "save":
            bots.bot_save(s, {}, ctx)
            s.run(60)
        elif tok == "probe":
            p = s.probe()
            print("f%d map %d (%d,%d) y %d facing %d field_ready %d battle %d" % (
                s.frame, p.map_id, p.x, p.z, p.y, p.facing, s.field_ready, s.in_battle))
        elif tok[0] == "w" and tok[1:].isdigit():
            s.run(int(tok[1:]))
        elif tok[0] == "t" and "," in tok:
            xy, _, n = tok[1:].partition("/")
            x, y = (int(v) for v in xy.split(","))
            s.run(int(n or 4), touch=(x, y))
            s.run(4)
        else:
            k, _, n = tok.partition("/")
            s.run(int(n or 2), k)
    s.quit()
    if shots:
        print("sheet %s" % run.contact_sheet(a.out, "poke", shots))
    print("frames %d, log %s" % (s.frame, os.path.join(a.out, "poke.log")))


if __name__ == "__main__":
    main()
