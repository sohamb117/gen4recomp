#!/usr/bin/env python3
"""tests/bwhgss/hgss_mod_example.py ROM OUTDIR: a HeartGold/SoulSilver content
package for the ROM view (games/ndsrec/pc/src/pc_bw_romview.c, which the
HG/SS build links too), made from the player's own cartridge so nothing of
the game is committed.

OUTDIR/example_hgss_menu/ is a package (mod.toml) claiming one NARC member,
narc/a/0/2/7/442: the main menu's text bank (pokeheartgold's msg_0442,
src/application/main_menu/main_menu.c) with its first line, "CONTINUE",
replaced by "CONTINUE (MOD)". Run it with
`np_headless ... --content OUTDIR -e PC_MODS=example_hgss_menu`.

The bank format is Gen 4's (features/ndsdata/src/gen4_text.c): u16 count,
u16 seed; count entries {u32 offset, u32 length in code units}, each XOR-ed
with k | k << 16, k = (seed * 765 * (index + 1)) & 0xFFFF; the strings' u16
codes XOR-ed with a key starting at (index + 1) * 596947 (truncated to u16)
and rising by 18749 per code. The text is encoded with the decomp's
games/heartgold/charmap.txt.
"""
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / 'games' / 'platinum' / 'pc'))
import modcook  # noqa: E402  (the cook step's digest, which the shell and pc_modfs check)

PKG = 'example_hgss_menu'
NARC = 'a/0/2/7'
BANK = 442
LINE = 0
OLD = 'CONTINUE'
TEXT = 'CONTINUE (MOD)'
GAMES = {b'IPKE': 'HeartGold', b'IPGE': 'SoulSilver'}


def rom_file(raw, path):
    """A file of the cartridge by its path, through the FNT and FAT."""
    fnt, _, fat, _ = struct.unpack_from('<IIII', raw, 0x40)

    def walk(did, prefix):
        off, fid = struct.unpack_from('<IH', raw, fnt + 8 * (did & 0xFFF))
        p = fnt + off
        while raw[p]:
            n = raw[p] & 0x7F
            name = raw[p + 1:p + 1 + n].decode('latin1')
            sub = raw[p] & 0x80
            p += 1 + n
            if sub:
                found = walk(struct.unpack_from('<H', raw, p)[0], prefix + name + '/')
                if found is not None:
                    return found
                p += 2
            else:
                if prefix + name == path:
                    return fid
                fid += 1
        return None

    fid = walk(0xF000, '')
    if fid is None:
        sys.exit('hgss_mod_example: no %s on the cartridge' % path)
    a, b = struct.unpack_from('<II', raw, fat + 8 * fid)
    return raw[a:b]


def narc_member(narc, i):
    hsize = struct.unpack_from('<H', narc, 0xC)[0]
    assert narc[:4] == b'NARC' and narc[hsize:hsize + 4] == b'BTAF'
    btaf = struct.unpack_from('<I', narc, hsize + 4)[0]
    btnf = struct.unpack_from('<I', narc, hsize + btaf + 4)[0]
    gmif = hsize + btaf + btnf + 8
    a, b = struct.unpack_from('<II', narc, hsize + 12 + 8 * i)
    return narc[gmif + a:gmif + b]


def charmap():
    """Text -> code for single characters, from the decomp's charmap (the
    last code for a text wins, as msgenc's encoder does)."""
    enc = {}
    for line in (ROOT / 'games' / 'heartgold' / 'charmap.txt').read_text(encoding='utf-8').splitlines():
        line = line.split('//', 1)[0].lstrip(' \t')
        if '=' not in line:
            continue
        code, text = line.split('=', 1)
        try:
            enc[text] = int(code, 16)
        except ValueError:
            continue
    return enc


def string_key(i):
    return ((i + 1) * 596947) & 0xFFFF


def decode_bank(bank):
    count, seed = struct.unpack_from('<HH', bank, 0)
    out = []
    for i in range(count):
        k = (seed * 765 * (i + 1)) & 0xFFFF
        k |= k << 16
        off, n = (v ^ k for v in struct.unpack_from('<II', bank, 4 + 8 * i))
        key, codes = string_key(i), []
        for c in struct.unpack_from('<%dH' % n, bank, off):
            codes.append(c ^ key)
            key = (key + 18749) & 0xFFFF
        out.append(codes)
    return seed, out


def encode_bank(seed, strings):
    count = len(strings)
    table = 4 + 8 * count
    head, body, off = struct.pack('<HH', count, seed), b'', table
    for i, codes in enumerate(strings):
        k = (seed * 765 * (i + 1)) & 0xFFFF
        k |= k << 16
        head += struct.pack('<II', off ^ k, len(codes) ^ k)
        key, enc = string_key(i), []
        for c in codes:
            enc.append(c ^ key)
            key = (key + 18749) & 0xFFFF
        data = struct.pack('<%dH' % len(enc), *enc)
        body += data
        off += len(data)
    return head + body


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    raw = Path(sys.argv[1]).read_bytes()
    if raw[12:16] not in GAMES:
        sys.exit('hgss_mod_example: %s is not HeartGold (IPKE) or SoulSilver (IPGE)' % sys.argv[1])
    bank = narc_member(rom_file(raw, NARC), BANK)
    enc = charmap()
    seed, strings = decode_bank(bank)
    want = [enc[ch] for ch in OLD] + [0xFFFF]
    if strings[LINE] != want:
        sys.exit('hgss_mod_example: bank %d line %d is not "%s"' % (BANK, LINE, OLD))
    strings[LINE] = [enc[ch] for ch in TEXT] + [0xFFFF]
    new = encode_bank(seed, strings)
    if decode_bank(new) != (seed, strings):
        sys.exit('hgss_mod_example: the rebuilt bank does not read back')
    pkg = os.path.join(sys.argv[2], PKG)
    member = os.path.join(pkg, 'narc', *NARC.split('/'))
    os.makedirs(member, exist_ok=True)
    with open(os.path.join(pkg, 'mod.toml'), 'w') as f:
        f.write('id = "%s"\nname = "Example: the main menu\'s CONTINUE (HeartGold/SoulSilver)"\n'
                'version = "1.0.0"\nauthors = ["nativeplat"]\n' % PKG)
    with open(os.path.join(member, str(BANK)), 'wb') as f:
        f.write(new)
    # Nothing to cook (no content/ or records/), but the shell installs and
    # pc_modfs boots only a cooked package: the digest of that empty input.
    os.makedirs(os.path.join(pkg, '.cooked'), exist_ok=True)
    with open(os.path.join(pkg, '.cooked', 'digest'), 'w') as f:
        f.write(modcook.digest_line(Path(pkg)) + '\n')
    print(pkg)


if __name__ == '__main__':
    main()
