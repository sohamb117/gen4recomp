#!/usr/bin/env python3
"""Two linked stations driven by the e2e bots, recorded as link schedules.

  tests/link/linkbot.py SCENARIO.json OUTDIR

Two np_gp instances (tests/gameplay/np_gp.c --lockstep) run the stations'
games in frame lockstep, trading the game's datagrams exactly as two
np_headless --lockstep instances do. The scenario's steps run in order, each
on one station while the other holds its input (none unless a `hold` step
set one), so a bot (walk_to, talk_to, auto_battle, ...) works on the station
it drives as it does in a milestone: the other one simply waits beside it,
frame for frame. Every frame both stations ran is recorded; at the end
OUTDIR/a.sched and OUTDIR/b.sched are the two input streams as np_headless
press schedules, which tests/link/run_link_tests.py replays on a pair of
np_headless instances, where they reach the same states on the same frames.

SCENARIO.json:
  {"games": ["heartgold", "soulsilver"],      station A's game, B's
   "saves": {"a": "A.sav", "b": "B.sav"},     copied into OUTDIR first; without
                                              it HG/SS stations are minted
                                              (linkpair.mint_hgss), as the replay does
   "recipes": {"b": "recipes/union-b.recipe"}, a D/P/Pt station's lab recipe (linkpair.mint)
   "opts": {"b": []},                         a station's np_gp -o options (default
                                              text_instant=1)
   "budget": 30000,                           frames
   "steps": [["a", "continue"],               run.boot_continue
             ["b", "walk_to", {"x": 6, "z": 6}],
             ["a", "shot", "name"],           both stations' screens, OUTDIR/s_FRAME_name_{a,b}.ppm
             ["b", "hold", {"keys": "a"}],    the input B holds while A is driven (and in
                                              `wait`); with "every": E, "n": N the keys
                                              are pressed N frames of each E (from frame
                                              0); {} holds nothing; side "both" sets both
             ["both", "wait", 120],           both run N frames on their holds
             ["a", "probe"],                  print A's map, tile and the objects' tiles
             ["both", "battle", {"max": N}],  both stations through a link battle (linked_battle)
             ["b", "play", {"file": F, "to": T}],  B runs press schedule F (absolute frames,
                                              relative to the repository root) up to frame T
             ...]}
Any bots.BOTS step works on its station (milestone.toml step names and
arguments); `press` and `tap` are bots too. Station ids are linkpair's
(A 0x111111, B 0x222222); PC_E2E=1 on both. Each game runs on its own
core build (run.Game: NP_CORE_BUILD, else build/core-<group>).
"""
import json
import os
import random
import shutil
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tests', 'e2e'))
sys.path.insert(0, os.path.join(ROOT, 'tests', 'gameplay'))
sys.path.insert(0, HERE)
import bots  # noqa: E402
import linkpair  # noqa: E402
import run  # noqa: E402
from np_e2e import HarnessError, Session, key_arg  # noqa: E402

OPTIONS = ['text_instant=1']


