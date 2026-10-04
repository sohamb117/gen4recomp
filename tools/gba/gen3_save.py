#!/usr/bin/env python3
"""Generate (and verify) a Pokemon Emerald save: the 128 KiB flash image.

    tools/gba/gen3_save.py OUT.sav [--trainer NAME] [--tid N] [--sid N] [--female]
        Writes a save with both slots valid (save counters 0 and 1, the
        second rotated by one sector) and eight Pokemon in PC box 1.
    tools/gba/gen3_save.py --verify SAV [--rom pokeemerald.gba]
        Checks every sector footer (signature, checksum over the chunk size,
        all 14 ids per slot), decrypts every boxed Pokemon (substruct order,
        XOR key, checksum) and prints what it finds. --rom cross-checks the
        SaveBlock1/2 sizes against the game header at 0x100 of a built
        pret/pokeemerald ROM. Exit 0 = valid.

Layout from pret/pokeemerald: include/save.h and src/save.c (14-sector slots,
SaveSector footer id/checksum/signature 0x08012025/counter, the sector of
chunk `id` in a slot is (id + rotation) % 14), include/global.h (SaveBlock2,
PokemonStorage), include/pokemon.h and src/pokemon.c (BoxPokemon, substructs
ordered by personality % 24, encrypted with otId ^ personality, checksum the
u16 sum of the decrypted substructs). The DS side that reads it is Platinum's
src/main_menu/ov97_02235D18.c (sector scan) and gba_migrator.c, which wants
at least six Pokemon in the boxes; nothing else in the GBA save is checked.
No ROM data is used or embedded: species/move ids and names are constants.
"""
import argparse
import struct
import sys

SECTOR_SIZE = 0x1000
SECTOR_DATA = 3968
SIGNATURE = 0x08012025
SECTORS_PER_SLOT = 14
FLASH_SIZE = 0x20000

# Emerald (pokeemerald's static asserts / linker report).
SAVEBLOCK2_SIZE = 3884    # 0xF2C
SAVEBLOCK1_SIZE = 15752   # 0x3D88
STORAGE_SIZE = 33744      # 0x83D0: currentBox + pad, 14x30 BoxPokemon, names, wallpapers
BOX_MON_SIZE = 80
BOXES, BOX_SLOTS = 14, 30
BOX_NAME_LEN = 9

LANGUAGE_ENGLISH = 2
VERSION_EMERALD = 3
BALL_POKE = 4
MAPSEC_ROUTE_101 = 16

# personality % 24 -> position of substruct type (growth, attacks, evs, misc)
SUBSTRUCT_POS = [
    (0, 1, 2, 3), (0, 1, 3, 2), (0, 2, 1, 3), (0, 3, 1, 2), (0, 2, 3, 1), (0, 3, 2, 1),
    (1, 0, 2, 3), (1, 0, 3, 2), (2, 0, 1, 3), (3, 0, 1, 2), (2, 0, 3, 1), (3, 0, 2, 1),
    (1, 2, 0, 3), (1, 3, 0, 2), (2, 1, 0, 3), (3, 1, 0, 2), (2, 3, 0, 1), (3, 2, 0, 1),
    (1, 2, 3, 0), (1, 3, 2, 0), (2, 1, 3, 0), (3, 1, 2, 0), (2, 3, 1, 0), (3, 2, 1, 0),
]

# Growth curves (exp needed for level n).
def exp_medium_fast(n): return n ** 3
def exp_medium_slow(n): return (6 * n ** 3) // 5 - 15 * n ** 2 + 100 * n - 140
def exp_slow(n): return (5 * n ** 3) // 4

