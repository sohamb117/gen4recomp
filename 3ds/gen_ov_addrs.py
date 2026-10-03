#!/usr/bin/env python3
"""3ds/gen_ov_addrs.py: write the overlay statics' addresses into the link.

pc_fs_overlay.c has to know where each overlay's writable statics live, so it
can put the first load's bytes back on every later one, on hardware the ROM
copy is that reset, and here they are host globals the loader initialised once.
On the desktop it asks the running binary's own .symtab through pc_sym.c. A
3DSX carries code, data and a relocation table and nothing else, so there is no
.symtab to ask and the addresses have to already be in the image.

They cannot be COMPILED in: they are link-time values, and a table holding them
would move the very things it names. So pc/gen_overlay_statics.py --addr-table
emits the table at its final size filled with a sentinel, and this writes the
real values into the linked ELF afterwards. Nothing moves, because only the
contents change, and there is no second link. The numbers cannot go stale
against the binary they describe, because they are inside it.

Every number is a difference, never an address. A 3DSX is relocated when it is
loaded, so an absolute address written here would be wrong by however far the
loader moved the image. Each entry is signed and relative to the table itself,
which is one more object in the same image, so the difference survives any
slide, and the table's own address is something the C can take at runtime
without help.

    3ds/gen_ov_addrs.py <elf> <overlay_statics.c>
"""

import os
import re
import struct
import subprocess
import sys

ROW = re.compile(r'\{\s*(\d+),\s*"([^"]+)",\s*(\d+)\s*\}')

TABLE = "pc_ov_static_addr"
ENTRY = 8               # int32 offset from the table, uint32 size

# An entry whose offset is not an offset says which of three things happened,
# in the size field. 3ds/src/3ds_sym.c reads them back the same way.
#
#   0    nobody ran this script. The generator writes it and it is the one
#        state that must never be mistaken for an answer.
#   1    the link kept no copy: --gc-sections comes in through 3dsx.specs and
#        discards a tentative definition nothing reads. Nothing to reset.
#   >=2  defined at that many addresses, so it cannot be assigned without a
#        file of origin, gcc's v0.2-style names for unnamed file-local
#        objects collide across translation units.
NOT_AN_OFFSET = -1
UNPATCHED, DROPPED = 0, 1


def nm_symbols(nm, elf):
    """name -> [(value, size)] over every defined symbol in the link."""
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


def file_offset(elf, vaddr, length):
    """Where `vaddr` lives in the file, through the program headers."""
    with open(elf, "rb") as fh:
        eh = fh.read(52)
        phoff, = struct.unpack("<I", eh[28:32])
        phentsize, phnum = struct.unpack("<HH", eh[42:46])
        fh.seek(phoff)
        ph = fh.read(phentsize * phnum)
    for i in range(phnum):
        p = ph[i * phentsize:(i + 1) * phentsize]
        p_type, p_offset, p_vaddr, _paddr, p_filesz = struct.unpack("<IIIII",
                                                                    p[:20])
        if p_type != 1:                 # PT_LOAD
            continue
        if p_vaddr <= vaddr and vaddr + length <= p_vaddr + p_filesz:
            return p_offset + (vaddr - p_vaddr)
    return None


def main(argv):
    if len(argv) != 3:
        sys.exit("usage: gen_ov_addrs.py <elf> <overlay_statics.c>")
    elf, statics = argv[1], argv[2]
    nm = os.environ.get("NM", "arm-none-eabi-nm")

    rows = ROW.findall(open(statics).read())
    if not rows:
        sys.exit("gen_ov_addrs: no rows in %s" % statics)

    syms = nm_symbols(nm, elf)
    if len(syms.get(TABLE, [])) != 1:
        sys.exit("gen_ov_addrs: %s is not one symbol in %s, was the table "
                 "generated with --addr-table?" % (TABLE, elf))
    base, tsize = syms[TABLE][0]
    if tsize != len(rows) * ENTRY:
        sys.exit("gen_ov_addrs: %s is %d bytes and %d rows need %d; the "
                 "generated table and the link disagree"
                 % (TABLE, tsize, len(rows), len(rows) * ENTRY))

    blob = bytearray()
    placed = ambiguous = 0
    dropped = []
    for _ov, name, _size in rows:
        where = syms.get(name, [])
        addrs = sorted({a for a, _s in where})
        if not addrs:
            blob += struct.pack("<iI", NOT_AN_OFFSET, DROPPED)
            dropped.append(name)
            continue
        if len(addrs) > 1:
            blob += struct.pack("<iI", NOT_AN_OFFSET, len(addrs))
            ambiguous += 1
            continue
        size = max(s for a, s in where if a == addrs[0])
        blob += struct.pack("<iI", addrs[0] - base, size)
        placed += 1

    off = file_offset(elf, base, len(blob))
    if off is None:
        sys.exit("gen_ov_addrs: %s at 0x%08X is in no loadable segment, a "
                 "zero initialiser would put it in .bss, which has no bytes "
                 "in the file" % (TABLE, base))

    with open(elf, "r+b") as fh:
        fh.seek(off)
        fh.write(blob)
        fh.flush()
        fh.seek(off)
        if fh.read(len(blob)) != bytes(blob):
            sys.exit("gen_ov_addrs: the table did not read back")

    print("  OVADDR  %d of %d overlay statics placed, %d ambiguous, "
          "%d dropped by the link%s"
          % (placed, len(rows), ambiguous, len(dropped),
             " (" + ", ".join(dropped) + ")" if dropped else ""))


if __name__ == "__main__":
    main(sys.argv)
