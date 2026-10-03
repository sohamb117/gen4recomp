#!/usr/bin/env python3
"""
3ds/tests/types_pin.py: keep 3ds/include/nitro/types.h a copy, not a fork.

The 3DS shadow of the SDK's types.h is the PC port's shadow with two lines
changed: `u64` and `s64` get `__attribute__((aligned(4)))`. That is one third
of the ABI answer 1.8 measured, AAPCS aligns an 8-byte type to 8 and the DS
compiler aligned it to 4, and 93 of the 251 struct disagreements were that
alone.

It cannot be an #include_next shadow. A typedef cannot be undefined and
redeclaring one with a different alignment is a conflicting type, so the file
has to carry the whole thing. That makes it a copy, and a copy of a
hundred-line header is a fork waiting to happen, silently, because the two
would still both compile.

So it is generated, and this script is the gate:

    3ds/tests/types_pin.py            verify
    3ds/tests/types_pin.py --emit     write it

Why the alignment is on the typedef and not a flag. -fpack-struct=4 fixes the
same 93 and then repacks `u32 x : 20` bitfields, moving nine structures,
seven of them save data, UndergroundRecord from 60 bytes to 40. Measured in
1.8, rejected there.

The source file now answers this itself, under `#ifdef __arm__`, because the
armhf build (pc/Makefile.arm) needs the same 4-alignment and has no shadow
directory of its own. So the rewrite below has a third rule: it strips the
conditional and leaves the attribute applied unconditionally, which is what
the 3DS (always ARM) would get from it anyway. The emitted file is
byte-identical to the one this generated before that block existed.

That makes this shadow REDUNDANT: pc/include/nitro/types.h compiled by
devkitARM already yields the 4-aligned typedefs. Retiring it means dropping
3ds/include from the 3DS include chain and deleting this script, and it is
left standing only because that is 3DS work and this was not. Whoever is next
in 3ds/ should do it.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "pc", "include", "nitro", "types.h")
OUT = os.path.join(ROOT, "3ds", "include", "nitro", "types.h")
ABI = os.path.join(ROOT, "3ds", "tests", "abi_objects.sh")

BANNER = """/*
 * 3DS shadow of nitro/types.h, generated from pc/include/nitro/types.h by
 * 3ds/tests/types_pin.py. Do not edit; edit that file, or the script.
 *
 * Two lines differ: u64 and s64 carry __attribute__((aligned(4))). ARM's
 * AAPCS aligns an 8-byte type to 8; the DS compiler aligned it to 4, and a
 * struct with a u64 in it lays out differently on the two. 93 of the 251
 * measured struct disagreements were this one thing.
 *
 * It is a whole copy rather than an #include_next shadow because a typedef
 * cannot be undefined, and redeclaring one with a different alignment is a
 * conflicting type. The copy is generated so it cannot fork.
 */

"""

RULES = [
    # The armhf conditional and the comment that introduces it. Both are
    # about a choice between two hosts; this file has only one.
    (re.compile(r"\n/\*\n \* The 64-bit pair carries an alignment.*?"
                r"^#ifdef __arm__\n"
                r"#define PC_ALIGN64 __attribute__\(\(aligned\(4\)\)\)\n"
                r"#else\n#define PC_ALIGN64\n#endif\n\n",
                re.M | re.S),
     "\n"),
    (re.compile(r"^typedef uint64_t PC_ALIGN64 u64;$", re.M),
     "typedef uint64_t __attribute__((aligned(4))) u64;"),
    (re.compile(r"^typedef int64_t PC_ALIGN64 s64;$", re.M),
     "typedef int64_t __attribute__((aligned(4))) s64;"),
]


def build():
    with open(SRC) as fh:
        text = fh.read()
    for pattern, repl in RULES:
        text, n = pattern.subn(repl, text)
        if n != 1:
            sys.exit("types_pin: %s no longer declares the type this rewrites "
                     "(%s matched %d times), fix the rule rather than trust it"
                     % (SRC, pattern.pattern, n))
    return BANNER + text


def abi_agrees():
    """abi_objects.sh keeps its own copy of this transform, because it has to
    measure the ABI with and without the answer applied. Both key on the same
    two strings; if one side ever changes them, say so here rather than let
    the measurement and the build drift apart."""
    with open(ABI) as fh:
        text = fh.read()
    return "aligned(4))) u64" in text and "aligned(4))) s64" in text


def main():
    want = build()
    if "--emit" in sys.argv[1:]:
        os.makedirs(os.path.dirname(OUT), exist_ok=True)
        with open(OUT, "w") as fh:
            fh.write(want)
        print("types_pin: wrote %s" % os.path.relpath(OUT, ROOT))
        return 0

    try:
        with open(OUT) as fh:
            got = fh.read()
    except OSError:
        print("types_pin: %s is missing" % os.path.relpath(OUT, ROOT))
        return 1

    if got != want:
        print("types_pin: %s is not what pc/include/nitro/types.h generates. "
              "Re-run with --emit." % os.path.relpath(OUT, ROOT))
        return 1
    if not abi_agrees():
        print("types_pin: 3ds/tests/abi_objects.sh no longer looks for the "
              "aligned(4) typedefs; the measurement and the build have drifted")
        return 1
    print("types_pin: nitro/types.h matches pc/include's, with u64 and s64 "
          "4-aligned")
    return 0


if __name__ == "__main__":
    sys.exit(main())
