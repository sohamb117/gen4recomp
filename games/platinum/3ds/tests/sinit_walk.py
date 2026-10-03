#!/usr/bin/env python3
"""
3ds/tests/sinit_walk.py: the overlay static initialisers, on this chain.

`FS_StartOverlay` is the one place in this port where a wrong answer jumps to
a DS address. The SDK's version walks a list of function pointers taken from
the overlay table, and that table came out of the real ROM, so every entry is
a guest address. On the PC port that killed the first battle anyone reached
(`0x0223D0F5`, which the ROM's own map names as battle_main.c's
NitroStaticInit with the thumb bit set). pc/src/pc_fs_overlay.c replaces the
walk with one over HOST function pointers, recorded at start-up by the
constructor in pc/include/nitro/sinit.h.

That machinery is four separate facts, and every one of them can be true on
the PC chain and quietly false here:

  1. The port's FS_StartOverlay is the one that ends up in the link. The SDK's
     is weakened, but a caller inside the SDK's own object could bind to it
     locally and never reach the linker's choice; which is the shape 5.4
     had to check for the five SPI files.
  2. The registrations happen at all. sinit.h's recorder is a gcc
     `constructor`, so it depends on devkitARM's crt0 running .init_array and
     on --gc-sections not collecting it.
  3. The needles find their translation units. The recorder stores
     __BASE_FILE__, which on this chain is the path of the STRIPPED COPY the
     transform produced, and pc_fs_overlay.c matches it with strstr.
  4. There is room for them. Over PC_SINIT_MAX registrations is an exit(2)
     at start-up.

It needs `make -f 3ds/Makefile game` to have run, and says so rather than
passing when the objects are not there.

    3ds/tests/sinit_walk.py            verify, print the summary
    3ds/tests/sinit_walk.py --list     one line per registering TU
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OBJ = os.path.join(ROOT, "build", "3ds", "obj")
OVERLAY_C = os.path.join(ROOT, "pc", "src", "pc_fs_overlay.c")
SINIT_H = os.path.join(ROOT, "pc", "include", "nitro", "sinit.h")
PORT_O = os.path.join(OBJ, "pc", "pc_fs_overlay.o")
SDK_O = os.path.join(OBJ, "game", "subprojects", "NitroSDK-4.2.30001",
                     "libraries", "fs", "src", "fs_overlay.o")

PREFIX = os.environ.get("DEVKITARM", "") + "/bin/arm-none-eabi-"


def tool(name, *args):
    return subprocess.run([PREFIX + name] + list(args),
                          capture_output=True, text=True).stdout


def linker_script():
    """devkitARM's 3dsx.ld, wherever this install put it."""
    dkp = os.environ.get("DEVKITPRO", os.path.expanduser("~/devkitpro"))
    for root, _dirs, files in os.walk(os.path.join(dkp, "devkitARM")):
        if "3dsx.ld" in files:
            return os.path.join(root, "3dsx.ld")
    return None


def needles():
    """The (overlay, needle) table out of pc_fs_overlay.c."""
    src = open(OVERLAY_C).read()
    body = src.split("sSinitNeedles[] = {", 1)[1].split("};", 1)[0]
    return re.findall(r'\{\s*(\d+),\s*"([^"]+)"\s*\}', body)


def sinit_max():
    src = open(OVERLAY_C).read()
    return int(re.search(r"#define PC_SINIT_MAX (\d+)", src).group(1))


def registrars():
    """Every game object whose TU registered a NitroStaticInit, and the
    __BASE_FILE__ string it recorded."""
    out = []
    for dirpath, _dirs, files in os.walk(os.path.join(OBJ, "game")):
        for f in files:
            if not f.endswith(".o"):
                continue
            path = os.path.join(dirpath, f)
            if " t pc_sinit_ctor" not in tool("nm", path):
                continue
            # The recorded string. __BASE_FILE__ is the file on the command
            # line, which the transform makes a .stripped.c under build/.
            base = None
            for s in tool("strings", path).splitlines():
                if s.endswith(".stripped.c") or s.endswith(".c"):
                    base = s
                    break
            out.append((path, base))
    return out


