"""Explore the Snowpoint Gym ice graph in one session: nodes (x, z, snowballs left), edges = one press + slide."""
import os, sys, tempfile, json
from collections import deque
R = "/Users/soham/Documents/code/nativeplat-e2e-dp/tests/e2e"
sys.path.insert(0, R)
import run, bots
from np_e2e import Session

game = run.Game("diamond"); game.tools()
work = tempfile.mkdtemp(); sav = work + "/x.sav"
open(sav, "wb").write(open(sys.argv[1], "rb").read())
open(work + "/m.recipe", "w").write("party-level 0 100\nwarp MAP_SNOWPOINT_GYM 0\n"); run.mint(game, work + "/m.recipe", sav, sav, work)
s = Session(game.gp, game.rom, game.name, sav, work + "/run.log", 400000, options=run.DEFAULT_OPTIONS)
run.boot_continue(s)
GOAL = {(11, 4)}
DOOR = (11, 28)
DIRS = ["UP", "DOWN", "LEFT", "RIGHT"]


class Ctx:
    dir = "/tmp"


def settle():
    last, still = None, 0
    for _ in range(400):
        s.run(4)
        if s.in_battle or not s.field_ready:
            bots._field_or_handle(s, {}, Ctx(), s.frame + 30000)
            last, still = None, 0
            continue
        p = s.probe()
        cur = (p.x, p.z)
        still = still + 1 if cur == last else 0
        last = cur
        if still >= 6:
            break
    p = s.probe()
    snow = frozenset((o[0], o[1]) for o in p.objects if o[3] == 118)
    return (p.x, p.z), snow


edges = {}  # (pos, dir) -> pos2 ; invalidated when a snowball on pos's row/column is crushed
pos, snow = settle()
log = []
def untried(c):
    return [d for d in DIRS if (c, d) not in edges and not (c == DOOR and d == "DOWN")]
while True:
    if pos in GOAL:
        print("GOAL reached", pos, flush=True)
        break
    prev = {pos: None}; q = deque([pos]); target = None
    while q:
        c = q.popleft()
        if untried(c):
            target = c; break
        for d in DIRS:
            n = edges.get((c, d))
            if n is not None and n not in prev and not (c == DOOR and d == "DOWN"):
                prev[n] = (c, d); q.append(n)
    if target is None:
        print("exhausted at", pos, "snow", len(snow), sorted(snow), flush=True)
        break
    path = []
    c = target
    while prev[c]:
        c, d = prev[c]; path.append(d)
    ok = True
    for d in reversed(path):
        exp = edges[(pos, d)]
        s.run(8, d)
        npos, nsnow = settle()
        log.append((pos, d, npos))
        if npos != exp:
            edges[(pos, d)] = npos
            ok = False
        if nsnow != snow:
            gone = snow - nsnow
            for k in list(edges):
                if any(k[0][0] == g[0] or k[0][1] == g[1] for g in gone):
                    del edges[k]
            print("crushed", sorted(gone), "left", len(nsnow), flush=True)
        pos, snow = npos, nsnow
        if not ok:
            break
    if not ok or pos != target:
        continue
    d = untried(pos)[0]
    s.run(8, d)
    npos, nsnow = settle()
    edges[(pos, d)] = npos
    log.append((pos, d, npos))
    if nsnow != snow:
        gone = snow - nsnow
        for k in list(edges):
            if any(k[0][0] == g[0] or k[0][1] == g[1] for g in gone):
                del edges[k]
        print("crushed", sorted(gone), "left", len(nsnow), flush=True)
    print("edge", pos, d, "->", npos, "snow", len(nsnow), "frame", s.frame, flush=True)
    pos, snow = npos, nsnow
json.dump([[list(a), d, list(b)] for a, d, b in log], open("/tmp/icelog.json", "w"))
s.kill()
