#!/usr/bin/env python3
"""Explore a map with the game as the oracle and print a `moves` route to a goal: for maps whose walkable surface the
probe's land grid does not show (Platinum's Distortion World: floating platforms, walls walked on, elevators).

    python3 tests/e2e/tools/pt_explore.py SAVE [--goal-tile "X,Z[,Y];X,Z[,Y];..."] [--goal-map MAP] [--avoid "X,Z[,Y];..."] [--steps JSON]
        [--toward X,Z] [--frames N] [--save-to OUT] [--shot PNG]

Boots SAVE through CONTINUE, runs --steps (milestone steps, JSON list), then explores online: a state is the probe's
(map, x, y, z); each untried direction of the current state is held until the state changes (or not: blocked), then
the walk goes on from wherever it landed; when the current state has no untried direction it walks the known graph to
the nearest one that has. Text that a step starts is advanced with B. Goals are reached in turn: each tile X,Z (Y: the
probe's y, twice the Distortion World's tileY), e.g. the event tiles that send its moving platforms off with the
player (a platform's place is state the walk cannot see, so each leg is explored afresh from where the last one
landed), then the map MAP (--goal-map: an elevator to the next floor). Prints the shortest known route of each leg,
joined, as `dirs` for the `moves` bot. --toward orders the tries of the map leg (directions toward X,Z first);
--avoid lists tiles never stepped on (the far ends' events, which would send a platform back with the player).
"""
import argparse
import collections
import json
import os
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import bots  # noqa: E402
import run  # noqa: E402
from np_e2e import Session  # noqa: E402

NAMES = "UDLR"  # bots.DIR_KEYS order: up, down, left, right
DELTA = [(0, -1), (0, 1), (-1, 0), (1, 0)]


def state(s):
    p = s.probe()
    return (p.map_id, p.x, p.y, p.z)


def settle(s, limit=600):
    """Until the player is free: text a step started is advanced with B."""
    waited = 0
    while not s.field_ready and waited < limit:
        if not s.run(20, until="field_ready=1"):
            s.run(2, "b")
        waited += 22
    s.run(4)


def move(s, d):
    """Hold direction d, without letting go, until the state changes (up to 48 frames: on a Distortion World wall
    a released key turns the player again and the step never starts); then settle. Returns the new state."""
    m, x, y, z = state(s)
    s.run(48, bots.DIR_KEYS[d], until=["x!=%d" % x, "z!=%d" % z, "y!=%d" % y, "map_id!=%d" % m, "in_battle=1"])
    s.run(20, until="field_ready=1")
    settle(s)
    return state(s)


