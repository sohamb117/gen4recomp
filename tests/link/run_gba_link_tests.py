#!/usr/bin/env python3
"""Two-station Gen 3 link cable tests (Ruby, Sapphire, Emerald) on the real cores.

    tests/link/run_gba_link_tests.py [--keep DIR] [NAME...]

Each scenario mints both saves, runs an np_headless pair over the link
cable (games/gba-common/pc/src/gba_link.c) from frame 0 with schedules/ and
checks the outcome:

  rs_trade      Ruby (A) and Sapphire (B) trade in the Trade Center: A's
                TORCHIC for B's MUDKIP. Both games save after the trade; each
                save's party must hold the other side's Pokemon.
  re_trade      the same between Ruby (A) and Emerald (B), whose Direct Corner
                attendant opens on TRADE CENTER.
  rs_battle     Ruby (A) and Sapphire (B) battle in the Colosseum (A's
                TORCHIC beats B's MUDKIP and RALTS): both stations enter and
                leave the battle at the same frames (in_battle 0 -> 1 -> 0)
                and show the same Win/Loss screen.

Saves: tests/rse/littleroot.sched to the house and a quick save (the
game's own save, as tests/rse/first_battle.sh does), then
tools/gba/gen3_warp.py points CONTINUE at Oldale's Pokemon Center 2F with a
party of two and the Pokedex flag (the Cable Club's condition). No save is
committed. The lower station id (A) is the parent. PC_GBA_LINK_WAIT=1 plugs
both cables at frame 0's end and the bus is exact (a transfer waits for the
partner's word at the same emulated line), so the outcome and its frame
numbers repeat whatever the host's timing. While linked, np_headless runs
at 60 Hz, so a trade costs about 80 s and the battle about 3.5 min.

np_headless comes from $NP_RSE_CORE (default build/core-rse), the ROMs
from .cache/gba (tools/rom_build.sh). Skips (exit 0, "SKIP") without them.
"""
import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'gba'))
import gen3_save  # noqa: E402
import gen3_warp  # noqa: E402

HEADLESS = os.path.join(os.environ.get('NP_RSE_CORE', os.path.join(ROOT, 'build', 'core-rse')), 'np_headless')
ROMS = {
    'ruby': os.path.join(ROOT, '.cache/gba/pokeruby/pokeruby.gba'),
    'sapphire': os.path.join(ROOT, '.cache/gba/pokeruby/pokesapphire.gba'),
    'emerald': os.path.join(ROOT, '.cache/gba/pokeemerald/pokeemerald.gba'),
}
IDS = {'a': '0x111111', 'b': '0x222222'}
# FLAG_SYS_POKEDEX_GET in SaveBlock1 (flags[] + 0x800 / 8)
DEX_FLAG = {'ruby': '0x1320.1', 'sapphire': '0x1320.1', 'emerald': '0x137C.1'}
TORCHIC, MUDKIP, ZIGZAGOON, RALTS = 280, 283, 288, 392  # Gen 3 internal species ids
PARTY = {'a': 'TORCHIC,ZIGZAGOON', 'b': 'MUDKIP,RALTS'}
TRADE_POS = {'ruby': '7,4', 'sapphire': '7,4', 'emerald': '10,4'}  # below the trade / Direct Corner attendant
COLOSSEUM_POS = '4,4'  # below the R/S Colosseum attendant
TRADED = {'a': [MUDKIP, ZIGZAGOON], 'b': [TORCHIC, RALTS]}

SCENARIOS = [
    dict(name='rs_trade', games=('ruby', 'sapphire'), pos=TRADE_POS, frames=6900,
         scheds=('gba-trade-a.sched', 'gba-trade-b.sched'), party=TRADED),
    dict(name='re_trade', games=('ruby', 'emerald'), pos=TRADE_POS, frames=6900,
         scheds=('gba-trade-a.sched', 'gba-trade-b.sched'), party=TRADED),
    # The bus is exact, so the battle's frames are fixed: in at 3605, out at
    # 14830 on both stations, and the Win/Loss screen at 15002 is one picture.
    dict(name='rs_battle', games=('ruby', 'sapphire'), pos={'ruby': COLOSSEUM_POS, 'sapphire': COLOSSEUM_POS},
         frames=15100, scheds=('gba-battle-a.sched', 'gba-battle-b.sched'),
         battle=[(3605, 1), (14830, 0)], result_frame=15002),
]


def house_save(game, work):
    """The game's own save in the Littleroot house (cached per work dir)."""
    path = os.path.join(work, game + '-house.sav')
    if not os.path.exists(path):
        log = os.path.join(work, game + '-house.log')
        with open(log, 'w') as f:
            rc = subprocess.run([HEADLESS, game, ROMS[game], '--save', path, '--frames', '11000', '--schedule',
                                 os.path.join(ROOT, 'tests/rse/littleroot.sched'), '-o', '10000:quicksave_seq=1'],
                                stdout=f, stderr=subprocess.STDOUT).returncode
        if rc != 0 or 'quicksave_result=1 map_id=256' not in open(log).read():
            raise RuntimeError('%s: no house save (%s)' % (game, log))
    return path