def main():
    bad = []

    if not os.path.isdir(os.path.join(OBJ, "game")):
        print("sinit_walk: no game objects, make -f 3ds/Makefile game",
              file=sys.stderr)
        return 2
    if not os.environ.get("DEVKITARM"):
        print("sinit_walk: DEVKITARM is not set", file=sys.stderr)
        return 2

    # 1. Whose FS_StartOverlay. The port's has to be strong, the SDK's weak,
    #    and (the part a strength check alone would miss) every relocation
    #    in EXECUTABLE code naming it has to be against the symbol. A
    #    relocation against the section .text.FS_StartOverlay would reach the
    #    SDK's own bytes whatever the linker decided about the symbol. The
    #    debug sections are full of exactly those and are not code.
    if " T FS_StartOverlay" not in tool("nm", PORT_O):
        bad.append("pc_fs_overlay.o does not define FS_StartOverlay strongly")
    if " W FS_StartOverlay" not in tool("nm", SDK_O):
        bad.append("the SDK's fs_overlay.o still has a strong FS_StartOverlay")

    section_relocs = 0
    for dirpath, _dirs, files in os.walk(os.path.join(OBJ, "game")):
        for f in files:
            if not f.endswith(".o"):
                continue
            sec = None
            for line in tool("objdump", "-r", os.path.join(dirpath, f)).splitlines():
                m = re.match(r"RELOCATION RECORDS FOR \[(.*)\]", line)
                if m:
                    sec = m.group(1)
                elif ".text.FS_StartOverlay" in line and sec is not None \
                        and not sec.startswith(".debug"):
                    section_relocs += 1
    if section_relocs:
        bad.append("%d code relocation(s) reach .text.FS_StartOverlay by "
                   "section, which the linker cannot redirect" % section_relocs)

    # 2. The registrations survive to the link. .init_array is what the crt0
    #    walks; --gc-sections is on, so the linker script has to KEEP it.
    ld = linker_script()
    if ld is None:
        bad.append("3dsx.ld not found under DEVKITPRO")
    elif not re.search(r"KEEP\s*\(\s*\*\(\.init_array\)\s*\)", open(ld).read()):
        bad.append("3dsx.ld does not KEEP .init_array; --gc-sections would "
                   "collect every sinit registration")
    if "constructor" not in open(SINIT_H).read():
        bad.append("pc/include/nitro/sinit.h no longer registers through a "
                   "constructor; this test checks the wrong mechanism")

    regs = registrars()
    for path, base in regs:
        if base is None:
            bad.append("%s registers a NitroStaticInit with no __BASE_FILE__ "
                       "string" % os.path.relpath(path, ROOT))
        if ".init_array" not in tool("objdump", "-h", path):
            bad.append("%s has pc_sinit_ctor and no .init_array section"
                       % os.path.relpath(path, ROOT))

    # 3. Every needle finds exactly one of them. More than one would run the
    #    wrong overlay's initialiser; none is the exit(2) at load time.
    bases = [b for _p, b in regs if b]
    for overlay, needle in needles():
        n = sum(1 for b in bases if needle in b)
        if n != 1:
            bad.append("needle %r (overlay %s) matches %d recorded TU(s)"
                       % (needle, overlay, n))

    # 4. Room for them.
    cap = sinit_max()
    if len(regs) > cap:
        bad.append("%d registering TU(s) over PC_SINIT_MAX %d" % (len(regs), cap))

    if "--list" in sys.argv:
        for path, base in sorted(regs, key=lambda r: r[0]):
            print("%-58s %s" % (os.path.relpath(path, ROOT),
                                os.path.basename(base or "-")))

    if bad:
        for b in bad:
            print("sinit_walk: " + b, file=sys.stderr)
        return 1

    print("sinit_walk: %d TU(s) register a NitroStaticInit (max %d), "
          "%d needle(s) each match one, FS_StartOverlay is the port's"
          % (len(regs), cap, len(needles())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
