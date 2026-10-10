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

--game picks the stations' games (platinum, diamond, pearl, black, white,
heartgold, soulsilver; one name for both, A:B for a cross-version pair,
`all` for every pair below); the default is platinum. Each scenario belongs
to one pair, because the press schedules are timed against what both
stations draw. Diamond and Pearl run on build/core-dp, Platinum on
build/core-plat, Black and White on build/core-bw, HeartGold and SoulSilver
on build/core-hgss (linkpair.py has the paths).

Black/White have no lab: their saves come from the e2e chain's milestone
10 (linkpair.mint_bw), and the trade check is relative to the parties the
mint produced (A's party slot 1 for B's slot 4). Neither have HeartGold/
SoulSilver: theirs are milestone 04's (linkpair.mint_hgss), and their
schedules were recorded by linkbot.py from scenarios/*.json (the e2e bots
driving two np_gp --lockstep stations), which run with PC_E2E and
text_instant as the scenarios' env and opts pass them here (opts: a list
for both stations, or a dict of lists per station). A new milestone 04 save
(the chain rerun) moves the Union Room's wandering avatars, so these
schedules are recorded again from their scenarios when it changes.

Skips (exit 0, "SKIP") without a ROM, an np_headless build, np_save4 (or,
for Black/White, np_save5 and the milestone saves; for HeartGold/SoulSilver
the milestone saves); the relay scenario also skips without `go`. About 35 s
of lockstep frames per 10000 on an idle machine. Interactive work on a
scenario's tail goes through linkpair.py serve/job (a forked checkpoint,
seconds per try).
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
    # Black's A with White's B in the Union Room (WM over np_host_net, the
    # same pc_wm.c model, TWL-SDK 5's WM library on the ARM9). A's party slot 1
    # for B's slot 4; the game saves both after the trade animation.
    dict(name='bw_trade', games=('black', 'white'),
         scheds={'a': 'schedules/bw-trade-a.sched', 'b': 'schedules/bw-trade-b.sched'},
         frames=17500, trade_slots=(0, 3), dump_from=12500, dump_every=100),
    # A Single Battle in the Union Room's battle room: both trade a move on
    # turn 1, A forfeits on turn 2; both stations leave the battle and stand
    # in the battle room (zone 150) again.
    dict(name='bw_battle', games=('black', 'white'),
         scheds={'a': 'schedules/bw-battle-a.sched', 'b': 'schedules/bw-battle-b.sched'},
         frames=18000, dump_from=13000, dump_every=100,
         logs={s: ('in_battle 0 -> 1', 'in_battle 1 -> 0', 'map_id 0 -> 150') for s in 'ab'}),
    # HeartGold's A with SoulSilver's B in the Union Room (the Pokemon
    # Center's counter, WM over np_host_net, the same pc_wm.c model as D/P's:
    # HG/SS build D/P's host fragment). Saves minted from the e2e chain's
    # Violet City (linkpair.mint_hgss); the schedules were recorded by
    # linkbot.py (scenarios/hgss-trade.json) on np_gp --lockstep stations,
    # which run with PC_E2E and text_instant, as here. A's party slot 1 for
    # B's slot 1; the game saves both after the animation.
    dict(name='hgss_trade', games=('heartgold', 'soulsilver'),
         scheds={'a': 'schedules/hgss-trade-a.sched', 'b': 'schedules/hgss-trade-b.sched'},
         frames=13735, trade_slots=(1, 1), env={'PC_E2E': '1'}, opts=['text_instant=1'],
         dump_from=9000, dump_every=200),
    # The same pair's battle: BATTLE in the Union Room, both enter both
    # Pokemon (the room's battles take two), then FIGHT and the first move
    # each turn and the next Pokemon on a faint (linkbot.linked_battle) until
    # the battle ends and both stations stand in the Union Room (map 2) again.
    dict(name='hgss_battle', games=('heartgold', 'soulsilver'),
         scheds={'a': 'schedules/hgss-battle-a.sched', 'b': 'schedules/hgss-battle-b.sched'},
         frames=20377, env={'PC_E2E': '1'}, opts=['text_instant=1'], dump_from=9000, dump_every=500,
         logs={s: ('map_id 158 -> 2', 'in_battle 0 -> 1', 'in_battle 1 -> 0') for s in 'ab'}),
    # HeartGold's A (milestone 04) with Platinum's B (union-b.recipe) in
    # the Union Room, each game's own WM traffic over the same pc_wm.c model.
    # B walks in on trade-b.sched (no text_instant, as recorded),
    # A talks to B, TRADE; A's party slot 1 for B's slot 0 (scenarios/
    # hg-pt-trade.json); both save after the animation.
    dict(name='hg_platinum_trade', games=('heartgold', 'platinum'), recipes={'b': UNION['b']},
         scheds={'a': 'schedules/hg-pt-trade-a.sched', 'b': 'schedules/hg-pt-trade-b.sched'},
         frames=17046, trade_slots=(1, 0), env={'PC_E2E': '1'}, opts={'a': ['text_instant=1']},
         dump_from=12000, dump_every=500),
    # The same with Diamond's B (dp-union-b.recipe), which walks in on
    # dp-trade-b.sched (scenarios/hg-dp-trade.json).
    dict(name='hg_diamond_trade', games=('heartgold', 'diamond'), recipes={'b': DP_UNION['b']},
         scheds={'a': 'schedules/hg-dp-trade-a.sched', 'b': 'schedules/hg-dp-trade-b.sched'},
         frames=15406, trade_slots=(1, 0), env={'PC_E2E': '1'}, opts={'a': ['text_instant=1']},
         dump_from=11000, dump_every=500),
]


def games_of(sc):
    return sc.get('games', ('platinum', 'platinum'))


def have_tools(games):
    paths = []
    for g in games:
        paths += linkpair.GAMES[g]
        if g in linkpair.BW_GAMES:
            paths += [linkpair.SAVE5, linkpair.bw_base_save(g)]
        elif g in linkpair.HGSS_GAMES:
            paths += [linkpair.SAVE4, linkpair.hgss_base_save(g)]
        elif linkpair.SAVE4 not in paths:
            paths.append(linkpair.SAVE4)
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
        out = os.path.join(d, side.upper() + '.sav')
        if games[side] in linkpair.BW_GAMES:
            saves[side] = linkpair.mint_bw(games[side], side, out)
        elif games[side] in linkpair.HGSS_GAMES:
            saves[side] = linkpair.mint_hgss(games[side], side, out)
        else:
            saves[side] = linkpair.mint(os.path.join(HERE, sc['recipes'][side]), out, games[side], base_dir=work)
    want_party = dict(sc.get('party', {}))
    if 'trade_slots' in sc:
        before = {side: linkpair.party(saves[side], games[side]) for side in 'ab'}
        sa, sb = sc['trade_slots']
        want_party = {'a': list(before['a']), 'b': list(before['b'])}
        want_party['a'][sa], want_party['b'][sb] = before['b'][sb], before['a'][sa]
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
        opts = sc.get('opts', ())
        for o in opts.get(side, ()) if isinstance(opts, dict) else opts:
            cmd += ['-o', o]
        if 'dump_every' in sc:
            cmd += ['--dump', dump, '--dump-every', str(sc['dump_every']), '--dump-from', str(sc['dump_from'])]
        procs[side] = subprocess.Popen(cmd, stdout=open(os.path.join(d, side + '.log'), 'w'),
                                       stderr=subprocess.STDOUT, env=env)
    rcs = {side: procs[side].wait() for side in 'ab'}
    if relay_proc:
        relay_proc.kill()
    fails = ['%s exited %d' % (s, rc) for s, rc in rcs.items() if rc != 0]
    for side, want in want_party.items():
        got = linkpair.party(saves[side], games[side])
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
    for side, needles in sc.get('logs', {}).items():
        log = open(os.path.join(d, side + '.log')).read()
        for needle in (needles,) if isinstance(needles, str) else needles:
            if needle not in log:
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
            sys.exit('run_link_tests: unknown game %s (%s)' % (g, ', '.join(sorted(linkpair.GAMES))))
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
