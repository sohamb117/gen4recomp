#!/usr/bin/env python3
"""Read the overlay statics' address table back out of the linked binary.

    3ds/tests/ov_addrs.py build/3ds/pokeplatinum.elf build/3ds/overlay_statics.c

3ds/gen_ov_addrs.py writes that table into the ELF after the link, because
this console has no .symtab at run time and pc_fs_overlay.c needs one thing
out of it: where each overlay's writable statics live, so it can put the first
load's bytes back on every later one. A wrong entry there does not fail the
link and does not fail a boot; it restores an overlay's statics from the
wrong address, which shows up as a corrupted Poketch app or a menu that
remembers the last visit, days later and nowhere near the cause.

So every entry is checked against `nm` over the same binary:

  placed     the offset, added to the table's own address, is the address nm
             gives that name, and the size is nm's size
  ambiguous  the name really is defined at that many addresses
  dropped    the name really is in no .symtab entry at all
  unpatched  none, ever; the sentinel means the post-link step did not run

What this cannot check is the relocation: a 3DSX is placed wherever the loader
likes, and offsets are used precisely so that does not matter. ov_addr_selftest()
in 3ds/src/3ds_sym.c is the half that runs on the console.
"""

import os
import re
import struct
import subprocess
import sys

ROW = re.compile(r'\{\s*(\d+),\s*"([^"]+)",\s*(\d+)\s*\}')
TABLE = "pc_ov_static_addr"
ENTRY = 8
NOT_AN_OFFSET = -1
UNPATCHED, DROPPED = 0, 1


def nm_symbols(nm, elf):
    out = subprocess.check_output([nm, "-S", "--defined-only", elf],
                                  text=True, errors="replace")
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            value, size, _typ, name = p
        elif len(p) == 3:
            value, _typ, name = p
            size = "0"
        else:
            continue
        try:
            syms.setdefault(name, []).append((int(value, 16), int(size, 16)))
        except ValueError:
            continue
    return syms


def read_table(elf, base, nbytes):
    with open(elf, "rb") as fh:
        eh = fh.read(52)
        phoff, = struct.unpack("<I", eh[28:32])
        phentsize, phnum = struct.unpack("<HH", eh[42:46])
        fh.seek(phoff)
        ph = fh.read(phentsize * phnum)
        for i in range(phnum):
            p = ph[i * phentsize:(i + 1) * phentsize]
            p_type, p_off, p_vaddr, _pa, p_filesz = struct.unpack("<IIIII",
                                                                  p[:20])
            if p_type == 1 and p_vaddr <= base \
                    and base + nbytes <= p_vaddr + p_filesz:
                fh.seek(p_off + (base - p_vaddr))
                return fh.read(nbytes)
    return None


def main(argv):
    if len(argv) != 3:
        sys.exit("usage: ov_addrs.py <elf> <overlay_statics.c>")
    elf, statics = argv[1], argv[2]
    nm = os.environ.get("NM", "arm-none-eabi-nm")

    rows = ROW.findall(open(statics).read())
    syms = nm_symbols(nm, elf)
    if len(syms.get(TABLE, [])) != 1:
        sys.exit("%s is not one symbol in the link" % TABLE)
    base, tsize = syms[TABLE][0]
    if tsize != len(rows) * ENTRY:
        sys.exit("%s is %d bytes and %d rows need %d"
                 % (TABLE, tsize, len(rows), len(rows) * ENTRY))

    blob = read_table(elf, base, tsize)
    if blob is None:
        sys.exit("%s at 0x%08X is in no loadable segment" % (TABLE, base))

    placed = ambiguous = dropped = 0
    bad = []
    for i, (_ov, name, _objsize) in enumerate(rows):
        off, size = struct.unpack("<iI", blob[i * ENTRY:(i + 1) * ENTRY])
        where = syms.get(name, [])
        addrs = sorted({a for a, _s in where})

        if off == NOT_AN_OFFSET and size == UNPATCHED:
            bad.append("%s: still the sentinel, gen_ov_addrs.py did not run"
                       % name)
        elif off == NOT_AN_OFFSET and size == DROPPED:
            dropped += 1
            if addrs:
                bad.append("%s: marked dropped but nm has %d address(es)"
                           % (name, len(addrs)))
        elif off == NOT_AN_OFFSET:
            ambiguous += 1
            if len(addrs) != size:
                bad.append("%s: marked %d addresses, nm has %d"
                           % (name, size, len(addrs)))
        else:
            placed += 1
            if len(addrs) != 1:
                bad.append("%s: placed but nm has %d addresses"
                           % (name, len(addrs)))
                continue
            if base + off != addrs[0]:
                bad.append("%s: table says 0x%08X, nm says 0x%08X"
                           % (name, base + off, addrs[0]))
            want = max(s for a, s in where if a == addrs[0])
            if size != want:
                bad.append("%s: table size %d, nm size %d" % (name, size, want))

    for line in bad[:8]:
        print("ov_addrs: %s" % line, file=sys.stderr)
    if len(bad) > 8:
        print("ov_addrs: ... and %d more" % (len(bad) - 8), file=sys.stderr)
    if bad:
        return 1

    print("ov_addrs: %d placed, %d ambiguous, %d dropped by the link, "
          "all agree with nm" % (placed, ambiguous, dropped))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
