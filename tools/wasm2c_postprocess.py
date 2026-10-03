#!/usr/bin/env python3
"""Give wasm2c output the DS cartridge's integer division, not wasm traps.

Usage: wasm2c_postprocess.py <generated .c files...>   (edits in place)

This is how core/cmake/NpGuestModule.cmake runs a POSTPROCESS script that is
not a .cmake file: `<script> <generated .c files...>`, after wasm2c and before
the C is compiled. Running it again on its own output is a no-op.

WHY. The ARM946E-S has no divide instruction, so every `/` and `%` the game
was built with is a call into mwcc's runtime (FP_fastI_v5t_LE.a), and that
runtime answers a zero divisor instead of faulting. The game divides by zero
in places a player reaches (games/platinum/pc/src/pc_div0_wrap.s names three:
the SDK particle emitter, the poffin steam spawner, the contest panel
scroller). wasm's i32/i64 div_s, div_u, rem_s and rem_u trap on a zero
divisor, and div_s also traps on INT_MIN / -1, so a guest compiled to wasm
dies where the cartridge carries on. The other hosts answer this already:
games/platinum/pc/src/pc_div0.c (x86 SIGFPE handler that decodes `div`/`idiv`)
and pc/src/pc_div0_wrap.s / 3ds/src/3ds_div0_wrap.s (--wrap of the four
32-bit __aeabi_[u]idiv[mod] entry points, pc/Makefile.arm "--wrap is the
divide-by-zero fix"). On wasm the division is an instruction, and wasm2c turns
each one into a macro, so the answer goes into the macros.

WHAT THE CARTRIDGE ANSWERS. Measured, not assumed: the routines below were
run under an ARM emulator (unicorn) straight out of the platinum ROM's
build/rom/main.sbin, at the addresses build/rom/main.nef.xMAP gives
(_s32_div_f 0x020E1F6C, _u32_div_f 0x020E2178, _ll_mod 0x020E1D14,
_ll_sdiv 0x020E1D24, _ll_udiv 0x020E1ED4, _ull_mod 0x020E1EE0):

    32-bit (_s32_div_f / _u32_div_f return quotient r0, remainder r1)
        7 / 0 = 7            7 % 0 = 0
       -7 / 0 = -7          -7 % 0 = 0
        INT_MIN / 0 = INT_MIN,  INT_MIN % 0 = 0
        0xFFFFFFFFu / 0 = 0xFFFFFFFFu, 0xFFFFFFFFu % 0 = 0
        INT_MIN / -1 = INT_MIN, INT_MIN % -1 = 0
        7 / 2 = 3, -7 / 2 = -3 r -1, 7 / -2 = -3 r 1   (C truncation)
    64-bit (_ll_sdiv, _ll_mod, _ll_udiv, _ull_mod)
        a / 0 = a            a % 0 = a      <- NOT 0: differs from 32-bit
        INT64_MIN / -1 = INT64_MIN, INT64_MIN % -1 = 0

Why, from the disassembly. _u32_div_f opens `cmp r1,#0; bxeq lr`, leaving
the numerator in r0 (quotient) and the zero divisor in r1 (remainder).
_s32_div_f records the result sign in r12 (`eor r12,r0,r1; and r12,#1<<31`,
plus bit 0 for a negative numerator), takes absolute values, and on a zero
divisor `beq`s straight to its tail, which negates r0 by bit 31 and r1 by
bit 0: so a / 0 == a, a % 0 == 0 for either sign. INT_MIN / -1 is an ordinary
unsigned 0x80000000 / 1 with a positive result sign, i.e. INT_MIN, remainder
0. All four 64-bit entries open with `orrs r5,r3,r2; bne ...; pop; bx lr`:
divisor zero returns r0:r1, the numerator, untouched, for the quotient AND
the remainder.

What the other hosts do, for comparison (not followed where it differs):
the 32-bit zero-divisor answer above is the one pc_div0.c, pc_div0_wrap.s and
3ds_div0_wrap.s give. INT_MIN / -1 is left to SIGFPE on x86 (pc_div0.c's
"including idiv overflowing on INT_MIN / -1, is left to die") and computed by
libgcc on ARM (INT_MIN, pc_div0_wrap.s "INT_MIN / -1 is not handled"); this
script gives the ROM's INT_MIN / -1 == INT_MIN, INT_MIN % -1 == 0. 64-bit
division by zero is not covered
upstream: pc_div0_fixup decodes only the F6/F7 div/idiv forms at the faulting
instruction and the ARM --wrap covers only the four 32-bit AEABI entries, not
__aeabi_[u]ldivmod.

Limit, the same as on every other host: only divisions that reach a wasm
div/rem instruction are answered. Where clang proves a divisor nonzero (UB
lets it), folds a comparison, or strength-reduces a constant divisor, there is
no instruction left to patch, exactly as with the ARM --wrap.

HOW. wasm2c 1.0.42 defines DIV_S/REM_S (behind I32_/I64_DIV_S/REM_S) and
DIV_U/REM_U (via DIVREM_U, used bare for both widths) once per module: in the
single .c output, or with --num-outputs=N in <module>-impl.h, which each
<module>_<i>.c includes. Both locations are handled: every #include
"*-impl.h" next to a given .c file is patched too. The block is matched
byte-for-byte; if a .c file and the -impl.h headers it includes contain
neither the wasm2c 1.0.42 text nor this script's replacement, it exits
non-zero, so a wabt upgrade that reshapes the macros cannot silently skip
the patch.
"""

