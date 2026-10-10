#!/usr/bin/env python3
"""Poké Transfer (Black/White's Download Play child, docs/BW_PLAN.md "Poké Transfer") on np_headless.

  tests/poketransfer/run_tests.py [--keep DIR] [NAME...]

Two stations in lockstep. Station A is Black in the Poké Transfer Lab
(pt-parent-black.sched: the scientist's questions, the save, then overlay 107's
DS Download Play screen). Station B is the child station (`poketransfer`)
with a Gen 4 card in slot 1: the card's ROM, and a copy of a save from the
e2e chain with six Pokémon added to box 1 (np_save4 add-box-mon). B's
firmware client (games/ndsrec/pc/src/pc_pt_dlplay.c) finds A's beacon,
connects, and receives the child program over the link. It checks every
received segment against the image this build recompiled and boots it.
The child reconnects to A as its own program and reads the card.

link               Platinum card, 12000 frames: B's log shows each step and
                   the card save is unchanged. By frame 11200 the child shows
                   BOX 1 with the six icons, read from the Platinum card.
transfer_platinum  The whole transfer (pt-child-platinum.sched, recorded by
transfer_heartgold catchbot.py): the six dragged into the frame, the capture
                   minigame's shots, YES to the transfer. The child writes the
                   card's save without the six, and Black saves them into its
                   boxes. Both saves must verify (np_save4 / np_save5) after
                   the games' own writes. The six personality values must be
                   in Black's boxes and in none of the card save's boxes.

Frames from 7000 on are kept in DIR/<case>/{a,b} for review.

Environment:
  NP_PT_HEADLESS       default build/core-poketransfer/np_headless (games/ndsrec VER=poketransfer)
  NP_BW_HEADLESS, NP_BLACK_ROM, NP_PLAT_ROM, NP_HG_ROM, NP_SAVE4, NP_SAVE5   tests/link/linkpair.py's
  NP_PT_PARENT_SAVE    default build/e2e/black/34-postgame-poke-transfer-lab/end.sav
  NP_PT_CARD_SAVE      Platinum: default build/e2e/platinum/25-pastoria-explosion/end.sav
  NP_PT_HG_CARD_SAVE   HeartGold: default build/e2e/heartgold/04-route30-31-violet/end.sav
The transfer schedules fit the saves they were recorded with (SAVE_SHA1);
other saves fail with that reason, and catchbot.py records new schedules.
Missing pieces skip (exit 0, "SKIP").
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'link'))
import linkpair  # noqa: E402

ROOT = linkpair.ROOT
HEADLESS = os.environ.get('NP_PT_HEADLESS', os.path.join(ROOT, 'build', 'core-poketransfer', 'np_headless'))
PARENT_HEADLESS, PARENT_ROM = linkpair.GAMES['black']
PARENT_SAVE = os.environ.get('NP_PT_PARENT_SAVE', os.path.join(ROOT, 'build', 'e2e', 'black',
                                                               '34-postgame-poke-transfer-lab', 'end.sav'))
PARENT_SCHED = os.path.join(HERE, 'pt-parent-black.sched')
# The Gen 4 cards: ROM, base save, the six added to box 1 (np_save4 add-box-mon,
# level 10, Tackle), the transfer's frames.
CARDS = {
    'platinum': dict(rom=linkpair.GAMES['platinum'][1],
                     save=os.environ.get('NP_PT_CARD_SAVE', os.path.join(ROOT, 'build', 'e2e', 'platinum',
                                                                         '25-pastoria-explosion', 'end.sav')),
                     boxed=(399, 396, 403, 401, 406, 54),  # Bidoof, Starly, Shinx, Kricketot, Budew, Psyduck
                     frames=17500),
    'heartgold': dict(rom=linkpair.GAMES['heartgold'][1],
                      save=os.environ.get('NP_PT_HG_CARD_SAVE', os.path.join(ROOT, 'build', 'e2e', 'heartgold',
                                                                             '04-route30-31-violet', 'end.sav')),
                      boxed=(161, 163, 16, 19, 10, 13),  # Sentret, Hoothoot, Pidgey, Rattata, Caterpie, Weedle
                      frames=18200),
}
# The saves pt-child-<card>.sched were recorded with (catchbot.py).
SAVE_SHA1 = {
    'parent': '6863b0020b6577d67cdec7f4afeaa16b1d2ff295',
    'platinum': 'c10324e609965d3e74d5a2bdf792bcc143205f9b',
    'heartgold': 'bf15a2c3fc4a6526cdf6fa6199a0562ab2e452b5',
}
LINK_FRAMES = 12000

# What station B's log must show, in order: the firmware client's download,
# the check of what it received, then the booted child reading the card.
CHILD_STEPS = (
    'pc_pt_dlplay: Download Play, waiting for a parent',
    'pc_pt_dlplay: connected to the parent',
    'pc_pt_dlplay: download information received, starting the download',
    'pc_pt_dlplay: every block received',
    'pc_pt_dlplay: image verified; booting the child',
    'pc_card_rom: save: loaded from the runtime',
)


def sha1(path):
    with open(path, 'rb') as f:
        return hashlib.sha1(f.read()).hexdigest()


def mint_card(card, out):
    """The card's base save with the six added to box 1."""
    c = CARDS[card]
    shutil.copyfile(c['save'], out)
    for species in c['boxed']:
        subprocess.run([linkpair.SAVE4, 'add-box-mon', out, c['rom'], str(species), '10', '33', '-o', out],
                       check=True, capture_output=True)


