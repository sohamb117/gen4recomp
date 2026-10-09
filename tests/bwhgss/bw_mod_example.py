#!/usr/bin/env python3
"""tests/bwhgss/bw_mod_example.py ROM OUTDIR: a Black/White content package
for the ROM view (games/ndsrec/pc/src/pc_bw_romview.c), made from the
player's own cartridge so nothing of the game is committed.

OUTDIR/example_bw_menu/ is a package (mod.toml) claiming one NARC member,
narc/a/0/0/2/179: the system text bank of the title's main menu with its
first line, "CONTINUE", replaced by "CONTINUE (MOD)". Run it with
`np_headless ... --content OUTDIR -e PC_MODS=example_bw_menu`.

The bank format is Gen 5's (tests/e2e/tools/bw_script.py parse_bank): a
16-byte header (u16 sections 1, u16 count, u32 section size, u32 0, u32 the
section's offset), the section's u32 size, `count` entries {u32 offset,
u16 length in code units including the 0xFFFF end, u16 attributes}, then the
strings, each XOR-keyed from (0x7C89 + i * 0x2983) and rotated left 3 per
code unit.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'e2e', 'tools'))
import bw_script  # noqa: E402

sys.path.insert(0, os.path.join(HERE, '..', '..', 'games', 'platinum', 'pc'))
import modcook  # noqa: E402  (the cook step's digest, which the shell and pc_modfs check)

PKG = 'example_bw_menu'
BANK = 179
LINE = 0
TEXT = 'CONTINUE (MOD)'


def crypt(codes, i):
    key = (0x7C89 + i * 0x2983) & 0xFFFF
    out = []
    for c in codes:
        out.append(c ^ key)
        key = ((key << 3) | (key >> 13)) & 0xFFFF
    return out


def rebuild(bank, line, text):
    n = struct.unpack_from('<H', bank, 2)[0]
    sec = struct.unpack_from('<I', bank, 12)[0]
    strings, attrs = [], []
    for i in range(n):
        off, ln, attr = struct.unpack_from('<IHH', bank, sec + 4 + 8 * i)
        codes = list(struct.unpack_from('<%dH' % ln, bank, sec + off))
        strings.append(crypt(codes, i))  # decrypted, 0xFFFF last
        attrs.append(attr)
    strings[line] = [ord(ch) for ch in text] + [0xFFFF]
    table = 4 + 8 * n
    body, entries, off = b'', b'', table
    for i in range(n):
        enc = struct.pack('<%dH' % len(strings[i]), *crypt(strings[i], i))
        entries += struct.pack('<IHH', off, len(strings[i]), attrs[i])
        body += enc
        off += len(enc)
    size = table + len(body)
    size += (-size) % 4
    section = struct.pack('<I', size) + entries + body
    section += b'\0' * (size - len(section))
    return struct.pack('<HHIII', 1, n, size, 0, 16) + section


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rom = bw_script.Rom(sys.argv[1])
    bank = rom.narc('a/0/0/2')[BANK]
    new = rebuild(bank, LINE, TEXT)
    check = bw_script.parse_bank(new)
    want = bw_script.parse_bank(bank)
    want[LINE] = TEXT
    if check != want:
        sys.exit('bw_mod_example: the rebuilt bank does not read back')
    pkg = os.path.join(sys.argv[2], PKG)
    member = os.path.join(pkg, 'narc', 'a', '0', '0', '2')
    os.makedirs(member, exist_ok=True)
    with open(os.path.join(pkg, 'mod.toml'), 'w') as f:
        f.write('id = "%s"\nname = "Example: the main menu\'s CONTINUE (Black/White)"\n'
                'version = "1.0.0"\nauthors = ["nativeplat"]\n' % PKG)
    with open(os.path.join(member, str(BANK)), 'wb') as f:
        f.write(new)
    # Nothing to cook (no content/ or records/), but the shell installs and
    # pc_modfs boots only a cooked package: the digest of that empty input.
    os.makedirs(os.path.join(pkg, '.cooked'), exist_ok=True)
    with open(os.path.join(pkg, '.cooked', 'digest'), 'w') as f:
        f.write(modcook.digest_line(__import__('pathlib').Path(pkg)) + '\n')
    print(pkg)


if __name__ == '__main__':
    main()
