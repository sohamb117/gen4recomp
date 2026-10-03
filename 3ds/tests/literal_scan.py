#!/usr/bin/env python3
"""
3ds/tests/literal_scan.py: every DS address still written as a number.

A guest address is not a host address on this console. The shadows moved the
constants the SDK spells with a name; what is left is the ones spelled with a
number, and each of those is either already handled, scheduled with a task
that names it, or accepted as unreachable. This script finds them and refuses
to let there be a fourth kind.

    3ds/tests/literal_scan.py            verify, print the summary
    3ds/tests/literal_scan.py --list     one line per hit
    3ds/tests/literal_scan.py --list unclassified

What it looks for. A 7- or 8-digit hex literal whose value falls inside a DS
memory region: ITCM, main RAM, the shared work area, WRAM, i/o, the palette,
VRAM, OAM, or the cartridge slot. Comments and string literals are stripped
first, prose about 0x04000130 is not code that pokes it, and counting it
would make the number meaningless.

Everything else in the ranges of interest is either a bit constant that only
looks like an address (0x04000000 is BIT_26, and the ARM7 wireless headers use
it that way) or a size. Those are not addresses and the rules below say so by
file, not by guesswork per line: a rule that fired on a value would let a real
address hide behind a plausible one.

CATEGORIES. Every hit has to match a rule, and every rule says which kind it
is and why:

  override  a 3DS file supplies the behaviour, or the shadow covers the
            header the literal lives in, or a named later task owns it
  patch     a pc/patches hunk rewrites the line
  dead      the code cannot run on this console, and the reason is stated
  mask      the value is inside a DS region and is not an address at all

A hit matching nothing is a failure. That is the whole point of the script:
the scan in the plan's measured-facts section was a snapshot, and a snapshot
does not notice the next literal somebody adds.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# name, low, high (inclusive). The AGB slot is included because 0x08000000 is
# this console's heap window: a literal there is the one that reads as valid
# host memory instead of faulting, which makes it the most dangerous kind.
REGIONS = [
    ("ITCM", 0x01FF8000, 0x01FFFFFF),
    ("main", 0x02000000, 0x023FFFFF),
    ("shared", 0x027E0000, 0x027FFFFF),
    ("WRAM", 0x03000000, 0x0300FFFF),
    ("ARM7 WRAM", 0x037F8000, 0x0380FFFF),
    ("I/O", 0x04000000, 0x04FFFFFF),
    ("palette", 0x05000000, 0x05000FFF),
    ("VRAM", 0x06000000, 0x06FFFFFF),
    ("OAM", 0x07000000, 0x07000FFF),
    ("AGB", 0x08000000, 0x0A00FFFF),
]

TREES = [
    "src", "lib", "include",
    "pc/src", "pc/hw", "pc/include", "pc/arm7snd",
    "tools/armrec",
    "subprojects/NitroSDK-4.2.30001/libraries",
    "subprojects/NitroSDK-4.2.30001/include",
    "subprojects/NitroSystem-071126.1",
    "subprojects/NitroWiFi-2.1.30003",
    "subprojects/NitroDWC-2.2.30008",
    "subprojects/libvct-1.3.1",
]

# path prefix, kind, why. First match wins, so the narrow rules come first.
#
# The kinds are what the plan asked for, a 3DS override, a pc/patches hunk,
# or accepted and named as dead, plus one the scan turned up that the plan
# did not anticipate and that must not be silent: `mask`, a value inside a DS
# region that is not an address at all. 0x07000000 is CARD_COMMAND_ID and the
# SOUND0CNT duty field; 0x03000000 picks two bits out of a Pokemon's
# personality value. Naming them is the difference between a rule that reads a
# file and a rule that reads a number, and a rule that reads a number would
# let a real address hide behind a plausible one.
#
# `asm` is folded into the kind that owns each file rather than being its own:
# A literal inside an mwcc `asm` block is unreachable by this compiler either
# way, and what matters is whether the file is linked at all.
RULES = [
    # ---- the game ----------------------------------------------------------
    ("src/debug.c", "dead",
     "no$gba's debug ports at 0x04FFFAxx, above the 1 MB I/O row and unmapped "
     "on PC too"),
    ("src/main_menu/ov97_02235D18.c", "override",
     "GBA cartridge reads through CTRDG_CpuCopy32. THE DANGEROUS CLASS: "
     "0x08000000 is this console's heap window, so an unhandled read returns "
     "live host memory instead of faulting. The cartridge window is a host "
     "pointer into the slab's 128 KB probe buffer now, and every one of these "
     "copies sits behind CTRDG_IsAgbCartridge(), which reads zeros and says "
     "no"),
    ("src/main_menu/distribution_cartridge.c", "override",
     "the distribution cartridge's signature and payload pointers, same "
     "0x08xxxxxx hazard as the migrator's and behind the same check. These "
     "two are above the probe buffer, so if they ever did run they would "
     "translate to NULL rather than to somebody's heap"),
    ("src/main_menu/gba_migrator.c", "mask",
     "0x03000000 picks two bits out of a personality value for the Unown "
     "letter. Not an address"),
    ("src/main_menu/gba_pokemon.c", "mask", "the same Unown letter mask"),
    ("src/pokemon.c", "mask", "the same Unown letter mask, written out"),

    # ---- the host models ---------------------------------------------------
    ("pc/hw", "override",
     "melonDS-derived hardware models. They speak guest addresses on purpose "
     "and the translator handles them"),
    ("pc/src", "override",
     "the PC frontend. 7.1 classifies every file: dropped, ported, or "
     "replaced by a 3ds/src file"),
    ("pc/include", "override",
     "the PC port's own shadows, which 3ds/include shadows in turn"),
    ("pc/arm7snd", "override",
     "the ARM7 sound tree, with its own include chain and register header"),
    ("tools/armrec", "override",
     "the runtime whose 3DS backend is 4.1; the guest map already pins its "
     "regions against this file"),

    # ---- SDK headers -------------------------------------------------------
    ("subprojects/NitroSDK-4.2.30001/include", "override",
     "SDK headers. Every address in them is a constant reached by name, and "
     "3ds/include shadows the memory map and the register base"),
    ("subprojects/NitroSystem-071126.1/include/nnsys/g3d/binres/res_struct.h",
     "mask", "NNS_G3D_TEXIMAGE_PARAM_T_SIZE_MASK, a texture parameter field"),

    # ---- SDK C that is not linked on this console --------------------------
    ("subprojects/NitroSDK-4.2.30001/libraries/wl_arm7", "dead",
     "ARM7 wireless. Not in the PC build's object list, and its 0x04000000 "
     "is BIT_26"),
    ("subprojects/NitroSDK-4.2.30001/libraries/wm_arm7", "dead",
     "ARM7 wireless MAC. Not linked"),
    ("subprojects/NitroSDK-4.2.30001/libraries/wvr", "dead",
     "the wireless VRAM relay, ARM7 side. Not linked"),
    ("subprojects/NitroSDK-4.2.30001/libraries/wm/src/wm_sp.c", "dead",
     "the wireless sub-processor half. Not linked"),
    ("subprojects/NitroSDK-4.2.30001/libraries/spi/src/mic_arm7.c", "dead",
     "ARM7 SPI. Not in the PC build's object list; the ARM7 is a model here, "
     "not a processor"),
    ("subprojects/NitroSDK-4.2.30001/libraries/spi/src/mic_arm7_2.c", "dead",
     "ARM7 SPI. Not linked"),
    ("subprojects/NitroSDK-4.2.30001/libraries/spi/src/tp_arm7.c", "dead",
     "ARM7 SPI. Not linked"),
    ("subprojects/NitroSDK-4.2.30001/libraries/spi/src/tp_arm7_2.c", "dead",
     "ARM7 SPI. Not linked; every literal is a #0x6000000 field mask anyway"),
    ("subprojects/NitroSDK-4.2.30001/libraries/spi/src/spi_arm7.c", "dead",
     "ARM7 SPI. Not linked"),
    ("subprojects/NitroSDK-4.2.30001/libraries/spi/src/pm_arm7.c", "dead",
     "ARM7 SPI. Not linked"),
    ("subprojects/NitroSDK-4.2.30001/libraries/rtc/src/gpio.c", "dead",
     "the RTC's ARM7 GPIO. Not linked"),
    ("subprojects/NitroSDK-4.2.30001/libraries/rtc/src/control.c", "dead",
     "the RTC's ARM7 control path. Not linked"),
    ("subprojects/NitroSDK-4.2.30001/libraries/card/src/card_pullOut_arm7.c",
     "dead", "card pull-out detection, ARM7 side. Not linked"),

    # ---- SDK C that is linked ----------------------------------------------
    ("subprojects/NitroSDK-4.2.30001/libraries/init/src/crt0.c", "override",
     "the DS crt0. Every literal is inside an mwcc asm block, and 5.3 puts "
     "3ds_main in front of NitroMain instead"),
    ("subprojects/NitroSDK-4.2.30001/libraries/mi/src/mi_dma.c", "override",
     "the DMA endpoint compares against 0x04000000 and 0x08000000. The translator "
     "decides which world a DMA address is in; the file is already on the "
     "no-inline list"),
    ("subprojects/NitroSDK-4.2.30001/libraries/os/src/os_callTrace.c",
     "override",
     "a 0x02000000-0x02400000 range check on a thread name pointer. Guest "
     "arithmetic over what is now a host pointer"),
    ("subprojects/NitroSDK-4.2.30001/libraries/os/src/os_exception.c",
     "override",
     "the debugger's own exception vector buffer, plus one asm literal"),
    ("subprojects/NitroSDK-4.2.30001/libraries/os/src/os_emulator.c",
     "override",
     "ensata's magic ports at 0x04FFF010, above the 1 MB I/O row and mapped "
     "by nothing. Reached only if the probe decides it is on an emulator, "
     "which needs VCOUNT to read exactly 270 at one moment, a 1-in-263 "
     "accident on hardware, and then a null dereference here. The translator's "
     "VCOUNT model owns it; the PC port carries the same hazard unmapped"),
    ("subprojects/NitroSDK-4.2.30001/libraries/snd/src/snd_command.c",
     "override",
     "the same ensata port at 0x04FFF200, behind OS_IsRunOnEmulator()"),
    ("subprojects/NitroSDK-4.2.30001/libraries/os/src/os_reset.c", "mask",
     "CARD_CTRL_CMD_MASK"),
    ("subprojects/NitroSDK-4.2.30001/libraries/snd/src/snd_bank.c", "override",
     "HW_MAIN_MEM used as a THRESHOLD, not an address: a wave offset below it "
     "is relative and above it absolute. The shadow makes it a host pointer "
     "and the test still holds, because every wave this port hands the SPU "
     "lives in the port window, which is a later row of the same slab"),
    ("subprojects/NitroSDK-4.2.30001/libraries/snd/src/snd_work.c", "mask",
     "the SOUND0CNT duty field"),
    ("subprojects/NitroSDK-4.2.30001/libraries/card/include/card_rom.h",
     "mask", "CARD_COMMAND_ID and CARD_COMMAND_MASK"),

    # ---- the dead stacks ---------------------------------------------------
    ("subprojects/NitroDWC-2.2.30008", "dead",
     "the Wi-Fi stack, dead here as it is on PC; these are report flags"),
    ("subprojects/NitroWiFi-2.1.30003", "dead", "the Wi-Fi stack"),
    ("subprojects/libvct-1.3.1", "dead", "voice chat, dead"),
]

HEX = re.compile(r"\b0[xX]([0-9a-fA-F]{7,8})\b")
BLOCK = re.compile(r"/\*.*?\*/", re.S)
LINE = re.compile(r"//[^\n]*")
STRING = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def region_of(value):
    for name, lo, hi in REGIONS:
        if lo <= value <= hi:
            return name
    return None


def strip(text):
    """Comments and string literals out, newlines kept so lines still count."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    text = BLOCK.sub(blank, text)
    text = LINE.sub(blank, text)
    text = STRING.sub(blank, text)
    return text