# (Gen 3 internal species id, name, growth, moves [(id, pp)], ability bit)
BOX1 = [
    (277, "TREECKO", exp_medium_slow, [(1, 35), (43, 30), (71, 25)]),     # Pound, Leer, Absorb
    (280, "TORCHIC", exp_medium_slow, [(10, 35), (45, 40), (52, 25)]),    # Scratch, Growl, Ember
    (283, "MUDKIP", exp_medium_slow, [(33, 35), (45, 40), (55, 25)]),     # Tackle, Growl, Water Gun
    (25, "PIKACHU", exp_medium_fast, [(84, 30), (45, 40), (39, 30), (98, 30)]),
    (133, "EEVEE", exp_medium_fast, [(33, 35), (39, 30), (28, 15)]),      # Tackle, Tail Whip, Sand-Attack
    (288, "ZIGZAGOON", exp_medium_fast, [(33, 35), (45, 40), (39, 30)]),
    (1, "BULBASAUR", exp_medium_slow, [(33, 35), (45, 40), (22, 25)]),    # ... Vine Whip
    (392, "RALTS", exp_slow, [(45, 40), (93, 25)]),                       # Growl, Confusion
]


def encode_text(s, length):
    """pokeemerald charmap.txt: space 00, 0-9 A1.., A-Z BB.., a-z D5..; EOS FF."""
    out = bytearray()
    for ch in s:
        if ch == " ":
            out.append(0x00)
        elif "0" <= ch <= "9":
            out.append(0xA1 + ord(ch) - ord("0"))
        elif "A" <= ch <= "Z":
            out.append(0xBB + ord(ch) - ord("A"))
        elif "a" <= ch <= "z":
            out.append(0xD5 + ord(ch) - ord("a"))
        else:
            raise ValueError(f"unsupported character {ch!r}")
    if len(out) > length:
        raise ValueError(f"{s!r} longer than {length}")
    return bytes(out + b"\xFF" * (length - len(out)))


def decode_text(b):
    s = ""
    for c in b:
        if c == 0xFF:
            break
        if c == 0x00:
            s += " "
        elif 0xA1 <= c <= 0xAA:
            s += chr(ord("0") + c - 0xA1)
        elif 0xBB <= c <= 0xD4:
            s += chr(ord("A") + c - 0xBB)
        elif 0xD5 <= c <= 0xEE:
            s += chr(ord("a") + c - 0xD5)
        else:
            s += "?"
    return s


def sector_checksum(data, size):
    words = struct.unpack_from(f"<{size // 4}I", data)
    total = sum(words) & 0xFFFFFFFF
    return ((total >> 16) + (total & 0xFFFF)) & 0xFFFF


def box_mon(species, name, growth, moves, level, personality, ot_id, ot_name, ivs):
    growth_s = struct.pack("<HHIBBH", species, 0, growth(level), 0, 70, 0)
    move_ids = [m for m, _ in moves] + [0] * (4 - len(moves))
    move_pp = [p for _, p in moves] + [0] * (4 - len(moves))
    attacks_s = struct.pack("<4H4B", *move_ids, *move_pp)
    evs_s = bytes(12)
    origin = (5 & 0x7F) | (VERSION_EMERALD << 7) | (BALL_POKE << 11)  # metLevel 5, otGender 0
    iv_word = 0
    for i, v in enumerate(ivs):
        iv_word |= (v & 31) << (5 * i)
    # isEgg (bit 30) and abilityNum (bit 31) clear: first ability, not an egg
    misc_s = struct.pack("<BBHII", 0, MAPSEC_ROUTE_101, origin, iv_word, 0)
    subs = [growth_s, attacks_s, evs_s, misc_s]

    plain = bytearray(48)
    pos = SUBSTRUCT_POS[personality % 24]
    for kind in range(4):
        plain[pos[kind] * 12:pos[kind] * 12 + 12] = subs[kind]
    checksum = sum(struct.unpack("<24H", plain)) & 0xFFFF
    key = ot_id ^ personality
    secure = struct.pack("<12I", *[w ^ key for w in struct.unpack("<12I", plain)])

    flags = 0x02  # hasSpecies
    head = struct.pack("<II", personality, ot_id) + encode_text(name, 10)
    head += bytes([LANGUAGE_ENGLISH, flags]) + encode_text(ot_name, 7)
    head += struct.pack("<BHH", 0, checksum, 0)
    mon = head + secure
    assert len(mon) == BOX_MON_SIZE
    return mon


