#!/usr/bin/env python3
"""Route a D/P walk through warps (doors, stairs, warp panels) with the game's own walkability.

    python3 tests/e2e/tools/dp_warps.py --game diamond SAVE --from MAP X Z --to MAP X Z [--maps MAP ...]

From the tile (X, Z) of the first map, the tool boots SAVE once per place it has to look at, warps the player there
(the guest's PC_WARP, pc/game/pc_dp_field.c), and reads the probe's step layers (np_e2e.h v3): the tiles the game's
movement check lets the player reach from there, heights and bridges included. Every warp of the map (zone_event,
tests/e2e/tools/dp_script.py) on or beside a reached tile is an edge to the warp it leads to, on the maps named by
--maps (default: --from's and --to's). A breadth-first search over those edges prints the route as the warp tiles to
walk onto, map by map, for walk_to steps. Map objects are not terrain: a warp reached only through one (a locked
door, a person standing still) is printed with the objects next to that way.
"""
import argparse
import collections
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import dp_script  # noqa: E402
import run  # noqa: E402
from np_e2e import DIR_DELTA, Session  # noqa: E402


def look(game, save, work, mapid, x, z):
    """Boot SAVE warped to (mapid, x, z); returns (layers, objects) once the player stands there."""
    sav = os.path.join(work, "look.sav")
    with open(save, "rb") as f, open(sav, "wb") as g:
        g.write(f.read())
    s = Session(game.gp, game.rom, game.name, sav, os.path.join(work, "look.log"), 20000,
                options=run.DEFAULT_OPTIONS, env={"PC_WARP": "%d:%d:%d:1" % (mapid, x, z)})
    try:
        run.boot_continue(s)
        # PC_WARP fires the first frame the field is free and its fade takes the field away again: let it finish
        s.run(600, until="map_id=%d" % mapid)
        s.run(150)
        s.run(600, until="field_ready=1")
        s.run(30)
        p = s.probe()
        if p is None or p.map_id != mapid or (p.x, p.z) != (x, z):
            raise SystemExit("could not stand at map %d (%d,%d): probe says %r" % (mapid, x, z, p))
        return p.layers() or {}, {(o[0], o[1]): o for o in p.objects}
    finally:
        s.kill()


def clear_reach(layers, objects, start):
    """Tiles reached from start over the layers' steps without entering a map object's tile."""
    seen = {start}
    q = collections.deque([start])
    while q:
        x, z = q.popleft()
        for moves in layers.get((x, z), {}).values():
            for tx, tz, _ in moves.values():
                if (tx, tz) not in seen and (tx, tz) in layers and (tx, tz) not in objects:
                    seen.add((tx, tz))
                    q.append((tx, tz))
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", required=True, choices=("diamond", "pearl"))
    ap.add_argument("save")
    ap.add_argument("--from", dest="start", nargs=3, required=True, metavar=("MAP", "X", "Z"))
    ap.add_argument("--to", dest="goal", nargs=3, required=True, metavar=("MAP", "X", "Z"))
    ap.add_argument("--maps", nargs="*", default=[])
    a = ap.parse_args()
    game = run.Game(a.game)
    game.tools()
    work = tempfile.mkdtemp(prefix="dp_warps.")
    start = (dp_script.resolve_map(a.start[0]), int(a.start[1]), int(a.start[2]))
    goal = (dp_script.resolve_map(a.goal[0]), int(a.goal[1]), int(a.goal[2]))
    maps = {start[0], goal[0]} | {dp_script.resolve_map(m) for m in a.maps}
    warps = {m: dp_script.parse_events(dp_script.map_table()[m]["events"])["warp"] for m in maps}
    comps = {m: [] for m in maps}  # map -> [(reached tiles, layers, objects)]
    came = {start: None}
    q = collections.deque([start])
    found = None
    while q and found is None:
        node = q.popleft()
        m, x, z = node
        comp = next((c for c in comps[m] if (x, z) in c[0]), None)
        if comp is None:
            # one look shows a 64x64 window: look again from the edge of what was reached until nothing is left out
            layers, objects = look(game, a.save, work, m, x, z)
            looked = {(x, z)}
            while True:
                edge = sorted(t for t, ls in layers.items() for mv in ls.values() for tx, tz, th in mv.values()
                              if th is None and (tx, tz) not in layers and t not in looked)
                if not edge:
                    break
                looked.add(edge[0])
                more, objs = look(game, a.save, work, m, *edge[0])
                for t, ls in more.items():
                    layers.setdefault(t, {}).update(ls)
                objects.update(objs)
                looked |= {t for t in edge if any(
                    (tx, tz) in layers for ls in layers[t].values() for tx, tz, th in ls.values() if th is None)}
            comp = (set(layers), layers, objects)
            comps[m].append(comp)
            print("map %s: from (%d,%d) %d tiles reachable (%d looks); warps beside them: %s" % (
                dp_script.map_name(m), x, z, len(layers), len(looked), " ".join(
                    "%d(%d,%d)" % (k, w["x"], w["z"]) for k, w in enumerate(warps[m])
                    if any((w["x"] + dx, w["z"] + dz) in layers for dx, dz in [(0, 0)] + DIR_DELTA))), flush=True)
        reached, layers, objects = comp
        if m == goal[0] and (goal[1], goal[2]) in reached:
            found = node
            break
        for k, w in enumerate(warps[m]):
            wt = (w["x"], w["z"])
            beside = wt in reached or any((wt[0] + dx, wt[1] + dz) in reached for dx, dz in DIR_DELTA)
            if not beside or w["dest"] not in maps:
                continue
            dest = warps[w["dest"]][w["warp"]]
            nxt = (w["dest"], dest["x"], dest["z"])
            if nxt not in came:
                came[nxt] = (node, k, wt)
                q.append(nxt)
    if found is None:
        raise SystemExit("no route from %s to %s over maps %s" % (start, goal, sorted(maps)))
    legs = []
    node = found
    while came[node] is not None:
        prev, k, wt = came[node]
        legs.append((prev, k, wt, node))
        node = prev
    legs.reverse()
    for prev, k, wt, node in legs:
        m, x, z = prev
        comp = next(c for c in comps[m] if (x, z) in c[0])
        clear = clear_reach(comp[1], comp[2], (x, z))
        near = clear | {(t[0] + dx, t[1] + dz) for t in clear for dx, dz in DIR_DELTA}
        note = "" if wt in near else "  (only past objects: %s)" % sorted(
            (o[0], o[1], o[2], o[3]) for t, o in comp[2].items() if t in comp[0] and t not in clear and any(
                (t[0] + dx, t[1] + dz) in clear for dx, dz in DIR_DELTA))
        print("%s (%d,%d): walk_to warp %d (%d,%d) -> %s (%d,%d)%s" % (
            dp_script.map_name(m), x, z, k, wt[0], wt[1], dp_script.map_name(node[0]), node[1], node[2], note))
    print("%s (%d,%d): walk_to the goal (%d,%d)" % (dp_script.map_name(goal[0]), found[1], found[2], goal[1], goal[2]))


if __name__ == "__main__":
    main()
