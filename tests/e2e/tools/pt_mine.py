#!/usr/bin/env python3
"""Read Platinum's Underground mining board out of guest memory and plan the taps that dig out its items.

    python3 tests/e2e/tools/pt_mine.py SAVE --steps STEPS.json [--clock "2009-03-22 21:00:00"] [--out DIR] [--play]

Boots SAVE, runs the milestone steps in STEPS.json (tools/ms_steps.py) that end on the mining board, then reads
sMiningEnv (its address from the wasm link map): buriedObjectGrid[10][13] (0 = nothing, k = buriedObjects[k-1]),
dirtLayers[10][13] and wallIntegrity (196 at the start). The grid and dirt arrays are found by layout: the 130-byte
grid, the 130-byte dirt array, the int printerID, then pickaxeSelected, sidebarTouchState, buttonSelected and
wallIntegrity (mining.c MiningEnv). Objects 1..N whose itemID is below MINING_ROCK_1 (62) are items; the first dig
(Underground_HasPlayerNeverMined) buries 3 items and no rocks.

The plan models mining.c: Mining_RemoveDirt takes 2 from the tapped cell, 1 from its 4 neighbours (the hammer: 2
from the neighbours and 1 from the diagonals), nothing more when the cell is a bare rock; each tap costs the wall 4
(pickaxe) or 8 (hammer) and the wall collapses at 0; an item is dug when all its cells have no dirt left. A tap on
a cell is at its centre (16c+8, 16(r+2)+8); the sidebar's hammer is at (232,80), the pickaxe at (232,144) (the
pickaxe is selected at the start). It prints the plan as milestone `tap` steps; --play also taps it and reads the
board again, with a shot of the end (DIR/after.png).
"""
import argparse
import itertools
import json
import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import bots  # noqa: E402
import run  # noqa: E402
from np_e2e import Session  # noqa: E402
from pt_ugspots import symbol  # noqa: E402

W, H = 13, 10
WALL = 196
ROCK_1 = 62
HAMMER_XY, PICKAXE_XY = (232, 80), (232, 144)
ADJ = ((0, 1), (0, -1), (-1, 0), (1, 0))
DIAG = ((1, 1), (-1, -1), (-1, 1), (1, -1))


def peek(s, addr, n):
    out = s._cmd("peek %d %d" % (addr, n))
    return bytes.fromhex(out.split()[1])


def read_board(s):
    ptr = int.from_bytes(peek(s, symbol("sMiningEnv"), 4), "little")
    if not ptr:
        sys.exit("no MiningEnv (not in the Underground?)")
    env = peek(s, ptr, 0x2000)
    for g in range(0, len(env) - 280):
        grid, dirt = env[g:g + 130], env[g + 130:g + 260]
        tail = (g + 260 + 3) & ~3
        if max(grid) > 8 or not max(grid) or min(dirt) < 1 or max(dirt) > 6 or env[tail + 7] != WALL:
            continue
        b = find_objects(env, g, grid)
        return ([list(grid[r * W:(r + 1) * W]) for r in range(H)], [list(dirt[r * W:(r + 1) * W]) for r in range(H)],
                decode_objects(env[b:b + 96], grid), ptr + g, ptr + tail + 7, ptr + b)
    sys.exit("no mining board in MiningEnv (the game not started yet, or already dug?)")


def find_objects(env, g, grid):
    """buriedObjects[8] {ptr, u8 itemID, x, y, unused, isDugUp} (12 bytes) sit before the grid: the nearest run of
    records that fits every object index the grid uses."""
    used = sorted(set(grid) - {0})
    for b in range(g - 96, 0, -4):
        recs = [env[b + 12 * i:b + 12 * i + 12] for i in range(8)]
        if all(int.from_bytes(recs[k - 1][0:4], "little") and 0 < recs[k - 1][4] < 70 and recs[k - 1][5] < W
               and recs[k - 1][6] < H for k in used):
            return b
    sys.exit("no buriedObjects before the grid")


def decode_objects(raw, grid):
    used = sorted(set(v for row in grid for v in row) - {0}) if isinstance(grid[0], list) else sorted(set(grid) - {0})
    return {k: {"item": raw[12 * k - 12 + 4], "x": raw[12 * k - 12 + 5], "y": raw[12 * k - 12 + 6],
                "dug": raw[12 * k - 12 + 8]} for k in used}


def hit(dirt, grid, objs, c, r, hammer):
    d = [row[:] for row in dirt]

    def dec(x, y, n):
        if 0 <= x < W and 0 <= y < H:
            d[y][x] = max(0, d[y][x] - n)

    dec(c, r, 2)
    k = grid[r][c]
    if k and objs[k]["item"] >= ROCK_1 and d[r][c] == 0:
        return d
    if hammer:
        for dx, dy in DIAG:
            dec(c + dx, r + dy, 1)
    for dx, dy in ADJ:
        dec(c + dx, r + dy, 2 if hammer else 1)
    return d


