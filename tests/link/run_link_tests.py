#!/usr/bin/env python3
"""Two-station link regression tests on the real Platinum core.

Each scenario mints both saves from recipes/ (pc_lab recipes, no committed
saves), runs an np_headless pair from frame 0 with schedules/ and checks the
outcome: the traded party in both saves (np_save4), the battle's result
screen identical on both stations, the Underground join in the wireless
trace. The lockstep scenarios repeat exactly, so their frame numbers are
fixed; the relay one runs the game's datagrams through server/relay with
10% loss on each side, so it checks only the outcome.

  tests/link/run_link_tests.py [--keep DIR] [NAME...]

Skips (exit 0, "SKIP") without the ROM, the np_headless build or np_save4;
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
TRADED = {'a': [390, 396], 'b': [387, 396]}  # CHIMCHAR to A, TURTWIG to B; STARLY stays

SCENARIOS = [
    dict(name='union_trade', recipes=UNION,
         scheds={'a': 'schedules/trade-a.sched', 'b': 'schedules/trade-b.sched'},
         frames=16800, party=TRADED),
    # B (CHIMCHAR's Ember) beats A; the WIN/LOSE screen is the same picture on
    # both stations, with WIN on the left for UNIONB.
    dict(name='union_battle', recipes=UNION,
         scheds={'a': 'schedules/battle-a.sched', 'b': 'schedules/battle-b.sched'},
         frames=19700, dump_from=10200, dump_every=120,
         same=[19441, 19561, 19681], red=(19561, 40, 71)),
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
]


def have_tools():
    for path in (linkpair.HEADLESS, linkpair.SAVE4, linkpair.ROM):
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


def run(sc, work):
    d = os.path.join(work, sc['name'])
    os.makedirs(d, exist_ok=True)
    saves = {}
    for side in 'ab':
        saves[side] = linkpair.mint(os.path.join(HERE, sc['recipes'][side]), os.path.join(d, side.upper() + '.sav'))
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
        cmd = [linkpair.HEADLESS, 'platinum', linkpair.ROM, '--frames', str(sc['frames']), '--save', saves[side],
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
    for fr in sc.get('same', []):
        pa, pb = (os.path.join(d, s, 'frame_%06d.ppm' % fr) for s in 'ab')
        if not (os.path.exists(pa) and os.path.exists(pb)) or open(pa, 'rb').read() != open(pb, 'rb').read():
            fails.append('frame %d differs between the stations' % fr)
    if 'red' in sc:
        fr, x, y = sc['red']
        r, g, b = frame_px(os.path.join(d, 'a', 'frame_%06d.ppm' % fr), x, y)
        if not (r > 180 and g < 120 and b < 120):
            fails.append('frame %d (%d,%d) is %s, not the red WIN label' % (fr, x, y, (r, g, b)))
    for side, needle in sc.get('logs', {}).items():
        if needle not in open(os.path.join(d, side + '.log')).read():
            fails.append('%s log lacks "%s"' % (side, needle))
    return 'FAIL: ' + '; '.join(fails) if fails else 'ok'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('names', nargs='*')
    ap.add_argument('--keep', help='work directory to keep (default: a temporary one, removed)')
    a = ap.parse_args()
    missing = have_tools()
    if missing:
        print('SKIP: %s missing' % missing)
        return 0
    work = a.keep or tempfile.mkdtemp(prefix='linktests-')
    os.makedirs(work, exist_ok=True)
    failed = 0
    for sc in SCENARIOS:
        if a.names and sc['name'] not in a.names:
            continue
        res = run(sc, work)
        print('%-18s %s' % (sc['name'], res), flush=True)
        failed += res.startswith('FAIL')
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
