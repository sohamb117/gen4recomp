#!/usr/bin/env python3
"""
Find decompiled C that declares an assembly function with fewer arguments than
the assembly actually reads.

Three walls in a row were the same defect:

    extern struct TextPrinter *sub_0201B6C8(void);   /* the decomp's decl */
    ...
    struct TextPrinter *printer = sub_0201B6C8();    /* ...called with none */

against a callee that is, in full,

    sub_0201B6C8: ldr r0, [r0, #0x10]
                  bx lr

On ARM this works: r0 still holds whatever the caller last put there, and the
decompilation faithfully reproduces the cartridge. It only breaks when the same
C is compiled for a host, where the calling convention is the compiler's rather
than the register file's: gcc passes nothing, the recompiled body reads
whatever was in the argument slot, and the port dereferences it.

What it measures: for every function defined in a .s file, whether it uses
r0 to r3 before defining them, which is its real arity. Then every C
declaration of that name is compared against it, and one with fewer parameters
is reported.

What it deliberately does not do: a `bl` counts as defining r0 to r3 rather
than using them, because a call clobbers them. Treating a call as a use would
make almost every function look like it takes four arguments. The cost is that
a function whose only use of r0 is to forward it is missed. It is a finder,
not a prover; every hit should be confirmed against the dump.

Usage:
    tools/armrec/find_regarg_calls.py [--root DIR] [--quiet]
"""

import argparse
import os
import re
import sys

ARG_REGS = ("r0", "r1", "r2", "r3")

FUNC_START = re.compile(r"^\s*(?:arm|thumb)_func_start\s+(\S+)")
FUNC_END = re.compile(r"^\s*(?:arm|thumb)_func_end\s+(\S+)")
LABEL = re.compile(r"^\s*(\S+):")

# A register mentioned anywhere in an operand list.
REG = re.compile(r"\br(\d{1,2})\b")


def regs(text):
    return {"r" + m.group(1) for m in REG.finditer(text)}


def defs_uses(line):
    """(defined, used) registers for one instruction, approximately."""
    line = line.split(";")[0].strip()
    if not line:
        return set(), set()

    m = re.match(r"^(\w+)(.*)$", line)
    if not m:
        return set(), set()
    op, rest = m.group(1).lower(), m.group(2)

    # A call clobbers r0-r3 and lr. See the docstring for why it is not a use.
    if op.startswith("bl") or op.startswith("blx") or op == "svc" or op == "swi":
        return {"r0", "r1", "r2", "r3", "lr"}, set()

    # Branches and returns read nothing we care about.
    if op.startswith("b") and not op.startswith("bic"):
        return set(), set()

    if op.startswith("push") or op.startswith("stm"):
        return set(), regs(rest)
    if op.startswith("pop") or op.startswith("ldm"):
        return regs(rest), set()

    # A store's first operand is a source, not a destination.
    if op.startswith("str"):
        return set(), regs(rest)

    ops = [o.strip() for o in rest.split(",") if o.strip()]
    if not ops:
        return set(), set()

    first = regs(ops[0])
    rest_regs = set()
    for o in ops[1:]:
        rest_regs |= regs(o)

    # cmp/cmn/tst/teq write only flags.
    if op in ("cmp", "cmn", "tst", "teq"):
        return set(), first | rest_regs

    # ldr rD, [rN, rM], rD defined, the address registers used.
    return first, rest_regs


def arity_of(body):
    """How many of r0..r3 the function reads before writing."""
    defined = set()
    used_first = set()
    for line in body:
        d, u = defs_uses(line)
        for r in u:
            if r not in defined:
                used_first.add(r)
        defined |= d
    n = 0
    for i, r in enumerate(ARG_REGS):
        if r in used_first:
            n = i + 1
    return n, used_first


def scan_asm(root):
    """{name: (arity, path, used_regs)} for every function defined in .s."""
    out = {}
    for base in ("arm9", "arm7"):
        for dirpath, _dirs, files in os.walk(os.path.join(root, base)):
            for f in files:
                if not f.endswith(".s"):
                    continue
                path = os.path.join(dirpath, f)
                try:
                    lines = open(path, errors="ignore").read().splitlines()
                except OSError:
                    continue
                name, body = None, []

                def flush(name, body):
                    if name is None:
                        return
                    n, used = arity_of(body)
                    out[name] = (n, os.path.relpath(path, root), used)

                for line in lines:
                    m = FUNC_START.match(line)
                    if m:
                        # Many functions have no explicit *_func_end, the next
                        # *_func_start is the terminator. Missing this made the
                        # first version of this script report one hit out of
                        # three known ones.
                        flush(name, body)
                        name, body = m.group(1), []
                        continue
                    if name is None:
                        continue
                    if FUNC_END.match(line):
                        flush(name, body)
                        name = None
                        continue
                    body.append(line)
                flush(name, body)
    return out


# `extern struct Foo *name(void);` / `void name(void);`, a declaration whose
# parameter list is exactly `void`.
DECL_VOID = re.compile(
    r"(?:^|\n)\s*(?:extern\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\s*\(\s*void\s*\)\s*;")


def scan_c(root):
    """{name: [paths]} for every C declaration taking no arguments."""
    out = {}
    for base in ("arm9", "arm7", "include"):
        for dirpath, _dirs, files in os.walk(os.path.join(root, base)):
            if os.sep + "build" + os.sep in dirpath:
                continue
            for f in files:
                if not (f.endswith(".c") or f.endswith(".h")):
                    continue
                path = os.path.join(dirpath, f)
                try:
                    s = open(path, errors="ignore").read()
                except OSError:
                    continue
                for m in DECL_VOID.finditer(s):
                    out.setdefault(m.group(1), []).append(
                        os.path.relpath(path, root))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    ap.add_argument("--quiet", action="store_true")
    ns = ap.parse_args()
    root = os.path.abspath(ns.root)

    asm = scan_asm(root)
    decls = scan_c(root)

    hits = []
    for name, paths in sorted(decls.items()):
        if name not in asm:
            continue
        arity, apath, used = asm[name]
        if arity > 0:
            hits.append((name, arity, apath, sorted(set(paths)), used))

    if not ns.quiet:
        print("Assembly functions declared `(void)` by decompiled C that "
              "actually read argument registers")
        print("(%d function%s defined in .s scanned)\n"
              % (len(asm), "" if len(asm) == 1 else "s"))
        for name, arity, apath, paths, used in hits:
            print("  %s reads %s, arity %d, defined in %s"
                  % (name, ", ".join(sorted(used & set(ARG_REGS))), arity, apath))
            for p in paths:
                print("      declared (void) in %s" % p)
        print("\n%d function%s to confirm against the dump."
              % (len(hits), "" if len(hits) == 1 else "s"))

    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
