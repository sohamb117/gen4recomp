#!/usr/bin/env python3
"""Poké Transfer (Black/White's Download Play child, docs/BW_PLAN.md "Poké Transfer") on np_headless.

  tests/poketransfer/run_tests.py [--keep DIR] [NAME...]

link  Two stations in lockstep. Station A is Black in the Poké Transfer Lab
      (pt-parent-black.sched: the scientist's questions, the save, overlay
      107's DS Download Play screen). Station B is the child station
      (`poketransfer`) with a Gen 4 card in slot 1: the Platinum ROM and a
      Platinum save with six Pokémon added to its boxes (np_save4
      add-box-mon). B's firmware client (games/ndsrec/pc/src/pc_pt_dlplay.c)
      finds A's beacon, connects, receives the child program over the link,
      checks every received segment against the image this build recompiled
      and boots it. The child reconnects to A as its own program and reads
      the card's save. The run must last 12000 frames on both stations with
      B's log showing each step. The card save must be unchanged, because
      nothing is transferred yet. Frames from 7000 on are kept in DIR/link/{a,b}
      for review. By frame 11200 the child shows its selection screen:
      BOX 1 with the six icons, read from the Platinum card, and "Please choose
      the six Pokémon to transfer".

Environment:
  NP_PT_HEADLESS   default build/core-poketransfer/np_headless
                   (games/ndsrec VER=poketransfer)
  NP_BW_HEADLESS, NP_BLACK_ROM   tests/link/linkpair.py's
  NP_PT_PARENT_SAVE   default build/e2e/black/34-postgame-poke-transfer-lab/end.sav
  NP_PLAT_ROM      default games/platinum/build/rom/pokeplatinum.us.nds
  NP_PT_CARD_SAVE  default build/e2e/platinum/25-pastoria-explosion/end.sav
  NP_SAVE4         tests/link/linkpair.py's
Missing pieces skip (exit 0, "SKIP").
"""
import argparse
import hashlib
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
CARD_ROM = linkpair.GAMES['platinum'][1]
CARD_SAVE = os.environ.get('NP_PT_CARD_SAVE', os.path.join(ROOT, 'build', 'e2e', 'platinum',
                                                           '25-pastoria-explosion', 'end.sav'))
FRAMES = 12000
# Six Pokémon put in the card save's boxes (np_save4 add-box-mon, level 10, Tackle):
# Bidoof, Starly, Shinx, Kricketot, Budew, Psyduck.
BOXED = (399, 396, 403, 401, 406, 54)

# What station B's log must show, in order: the firmware client's download,
# the check of what it received, then the booted child reading the card.
CHILD_STEPS = (
    'pc_pt_dlplay: Download Play, waiting for a parent',
    'pc_pt_dlplay: connected to the parent',
    'pc_pt_dlplay: download information received, starting the download',
    'pc_pt_dlplay: every block received',
    'pc_pt_dlplay: image verified; booting the child',
    'pc_card_rom: save: loaded from the runtime',
    'frames %d ' % FRAMES,
)


def sha1(path):
    with open(path, 'rb') as f:
        return hashlib.sha1(f.read()).hexdigest()


def link(work):
    d = os.path.join(work, 'link')
    shutil.rmtree(d, ignore_errors=True)
    for side in 'ab':
        os.makedirs(os.path.join(d, side))
    a_sav, b_sav = os.path.join(d, 'a.sav'), os.path.join(d, 'b.sav')
    shutil.copyfile(PARENT_SAVE, a_sav)
    shutil.copyfile(CARD_SAVE, b_sav)
    for species in BOXED:
        subprocess.run([linkpair.SAVE4, 'add-box-mon', b_sav, CARD_ROM, str(species), '10', '33', '-o', b_sav],
                       check=True, capture_output=True)
    card_before = sha1(b_sav)
    port = 42000 + (os.getpid() % 2000) * 2
    common = ['--frames', str(FRAMES), '--dump-every', '300', '--dump-from', '7000']
    with open(os.path.join(d, 'a.log'), 'w') as alog, open(os.path.join(d, 'b.log'), 'w') as blog:
        a = subprocess.Popen([PARENT_HEADLESS, 'black', PARENT_ROM, '--save', a_sav,
                              '--schedule', os.path.join(HERE, 'pt-parent-black.sched'),
                              '--lockstep', '%d:%d' % (port, port + 1), '--net-id', linkpair.IDS['a'],
                              '--dump', os.path.join(d, 'a')] + common, stdout=alog, stderr=subprocess.STDOUT)
        b = subprocess.Popen([HEADLESS, 'poketransfer', CARD_ROM, '--save', b_sav,
                              '--lockstep', '%d:%d' % (port + 1, port), '--net-id', linkpair.IDS['b'],
                              '--dump', os.path.join(d, 'b')] + common, stdout=blog, stderr=subprocess.STDOUT)
        try:
            rb = b.wait(timeout=1800)
            ra = a.wait(timeout=60)
        except subprocess.TimeoutExpired:
            a.kill()
            b.kill()
            return 'FAIL: timed out (link/a.log, link/b.log)'
    fails = []
    blines = open(os.path.join(d, 'b.log'), errors='replace').read()
    pos = 0
    for step in CHILD_STEPS:
        i = blines.find(step, pos)
        if i < 0:
            fails.append('station B never logged %r after the previous step' % step)
            break
        pos = i + len(step)
    if rb != 0:
        fails.append('station B exited %d' % rb)
    if ra != 0 or 'frames %d ' % FRAMES not in open(os.path.join(d, 'a.log'), errors='replace').read():
        fails.append('station A exited %d before frame %d' % (ra, FRAMES))
    if sha1(b_sav) != card_before:
        fails.append('the card save changed')
    return 'FAIL: ' + '; '.join(fails) + ' (link/*.log)' if fails else 'ok'


CASES = {'link': (link, (HEADLESS, PARENT_HEADLESS, PARENT_ROM, PARENT_SAVE, CARD_ROM, CARD_SAVE, linkpair.SAVE4))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('names', nargs='*')
    ap.add_argument('--keep', help='work directory to keep (default: a temporary one, removed)')
    a = ap.parse_args()
    work = a.keep or tempfile.mkdtemp(prefix='poketransfer-')
    os.makedirs(work, exist_ok=True)
    failed = 0
    for name in a.names or sorted(CASES):
        fn, needs = CASES[name]
        missing = [p for p in needs if not os.path.exists(p)]
        res = 'SKIP: %s missing' % missing[0] if missing else fn(work)
        print('%-8s %s' % (name, res), flush=True)
        failed += res.startswith('FAIL')
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
