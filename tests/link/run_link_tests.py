#!/usr/bin/env python3
"""Two-station link regression tests on the real cores.

Each scenario mints both saves from recipes/ (lab recipes, no committed
saves), runs an np_headless pair from frame 0 with schedules/ and checks the
outcome: the traded party in both saves (np_save4), the battle's result
screen identical on both stations, the Underground join in the wireless
trace. The lockstep scenarios repeat exactly, so their frame numbers are
fixed; the relay one runs the game's datagrams through server/relay with
10% loss on each side, so it checks only the outcome.

  tests/link/run_link_tests.py [--game A[:B]] [--keep DIR] [NAME...]

--game picks the stations' games (platinum, diamond, pearl; one name for
both, A:B for a cross-version pair, `all` for every pair below); the
default is platinum. Each scenario belongs to one pair, because the press
schedules are timed against what both stations draw. Diamond and Pearl run
on build/core-dp, Platinum on build/core-plat (linkpair.py has the paths).

Skips (exit 0, "SKIP") without a ROM, an np_headless build or np_save4;
the relay scenario also skips without `go`. About 35 s of lockstep frames
per 10000 on an idle machine. Interactive work on a scenario's tail goes
through linkpair.py serve/job (a forked checkpoint, seconds per try).
"""
import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import linkpair  # noqa: E402

UNION = {'a': 'recipes/union-a.recipe', 'b': 'recipes/union-b.recipe'}
UG = {'a': 'recipes/ug-a.recipe', 'b': 'recipes/ug-b.recipe'}
DP_UNION = {'a': 'recipes/dp-union-a.recipe', 'b': 'recipes/dp-union-b.recipe'}
DP_TRADE = {'a': 'schedules/dp-trade-a.sched', 'b': 'schedules/dp-trade-b.sched'}
TRADED = {'a': [390, 396], 'b': [387, 396]}  # CHIMCHAR to A, TURTWIG to B; STARLY stays

SCENARIOS = [
    dict(name='union_trade', recipes=UNION,
         scheds={'a': 'schedules/trade-a.sched', 'b': 'schedules/trade-b.sched'},
         frames=16800, party=TRADED),
    # B trades KADABRA: it evolves into ALAKAZAM on A's station after the trade
    # animation (a link trade evolves, an in-game NPC trade never does:
    # unk_0203D1B8.c:1026), and A's save keeps the ALAKAZAM.
    dict(name='union_trade_evolve', recipes={'a': UNION['a'], 'b': 'recipes/union-b-kadabra.recipe'},
         scheds={'a': 'schedules/trade-evolve-a.sched', 'b': 'schedules/trade-evolve-b.sched'},
         frames=18500, party={'a': [65, 396], 'b': [387, 396]}),
    # B (CHIMCHAR's Ember) beats A; the WIN/LOSE screen is the same picture on
    # both stations, with WIN on the left for UNIONB. `win` is checked over
    # every dump from dump_from to frames, the bound: at least two dumps must
    # show the red WIN label at (x, y) on both stations and be the same
    # picture, wherever the battle's length puts them.
    dict(name='union_battle', recipes=UNION,
         scheds={'a': 'schedules/battle-a.sched', 'b': 'schedules/battle-b.sched'},
         frames=21500, dump_from=17000, dump_every=60, win=(40, 71)),
    # B enters 37 frames after A: the comm manager seeds its parent/child
    # alternation from the RTC and the VBlank counter, and two lockstep
    # stations with the port's fixed clock and the same frame would draw the
    # same numbers forever (both parent, then both child, never meeting).
    dict(name='underground_meet', recipes=UG,
         scheds={'a': 'schedules/ug-a.sched', 'b': 'schedules/ug-b.sched'},
         frames=7400, env={'PC_WM_TRACE': '1'},
         logs={'a': 'joined as aid', 'b': 'joined as aid'}),
    dict(name='relay_trade', recipes=UNION, relay=True, drop=10,
         scheds={'a': 'schedules/relay-trade-a.sched', 'b': 'schedules/relay-trade-b.sched'},
         frames=19000, party=TRADED),
    # Diamond and Pearl. The trade is saved by the game once the animation
    # ends, so the parties are checked without leaving the trade screen.
    dict(name='dp_trade', games=('diamond', 'diamond'), recipes=DP_UNION, scheds=DP_TRADE,
         frames=11500, party=TRADED),
    # As union_battle: B wins, WIN (left) for UNIONB on both stations.
    dict(name='dp_battle', games=('diamond', 'diamond'), recipes=DP_UNION,
         scheds={'a': 'schedules/dp-battle-a.sched', 'b': 'schedules/dp-battle-b.sched'},
         frames=17000, dump_from=13500, dump_every=60, win=(47, 92)),
    # Cross-version, as the cartridges allow: Diamond's A with Pearl's B, and
    # with Platinum's (a Platinum save and Platinum's own walk into the room).
    dict(name='dp_pearl_trade', games=('diamond', 'pearl'), recipes=DP_UNION, scheds=DP_TRADE,
         frames=11500, party=TRADED),
    dict(name='dp_platinum_trade', games=('diamond', 'platinum'),
         recipes={'a': 'recipes/dp-union-a.recipe', 'b': 'recipes/union-b.recipe'},
         scheds={'a': 'schedules/dp-pt-trade-a.sched', 'b': 'schedules/dp-pt-trade-b.sched'},
         frames=13000, party=TRADED),
]


def games_of(sc):
    return sc.get('games', ('platinum', 'platinum'))