def decode_box_mon(mon):
    personality, ot_id = struct.unpack_from("<II", mon, 0)
    flags = mon[19]
    checksum = struct.unpack_from("<H", mon, 28)[0]
    key = ot_id ^ personality
    plain = struct.pack("<12I", *[w ^ key for w in struct.unpack_from("<12I", mon, 32)])
    pos = SUBSTRUCT_POS[personality % 24]
    growth = plain[pos[0] * 12:pos[0] * 12 + 12]
    attacks = plain[pos[1] * 12:pos[1] * 12 + 12]
    misc = plain[pos[3] * 12:pos[3] * 12 + 12]
    species, item, exp = struct.unpack_from("<HHI", growth)
    moves = [m for m in struct.unpack_from("<4H", attacks) if m]
    iv_word = struct.unpack_from("<I", misc, 4)[0]
    return {
        "personality": personality,
        "species": species,
        "item": item,
        "exp": exp,
        "moves": moves,
        "nickname": decode_text(mon[8:18]),
        "ot": decode_text(mon[20:27]),
        "has_species": bool(flags & 0x02),
        "is_egg": bool(flags & 0x04) or bool(iv_word >> 30 & 1),
        "checksum_ok": checksum == (sum(struct.unpack("<24H", plain)) & 0xFFFF),
    }


def build(trainer, tid, sid, female):
    otid = tid | (sid << 16)
    sb2 = bytearray(SAVEBLOCK2_SIZE)
    sb2[0:8] = encode_text(trainer, 8)
    sb2[8] = 1 if female else 0
    struct.pack_into("<I", sb2, 0x0A, otid)
    struct.pack_into("<HBB", sb2, 0x0E, 42, 0, 0)  # play time 42:00

    sb1 = bytearray(SAVEBLOCK1_SIZE)

    storage = bytearray(STORAGE_SIZE)
    storage[0] = 0  # currentBox
    for slot, (species, name, growth, moves) in enumerate(BOX1):
        personality = 0x1F2E3D4C * (slot + 1) & 0xFFFFFFFF
        ivs = [(slot * 7 + i * 3) % 32 for i in range(6)]
        mon = box_mon(species, name, growth, moves, 10 + slot, personality, otid, trainer, ivs)
        storage[4 + slot * BOX_MON_SIZE:4 + (slot + 1) * BOX_MON_SIZE] = mon
    names = 4 + BOXES * BOX_SLOTS * BOX_MON_SIZE
    for b in range(BOXES):
        storage[names + b * BOX_NAME_LEN:names + (b + 1) * BOX_NAME_LEN] = encode_text(f"BOX{b + 1}", BOX_NAME_LEN)
    wallpapers = names + BOXES * BOX_NAME_LEN
    for b in range(BOXES):
        storage[wallpapers + b] = b % 4

    chunks = [bytes(sb2)]
    chunks += [bytes(sb1[i * SECTOR_DATA:(i + 1) * SECTOR_DATA]) for i in range(4)]
    chunks += [bytes(storage[i * SECTOR_DATA:(i + 1) * SECTOR_DATA]) for i in range(9)]

    flash = bytearray(b"\xFF" * FLASH_SIZE)
    for slot, (counter, rotation) in enumerate([(0, 0), (1, 1)]):
        for cid, data in enumerate(chunks):
            sector = bytearray(SECTOR_SIZE)
            sector[:len(data)] = data
            struct.pack_into("<HHII", sector, 0xFF4, cid, sector_checksum(sector, len(data)), SIGNATURE, counter)
            phys = slot * SECTORS_PER_SLOT + (cid + rotation) % SECTORS_PER_SLOT
            flash[phys * SECTOR_SIZE:(phys + 1) * SECTOR_SIZE] = sector
    return bytes(flash)


def chunk_size(cid, sb1_size, sb2_size):
    if cid == 0:
        return sb2_size
    if cid == 4:
        return sb1_size - 3 * SECTOR_DATA
    if cid == 13:
        return STORAGE_SIZE - 8 * SECTOR_DATA
    return SECTOR_DATA


