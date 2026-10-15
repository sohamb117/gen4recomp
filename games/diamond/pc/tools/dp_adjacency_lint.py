#!/usr/bin/env python3
"""pc/tools/dp_adjacency_lint.py: assembly that reaches past a C-defined object.

    dp_adjacency_lint.py MAP ASM...

ASM: directories searched for *.s, or .s files (HG/SS pass the exact list
armrec translates, staged overlay copies included).

Exits 1 on a hit. The ROM's assembly (arm9/asm, the overlays, and the mwcc
`asm` bodies pc/tools/dp_extract_asm.py pulls out of C files) addresses data
the way the ROM's link laid it out: `ldr r7, =UNK_021C59F4; ldr r5, [r7, #4]`
reads the word 4 bytes after a 1-byte variable because, on the ROM, that is
where UNK_021C59F8 sits. Data the assembly itself defines keeps the ROM's
layout (armrec places it from the xMAP); data the decompiled C defines is
laid out by clang/wasm-ld, which owes the asm nothing. That was the
Continue crash (pc/patches/arm9/src/unk_0202F150.c.patch).

Inputs: the wasm link map (MAP: every C object's data symbol and size) and
the assembly. For each literal-pool load of a C-defined data symbol, the
straight-line code after it (to the first branch, label or overwrite of the
register) is scanned for a load/store or `add` through that register at an
offset >= the object's size. A linear scan under-approximates: a hit is
real, a miss proves nothing.
"""
import glob
import os
import re
import sys

SYM = re.compile(r"^\s*([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+\S*/obj/(?:game|pcgame|host|hgsrc)/\S+:"
                 r"\(\.(?:bss|data|rodata)\.(\w+)\)")
LD = re.compile(r"^\s*ldr\s+(r\d+),\s*=?\s*(\w+)\s*$")
LIT = re.compile(r"^\s*(\w+):\s*\.word\s+(\w+)")
MEM = re.compile(r"^\s*(ldr|str)(?:b|h|sb|sh)?\s+(r\d+),\s*\[(r\d+)(?:,\s*#(0x[0-9a-fA-F]+|\d+))?\]")
ADD = re.compile(r"^\s*add\s+(r\d+),\s*(?:(r\d+),\s*)?#(0x[0-9a-fA-F]+|\d+)")
WRITE = re.compile(r"^\s*(ldr\w*|mov|add|sub|lsl|lsr|asr|and|orr|eor|neg|mul)\s+(r\d+)\b")
STOP = re.compile(r"^\s*(bx|pop|b|bl|blx|thumb_func_end|arm_func_end)\b|^\w+:")


def main(argv):
    if len(argv) < 3:
        sys.exit("usage: dp_adjacency_lint.py MAP ASMDIR...")
    size = {}
    with open(argv[1], errors="replace") as f:
        for line in f:
            m = SYM.match(line)
            if m:
                size[m.group(3)] = int(m.group(2), 16)
    hits = set()
    for d in argv[2:]:
        for path in (glob.glob(os.path.join(d, "**", "*.s"), recursive=True) if os.path.isdir(d)
                     else [d] if d.endswith(".s") else []):
            lines = open(path, errors="replace").read().split("\n")
            lit = {m.group(1): m.group(2) for m in (LIT.match(l) for l in lines) if m}
            for i, raw in enumerate(lines):
                m = LD.match(raw.split(";")[0])
                if not m:
                    continue
                reg, sym = m.group(1), lit.get(m.group(2), m.group(2))
                if sym not in size:
                    continue
                for raw2 in lines[i + 1:i + 60]:
                    s = raw2.split(";")[0]
                    if STOP.match(s):
                        break
                    mm = MEM.match(s)
                    if mm and mm.group(3) == reg and int(mm.group(4) or "0", 0) >= size[sym]:
                        hits.add((sym, size[sym], os.path.relpath(path), s.strip()))
                    ad = ADD.match(s)
                    if ad and (ad.group(2) or ad.group(1)) == reg and int(ad.group(3), 0) >= size[sym]:
                        hits.add((sym, size[sym], os.path.relpath(path), s.strip()))
                    w = WRITE.match(s)
                    if w and w.group(2) == reg:
                        break
    for sym, n, path, insn in sorted(hits):
        print("dp_adjacency_lint: %s (%d bytes, defined in C) reached past its end by `%s` in %s"
              % (sym, n, insn, path))
    print("  ADJACENT %d assembly accesses past a C-defined object (%d C data symbols)" % (len(hits), len(size)))
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
