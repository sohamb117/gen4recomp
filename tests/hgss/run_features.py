#!/usr/bin/env python3
"""HeartGold/SoulSilver feature checks that need the game's own field, on np_headless.

  tests/hgss/run_features.py [--game heartgold|soulsilver|both] [--keep DIR] [NAME...]

mystery_gift  Two Wonder Cards stored the way the game stores a received gift
              (np_save4 add-gift: a Rare Candy item card and a Pokewalker
              course card for course 24), then the game's own delivery: CONTINUE
              in Violet City, the Poke Mart's Mystery Gift deliveryman hands out
              both, and the game's save (the host quick save, -o) keeps them:
              the Rare Candy in the medicine pocket, both gifts consumed, course
              24 among the Pokewalker's unlocked courses (np_save4 dump).

The start save is the e2e chain's milestone 04 end save in Violet City
(build/e2e/<game>/04-route30-31-violet/end.sav, or NP_HGSS_LINK_BASE_<GAME>,
as tests/link/linkpair.py's mint_hgss). The schedules were recorded from the
e2e bots on np_gp (PC_E2E=1, text_instant=1), so the runs pass the same.
Environment: NP_HGSS_HEADLESS (default build/core-hgss/np_headless),
NP_HG_ROM, NP_SS_ROM, NP_SAVE4. Missing pieces skip (exit 0, "SKIP").
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'link'))
import linkpair  # noqa: E402

ITEM_RARE_CANDY = 50
WALKER_COURSE = 24  # a special course only a gift unlocks
# The quick save's frame in each game's recorded walk (mystery-gift-<game>.sched).
MG_SAVE_FRAME = {'heartgold': 3057, 'soulsilver': 3058}


def wonder_card(mg_type, event_id, data):
    """A Wonder Card (.pcd, 856 bytes) as features/save4 save4_mg_build_card
    lays it out: the gift's type and data, an empty title and description
    (the text terminator 0xFFFF; the deliveryman shows neither), the card's
    event id, and the flags that it has a card and a stored gift."""
    card = bytearray(856)
    struct.pack_into('<H', card, 0, mg_type)
    card[4:4 + len(data)] = data
    struct.pack_into('<H', card, 0x104, 0xFFFF)            # title
    struct.pack_into('<I', card, 0x104 + 0x48, 0x7)       # validGames
    struct.pack_into('<H', card, 0x104 + 0x4C, event_id)  # the event id
    card[0x104 + 0x4E] = (1 << 2) | (1 << 3)               # hasWonderCard, savePgt
    struct.pack_into('<H', card, 0x154, 0xFFFF)            # description
    return bytes(card)


def dump(sav):
    return json.loads(subprocess.run([linkpair.SAVE4, 'dump', sav], capture_output=True, text=True,
                                     check=True).stdout)


def mystery_gift(game, work):
    sav = os.path.join(work, 'mg.sav')
    shutil.copyfile(linkpair.hgss_base_save(game), sav)
    for name, mg_type, event_id, data in (('item.pcd', 3, 42, struct.pack('<I', ITEM_RARE_CANDY)),
                                          ('walker.pcd', 14, 43, bytes([WALKER_COURSE]))):
        path = os.path.join(work, name)
        with open(path, 'wb') as f:
            f.write(wonder_card(mg_type, event_id, data))
        subprocess.run([linkpair.SAVE4, 'add-gift', sav, path], check=True, capture_output=True)
    before = dump(sav)
    if before['mystery_gift']['pgts'] != 2:
        return 'FAIL: the start save holds %d gifts, want 2' % before['mystery_gift']['pgts']
    p = subprocess.run(linkpair.core(game) + ['--frames', '3400', '--save', sav, '--schedule',
                                             os.path.join(HERE, 'mystery-gift-%s.sched' % game),
                                             '-e', 'PC_E2E=1', '-o', 'text_instant=1',
                                             '-o', '%d:quicksave_seq=1' % MG_SAVE_FRAME[game]],
                       capture_output=True, text=True)
    with open(os.path.join(work, 'mg.log'), 'w') as f:
        f.write(p.stdout + p.stderr)
    after = dump(sav)
    fails = []
    if p.returncode != 0:
        fails.append('np_headless exited %d' % p.returncode)
    if 'quicksave_result 0 -> 1' not in p.stdout + p.stderr:
        fails.append('the quick save did not happen')
    candies = sum(i['qty'] for i in after['bag']['medicine'] if i['item'] == ITEM_RARE_CANDY)
    had = sum(i['qty'] for i in before['bag']['medicine'] if i['item'] == ITEM_RARE_CANDY)
    if candies != had + 1:
        fails.append('%d Rare Candy after, %d before' % (candies, had))
    if after['mystery_gift']['pgts'] != 0:
        fails.append('%d gifts left undelivered' % after['mystery_gift']['pgts'])
    courses = after['pokewalker']['unlocked_courses']
    if WALKER_COURSE not in courses or WALKER_COURSE in before['pokewalker']['unlocked_courses']:
        fails.append('Pokewalker courses %s before, %s after' % (before['pokewalker']['unlocked_courses'],
                                                                 courses))
    return 'FAIL: ' + '; '.join(fails) if fails else 'ok (courses %s)' % courses


CASES = {'mystery_gift': mystery_gift}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('names', nargs='*')
    ap.add_argument('--game', default='both', choices=('heartgold', 'soulsilver', 'both'))
    ap.add_argument('--keep', help='work directory to keep (default: a temporary one, removed)')
    a = ap.parse_args()
    games = ('heartgold', 'soulsilver') if a.game == 'both' else (a.game,)
    work = a.keep or tempfile.mkdtemp(prefix='hgss-features-')
    failed = 0
    for name in a.names or sorted(CASES):
        for game in games:
            need = list(linkpair.GAMES[game]) + [linkpair.SAVE4, linkpair.hgss_base_save(game)]
            missing = [p for p in need if not os.path.exists(p)]
            d = os.path.join(work, '%s-%s' % (name, game))
            os.makedirs(d, exist_ok=True)
            res = 'SKIP: %s missing' % missing[0] if missing else CASES[name](game, d)
            print('%-14s %-11s %s' % (name, game, res), flush=True)
            failed += res.startswith('FAIL')
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