def explore(s, start, goal, a, gi, avoid=()):
    """Explore from `start` until `goal` ((x, z), (x, z, y) or ("map", id)); returns (route, state) or None."""
    if goal[0] == "map":
        def done(st):
            return st[0] == goal[1]
        toward = tuple(int(v) for v in a.toward.split(",")) if a.toward else None
    else:
        def done(st):
            return (st[1], st[3]) == goal[:2] and (len(goal) < 3 or st[2] == goal[2])
        toward = goal[:2]
    print("goal %d %s from %s" % (gi, goal, start))
    graph = collections.defaultdict(dict)  # state -> {d: next state}

    def order(st):
        ds = [0, 1, 2, 3]
        if toward:
            ds.sort(key=lambda d: abs(st[1] + DELTA[d][0] - toward[0]) + abs(st[3] + DELTA[d][1] - toward[1]))
        return ds

    def path(src, want):
        prev = {src: None}
        q = collections.deque([src])
        while q:
            u = q.popleft()
            if want(u):
                out = []
                while prev[u] is not None:
                    u, d = prev[u]
                    out.append(d)
                return out[::-1]
            for d, v in graph[u].items():
                if v != u and v not in prev:
                    prev[v] = (u, d)
                    q.append(v)
        return None

    cur, tries = start, 0
    while not done(cur) and s.remaining > 200:
        untried = [d for d in order(cur) if d not in graph[cur]]
        if untried:
            d = untried[0]
            tx, tz = cur[1] + DELTA[d][0], cur[3] + DELTA[d][1]
            if any((tx, tz) == t[:2] and (len(t) < 3 or t[2] == cur[2]) for t in avoid):
                graph[cur][d] = cur  # never stepped on (an event that would send the walk back)
                continue
            nxt = move(s, d)
            graph[cur][d] = nxt
            tries += 1
            if nxt != cur:
                print("  %s %s -> %s" % (NAMES[d], cur, nxt))
            if nxt != cur and goal[0] != "map" and (tx, tz) == goal[:2] and (len(goal) < 3 or cur[2] == goal[2]):
                # stepping onto the goal's event tile set it off (a platform carried the player away at once)
                route = path(start, lambda u: u == cur) + [d]
                settle(s, 8000)
                after = state(s)
                print("goal %d set off from %s; settled at %s" % (gi, cur, after))
                return route, after
            cur = nxt
            continue
        route = path(cur, lambda u: any(d not in graph[u] for d in range(4)))
        if route is None:
            print("explored everything reachable (%d states) without goal %d" % (len(graph), gi))
            return None
        for d in route:
            got = move(s, d)
            if got != graph[cur][d]:
                graph[cur][d] = got
                cur = got
                break
            cur = got
    print("at %s after %d tries, %d states, frame %d" % (cur, tries, len(graph), s.frame))
    if not done(cur):
        return None
    settle(s, 8000)  # a platform rides on, an elevator to the next floor, the next floor's scene
    after = state(s)
    print("goal %d reached; settled at %s" % (gi, after))
    return path(start, done), after


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("save")
    ap.add_argument("--steps")
    ap.add_argument("--goal-map")
    ap.add_argument("--goal-tile")
    ap.add_argument("--toward")
    ap.add_argument("--avoid", help="tiles never stepped on, X,Z[,Y];...: events that would carry the walk back")
    ap.add_argument("--frames", type=int, default=60000)
    ap.add_argument("--save-to")
    ap.add_argument("--shot")
    ap.add_argument("--try", dest="try_", help="moves to play and print one by one instead of exploring, e.g. "
                    "'U7 L2 R': each held as `move` holds it; a move that changes nothing is printed as blocked")
    a = ap.parse_args()
    game = run.Game("platinum")
    game.tools()
    work = tempfile.mkdtemp(prefix="pt_explore.")
    sav = os.path.join(work, "x.sav")
    with open(a.save, "rb") as f, open(sav, "wb") as g:
        g.write(f.read())
    s = Session(game.gp, game.rom, game.name, sav, os.path.join(work, "run.log"), a.frames, options=run.DEFAULT_OPTIONS)
    ctx = run.Ctx(game, types.SimpleNamespace(dir=work))
    goal_map = game.resolve(a.goal_map) if a.goal_map else None
    saved = False
    try:
        run.boot_continue(s)
        for step in json.loads(a.steps or "[]"):
            bots.BOTS[step["do"]](s, step, ctx)
        if a.try_:
            for tok in a.try_.split():
                if tok == "A":  # talk: A, then the text it starts advanced
                    s.run(4, "a")
                    s.run(40)
                    settle(s)
                    print("  A -> %s" % (state(s),))
                    continue
                if tok[0] == "W":  # Wn: wait n frames (an elevator ride), then settle
                    s.run(int(tok[1:]))
                    settle(s)
                    print("  %s -> %s" % (tok, state(s)))
                    continue
                d = NAMES.index(tok[0])
                for _ in range(int(tok[1:] or 1)):
                    before = state(s)
                    after = move(s, d)
                    print("  %s %s -> %s%s" % (tok[0], before, after, "  BLOCKED" if after == before else ""))
        goals = [tuple(int(v) for v in g.split(",")) for g in a.goal_tile.split(";")] if a.goal_tile else []
        avoid = [tuple(int(v) for v in g.split(",")) for g in a.avoid.split(";")] if a.avoid else []
        if goal_map is not None:
            goals.append(("map", goal_map))
        runs = []
        cur = state(s)
        for gi, goal in enumerate(goals):
            seg = explore(s, cur, goal, a, gi, avoid)
            if seg is None:
                break
            route, cur = seg
            for d in route:
                if runs and runs[-1][0] == NAMES[d]:
                    runs[-1][1] += 1
                else:
                    runs.append([NAMES[d], 1])
            print("dirs so far = %s" % json.dumps(runs).replace("],[", "], ["))
        else:
            print("GOAL: all %d goals" % len(goals))
        if a.shot:
            ppm = os.path.join(work, "shot.ppm")
            s.dump(ppm)
            os.replace(run.ppm_to_png(ppm), a.shot)
        if a.save_to:
            bots.BOTS["save"](s, {}, ctx)
            s.quit()
            saved = True
            with open(sav, "rb") as f, open(a.save_to, "wb") as g:
                g.write(f.read())
    finally:
        if not saved:
            s.kill()
        print("log: %s" % os.path.join(work, "run.log"))


if __name__ == "__main__":
    main()
