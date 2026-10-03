#!/usr/bin/env python3
"""3ds/tests/snd_reg_pin.py: the ARM7 sound driver's registers, all of them.

The SDK spells an ARM9 register as HW_REG_BASE plus an offset, so one macro
moved all 329 of them. The decompiled sound driver has no such base: each of
its registers is a literal address, and 3ds/include/arm7snd/registers.h
rewrites every one against the slab's I/O row. A list that long, written out
by hand, is a list that will one day be missing a register somebody added,
and a missed one still compiles and still stores somewhere.

So the list is derived here from the two headers the shadow shadows, and this
fails if the shadow's set is not exactly the derived one.

    3ds/tests/snd_reg_pin.py

What counts as one. A macro whose body dereferences an address literal in the
DS I/O page: `(*(REGType16v *)0x4000500)`. A macro built out of another,
reg_SOUNDxCNT_KEYON is reg_SOUNDxCNT_STAT with a bit set, carries no address
and needs nothing, because the macro it is built from expands at the use site.

REG_DMA0SAD_ADDR is the one address the shadow deliberately leaves alone: it
is a bare constant, not a dereference, and nothing in the driver uses it. What
it would MEAN to move it depends on what reads it; a DMA model wants the
guest address, a load wants the host one, so this fails if a use appears
rather than guessing now.

And the objects, when a build has produced them. The header check says the
macros were rewritten; the disassembly says no I/O literal survived into the
code, which is the claim that actually matters. Skipped, loudly, with no
objects to read.
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SOURCES = [
    "pc/arm7snd/include/registers.h",
    "pc/arm7snd/include/nitro/registers_shared.h",
]
SHADOW = "3ds/include/arm7snd/registers.h"
RAWDIR = "build/3ds/arm7snd-raw"

# The DS I/O page. Everything the driver touches is in the first 64 KB of it.
IO_LO = 0x04000000
IO_HI = 0x04100000

DEFINE = re.compile(r"^\s*#\s*define\s+(reg_[A-Za-z0-9_]+)")
DEREF = re.compile(r"\*\s*\(\s*REGType\d+v\s*\*\s*\)\s*\(?\s*(0[xX][0-9A-Fa-f]+)")


def defined_with_literal(path):
    """reg_* macros whose body dereferences an address in the I/O page."""
    found = {}
    for line in open(os.path.join(ROOT, path)):
        m = DEFINE.match(line)
        if not m:
            continue
        d = DEREF.search(line)
        if not d:
            continue
        addr = int(d.group(1), 16)
        if IO_LO <= addr < IO_HI:
            found[m.group(1)] = addr
    return found


def main():
    want = {}
    for path in SOURCES:
        want.update(defined_with_literal(path))

    shadow = os.path.join(ROOT, SHADOW)
    try:
        text = open(shadow).read()
    except OSError as e:
        print(f"snd_reg_pin: {e}")
        return 1

    # A macro is moved when the shadow both undefines it and redefines it
    # through the base. Either half alone is a mistake worth failing on: an
    # #undef with no #define deletes a register, and a #define with no #undef
    # is a redefinition whose winner depends on include order.
    undone = set(re.findall(r"^\s*#\s*undef\s+(reg_[A-Za-z0-9_]+)", text, re.M))
    moved = set(re.findall(r"^\s*#\s*define\s+(reg_[A-Za-z0-9_]+)[^\n]*A7SND_IO", text, re.M))

    bad = 0
    for name, addr in sorted(want.items()):
        if name not in moved:
            print(f"  {name} ({addr:#010x}) is a DS address the shadow does not move")
            bad += 1
        elif name not in undone:
            print(f"  {name} is redefined without an #undef")
            bad += 1
    for name in sorted(moved - set(want)):
        print(f"  {name} is moved but no longer defined by the driver's headers")
        bad += 1

    # The bare constant. Nothing dereferences it, and the shadow says so.
    used = subprocess.run(
        ["grep", "-rl", "REG_DMA0SAD_ADDR", os.path.join(ROOT, "pc/arm7snd/src")],
        capture_output=True, text=True).stdout.strip()
    if used:
        print("  REG_DMA0SAD_ADDR is used now: " + used.replace(ROOT + "/", ""))
        print("    measure what reads it before converting it")
        bad += 1

    # And what the compiler produced, if there is any.
    objs = []
    rawdir = os.path.join(ROOT, RAWDIR)
    if os.path.isdir(rawdir):
        objs = sorted(os.path.join(rawdir, f) for f in os.listdir(rawdir)
                      if f.endswith(".o"))
    left = 0
    if objs:
        objdump = os.environ.get("OBJDUMP", "arm-none-eabi-objdump")
        hit = re.compile(r"0x0?4000[0-9a-f]{3}\b")
        for o in objs:
            try:
                dis = subprocess.run([objdump, "-d", o], capture_output=True,
                                     text=True).stdout
            except OSError as e:
                print(f"snd_reg_pin: {e}")
                return 1
            for line in dis.splitlines():
                if hit.search(line):
                    print(f"  {os.path.basename(o)}: {line.strip()}")
                    left += 1
        if left:
            print(f"snd_reg_pin: {left} DS I/O address(es) survived into the objects")
            bad += 1

    if bad:
        print(f"snd_reg_pin: {bad} problem(s) across {len(want)} register(s)")
        return 1

    where = f", none left in {len(objs)} object(s)" if objs else \
            "; NO OBJECTS READ (make -f 3ds/Makefile game)"
    print(f"snd_reg_pin: {len(want)} sound-driver register(s) reach the slab{where}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
