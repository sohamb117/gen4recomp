#!/usr/bin/env python3
"""
elfsyms: the symbol table of a decomp's ELF (pokeemerald.elf, pokeruby.elf),
as the bridge needs it.

    elfsyms.py GAME.elf OUT.json

The ELF is the only source of addresses: every function and every object,
including statics, sits where the cartridge has it. Output (JSON):

    {"globals": {name: [addr, size, kind]},
     "locals":  {file: {name: [addr, size, kind]}},
     "sections": [[name, addr, size], ...]}

kind is "F" (function; addr is the pointer value code uses: odd for Thumb,
even for ARM), "O" (an object, or a NOTYPE label in a data section, which is
how the asm data files define theirs) or "L" (a NOTYPE label in code).
`file` is the FILE symbol's name (the object's basename, e.g. "main.o");
locals follow their FILE symbol in the table.
"""
import json
import struct
import sys

STT_NOTYPE, STT_OBJECT, STT_FUNC, STT_SECTION, STT_FILE = 0, 1, 2, 3, 4
STB_LOCAL = 0
SHF_ALLOC, SHF_EXECINSTR = 2, 4
SHN_ABS = 0xFFF1


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise SystemExit(f"elfsyms: {sys.argv[1]} is not a little-endian ELF32 file")
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    # (name, type, flags, addr, offset, size, link, info, addralign, entsize)
    secs = [struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]

    def cstr(off):
        return data[off:data.index(b"\0", off)].decode("latin-1")

    names = [cstr(secs[e_shstrndx][4] + s[0]) for s in secs]
    symtab = secs[names.index(".symtab")]
    strtab = secs[symtab[6]]
    mapped = {i for i, s in enumerate(secs) if s[2] & SHF_ALLOC and s[3] != 0}

    out_g, out_l, cur = {}, {}, ""
    for off in range(symtab[4], symtab[4] + symtab[5], 16):
        st_name, value, size, info, _other, shndx = struct.unpack_from("<IIIBBH", data, off)
        typ, bind = info & 15, info >> 4
        name = cstr(strtab[4] + st_name) if st_name else ""
        if typ == STT_FILE:
            cur = name
            continue
        if not name or typ not in (STT_NOTYPE, STT_OBJECT, STT_FUNC):
            continue
        if shndx == SHN_ABS:
            # `.set` constants the C reads as addresses (gNumMusicPlayers)
            if bind != STB_LOCAL:
                out_g[name] = [value, 0, "O"]
            continue
        if shndx not in mapped:
            continue
        if name[0] in "$.":
            continue  # mapping symbols ($a/$t/$d) and assembler temporaries
        if typ == STT_FUNC:
            kind = "F"
        elif typ == STT_NOTYPE and secs[shndx][2] & SHF_EXECINSTR:
            kind = "L"
        else:
            kind = "O"
        rec = [value, size, kind]
        if bind == STB_LOCAL:
            out_l.setdefault(cur, {})[name] = rec
        else:
            out_g[name] = rec
    sections = [[names[i], s[3], s[5]] for i, s in enumerate(secs) if i in mapped]
    with open(sys.argv[2], "w") as f:
        json.dump({"globals": out_g, "locals": out_l, "sections": sections}, f, separators=(",", ":"))


if __name__ == "__main__":
    main()