import os
import re
import sys

MARKER = "/* np-div0: DS cartridge integer division (tools/wasm2c_postprocess.py) */"

ORIGINAL = r"""#define DIV_S(ut, min, x, y)                                  \
  ((UNLIKELY((y) == 0))                  ? TRAP(DIV_BY_ZERO)  \
   : (UNLIKELY((x) == min && (y) == -1)) ? TRAP(INT_OVERFLOW) \
                                         : (ut)((x) / (y)))

#define REM_S(ut, min, x, y)                                 \
  ((UNLIKELY((y) == 0))                  ? TRAP(DIV_BY_ZERO) \
   : (UNLIKELY((x) == min && (y) == -1)) ? 0                 \
                                         : (ut)((x) % (y)))

#define I32_DIV_S(x, y) DIV_S(u32, INT32_MIN, (s32)x, (s32)y)
#define I64_DIV_S(x, y) DIV_S(u64, INT64_MIN, (s64)x, (s64)y)
#define I32_REM_S(x, y) REM_S(u32, INT32_MIN, (s32)x, (s32)y)
#define I64_REM_S(x, y) REM_S(u64, INT64_MIN, (s64)x, (s64)y)

#define DIVREM_U(op, x, y) \
  ((UNLIKELY((y) == 0)) ? TRAP(DIV_BY_ZERO) : ((x)op(y)))

#define DIV_U(x, y) DIVREM_U(/, x, y)
#define REM_U(x, y) DIVREM_U(%, x, y)
"""

# x / 0 == x at both widths; x % 0 == 0 at 32 bits, x at 64 bits;
# MIN / -1 == MIN, MIN % -1 == 0. wasm2c passes u32/u64 (s32/s64 after the
# I*_ casts) operands, so sizeof picks the width; it is a constant, folded.
REPLACEMENT = MARKER + r"""
#define DIV_S(ut, min, x, y)                                   \
  ((UNLIKELY((y) == 0))                  ? (ut)(x)             \
   : (UNLIKELY((x) == min && (y) == -1)) ? (ut)(x)             \
                                         : (ut)((x) / (y)))

#define REM_S(ut, min, x, y)                                   \
  ((UNLIKELY((y) == 0))                  ? (sizeof(ut) == 8 ? (ut)(x) : (ut)0) \
   : (UNLIKELY((x) == min && (y) == -1)) ? (ut)0               \
                                         : (ut)((x) % (y)))

#define I32_DIV_S(x, y) DIV_S(u32, INT32_MIN, (s32)x, (s32)y)
#define I64_DIV_S(x, y) DIV_S(u64, INT64_MIN, (s64)x, (s64)y)
#define I32_REM_S(x, y) REM_S(u32, INT32_MIN, (s32)x, (s32)y)
#define I64_REM_S(x, y) REM_S(u64, INT64_MIN, (s64)x, (s64)y)

#define DIV_U(x, y) ((UNLIKELY((y) == 0)) ? (x) : ((x) / (y)))
#define REM_U(x, y) \
  ((UNLIKELY((y) == 0)) ? (sizeof((x) % (y)) == 8 ? (x) : 0) : ((x) % (y)))
"""

IMPL_INCLUDE = re.compile(r'^#include "([^"]+-impl\.h)"', re.M)


def die(msg):
    sys.stderr.write("wasm2c_postprocess: error: %s\n" % msg)
    sys.exit(1)


def state(text, path):
    has_orig = ORIGINAL in text
    has_new = MARKER in text
    if has_orig and has_new:
        die("%s: both the wasm2c division macros and the patched ones" % path)
    if has_new and REPLACEMENT not in text:
        die("%s: patch marker present but the patched block was altered" % path)
    return "original" if has_orig else "patched" if has_new else "absent"


def main(argv):
    if not argv:
        die("usage: wasm2c_postprocess.py <generated .c files...>")
    contents = {}  # path -> text, everything read, patched or not

    def load(path):
        if path not in contents:
            try:
                with open(path, "r", encoding="utf-8", newline="") as f:
                    contents[path] = f.read()
            except OSError as e:
                die("%s: %s" % (path, e))
        return contents[path]

    to_patch = []
    for arg in argv:
        path = os.path.abspath(arg)
        text = load(path)
        candidates = [path] + [
            os.path.join(os.path.dirname(path), inc)
            for inc in IMPL_INCLUDE.findall(text)
        ]
        found = False
        for cand in candidates:
            if cand != path and not os.path.exists(cand):
                die("%s: includes %s, which does not exist" % (arg, cand))
            st = state(load(cand), cand)
            if st == "absent":
                continue
            found = True
            if st == "original" and cand not in to_patch:
                to_patch.append(cand)
        if not found:
            die("%s: wasm2c's DIV_S/REM_S/DIV_U/REM_U definitions (wabt 1.0.42 "
                "text) not found in it or its -impl.h; wasm2c changed, update "
                "ORIGINAL/REPLACEMENT in this script" % arg)

    for path in to_patch:
        text = contents[path]
        if text.count(ORIGINAL) != 1:
            die("%s: division macro block appears %d times" % (path, text.count(ORIGINAL)))
        text = text.replace(ORIGINAL, REPLACEMENT)
        if "TRAP(DIV_BY_ZERO)" in text:
            die("%s: a TRAP(DIV_BY_ZERO) survived the patch" % path)
        tmp = path + ".np-div0.tmp"
        with open(tmp, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        os.replace(tmp, path)


if __name__ == "__main__":
    main(sys.argv[1:])