def plan(grid, dirt, objs, order):
    """Greedy over the items in `order`: each tap the (tool, cell) that clears the most dirt off the current item's
    cells per point of wall (the other items' cells break ties)."""
    items = [k for k in order]
    cells = {k: [(c, r) for r in range(H) for c in range(W) if grid[r][c] == k] for k in items}
    wall, taps = WALL, []
    for k in items:
        while wall > 0 and any(dirt[r][c] for c, r in cells[k]):
            best = None
            for r in range(H):
                for c in range(W):
                    for hammer in (False, True):
                        d = hit(dirt, grid, objs, c, r, hammer)
                        gain = sum(dirt[y][x] - d[y][x] for x, y in cells[k])
                        rest = sum(dirt[y][x] - d[y][x] for j in items if j != k for x, y in cells[j])
                        cost = 8 if hammer else 4
                        score = (gain / cost, rest / cost, not hammer)
                        if gain and (best is None or score > best[0]):
                            best = (score, c, r, hammer, d)
            _, c, r, hammer, dirt = best
            taps.append((c, r, hammer))
            wall = max(0, wall - (8 if hammer else 4))
    done = [k for k in items if not any(dirt[r][c] for c, r in cells[k])]
    return taps, done, wall


def best_plan(grid, dirt, objs):
    items = [k for k in objs if objs[k]["item"] < ROCK_1]
    best = None
    for order in itertools.permutations(items):
        taps, done, wall = plan(grid, [row[:] for row in dirt], objs, order)
        key = (len(done), wall)
        if best is None or key > best[0]:
            best = (key, order, taps, done, wall)
    return best[1:]


def show(grid, dirt, objs):
    for r in range(H):
        print("  " + " ".join("%s%d" % (".abcdefgh"[grid[r][c]] if grid[r][c] else ".", dirt[r][c])
                              for c in range(W)))
    for k, o in sorted(objs.items()):
        print("  %s: object %d itemID %d at (%d,%d)%s%s" % ("_abcdefgh"[k], k, o["item"], o["x"], o["y"],
              " rock" if o["item"] >= ROCK_1 else "", " dug" if o["dug"] else ""))


def tap_steps(taps):
    out, hammer = [], False
    for c, r, h in taps:
        if h != hammer:
            x, y = HAMMER_XY if h else PICKAXE_XY
            out.append({"do": "tap", "x": x, "y": y, "gap": 20, "note": "the %s" % ("hammer" if h else "pickaxe")})
            hammer = h
        xy = (16 * c + 8, 16 * (r + 2) + 8)
        if out and out[-1].get("_xy") == xy:
            out[-1]["times"] = out[-1].get("times", 1) + 1
        else:
            out.append({"do": "tap", "x": xy[0], "y": xy[1], "gap": 12, "_xy": xy})
    for st in out:
        st.pop("_xy", None)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("save")
    ap.add_argument("--steps", required=True)
    ap.add_argument("--clock", default="2009-03-22 21:00:00")
    ap.add_argument("--out", default="build/e2e/poke/mine")
    ap.add_argument("--play", action="store_true")
    a = ap.parse_args()
    game = run.Game("platinum")
    game.tools()
    os.makedirs(a.out, exist_ok=True)
    sav = os.path.join(a.out, "mine.sav")
    with open(a.save, "rb") as f, open(sav, "wb") as g:
        g.write(f.read())
    s = Session(game.gp, game.rom, game.name, sav, os.path.join(a.out, "mine.log"), 400000,
                options=run.DEFAULT_OPTIONS, env={"PC_E2E": "1", "PC_RTC": a.clock})
    s.shot_dir = a.out
    run.boot_continue(s)
    ctx = run.Ctx(game, types.SimpleNamespace(dir=a.out))
    for st in json.load(open(a.steps)):
        bots.BOTS[st["do"]](s, st, ctx)
    grid, dirt, objs, gaddr, waddr, oaddr = read_board(s)
    print("board at frame %d (grid %#x, wall %#x)" % (s.frame, gaddr, waddr))
    show(grid, dirt, objs)
    order, taps, done, wall = best_plan(grid, dirt, objs)
    print("plan: %d taps (%d hammer), items %s of %s dug, wall left %d" % (
        len(taps), sum(h for _, _, h in taps), done, order, wall))
    steps = tap_steps(taps)
    print(json.dumps(steps))
    with open(os.path.join(a.out, "taps.json"), "w") as f:
        json.dump(steps, f)
    if a.play:
        for st in steps:
            bots.BOTS[st["do"]](s, st, ctx)
        s.run(2)
        env = peek(s, gaddr, 260)
        wall_now = peek(s, waddr, 1)[0]
        print("after: frame %d wall %d" % (s.frame, wall_now))
        grid2 = [list(env[r * W:(r + 1) * W]) for r in range(H)]
        show(grid2, [list(env[130 + r * W:130 + (r + 1) * W]) for r in range(H)],
             decode_objects(peek(s, oaddr, 96), grid2))
        ppm = os.path.join(a.out, "after.ppm")
        s.dump(ppm)
        print("shot", run.ppm_to_png(ppm))
    print(s.quit())


if __name__ == "__main__":
    main()