def verify(path, rom):
    sb2_size, sb1_size = SAVEBLOCK2_SIZE, SAVEBLOCK1_SIZE
    if rom:
        with open(rom, "rb") as f:
            f.seek(0x100 + 0x88)
            rom_sb2, rom_sb1 = struct.unpack("<II", f.read(8))
            f.seek(0xAC)
            code = f.read(4).decode("ascii", "replace")
        print(f"rom {code}: SaveBlock2 {rom_sb2} B, SaveBlock1 {rom_sb1} B")
        if (rom_sb2, rom_sb1) != (sb2_size, sb1_size):
            print("FAIL: ROM header sizes differ from the generator's layout")
            return 1
    with open(path, "rb") as f:
        flash = f.read()
    if len(flash) != FLASH_SIZE:
        print(f"FAIL: {len(flash)} bytes, want {FLASH_SIZE}")
        return 1
    best = None
    for slot in range(2):
        found = {}
        for i in range(SECTORS_PER_SLOT):
            sec = flash[(slot * SECTORS_PER_SLOT + i) * SECTOR_SIZE:][:SECTOR_SIZE]
            cid, cks, sig, counter = struct.unpack_from("<HHII", sec, 0xFF4)
            if sig != SIGNATURE or cid >= SECTORS_PER_SLOT:
                continue
            if cks != sector_checksum(sec, chunk_size(cid, sb1_size, sb2_size)):
                print(f"FAIL: slot {slot} sector {i} (chunk {cid}) checksum")
                return 1
            found[cid] = (sec, counter)
        if not found:
            print(f"slot {slot}: empty")
            continue
        if len(found) != SECTORS_PER_SLOT:
            print(f"FAIL: slot {slot} has chunks {sorted(found)}")
            return 1
        counter = found[0][1]
        print(f"slot {slot}: valid, counter {counter}")
        if best is None or counter > best[0]:
            best = (counter, found)
    if best is None:
        print("FAIL: no valid slot")
        return 1
    found = best[1]
    sb2 = found[0][0]
    storage = b"".join(found[c][0][:SECTOR_DATA] for c in range(5, 14))[:STORAGE_SIZE]
    tid, sid = struct.unpack_from("<HH", sb2, 0x0A)
    print(f"trainer {decode_text(sb2[0:8])} TID {tid} SID {sid}")
    count = 0
    for i in range(BOXES * BOX_SLOTS):
        mon = storage[4 + i * BOX_MON_SIZE:4 + (i + 1) * BOX_MON_SIZE]
        if not any(mon):
            continue
        m = decode_box_mon(mon)
        state = "" if m["has_species"] else "  (no species: migrated away)"
        print(f"  box {i // BOX_SLOTS + 1} slot {i % BOX_SLOTS + 1}: {m['nickname']} species {m['species']} "
              f"exp {m['exp']} moves {m['moves']} OT {m['ot']}{state}")
        if not m["checksum_ok"]:
            print("FAIL: Pokemon checksum")
            return 1
        if m["has_species"] and not m["is_egg"]:
            count += 1
    print(f"{count} Pokemon in the boxes" + ("" if count >= 6 else " (Pal Park offers migration from six)"))
    print("ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--trainer", default="MAY")
    ap.add_argument("--tid", type=int, default=12345)
    ap.add_argument("--sid", type=int, default=54321)
    ap.add_argument("--female", action="store_true")
    ap.add_argument("--verify", metavar="SAV")
    ap.add_argument("--rom")
    a = ap.parse_args()
    if a.verify:
        return verify(a.verify, a.rom)
    if not a.out:
        ap.error("OUT.sav or --verify SAV")
    with open(a.out, "wb") as f:
        f.write(build(a.trainer, a.tid & 0xFFFF, a.sid & 0xFFFF, a.female))
    return 0


if __name__ == "__main__":
    sys.exit(main())
