#!/usr/bin/env python3
"""Black/White feature checks that need the game's own field, on np_headless.

  tests/bwhgss/run_features.py [--game black|white|both] [--keep DIR] [NAME...]

mystery_gift  Two Wonder Cards stored the way the shell stores a received
              gift (np_save5 add-gift: a Rare Candy item card and a Pikachu
              Pokemon card), then the game's own delivery: CONTINUE in
              Striaton City, the Pokemon Center's Mystery Gift deliveryman
              (there only while a card waits) hands out both, and the game's
              save (the host quick save, -o) keeps them: the Rare Candy in the
              medicine pocket, Pikachu at the end of the party, both cards
              marked used. Black/White need no unlock for it: MYSTERY GIFT is
              on the main menu from the first save.

The start save is the e2e chain's milestone 07 end save in Striaton City
(build/e2e/<game>/07-striaton-dreamyard-monkey/end.sav, or
NP_BW_MG_BASE_<GAME>). The schedules were recorded from the e2e bots on
np_gp (PC_E2E=1, text_instant=1), so the runs pass the same.
Environment: NP_BW_HEADLESS (default build/core-bw/np_headless),
NP_BLACK_ROM, NP_WHITE_ROM, NP_SAVE5 (tests/link/linkpair.py's). Missing
pieces skip (exit 0, "SKIP").
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
SPECIES_PIKACHU = 25
MG_BASE = '07-striaton-dreamyard-monkey'
# The quick save's frame in each game's recorded walk (bw-mystery-gift-<game>.sched).
MG_SAVE_FRAME = {'black': 7000, 'white': 7100}


def title(text):
    """A card title: UTF-16LE, 0xFFFF-terminated, in its 0x4A bytes."""
    raw = text.encode('utf-16-le') + b'\xff\xff'
    return raw + bytes(0x4A - len(raw))


def wonder_card(card_type, card_id, body, name):
    """A Gen 5 Wonder Card (.pgf, 0xCC bytes): the gift in 0x00..0x5F, the
    title at 0x60, the card id at 0xB0, the type at 0xB3 (1 Pokemon, 2 item)
    and the flags at 0xB4 clear (not yet received from the deliveryman)."""
    card = bytearray(0xCC)
    card[:len(body)] = body
    card[0x60:0x60 + 0x4A] = title(name)
    struct.pack_into('<H', card, 0xB0, card_id)
    card[0xB3] = card_type
    return bytes(card)


def item_body(item):
    return struct.pack('<H', item)


def pokemon_body(species, level, move):
    """The Pokemon gift fields: ball, first move, species, no nickname and no
    OT (0xFFFF: the species name and the player), random nature and IVs."""
    b = bytearray(0x60)
    b[0x0E] = 4                                  # Poke Ball
    struct.pack_into('<H', b, 0x12, move)
    struct.pack_into('<H', b, 0x1A, species)
    b[0x1E:0x1E + 22] = b'\xff' * 22             # nickname
    b[0x34] = 0xFF                               # nature
    b[0x3C] = level                              # met level
    b[0x43:0x49] = b'\xff' * 6                   # IVs
    b[0x4A:0x4A + 16] = b'\xff' * 16             # OT name
    b[0x5B] = level
    return bytes(b)


def base_save(game):
    return os.environ.get('NP_BW_MG_BASE_' + game.upper(),
                          os.path.join(linkpair.ROOT, 'build', 'e2e', game, MG_BASE, 'end.sav'))


def dump(sav):
    return json.loads(subprocess.run([linkpair.SAVE5, 'dump', sav], capture_output=True, text=True,
                                     check=True).stdout)


def candies(d):
    return sum(i['qty'] for i in d['bag']['medicine'] if i['item'] == ITEM_RARE_CANDY)


def mystery_gift(game, work):
    sav = os.path.join(work, 'mg.sav')
    shutil.copyfile(base_save(game), sav)
    for name, card in (('item.pgf', wonder_card(2, 42, item_body(ITEM_RARE_CANDY), 'Rare Candy')),
                       ('pokemon.pgf', wonder_card(1, 43, pokemon_body(SPECIES_PIKACHU, 10, 84), 'Pikachu'))):
        path = os.path.join(work, name)
        with open(path, 'wb') as f:
            f.write(card)
        subprocess.run([linkpair.SAVE5, 'add-gift', sav, path], check=True, capture_output=True)
    before = dump(sav)
    if [c['used'] for c in before['mystery_gift']['cards']] != [False, False]:
        return 'FAIL: the start save holds cards %s, want two unreceived' % before['mystery_gift']['cards']
    p = subprocess.run(linkpair.core(game) + ['--frames', '7400', '--save', sav, '--schedule',
                                             os.path.join(HERE, 'bw-mystery-gift-%s.sched' % game),
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
    if candies(after) != candies(before) + 1:
        fails.append('%d Rare Candy after, %d before' % (candies(after), candies(before)))
    party = [m['species'] for m in after['party']]
    if party != [m['species'] for m in before['party']] + [SPECIES_PIKACHU]:
        fails.append('party %s after, %s before' % (party, [m['species'] for m in before['party']]))
    used = [c['used'] for c in after['mystery_gift']['cards']]
    if used != [True, True]:
        fails.append('cards used %s after delivery' % used)
    return 'FAIL: ' + '; '.join(fails) if fails else 'ok (party %s)' % party


CASES = {'mystery_gift': mystery_gift}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('names', nargs='*')
    ap.add_argument('--game', default='both', choices=('black', 'white', 'both'))
    ap.add_argument('--keep', help='work directory to keep (default: a temporary one, removed)')
    a = ap.parse_args()
    games = ('black', 'white') if a.game == 'both' else (a.game,)
    work = a.keep or tempfile.mkdtemp(prefix='bw-features-')
    failed = 0
    for name in a.names or sorted(CASES):
        for game in games:
            need = list(linkpair.GAMES[game]) + [linkpair.SAVE5, base_save(game)]
            missing = [p for p in need if not os.path.exists(p)]
            d = os.path.join(work, '%s-%s' % (name, game))
            os.makedirs(d, exist_ok=True)
            res = 'SKIP: %s missing' % missing[0] if missing else CASES[name](game, d)
            print('%-14s %-6s %s' % (name, game, res), flush=True)
            failed += res.startswith('FAIL')
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
