#!/usr/bin/env python3
"""Records the child's touch schedule for tests/poketransfer/run_tests.py transfer_<card>.

  tests/poketransfer/catchbot.py platinum|heartgold [--work DIR] [--port N]

The capture minigame (the child's mb_cap_*) is played with the stylus: the ball
is pulled back on the bottom screen's slingshot and lands where the reticle
shows. Pulling (dx, dy) from the ball at (128, 85) lands near
(128 - 2.43 dx, 201 - 2.48 dy) on the top screen, and the ball needs about 24
frames to land. The six Pokémon hop between the bushes and hide; a ball that
lands on a hidden one flushes it out.

The two stations run in lockstep, so a run repeats exactly. This tool plays
the game greedily from --fork-at checkpoints (np_headless). It records a
stretch, finds what moved against the field's background (Pokémon, or the
parts that show from a bush), and tries shots that land where something will
be. A shot is kept when the icon bar shows one more Poké Ball. When nothing in
the stretch is caught, one flush shot is kept at something that showed, and
the next stretch starts after it. Then the post-game taps are added: the "Pokémon
CANNOT be returned" message, and YES to "Transfer these Pokémon?" (645 and 745
frames after the last kept shot). The result is written to
pt-child-<card>.sched. Inputs are run_tests.py's (the same saves, ROMs and
cores); a recorded schedule fits only those. It takes 10-40 minutes.
"""
import argparse
import collections
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_tests  # noqa: E402

BALL = (128, 85)
# The selection screen's BOX 1 icons and the frame to the right, the YES of
# "The six chosen Pokémon will be transferred."; the minigame's first frame.
ICONS = [(24, 48), (48, 48), (70, 48), (96, 48), (120, 48), (144, 48)]
FRAME_TARGET = (212, 100)
DRAG_FROM, CHOSEN_YES = 11400, (11780, 226, 109)
GAME_FROM = 13320
TIMER_END = 19500
ICON_SLOTS = (20, 44, 67, 92, 117, 141)  # the icon bar, bottom screen y 160..181
BALL_RED = (251, 97, 65)  # the bar's Poké Ball icon