def have_tools(games):
    paths = [linkpair.SAVE4]
    for g in games:
        paths += linkpair.GAMES[g]
    for path in paths:
        if not os.path.exists(path):
            return path
    return None


def start_relay(work):
    if not shutil.which('go'):
        return None, None
    binary = os.path.join(work, 'relay')
    subprocess.run(['go', 'build', '-o', binary, '.'], cwd=os.path.join(linkpair.ROOT, 'server', 'relay'),
                   check=True)
    port = random.randrange(41000, 49000)
    proc = subprocess.Popen([binary, '-listen', '127.0.0.1:%d' % port], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    return proc, '127.0.0.1:%d' % port


def frame_px(path, x, y):
    w, h, px = linkpair.readppm(path)
    o = (y * w + x) * 3
    return tuple(px[o:o + 3])


def is_red(rgb):
    r, g, b = rgb
    return r > 180 and g < 120 and b < 120


def run(sc, work):
    d = os.path.join(work, sc['name'])
    os.makedirs(d, exist_ok=True)
    games = dict(zip('ab', games_of(sc)))
    saves = {}
    for side in 'ab':
        saves[side] = linkpair.mint(os.path.join(HERE, sc['recipes'][side]), os.path.join(d, side.upper() + '.sav'),
                                    games[side], base_dir=work)
    relay_proc = None
    extra = {'a': [], 'b': []}
    if sc.get('relay'):
        relay_proc, addr = start_relay(work)
        if relay_proc is None:
            return 'SKIP (no go for server/relay)'
        pin = str(random.randrange(1000, 9999))
        for i, side in enumerate('ab'):
            extra[side] = ['--net', str(random.randrange(20000, 40000)), '--net-relay', addr, '--net-pin', pin,
                           '--net-drop', str(sc['drop'])]
    port = random.randrange(20000, 40000)
    ports = {'a': (port, port + 1), 'b': (port + 1, port)}
    procs = {}
    env = dict(os.environ, **sc.get('env', {}))
    for side in 'ab':
        dump = os.path.join(d, side)
        os.makedirs(dump, exist_ok=True)
        cmd = linkpair.core(games[side]) + ['--frames', str(sc['frames']), '--save', saves[side],
               '--schedule', os.path.join(HERE, sc['scheds'][side]), '--lockstep', '%d:%d' % ports[side],
               '--net-id', linkpair.IDS[side]] + extra[side]
        if 'dump_every' in sc:
            cmd += ['--dump', dump, '--dump-every', str(sc['dump_every']), '--dump-from', str(sc['dump_from'])]
        procs[side] = subprocess.Popen(cmd, stdout=open(os.path.join(d, side + '.log'), 'w'),
                                       stderr=subprocess.STDOUT, env=env)
    rcs = {side: procs[side].wait() for side in 'ab'}
    if relay_proc:
        relay_proc.kill()
    fails = ['%s exited %d' % (s, rc) for s, rc in rcs.items() if rc != 0]
    for side, want in sc.get('party', {}).items():
        got = linkpair.party(saves[side])
        if got != want:
            fails.append('%s party %s, want %s' % (side, got, want))
    if 'win' in sc:
        x, y = sc['win']
        shown = 0
        for name in sorted(os.listdir(os.path.join(d, 'a'))):
            pa, pb = (os.path.join(d, s, name) for s in 'ab')
            if not os.path.exists(pb) or not all(is_red(frame_px(p, x, y)) for p in (pa, pb)):
                continue
            if open(pa, 'rb').read() != open(pb, 'rb').read():
                fails.append('%s shows WIN on both stations but differs between them' % name)
            shown += 1
        if shown < 2:
            fails.append('%d dumps (from frame %d to %d) show the red WIN label at (%d,%d) on both stations, '
                         'want 2 or more' % (shown, sc['dump_from'], sc['frames'], x, y))
    for side, needle in sc.get('logs', {}).items():
        if needle not in open(os.path.join(d, side + '.log')).read():
            fails.append('%s log lacks "%s"' % (side, needle))
    return 'FAIL: ' + '; '.join(fails) if fails else 'ok'


def pairs(spec):
    """--game's value as the (A, B) pairs it selects."""
    if spec == 'all':
        return sorted({games_of(sc) for sc in SCENARIOS})
    a, _, b = spec.partition(':')
    b = b or a
    for g in (a, b):
        if g not in linkpair.GAMES:
            sys.exit('run_link_tests: unknown game %s (platinum, diamond, pearl)' % g)
    return [(a, b)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('names', nargs='*')
    ap.add_argument('--game', default='platinum', help='A[:B] station games, or all (default platinum)')
    ap.add_argument('--keep', help='work directory to keep (default: a temporary one, removed)')
    a = ap.parse_args()
    want = pairs(a.game)
    chosen = [sc for sc in SCENARIOS if games_of(sc) in want and (not a.names or sc['name'] in a.names)]
    if not chosen:
        print('run_link_tests: no scenario for %s%s' % (a.game, ' named ' + ' '.join(a.names) if a.names else ''))
        return 2
    work = a.keep or tempfile.mkdtemp(prefix='linktests-')
    os.makedirs(work, exist_ok=True)
    failed = 0
    for sc in chosen:
        missing = have_tools(games_of(sc))
        res = 'SKIP: %s missing' % missing if missing else run(sc, work)
        print('%-24s %-18s %s' % (sc['name'], ':'.join(games_of(sc)), res), flush=True)
        failed += res.startswith('FAIL')
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
