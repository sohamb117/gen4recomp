#!/usr/bin/env python3
"""3ds/tests/vram_pin.py: the VRAM placement model is armrec's, still.

3ds/src/3ds_vram.c holds a copy of five tables and four functions from
tools/armrec/armrec_rt.c. They are not a reading of a hardware document:
`pcdiff-melon --vram-selftest` wrote a marker through all 2,304 VRAMCNT
configurations on a console and read it back at every 16 KB address, and that
sweep is where every number in them comes from, including the two cases
reasoning gets wrong, F and G answering at two window blocks rather than one.

A copy that can drift is worse than no copy, so this compares them. The two
files spell four names differently, on purpose: the 3DS side does not define
armrec's public symbols (that collision belongs to the task that decides how
much of armrec_rt.c this console compiles). Those four renames are applied
before the comparison and are the only difference allowed. Comments and
whitespace are stripped; everything else must match byte for byte.

    3ds/tests/vram_pin.py            # from the repo root
"""

import re
import sys

ARMREC = "tools/armrec/armrec_rt.c"
PORT = "3ds/src/3ds_vram.c"

# The 3DS spelling for each of armrec's names. Nothing else may differ.
RENAMES = {
    "ARM_VRAM_BANKS": "VRAM_BANKS",
    "ARM_VRAM_BLK": "VRAM_BLK",
    "ARMREC_EXTPAL_ABG": "VRAM_EXTPAL_ABG",
    "ARMREC_EXTPAL_BBG": "VRAM_EXTPAL_BBG",
    "ARMREC_EXTPAL_AOBJ": "VRAM_EXTPAL_AOBJ",
    "ARMREC_EXTPAL_BOBJ": "VRAM_EXTPAL_BOBJ",
    "armrec_vram_bank_size": "vram_bank_size",
    "armrec_vram_bank_ptr": "vram_bank_ptr",
    "armrec_vram_lookup": "vram_lookup",
    "armrec_vram_extpal": "vram_extpal",
    "armrec_vram_texture": "vram_texture",
    "armrec_vram_texpal": "vram_texpal",
    "armrec_vram_bank_in_lcdc": "vram_bank_in_lcdc",
}

# Each entry is (what to call it, the text that starts it in armrec_rt.c, the
# text that starts it in 3ds_vram.c). The body compared is from the first `{`
# after that text to its matching `}`.
ITEMS = [
    ("vram_win", "} vram_win[VW_COUNT] = ", "} vram_win[VW_COUNT] = "),
    ("vram_blocks", "vram_blocks[ARM_VRAM_BANKS] = ", "vram_blocks[VRAM_BANKS] = "),
    ("vram_lcdc_blk", "vram_lcdc_blk[ARM_VRAM_BANKS] = ", "vram_lcdc_blk[VRAM_BANKS] = "),
    ("vram_cnt_mask", "vram_cnt_mask[ARM_VRAM_BANKS] = ", "vram_cnt_mask[VRAM_BANKS] = "),
    ("vram_cnt_reg", "vram_cnt_reg[ARM_VRAM_BANKS] = ", "vram_cnt_reg[VRAM_BANKS] = "),
    ("vram_place", "static void vram_place(int win", "static void vram_place(int win"),
    ("vram_place_run", "static void vram_place_run(", "static void vram_place_run("),
    ("vram_place_bank", "static void vram_place_bank(", "static void vram_place_bank("),
    ("vram_lookup", "int armrec_vram_lookup(", "int vram_lookup("),
    ("vram_extpal", "void *armrec_vram_extpal(", "void *vram_extpal("),
    ("vram_texture", "void *armrec_vram_texture(", "void *vram_texture("),
    ("vram_texpal", "void *armrec_vram_texpal(", "void *vram_texpal("),
    ("vram_bank_in_lcdc", "int armrec_vram_bank_in_lcdc(",
     "int vram_bank_in_lcdc("),
]

# The two helpers the four roles above claim through are NOT pinned, and the
# difference is the whole reason: armrec prints the nine VRAMCNT values and
# aborts when two banks claim one slot, and this console has no stderr, so the
# 3DS copy counts it the way vram_overlap() counts a window block. Only the
# bodies that decide WHICH bank holds WHICH slot are compared, because those
# are the ones that can drift into looking right and being wrong.


def body(text, start_marker, where):
    at = text.find(start_marker)
    if at < 0:
        sys.exit(f"vram_pin: {where}: cannot find {start_marker!r}")
    open_at = text.find("{", at)
    if open_at < 0:
        sys.exit(f"vram_pin: {where}: no body after {start_marker!r}")
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_at : i + 1]
    sys.exit(f"vram_pin: {where}: unbalanced braces after {start_marker!r}")


def normalise(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    for old, new in RENAMES.items():
        src = re.sub(rf"\b{old}\b", new, src)
    return re.sub(r"\s+", " ", src).strip()


def main():
    try:
        armrec = open(ARMREC).read()
        port = open(PORT).read()
    except OSError as e:
        sys.exit(f"vram_pin: {e}")

    bad = 0
    for name, mark_a, mark_p in ITEMS:
        a = normalise(body(armrec, mark_a, ARMREC))
        p = normalise(body(port, mark_p, PORT))
        if a != p:
            bad += 1
            print(f"  {name} differs from {ARMREC}")
            print(f"    armrec: {a[:160]}")
            print(f"    3ds:    {p[:160]}")

    if bad:
        print(f"vram_pin: {bad} of {len(ITEMS)} copied items differ")
        return 1
    print(f"vram_pin: {len(ITEMS)} tables and functions match {ARMREC}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
