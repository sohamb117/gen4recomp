#!/usr/bin/env python3
"""
The ARM9 link of HeartGold/SoulSilver as main.lsf states it, for the wasm
build (pc/Makefile.wasm).

pokeheartgold keeps every ARM9 source flat (src/, asm/, lib/*/asm, with
subdirectories by subsystem, not by overlay); which overlay an object
belongs to is said only by main.lsf, whose `Overlay <name>` blocks are the
ROM's overlay ids in order (OVY_0 is 0, `field` is 1, ...). The ROM links
exactly the objects listed there, so that list, not a directory walk, is
what the wasm build compiles.

    hg_lsf.py main.lsf c          C sources of linked objects: `path ovl`
    hg_lsf.py main.lsf s          assembly of linked objects: `path ovl`
    hg_lsf.py main.lsf ovmap      gen_overlay_statics.py's map for the
                                  overlay C objects: `hg/<flat>.c ovl`

`ovl` is the overlay id, or `-` for the static module and its autoloads
(ITCM, DTCM). Paths are relative to the game root and keep their suffix.

The ds_protect overlay's objects (lib/dsprot/*_encrypted.o, *_encoded.o,
*_decoder.o) are made by the ROM build from lib/dsprot/src/*.c, encrypted
or encoded for the cartridge; there is no source of that name. The wasm
build compiles the plain C (lib/dsprot/src/<module>.c), once per module,
which is the code the decrypters reveal at run time on hardware. The
generated coretests decoder has no C module and is dropped.
"""
import os
import re
import sys

BLOCK_RE = re.compile(r"^\s*(Static|Autoload|Overlay)\s+(\S+)")
OBJECT_RE = re.compile(r"^\s*Object\s+(\S+)\.o\b")
DSPROT_RE = re.compile(r"^lib/dsprot/(\w+?)(?:_encrypted|_decrypter_encoded|"
                       r"_decrypter_decoder|_encoded|_decoder)?$")


def objects(lsf):
    """[(path without suffix, overlay id or None)] in link order, unique."""
    root = os.path.dirname(os.path.abspath(lsf))
    out = []
    seen = set()
    ovl = None
    next_ovl = 0
    with open(lsf) as fh:
        for line in fh:
            line = line.split("#", 1)[0]
            m = BLOCK_RE.match(line)
            if m:
                if m.group(1) == "Overlay":
                    ovl = next_ovl
                    next_ovl += 1
                else:
                    ovl = None
                continue
            m = OBJECT_RE.match(line)
            if not m:
                continue
            path = m.group(1)
            d = DSPROT_RE.match(path)
            if d:
                path = "lib/dsprot/src/" + d.group(1)
                if not os.path.exists(os.path.join(root, path + ".c")):
                    continue
            if path in seen:
                continue
            seen.add(path)
            out.append((path, ovl))
    return root, out


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in ("c", "s", "ovmap"):
        sys.exit("usage: hg_lsf.py main.lsf c|s|ovmap")
    root, objs = objects(sys.argv[1])
    mode = sys.argv[2]
    missing = []
    for path, ovl in objs:
        tag = "-" if ovl is None else str(ovl)
        c = os.path.exists(os.path.join(root, path + ".c"))
        s = os.path.exists(os.path.join(root, path + ".s"))
        if not c and not s:
            missing.append(path)
        elif mode == "c" and c:
            print("%s.c %s" % (path, tag))
        elif mode == "s" and s and not c:
            print("%s.s %s" % (path, tag))
        elif mode == "ovmap" and c and ovl is not None:
            print("hg/%s.c %d" % (path.replace("/", "_"), ovl))
    if missing:
        sys.exit("hg_lsf: no source for %s" % ", ".join(missing))


if __name__ == "__main__":
    main()