# ---- frames --------------------------------------------------------------------
def load_ppm(path):
    d = open(path, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:
        while d[i:i + 1].isspace():
            i += 1
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        parts.append(d[i:j])
        i = j
    return d[i + 1:i + 1 + int(parts[1]) * int(parts[2]) * 3]


def px(img, x, y):
    o = (y * 256 + x) * 3
    return img[o], img[o + 1], img[o + 2]


def caught(path):
    img = load_ppm(path)
    n = 0
    for cx in ICON_SLOTS:
        red = sum(1 for y in range(352, 374) for x in range(cx - 11, cx + 11) if px(img, x, y) == BALL_RED)
        n += red >= 15
    return n


def frames(d):
    return [(int(os.path.basename(f)[6:12]), f) for f in sorted(glob.glob(os.path.join(d, 'frame_*.ppm')))]


def background(fs):
    imgs = [load_ppm(f) for _, f in fs[::10]]
    bg = bytearray(256 * 192 * 3)
    for i in range(0, len(bg), 3):
        bg[i:i + 3] = collections.Counter(bytes(im[i:i + 3]) for im in imgs).most_common(1)[0][0]
    return bytes(bg)


def blobs(img, bg):
    """Centre and size of what differs from the field's background."""
    fg = set()
    for y in range(20, 185):
        for x in range(8, 248):
            o = (y * 256 + x) * 3
            if img[o:o + 3] != bg[o:o + 3]:
                fg.add((x, y))
    out, seen = [], set()
    for p in fg:
        if p in seen:
            continue
        stack, comp = [p], []
        seen.add(p)
        while stack:
            q = stack.pop()
            comp.append(q)
            for dx in (-2, -1, 0, 1, 2):
                for dy in (-2, -1, 0, 1, 2):
                    r = (q[0] + dx, q[1] + dy)
                    if r in fg and r not in seen:
                        seen.add(r)
                        stack.append(r)
        if len(comp) >= 8:
            out.append((sum(c[0] for c in comp) // len(comp), sum(c[1] for c in comp) // len(comp), len(comp)))
    return out


# ---- schedules -----------------------------------------------------------------
def shot(release, X, Y):
    dx, dy = round((128 - X) / 2.43), round((201 - Y) / 2.48)
    x0, y0 = BALL
    f, out = release - 21, []
    for _ in range(3):
        out.append('%d:tap:%d:%d:1' % (f, x0, y0))
        f += 1
    for i in range(1, 13):
        out.append('%d:tap:%d:%d:1' % (f, x0 + dx * i // 12, y0 + dy * i // 12))
        f += 1
    for _ in range(6):
        out.append('%d:tap:%d:%d:1' % (f, x0 + dx, y0 + dy))
        f += 1
    return out


def selection():
    """Each icon dragged into the frame, then YES."""
    out, f = [], DRAG_FROM
    for (x, y) in ICONS:
        for _ in range(4):
            out.append('%d:tap:%d:%d:1' % (f, x, y))
            f += 1
        for i in range(1, 17):
            out.append('%d:tap:%d:%d:1' % (f, x + (FRAME_TARGET[0] - x) * i // 16, y + (FRAME_TARGET[1] - y) * i // 16))
            f += 1
        for _ in range(4):
            out.append('%d:tap:%d:%d:1' % (f, FRAME_TARGET[0], FRAME_TARGET[1]))
            f += 1
        f += 30
    out.append('%d:tap:%d:%d:4' % CHOSEN_YES)
    return out


def schedule(shots, finish=True):
    lines = selection()
    for s in shots:
        lines += shot(*s)
    if finish and shots:
        last = shots[-1][0]
        lines += ['%d:tap:128:100:4' % (last + 645), '%d:tap:215:59:4' % (last + 745)]
    return '\n'.join(lines) + '\n'


# ---- the lockstep pair from a checkpoint -----------------------------------------
class Pair:
    def __init__(self, d, fork_at, port, card, child_pre):
        self.d, self.n = d, 0
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        self.fa, self.fb = os.path.join(d, 'ctl_a'), os.path.join(d, 'ctl_b')
        os.mkfifo(self.fa)
        os.mkfifo(self.fb)
        shutil.copyfile(run_tests.PARENT_SAVE, os.path.join(d, 'A.sav'))
        run_tests.mint_card(card, os.path.join(d, 'B.sav'))
        pre = os.path.join(d, 'pre.sched')
        open(pre, 'w').write(child_pre)
        common = ['--frames', str(fork_at + 1)]
        a = [run_tests.PARENT_HEADLESS, 'black', run_tests.PARENT_ROM, '--save', os.path.join(d, 'A.sav'),
             '--schedule', run_tests.PARENT_SCHED, '--lockstep', '%d:%d' % (port, port + 1),
             '--net-id', run_tests.linkpair.IDS['a'], '--fork-at', '%d:%s' % (fork_at, self.fa)] + common
        b = [run_tests.HEADLESS, 'poketransfer', run_tests.CARDS[card]['rom'], '--save', os.path.join(d, 'B.sav'),
             '--schedule', pre, '--lockstep', '%d:%d' % (port + 1, port), '--net-id', run_tests.linkpair.IDS['b'],
             '--fork-at', '%d:%s' % (fork_at, self.fb)] + common
        self.pa = subprocess.Popen(a, stdout=open(os.path.join(d, 'base_a.log'), 'w'), stderr=subprocess.STDOUT)
        self.pb = subprocess.Popen(b, stdout=open(os.path.join(d, 'base_b.log'), 'w'), stderr=subprocess.STDOUT)
        self.wa, self.wb = open(self.fa, 'w'), open(self.fb, 'w')

    def job(self, child_sched, frames, every=0):
        """Both stations on from the checkpoint to `frames`; the child's dumps
        every `every` frames (0: the last frame only) in the job's b/."""
        self.n += 1
        jd = os.path.join(self.d, 'job')
        shutil.rmtree(jd, ignore_errors=True)
        os.makedirs(os.path.join(jd, 'b'))
        cs = os.path.join(jd, 'child.sched')
        open(cs, 'w').write(child_sched)
        for s in 'AB':
            shutil.copyfile(os.path.join(self.d, s + '.sav'), os.path.join(jd, s + '.sav'))
        la, lb = os.path.join(jd, 'a.log'), os.path.join(jd, 'b.log')
        self.wa.write('%s %d - 0 %s %s\n' % (run_tests.PARENT_SCHED, frames, os.path.join(jd, 'A.sav'), la))
        self.wb.write('%s %d %s %d %s %s\n' % (cs, frames, os.path.join(jd, 'b'), every or 1000000,
                                               os.path.join(jd, 'B.sav'), lb))
        self.wa.flush()
        self.wb.flush()
        t0 = time.time()
        while True:
            done = sum(1 for log in (la, lb) if os.path.exists(log)
                       and ('frames %d ' % frames in open(log, errors='replace').read()
                            or 'FAILED' in open(log, errors='replace').read()))
            if done == 2:
                return jd
            if time.time() - t0 > 600:
                raise RuntimeError('job timed out: ' + jd)
            time.sleep(0.2)

    def close(self):
        self.wa.close()
        self.wb.close()
        self.pa.wait(timeout=60)
        self.pb.wait(timeout=60)


# ---- the search ----------------------------------------------------------------
def play(card, work, port, log):
    shots, ncaught, flushed, t, bg = [], 0, [], GAME_FROM, None
    while ncaught < 6 and t < TIMER_END:
        p = Pair(os.path.join(work, 'pair'), t, port, card, schedule(shots, finish=False))
        hit = flushed_now = False
        try:
            jd = p.job(schedule(shots, finish=False), t + 600, every=2)
            fs = frames(os.path.join(jd, 'b'))
            if bg is None:
                bg = background(fs)
            seq = [(f, [b for b in blobs(load_ppm(path), bg) if 30 <= b[1] <= 180 and 20 <= b[0] <= 236
                        and b[2] <= 600]) for f, path in fs if t + 30 <= f][::2]

            def pick(lo, hi):
                out, seen = [], set()
                for f, bl in seq:
                    for x, y, n in bl:
                        k = (f // 12, x // 8, y // 8)
                        if lo <= n < hi and k not in seen:
                            seen.add(k)
                            out.append((f, x, y))
                return out
            tries = 0
            for (land, X, Y) in pick(60, 601):
                for off in (24, 30, 18, 36):
                    rel = land - off
                    if rel < t + 22:
                        continue
                    jd2 = p.job(schedule(shots + [(rel, X, Y)], finish=False), rel + 150)
                    n = caught(sorted(glob.glob(os.path.join(jd2, 'b', '*.ppm')))[-1])
                    tries += 1
                    log.write('  try %d at %d,%d -> %d\n' % (rel, X, Y, n))
                    if n > ncaught:
                        shots, ncaught, hit, flushed = shots + [(rel, X, Y)], n, True, []
                        break
                if hit or tries >= 25:
                    break
            if not hit:
                done = {(x // 16, y // 16) for (_, x, y) in flushed}
                flush = [c for c in pick(8, 60) + pick(60, 601)
                         if c[0] - 24 >= t + 22 and (c[1] // 16, c[2] // 16) not in done]
                if flush:
                    land, X, Y = flush[0]
                    shots.append((land - 24, X, Y))
                    flushed.append(shots[-1])
                    flushed_now = True
                    log.write('flush %s\n' % (shots[-1],))
        finally:
            p.close()
        t = shots[-1][0] + 100 if hit else shots[-1][0] + 40 if flushed_now else t + 400
        log.write('caught %d, next %d: %s\n' % (ncaught, t, shots))
        log.flush()
    return shots, ncaught


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('card', choices=sorted(run_tests.CARDS))
    ap.add_argument('--work', help='work directory (default: a temporary one, removed)')
    ap.add_argument('--port', type=int, default=43100)
    a = ap.parse_args()
    work = a.work or tempfile.mkdtemp(prefix='catchbot-')
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, 'bot.log'), 'a') as log:
        shots, n = play(a.card, work, a.port, log)
    if n < 6:
        print('caught %d of 6 before the timer; see %s' % (n, os.path.join(work, 'bot.log')))
        return 1
    out = os.path.join(HERE, 'pt-child-%s.sched' % a.card)
    with open(out, 'w') as f:
        f.write('# The child station with the %s card (tests/poketransfer/catchbot.py %s): the six\n'
                '# dragged into the frame and YES, then the capture minigame\'s shots\n'
                '# (released at %s), the message tapped on and YES to the transfer.\n'
                % (a.card, a.card, ', '.join(str(s[0]) for s in shots)))
        f.write(schedule(shots))
    print('caught 6 with %d shots; wrote %s' % (len(shots), out))
    if not a.work:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