class Station(Session):
    """One station of the pair. Every frame it runs, its peer runs too: a run
    without a stop condition goes to both in one command each; one with a
    condition goes a frame at a time, so both stop on the frame it holds."""

    def __init__(self, side, *a, **kw):
        Session.__init__(self, *a, **kw)
        self.side = side
        self.peer = None
        self.hold = {}  # what this station holds while the peer is driven (held_input)
        self.rec = []   # [frame, n, keys, touch] runs, merged on write

    def held_input(self, frame):
        """(keys, touch, frames it stays so) the hold gives at `frame`: `keys`
        and `touch` throughout, or with `every`, pressed for the first `n`
        frames of each `every` (counted from frame 0) and released between."""
        h = self.hold
        keys, touch, every = h.get('keys'), h.get('touch'), int(h.get('every', 0))
        if not every:
            return keys, touch, 1 << 30
        phase, n = frame % every, int(h.get('n', 4))
        if phase < n:
            return keys, touch, n - phase
        return None, None, every - phase

    def _record(self, f0, n, keys, touch):
        k, t = key_arg(keys), tuple(touch) if touch else None
        if self.rec and self.rec[-1][2:] == [k, t] and self.rec[-1][0] + self.rec[-1][1] == f0:
            self.rec[-1][1] += n
        else:
            self.rec.append([f0, n, k, t])

    def _both(self, n, keys, touch, until):
        """n frames with this station's input and the peer's hold; True if a
        stop condition (one frame at most) held."""
        p = self.peer
        while n > 0:
            f0 = self.frame
            if p.frame != f0:
                raise HarnessError('stations out of step: %s at %d, %s at %d' % (self.side, f0, p.side, p.frame))
            pk, pt, plen = p.held_input(f0)
            m = min(n, plen)
            mine = self._run_line(m, keys, touch, until)
            theirs = p._run_line(m, pk, pt, ())
            self._send(mine)
            p._send(theirs)
            hit = self._ran(self._recv(mine))
            ran = self.frame - f0
            if hit and ran < m:
                # np_gp stopped this side early, the peer would not: a stop
                # condition only ever runs one frame (_run).
                raise HarnessError('a stop condition ended a %d-frame lockstep run' % m)
            p._ran(p._recv(theirs))
            self._record(f0, ran, keys, touch)
            p._record(f0, ran, pk, pt)
            if hit:
                return True
            n -= m
        return False

    def _run(self, n, keys, touch, until):
        if not until:
            return self._both(n, keys, touch, ())
        for _ in range(n):
            if self._both(1, keys, touch, until):
                return True
        return False

    def wait(self, n):
        """n frames with both stations on their holds."""
        end = self.frame + n
        while self.frame < end:
            keys, touch, run = self.held_input(self.frame)
            self.run(min(run, end - self.frame), keys, touch)

    def schedule(self, path):
        with open(path, 'w') as f:
            f.write('# linkbot.py station %s (%s): every frame from 0, merged runs\n' % (self.side, self.game))
            for f0, n, k, t in self.rec:
                if t:
                    f.write('%d:tap:%d:%d:%d\n' % (f0, t[0], t[1], n))
                elif k != 'none':
                    f.write('%d:%s:%d\n' % (f0, k, n))
        return len([r for r in self.rec if r[3] or r[2] != 'none'])


def parse_schedule(path):
    """A press schedule (np_headless's grammar: F:keys[:N[:R:C]],
    F:tap:X:Y[:N[:R:C]], "+D" relative frames, '#' comments) as a function
    of the frame: (keys, touch) held then."""
    steps, prev = [], 0
    for line in open(path):
        for step in line.split('#', 1)[0].replace(';', '\n').split():
            rel = step.startswith('+')
            parts = (step[1:] if rel else step).split(':')
            frame = int(parts[0]) + (prev if rel else 0)
            prev = frame
            tap, rest = None, parts[2:]
            if parts[1] == 'tap':
                tap, rest = (int(parts[2]), int(parts[3])), parts[4:]
            nums = [int(v) for v in rest]
            n = nums[0] if nums and nums[0] >= 1 else 6
            every, count = (nums[1], nums[2]) if len(nums) >= 3 else (0, 1)
            steps.append((frame, None if tap else parts[1], tap, n, every, count))

    def at(k):
        keys, touch = set(), None
        for frame, key, tap, n, every, count in steps:
            off = k - frame
            if off < 0:
                continue
            if every > 0:
                if off // every >= count:
                    continue
                off %= every
            if off >= n:
                continue
            if tap:
                touch = tap
            elif key != 'none':
                keys.update(key.split('+'))
        return '+'.join(sorted(keys)) or None, touch
    return at


def play(s, path, to):
    """Station s runs a press schedule's inputs (absolute frames) up to frame `to`."""
    at = parse_schedule(path)
    while s.frame < to:
        keys, touch = at(s.frame)
        n = 1
        while s.frame + n < to and at(s.frame + n) == (keys, touch):
            n += 1
        s.run(n, keys, touch)


# np_e2e_block.ui: the battle's action menu and its party screen.
UI_BATTLE_MENU, UI_BATTLE_PARTY = 1, 2