def run_pair(work, name, card, frames, child_sched=None):
    """Both stations from frame 0; returns (dir, A's log, B's log, A's rc, B's rc)."""
    d = os.path.join(work, name)
    shutil.rmtree(d, ignore_errors=True)
    for side in 'ab':
        os.makedirs(os.path.join(d, side))
    shutil.copyfile(PARENT_SAVE, os.path.join(d, 'a.sav'))
    mint_card(card, os.path.join(d, 'b.sav'))
    shutil.copyfile(os.path.join(d, 'b.sav'), os.path.join(d, 'b-before.sav'))
    port = 42000 + (os.getpid() % 2000) * 2
    common = ['--frames', str(frames), '--dump-every', '300', '--dump-from', '7000']
    with open(os.path.join(d, 'a.log'), 'w') as alog, open(os.path.join(d, 'b.log'), 'w') as blog:
        a = subprocess.Popen([PARENT_HEADLESS, 'black', PARENT_ROM, '--save', os.path.join(d, 'a.sav'),
                              '--schedule', PARENT_SCHED, '--lockstep', '%d:%d' % (port, port + 1),
                              '--net-id', linkpair.IDS['a'], '--dump', os.path.join(d, 'a')] + common,
                             stdout=alog, stderr=subprocess.STDOUT)
        b = subprocess.Popen([HEADLESS, 'poketransfer', CARDS[card]['rom'], '--save', os.path.join(d, 'b.sav'),
                              '--lockstep', '%d:%d' % (port + 1, port), '--net-id', linkpair.IDS['b'],
                              '--dump', os.path.join(d, 'b')] + common
                             + (['--schedule', child_sched] if child_sched else []),
                             stdout=blog, stderr=subprocess.STDOUT)
        try:
            rb = b.wait(timeout=1800)
            ra = a.wait(timeout=60)
        except subprocess.TimeoutExpired:
            a.kill()
            b.kill()
            ra = rb = -1
    read = lambda s: open(os.path.join(d, s + '.log'), errors='replace').read()  # noqa: E731
    return d, read('a'), read('b'), ra, rb


def check_run(alog, blog, ra, rb, frames):
    fails, pos = [], 0
    for step in CHILD_STEPS + ('frames %d ' % frames,):
        i = blog.find(step, pos)
        if i < 0:
            fails.append('station B never logged %r after the previous step' % step)
            break
        pos = i + len(step)
    if rb != 0:
        fails.append('station B exited %d' % rb)
    if ra != 0 or 'frames %d ' % frames not in alog:
        fails.append('station A exited %d before frame %d' % (ra, frames))
    return fails


def dump(tool, rom, sav):
    p = subprocess.run([tool, 'dump', rom, sav], capture_output=True, text=True)
    return json.loads(p.stdout) if p.returncode == 0 else None


