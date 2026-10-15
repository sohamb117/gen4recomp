#!/usr/bin/env python3
"""
gbamod: cook a GBA runtime content package's text patches into the data
patches the cores apply at boot (games/gba-common/pc/src/gba_mods.c).

    gbamod.py PKG [--cache .cache/gba]

PKG/text.txt holds one replacement per line ('#' starts a comment):

    GAME SYMBOL = "TEXT"

GAME is ruby, sapphire or emerald; SYMBOL a string in that game's ROM
(an ELF symbol of the decomp, e.g. gText_MainMenuNewGame); TEXT is encoded
with the decomp's charmap.txt and must fit the original (its 0xFF
terminator included). Writes PKG/<game>.ips for every game named: an IPS
patch of the new bytes at the symbol's ROM offset. The patch holds only the
package's own text; addresses come from the decomp's ELF in the cache.
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
DECOMP = {"ruby": ("pokeruby", "pokeruby.elf"), "sapphire": ("pokeruby", "pokesapphire.elf"),
          "emerald": ("pokeemerald", "pokeemerald.elf")}
LINE = re.compile(r'^(\w+)\s+(\w+)\s*=\s*"(.*)"$')


def charmap(path):
    """Single characters of charmap.txt: 'A' = BB."""
    m = {}
    for line in open(path, encoding="utf-8"):
        r = re.match(r"^'(.|\\')'\s*=\s*([0-9A-Fa-f]{2})\s*$", line.split("@")[0].strip())
        if r:
            m.setdefault(r.group(1).replace("\\'", "'"), int(r.group(2), 16))
    return m


def symbols(elf):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "syms.json")
        subprocess.run([sys.executable, os.path.join(HERE, "elfsyms.py"), elf, out], check=True)
        return json.load(open(out))["globals"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pkg")
    ap.add_argument("--cache", default=os.path.join(ROOT, ".cache", "gba"))
    a = ap.parse_args()
    edits = {}
    for n, line in enumerate(open(os.path.join(a.pkg, "text.txt"), encoding="utf-8"), 1):
        line = line.split("#")[0].strip()
        if not line:
            continue
        m = LINE.match(line)
        if not m or m.group(1) not in DECOMP:
            raise SystemExit(f"gbamod: text.txt:{n}: want GAME SYMBOL = \"TEXT\" (GAME: ruby, sapphire, emerald)")
        edits.setdefault(m.group(1), []).append((n, m.group(2), m.group(3)))
    for game, items in sorted(edits.items()):
        repo, elf = DECOMP[game]
        cmap = charmap(os.path.join(a.cache, repo, "charmap.txt"))
        syms = symbols(os.path.join(a.cache, repo, elf))
        ips = bytearray(b"PATCH")
        for n, sym, text in items:
            if sym not in syms:
                raise SystemExit(f"gbamod: text.txt:{n}: {game} has no symbol {sym}")
            addr, size, _ = syms[sym]
            try:
                data = bytes(cmap[c] for c in text) + b"\xff"
            except KeyError as e:
                raise SystemExit(f"gbamod: text.txt:{n}: {e} is not in {game}'s charmap")
            if len(data) > size:
                raise SystemExit(f"gbamod: text.txt:{n}: {len(data)} bytes do not fit {sym} ({size})")
            off = addr - 0x08000000
            if not 0 <= off < 1 << 24:
                raise SystemExit(f"gbamod: text.txt:{n}: {sym} is not in the first 16 MB of the ROM")
            ips += struct.pack(">I", off)[1:] + struct.pack(">H", len(data)) + data
        ips += b"EOF"
        out = os.path.join(a.pkg, game + ".ips")
        open(out, "wb").write(ips)
        print(f"{out}: {len(items)} string(s), {len(ips)} bytes")


if __name__ == "__main__":
    main()