def classify(rel):
    for prefix, kind, why in RULES:
        if rel == prefix or rel.startswith(prefix.rstrip("/") + "/"):
            return kind, why
    return None, None


def scan():
    hits = []
    for tree in TREES:
        base = os.path.join(ROOT, tree)
        for dirpath, _dirs, files in os.walk(base):
            for fn in files:
                if not fn.endswith((".c", ".h")):
                    continue
                path = os.path.join(dirpath, fn)
                rel = os.path.relpath(path, ROOT)
                try:
                    with open(path, errors="replace") as fh:
                        text = strip(fh.read())
                except OSError:
                    continue
                for n, line in enumerate(text.split("\n"), 1):
                    for m in HEX.finditer(line):
                        region = region_of(int(m.group(1), 16))
                        if region is None:
                            continue
                        kind, why = classify(rel)
                        hits.append((rel, n, m.group(0), region, kind, why))
    return hits


def main():
    args = sys.argv[1:]
    hits = scan()
    unclassified = [h for h in hits if h[4] is None]

    if "--list" in args:
        want = args[args.index("--list") + 1] if len(args) > args.index("--list") + 1 else None
        for rel, n, lit, region, kind, _why in sorted(hits):
            if want == "unclassified" and kind is not None:
                continue
            print("%s:%d  %s  %-9s  %s" % (rel, n, lit, region, kind or "UNCLASSIFIED"))
        print()

    by_kind = {}
    for h in hits:
        by_kind[h[4] or "unclassified"] = by_kind.get(h[4] or "unclassified", 0) + 1

    if unclassified:
        for rel, n, lit, region, _k, _w in sorted(unclassified)[:20]:
            print("literal_scan: %s:%d %s (%s) matches no rule" % (rel, n, lit, region))
        print("literal_scan: %d unclassified DS address literal(s)" % len(unclassified))
        return 1

    parts = ", ".join("%d %s" % (by_kind[k], k) for k in sorted(by_kind))
    print("literal_scan: %d DS address literals, all accounted for (%s)"
          % (len(hits), parts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