def mint(game, pos, party, out, work):
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools/gba/gen3_warp.py'), house_save(game, work), out,
                    '--map', '2.3', '--pos', pos, '--party', party, '--sb1-bit', DEX_FLAG[game]],
                   check=True, stdout=subprocess.DEVNULL)


def party(path):
    """Species of the party in the save's newest slot."""
    img = open(path, 'rb').read()
    b1 = gen3_warp.find_sector(img, gen3_warp.newest_slot(img), 1) * gen3_warp.SECTOR
    out = []
    for i in range(img[b1 + 0x234]):
        mon = gen3_save.decode_box_mon(img[b1 + 0x238 + 100 * i:b1 + 0x238 + 100 * i + 80])
        out.append(mon['species'] if mon['checksum_ok'] else None)
    return out


def battle_frames(log):
    """The frames of the in_battle changes in a station's log."""
    return [(int(f), int(v)) for f, v in re.findall(r'\[status\] frame (\d+): in_battle \d+ -> (\d+)', log)]


def run(sc, work):
    d = os.path.join(work, sc['name'])
    os.makedirs(d, exist_ok=True)
    games = dict(zip('ab', sc['games']))
    saves = {}
    for side in 'ab':
        saves[side] = os.path.join(d, side.upper() + '.sav')
        mint(games[side], sc['pos'][games[side]], PARTY[side], saves[side], work)
    port = random.randrange(20000, 40000)
    procs = {}
    for i, side in enumerate('ab'):
        cmd = [HEADLESS, games[side], ROMS[games[side]], '--frames', str(sc['frames']), '--save', saves[side],
               '--schedule', os.path.join(HERE, 'schedules', sc['scheds'][i]), '--net', str(port + 2 * i),
               '--net-id', IDS[side], '--net-wait', '30', '-e', 'PC_GBA_LINK_WAIT=1']
        if 'result_frame' in sc:
            os.makedirs(os.path.join(d, side), exist_ok=True)
            cmd += ['--dump', os.path.join(d, side), '--dump-from', str(sc['result_frame']), '--dump-every', '100000']
        procs[side] = subprocess.Popen(cmd, stdout=open(os.path.join(d, side + '.log'), 'w'),
                                       stderr=subprocess.STDOUT)
    rcs = {side: procs[side].wait() for side in 'ab'}
    fails = ['%s exited %d' % (s, rc) for s, rc in rcs.items() if rc != 0]
    logs = {side: open(os.path.join(d, side + '.log')).read() for side in 'ab'}
    for side in 'ab':
        if 'link_active 0 -> 1' not in logs[side]:
            fails.append('%s: the link never came up' % side)
    for side, want in sc.get('party', {}).items():
        got = party(saves[side])
        if got != want:
            fails.append('%s party %s, want %s' % (side, got, want))
    if 'battle' in sc:
        for side in 'ab':
            seen = battle_frames(logs[side])
            if seen != sc['battle']:
                fails.append('%s: in_battle changes (frame, value) %s, want %s' % (side, seen, sc['battle']))
        name = 'frame_%06d.ppm' % (sc['result_frame'] + 1)
        shots = [os.path.join(d, side, name) for side in 'ab']
        if not all(os.path.exists(p) for p in shots):
            fails.append('no %s dump' % name)
        else:
            pics = [open(p, 'rb').read() for p in shots]
            if pics[0] != pics[1]:
                fails.append('the Win/Loss screen (%s) differs between the stations' % name)
            elif not any(pics[0].split(b'\n', 3)[3]):
                fails.append('%s is black' % name)
    return 'FAIL: ' + '; '.join(fails) if fails else 'ok'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('names', nargs='*')
    ap.add_argument('--keep', help='work directory to keep (default: a temporary one, removed)')
    a = ap.parse_args()
    chosen = [sc for sc in SCENARIOS if not a.names or sc['name'] in a.names]
    if not chosen:
        print('run_gba_link_tests: no scenario named %s' % ' '.join(a.names))
        return 2
    work = a.keep or tempfile.mkdtemp(prefix='gbalinktests-')
    os.makedirs(work, exist_ok=True)
    failed = 0
    for sc in chosen:
        missing = next((p for p in [HEADLESS] + [ROMS[g] for g in sc['games']] if not os.path.exists(p)), None)
        res = 'SKIP: %s missing' % missing if missing else run(sc, work)
        print('%-12s %-17s %s' % (sc['name'], ':'.join(sc['games']), res), flush=True)
        failed += res.startswith('FAIL')
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