def linked_battle(a, b, limit):
    """Both stations through a link battle, from its start to both being back
    on the field: whichever station a battle menu waits on presses A (FIGHT,
    then the first move, on the default target); a party screen asking for a
    replacement gets RIGHT then A (the next slot) and A on its SHIFT page.
    Inputs are decided every 8 frames from each station's probe, pressed for
    4 frames and released for 4, so each press is one press."""
    start = a.frame
    plan = {'a': [], 'b': []}
    entered = False
    while a.frame - start < limit:
        keys = {}
        for s in (a, b):
            if not plan[s.side]:
                p = s.probe()
                if p is not None and p.ui == UI_BATTLE_MENU:
                    plan[s.side] = ['a']
                elif p is not None and p.ui == UI_BATTLE_PARTY:
                    plan[s.side] = ['right', 'a'] if p.ui_arg == 0 else ['a']
            keys[s.side] = plan[s.side].pop(0) if plan[s.side] else None
        b.hold = {'keys': keys['b']}
        a.run(4, keys['a'])
        b.hold = {}
        a.run(4)
        entered |= bool(a.in_battle or b.in_battle)
        if entered and not a.in_battle and not b.in_battle:
            return a.frame - start
    raise HarnessError('link battle not over after %d frames' % limit)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sc = json.load(open(sys.argv[1]))
    out = os.path.abspath(sys.argv[2])
    os.makedirs(out, exist_ok=True)
    games = dict(zip('ab', sc['games']))
    budget = int(sc.get('budget', 30000))
    port = random.randrange(20000, 40000)
    ports = {'a': (port, port + 1), 'b': (port + 1, port)}
    st, ctx = {}, {}
    for side in 'ab':
        g = run.Game(games[side])
        g.tools()
        sav = os.path.join(out, side.upper() + '.sav')
        if 'saves' in sc:
            shutil.copyfile(sc['saves'][side], sav)
        elif games[side] in linkpair.HGSS_GAMES:
            linkpair.mint_hgss(games[side], side, sav)
        elif side in sc.get('recipes', {}):
            linkpair.mint(os.path.join(HERE, sc['recipes'][side]), sav, games[side], base_dir=out)
        else:
            sys.exit('linkbot: %s station %s has no "saves" or "recipes" entry' % (games[side], side))
        st[side] = Station(side, g.gp, g.rom, games[side], sav, os.path.join(out, side + '.log'), budget,
                           options=sc.get('opts', {}).get(side, OPTIONS), env=sc.get('env'),
                           extra_args=['--lockstep', '%d:%d' % ports[side], '--net-id', linkpair.IDS[side]])
        ctx[side] = run.Ctx(g, types.SimpleNamespace(dir=out, data={}, name='linkbot-' + side))
    st['a'].peer, st['b'].peer = st['b'], st['a']
    result = 'ok'
    i = -1
    try:
        for i, step in enumerate(sc['steps']):
            side, name = step[0], step[1]
            arg = step[2] if len(step) > 2 else {}
            s = st['a' if side == 'both' else side]
            s.note('linkbot step %d: %s %s %s' % (i, side, name, arg))
            s.peer.note('linkbot step %d (peer): %s %s %s' % (i, side, name, arg))
            if name == 'continue':
                run.boot_continue(s)
            elif name == 'shot':
                for x in 'ab':
                    st[x].dump(os.path.join(out, 's_%06d_%s_%s.ppm' % (s.frame, arg, x)))
            elif name == 'hold':
                for x in ('ab' if side == 'both' else side):
                    st[x].hold = dict(arg)
            elif name == 'wait':
                s.wait(int(arg))
            elif name == 'battle':
                n = linked_battle(st['a'], st['b'], int(arg.get('max', 20000)))
                s.note('linkbot: the link battle ended after %d frames' % n)
            elif name == 'play':
                play(s, os.path.join(ROOT, arg['file']), int(arg['to']))
            elif name == 'probe':
                p = s.probe()
                print('%s frame %d: map %d (%d,%d) facing %d, objects %s' % (
                    s.side, s.frame, p.map_id, p.x, p.z, p.facing,
                    ' '.join('%d@(%d,%d)' % (o[2], o[0], o[1]) for o in p.objects)), flush=True)
            else:
                bots.BOTS[name](s, arg, ctx[s.side])
    except Exception as e:
        result = 'FAIL step %d: %s: %s' % (i, type(e).__name__, e)
        for x in 'ab':
            try:
                st[x].dump(os.path.join(out, 's_%06d_fail_%s.ppm' % (st[x].frame, x)))
            except Exception:
                pass
    frame = st['a'].frame
    for x in 'ab':
        print('%s.sched: %d steps' % (x, st[x].schedule(os.path.join(out, x + '.sched'))))
    for x in 'ab':
        st[x].kill()
    print('linkbot: %s at frame %d' % (result, frame))
    return 0 if result == 'ok' else 1


if __name__ == '__main__':
    sys.exit(main())
