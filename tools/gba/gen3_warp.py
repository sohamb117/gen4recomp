#!/usr/bin/env python3
"""Point a Ruby/Sapphire/Emerald save's CONTINUE at another map (test fixtures).

    tools/gba/gen3_warp.py IN.sav OUT.sav --map GROUP.NUM --pos X,Y [--party NAME,NAME...]
                           [--sb1-bit OFFSET.BIT]...

Sets the continue-game warp (SaveBlock1 offset 0x0C: map group, map number,
warp -1, x, y) and its flag (SaveBlock2 offset 0x09, bit 0), so CONTINUE
warps into that map with its own objects, as the game does after a link
room or a secret base. --party replaces the party (count at SaveBlock1
0x234, Pokemon at 0x238, 100 bytes each) with level-5 Pokemon of
tools/gba/gen3_save.py's list, owned by the save's trainer: a trade needs
two. --sb1-bit sets one bit of SaveBlock1, e.g. an event flag (flags[] at
0x1220 in Ruby/Sapphire, 0x1270 in Emerald; FLAG_SYS_POKEDEX_GET, which
opens the Cable Club, is 0x1320.1 and 0x137C.1). Only the newest save slot
is changed; each
changed sector's checksum is recomputed over the same length it was
computed over (found by matching the stored one), so the tool needs no
per-game sizes. Layout: pret/pokeemerald and pokeruby include/global.h,
src/save.c (14-sector slots, footer id/checksum/signature/counter).
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen3_save  # noqa: E402

SECTOR = 0x1000
DATA = 3968
SIGNATURE = 0x08012025
SLOT_SECTORS = 14


def checksum(data, size):
    s = sum(struct.unpack_from('<%dI' % (size // 4), data))
    return ((s >> 16) + s) & 0xFFFF


def footer(img, sec):
    sid, cs, sig, counter = struct.unpack_from('<HHII', img, sec * SECTOR + 0xFF4)
    return sid, cs, sig, counter


def newest_slot(img):
    best = None
    for slot in (0, 1):
        counters = [footer(img, slot * SLOT_SECTORS + i)[3] for i in range(SLOT_SECTORS)
                    if footer(img, slot * SLOT_SECTORS + i)[2] == SIGNATURE]
        if len(counters) == SLOT_SECTORS and (best is None or counters[0] > best[1]):
            best = (slot, counters[0])
    if best is None:
        raise SystemExit('gen3_warp: no complete save slot')
    return best[0]


def find_sector(img, slot, sid):
    for i in range(SLOT_SECTORS):
        sec = slot * SLOT_SECTORS + i
        if footer(img, sec)[0] == sid:
            return sec
    raise SystemExit('gen3_warp: sector id %d missing' % sid)


def size_of(img, sec):
    """The length the stored checksum covers (multiple of 4, <= DATA)."""
    data = img[sec * SECTOR:sec * SECTOR + DATA]
    stored = footer(img, sec)[1]
    for size in range(DATA, 0, -4):
        if checksum(data, size) == stored:
            return size
    raise SystemExit('gen3_warp: sector %d has a bad checksum' % sec)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('--map', required=True, help='GROUP.NUM (include/constants/map_groups.h: num | group << 8)')
    ap.add_argument('--pos', required=True, help='X,Y in metatiles')
    ap.add_argument('--sb1-bit', action='append', default=[], help='OFFSET.BIT in SaveBlock1')
    ap.add_argument('--party', help='comma-separated: ' + ','.join(m[1] for m in gen3_save.BOX1))
    a = ap.parse_args()
    group, num = (int(v, 0) for v in a.map.split('.'))
    x, y = (int(v, 0) for v in a.pos.split(','))
    img = bytearray(open(a.src, 'rb').read())
    if len(img) < 2 * SLOT_SECTORS * SECTOR:
        raise SystemExit('gen3_warp: %s is not a 128 KiB flash save' % a.src)
    slot = newest_slot(img)
    sb2, sb1 = find_sector(img, slot, 0), find_sector(img, slot, 1)
    sizes = {sb2: size_of(img, sb2), sb1: size_of(img, sb1)}
    for spec in a.sb1_bit:
        off, bit = (int(v, 0) for v in spec.split('.'))
        sec = find_sector(img, slot, 1 + off // DATA)
        sizes.setdefault(sec, size_of(img, sec))
        img[sec * SECTOR + off % DATA] |= 1 << bit
    b1 = sb1 * SECTOR
    struct.pack_into('<bbbxhh', img, b1 + 0x0C, group, num, -1, x, y)
    img[sb2 * SECTOR + 0x09] |= 1
    if a.party:
        b2 = sb2 * SECTOR
        ot_name = gen3_save.decode_text(img[b2:b2 + 7])
        ot_id = struct.unpack_from('<I', img, b2 + 0x0A)[0]
        names = a.party.split(',')
        kinds = {m[1]: m for m in gen3_save.BOX1}
        party = bytearray(600)
        for i, n in enumerate(names):
            species, name, growth, moves = kinds[n]
            box = gen3_save.box_mon(species, name, growth, moves, 5, 0x1000 * (i + 1) + species, ot_id, ot_name,
                                    (20, 20, 20, 20, 20, 20))
            # status, level, mail, HP, max HP, Attack, Defense, Speed, Sp. Atk, Sp. Def
            party[100 * i:100 * i + 100] = box + struct.pack('<IBBHHHHHHH', 0, 5, 0xFF, 20, 20, 11, 11, 11, 11, 11)
        img[b1 + 0x234] = len(names)
        img[b1 + 0x238:b1 + 0x238 + 600] = party
    for sec, size in sizes.items():
        struct.pack_into('<H', img, sec * SECTOR + 0xFF6, checksum(img[sec * SECTOR:sec * SECTOR + DATA], size))
    open(a.out, 'wb').write(img)
    print('%s: slot %d, continue at map %d.%d (%d,%d)%s' % (a.out, slot, group, num, x, y,
                                                            ', party ' + a.party if a.party else ''))


if __name__ == '__main__':
    main()