def verified(tool, sav):
    return subprocess.run([tool, 'verify', sav], capture_output=True).returncode == 0


def box_pids(d):
    """{pid: species} of every boxed Pokémon in a np_save4 / np_save5 dump."""
    return {int(m['pid'], 16): m['species'] for b in d['boxes'] for m in b.get('mons', [])}


def link(work):
    d, alog, blog, ra, rb = run_pair(work, 'link', 'platinum', LINK_FRAMES)
    fails = check_run(alog, blog, ra, rb, LINK_FRAMES)
    if sha1(os.path.join(d, 'b.sav')) != sha1(os.path.join(d, 'b-before.sav')):
        fails.append('the card save changed')
    return 'FAIL: ' + '; '.join(fails) + ' (link/*.log)' if fails else 'ok'


def transfer(work, card):
    c = CARDS[card]
    for what, path in (('parent', PARENT_SAVE), (card, c['save'])):
        if sha1(path) != SAVE_SHA1[what]:
            return ('FAIL: %s is not the save pt-child-%s.sched was recorded with (sha1 %s); record it again with '
                    'tests/poketransfer/catchbot.py %s' % (path, card, SAVE_SHA1[what], card))
    name = 'transfer_' + card
    d, alog, blog, ra, rb = run_pair(work, name, card, c['frames'], os.path.join(HERE, 'pt-child-%s.sched' % card))
    fails = check_run(alog, blog, ra, rb, c['frames'])
    before = dump(linkpair.SAVE4, c['rom'], os.path.join(d, 'b-before.sav'))
    six = {pid: sp for pid, sp in box_pids(before).items() if sp in c['boxed']}
    if len(six) != 6:
        fails.append('the minted card save holds %d of the six' % len(six))
    for tool, sav, base, who in ((linkpair.SAVE4, 'b.sav', os.path.join(d, 'b-before.sav'), 'the card'),
                                 (linkpair.SAVE5, 'a.sav', PARENT_SAVE, 'Black')):
        if sha1(os.path.join(d, sav)) == sha1(base):
            fails.append('%s save was not written' % who)
        if not verified(tool, os.path.join(d, sav)):
            fails.append('%s save does not verify' % who)
    after4 = dump(linkpair.SAVE4, c['rom'], os.path.join(d, 'b.sav'))
    after5 = dump(linkpair.SAVE5, PARENT_ROM, os.path.join(d, 'a.sav'))
    if after4 is None or after5 is None:
        fails.append('a save does not dump')
    else:
        left = sorted(set(six) & set(box_pids(after4)))
        if left:
            fails.append('still in the card save: %s' % ', '.join('%08X' % p for p in left))
        bw = box_pids(after5)
        missing = sorted(p for p, sp in six.items() if bw.get(p) != sp)
        if missing:
            fails.append('not in Black\'s boxes: %s' % ', '.join('%08X' % p for p in missing))
    return 'FAIL: ' + '; '.join(fails) + ' (%s/*.log)' % name if fails else 'ok'


def needs(card):
    return (HEADLESS, PARENT_HEADLESS, PARENT_ROM, PARENT_SAVE, CARDS[card]['rom'], CARDS[card]['save'],
            linkpair.SAVE4, linkpair.SAVE5)


CASES = {
    'link': (link, needs('platinum')),
    'transfer_platinum': (lambda w: transfer(w, 'platinum'), needs('platinum')),
    'transfer_heartgold': (lambda w: transfer(w, 'heartgold'), needs('heartgold')),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('names', nargs='*')
    ap.add_argument('--keep', help='work directory to keep (default: a temporary one, removed)')
    a = ap.parse_args()
    work = a.keep or tempfile.mkdtemp(prefix='poketransfer-')
    os.makedirs(work, exist_ok=True)
    failed = 0
    for name in a.names or list(CASES):
        fn, need = CASES[name]
        missing = [p for p in need if not os.path.exists(p)]
        res = 'SKIP: %s missing' % missing[0] if missing else fn(work)
        print('%-19s %s' % (name, res), flush=True)
        failed += res.startswith('FAIL')
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
