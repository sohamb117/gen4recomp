#!/usr/bin/env python3
"""Poké Transfer (Black/White's Download Play child, docs/BW_PLAN.md "Poké Transfer") on np_headless.

  tests/poketransfer/run_tests.py [--keep DIR] [NAME...]

boot  The child station alone, a Gen 4 card in its slot 1 (the Platinum ROM): it
      boots as a Download Play child does. Its own ROM header is at
      HW_ROM_HEADER_BUF (SYACHI_MB, NTRJ), the inserted card's at
      HW_CARD_ROM_HEADER (CPUE), and MB_TYPE_MULTIBOOT at HW_WM_BOOT_BUF
      (TWL-SDK's shared page, 0x02FFF000 up). The child checks that word,
      shows "Loading..." while it reconnects to its parent, and with no
      parent gives up after its own 1800-frame timeout ("Unable to connect to
      the other DS system."). The run must last 2600 frames cleanly; frames
      are kept in DIR/boot for review.

Environment: NP_PT_HEADLESS (default build/core-poketransfer/np_headless,
games/ndsrec VER=poketransfer), NP_PT_CARD_ROM (default the Platinum ROM build,
games/platinum/build/rom/pokeplatinum.us.nds). Missing pieces skip (exit 0, "SKIP").
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
HEADLESS = os.environ.get('NP_PT_HEADLESS', os.path.join(ROOT, 'build', 'core-poketransfer', 'np_headless'))
CARD_ROM = os.environ.get('NP_PT_CARD_ROM', os.path.join(ROOT, 'games', 'platinum', 'build', 'rom',
                                                         'pokeplatinum.us.nds'))

# TWL-SDK 5's shared page (the 4.2 SDK's addresses 8 MB up, as the B/W cores have them).
HW_CARD_ROM_HEADER = 0x02FFFA80
HW_WM_BOOT_BUF = 0x02FFFC40
HW_ROM_HEADER_BUF = 0x02FFFE00


def watch(addr, length, frames=3):
    """The guest bytes at addr as np_headless --watch first reports them."""
    p = subprocess.run([HEADLESS, 'poketransfer', CARD_ROM, '--frames', str(frames),
                        '--watch', '0x%08X:%d@0' % (addr, length)], capture_output=True, text=True)
    for line in p.stderr.splitlines():
        if line.startswith('[watch]'):
            return bytes.fromhex(''.join(line.split(':', 1)[1].split()))
    return None


def boot(work):
    fails = []
    card = open(CARD_ROM, 'rb').read(16)
    for name, addr, want in (('own header', HW_ROM_HEADER_BUF, b'SYACHI_MB\0\0\0NTRJ'),
                             ('card header', HW_CARD_ROM_HEADER, card),
                             ('boot type', HW_WM_BOOT_BUF, b'\x02\x00')):
        got = watch(addr, len(want))
        if got != want:
            fails.append('%s at 0x%08X: %r, want %r' % (name, addr, got, want))
    d = os.path.join(work, 'boot')
    os.makedirs(d, exist_ok=True)
    p = subprocess.run([HEADLESS, 'poketransfer', CARD_ROM, '--frames', '2600', '--dump', d,
                        '--dump-every', '300', '--dump-from', '100'], capture_output=True, text=True)
    with open(os.path.join(work, 'boot.log'), 'w') as f:
        f.write(p.stdout + p.stderr)
    if p.returncode != 0 or 'frames 2600' not in p.stdout + p.stderr:
        fails.append('np_headless exited %d before frame 2600 (boot.log)' % p.returncode)
    return 'FAIL: ' + '; '.join(fails) if fails else 'ok'


CASES = {'boot': boot}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('names', nargs='*')
    ap.add_argument('--keep', help='work directory to keep (default: a temporary one, removed)')
    a = ap.parse_args()
    work = a.keep or tempfile.mkdtemp(prefix='poketransfer-')
    os.makedirs(work, exist_ok=True)
    failed = 0
    for name in a.names or sorted(CASES):
        missing = [p for p in (HEADLESS, CARD_ROM) if not os.path.exists(p)]
        res = 'SKIP: %s missing' % missing[0] if missing else CASES[name](work)
        print('%-8s %s' % (name, res), flush=True)
        failed += res.startswith('FAIL')
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
