#!/usr/bin/env python3
"""
Extract CodeWarrior `asm` function bodies from a C source into a .s file that
tools/armrec/armrec.py can recompile.

This is the other half of strip_asm.py. Where that removes the assembly so the
surrounding C compiles, this preserves it so the recompiler can turn it into C
of its own.

The point is testing. A hand-written host implementation of an assembly
function has no obvious oracle, but the recompiler is independently
validated (MD5 known-answer for ARM, a 740k-comparison differential for
Thumb), so running the *same* assembly through it produces a reference to diff
against. See pc/tests/test_fx.c.

It is also a fallback for porting: anything too fiddly to transliterate by hand
can simply be recompiled instead, at the cost of running slower than native C.

Usage:
    extract_asm.py INPUT.c OUTPUT.s [--base 0x02000000]
"""

import argparse
import re
import sys

ASM_START = re.compile(r"^(?:static\s+)?asm\s+[A-Za-z_][\w \t\*]*\(")
NAME = re.compile(r"^(?:static\s+)?asm\s+[\w \t\*]*?([A-Za-z_]\w*)\s*\(")
CLOSE = re.compile(r"^\}\s*$")
INLINE_ASM = re.compile(r"^\s*asm\s*$")

# The parameter list of an asm function, from the "(" to the matching ")".
PARAMS = re.compile(r"^(?:static\s+)?asm\s+[\w \t\*]*?[A-Za-z_]\w*\s*\((.*?)\)\s*\{?\s*$")
PROTOTYPE = re.compile(r"^(?:static\s+)?asm\s+[^{]*\)\s*;\s*$")
IDENT = re.compile(r"[A-Za-z_]\w*")

# A parameter that does not occupy exactly one register. AAPCS softfp does put
# a float or a double in the core registers, but only the first four *words* of
# the argument list are in registers and a double eats two of them, so the
# positional rule below stops being true the moment one appears; a struct passed
# by value is worse again. Only applied when the declaration has no `*`,
# `struct Mtx22 *mtx` is an ordinary pointer and the whole FX_mtx* family is
# written that way. Refuse rather than guess: no asm function in this tree
# passes one of these by value, and if upstream adds one the failure should be
# loud.
UNMAPPABLE = re.compile(r"\b(?:float|double|struct|union)\b")

# "ldr r1, =SomeSymbol", the assembler's shorthand for "load the word I will
# park in a literal pool for you". Disassembly never contains it, because by
# then the pool exists and the load is PC-relative, so armrec.py only
# understands the resolved form. See pool_literals().
LDR_EQ = re.compile(
    r"^(\s*ldr[a-z]{0,2}\s+[A-Za-z_]\w*\s*,\s*)=\s*(.+?)\s*((?:;|//|/\*).*)?$")

LABEL = re.compile(r"^\s*[A-Za-z_.$?][\w.$?]*:")
# A .short that the author had to write as a comment. See jump_tables().
COMMENTED_SHORT = re.compile(r"^\s*(?://|;)\s*(\.(?:short|hword)\s+\S.*)$")


def param_registers(decl, name, path, lineno):
    """
    Map each named parameter of an `asm` function to the register it arrives in.

    mwcc lets the body of an `asm` function refer to its arguments by name,
    `str newpc, [context, #0x40]` rather than `str r1, [r0, #0x40]`, and binds
    each `register` parameter to the AAPCS register for its position. The .s
    files in this tree are disassembly, so the names are long gone by the time
    armrec.py sees them and it has no idea what a `newpc` is; without this it
    emits `armrec_trap("bad register 'newpc'")` for most of OS_context.c.

    The positional rule is measured rather than assumed. OS_InitContext is in
    the ROM at file offset 0xCFAF5 and assembles to

        E2811004  add r1, r1, #4        <- "add newpc, newpc, #4"
        E5801040  str r1, [r0, #0x40]   <- "str newpc, [context, #0x40]"
        E5802044  str r2, [r0, #0x44]   <- "str newsp, [context, #0x44]"

    i.e. context/newpc/newsp are r0/r1/r2 in declaration order. The assembly is
    self-consistent with that on its own too; it writes `ands r1, newpc, #1`,
    which only makes sense if newpc is already r1, but the image settles it.

    Returns {name: "rN"}. Empty when the function names nothing.
    """
    decl = decl.strip()
    if not decl or decl == "void":
        return {}

    mapping = {}
    for i, part in enumerate(decl.split(",")):
        part = part.strip()
        if not part:
            continue
        if "*" not in part and UNMAPPABLE.search(part):
            raise SystemExit(
                "%s:%d: %s takes `%s` by value, which does not follow the "
                "one-argument-one-register rule this substitution assumes."
                % (path, lineno, name, part))
        if i > 3:
            raise SystemExit(
                "%s:%d: %s has more than four parameters, so argument %d "
                "arrives on the stack rather than in a register."
                % (path, lineno, name, i + 1))
        ident = IDENT.findall(part)
        if not ident:
            raise SystemExit("%s:%d: cannot read parameter %d of %s"
                             % (path, lineno, i + 1, name))
        mapping[ident[-1]] = "r%d" % i
    return mapping


def substitute_params(body, mapping):
    """Rewrite parameter names in a body to the registers they stand for."""
    if not mapping:
        return body, 0
    pat = re.compile(r"\b(%s)\b" % "|".join(re.escape(k) for k in mapping))
    out, hits = [], 0
    for line in body:
        new, n = pat.subn(lambda m: mapping[m.group(1)], line)
        out.append(new)
        hits += n
    return out, hits


def extract(lines, path="<input>"):
    """Return [(name, [body_lines])] for each top-level asm function."""
    out = []
    i = 0
    n = len(lines)
    while i < n:
        if INLINE_ASM.match(lines[i]):
            raise SystemExit(
                "%s:%d: statement-level 'asm { ... }' block; extract it by "
                "hand." % (path, i + 1))
        if not ASM_START.match(lines[i]):
            i += 1
            continue
        # A prototype (`asm void OS_IrqHandler_ThreadSwitch(void);`, HG/SS's
        # os_irqHandler.c) declares an asm function defined elsewhere.
        if PROTOTYPE.match(lines[i]):
            i += 1
            continue
        m = NAME.match(lines[i])
        name = m.group(1) if m else None
        if name is None:
            raise SystemExit("%s:%d: cannot read function name" % (path, i + 1))
        pm = PARAMS.match(lines[i].rstrip("\n"))
        if pm is None:
            raise SystemExit("%s:%d: cannot read the parameter list of %s "
                             "(does it span lines?)" % (path, i + 1, name))
        params = param_registers(pm.group(1), name, path, i + 1)

        # The body may open on the declaration line or the next one.
        j = i
        if "{" not in lines[i]:
            j += 1
        j += 1
        body = []
        while j < n and not CLOSE.match(lines[j]):
            body.append(lines[j].rstrip("\n"))
            j += 1
        if j >= n:
            raise SystemExit("%s:%d: unterminated asm function %s"
                             % (path, i + 1, name))
        body, hits = substitute_params(body, params)
        out.append((name, body, sorted(params.items()) if hits else []))
        i = j + 1
    return out


def jump_tables(body):
    """
    Restore the Thumb branch tables that mwcc's inline assembler could not say.

    A Thumb switch reads its offsets out of a table sitting in .text right
    after the computed branch. mwcc's `asm` blocks have no directive for a bare
    constant, so the two game files that switch inside one encode the table as
    *instructions whose encodings happen to equal the offsets they need*,
    `lsl r6, r0, #0x0` assembles to 0x000A, and write the table they actually
    meant beside it in a comment. Both authors left a note about it; one of them
    is "huge hack to get the correct jump offset. Is there a way to write
    constants?".

    Recompiling those instructions produces nonsense, because the table is read
    as data at run time and never executed. Recompiling the comment produces
    the table. So: a run of lines between two labels that contains commented-out
    .short directives is replaced by exactly those directives. Nothing else in
    such a run is reachable; it lies between the computed branch and the first
    case label, and the rule cannot fire anywhere else, because these are the
    only ten commented .short lines in any inline asm in the tree.
    """
    out = []
    seg = []

    def flush():
        shorts = [m.group(1) for m in map(COMMENTED_SHORT.match, seg) if m]
        out.extend("\t" + s for s in shorts) if shorts else out.extend(seg)
        del seg[:]

    for line in body:
        if LABEL.match(line):
            flush()
            out.append(line)
        else:
            seg.append(line)
    flush()
    return out


def pool_literals(name, body):
    """
    Rewrite "ldr rN, =EXPR" into a PC-relative load plus a literal pool.

    mwcc's inline assembler accepts the "=" shorthand and builds the pool
    itself; the .s files in this tree are disassembly, so the pool is already
    materialised and armrec.py only knows that form. Emitting one here keeps
    the two paths on the same code in armrec rather than adding a second way to
    resolve a constant.

    The pool goes at the end of the function, exactly where the assembler puts
    it, and identical expressions share one word.

    Returns (rewritten_body, pool_lines).
    """
    pool = []      # [(label, expr)], in first-use order
    seen = {}      # expr -> label
    out = []
    for line in body:
        m = LDR_EQ.match(line)
        if not m:
            out.append(line)
            continue
        head, expr, comment = m.group(1), m.group(2), m.group(3) or ""
        label = seen.get(expr)
        if label is None:
            label = "_%s_lit%d" % (name, len(pool))
            seen[expr] = label
            pool.append((label, expr))
        out.append("%s%s%s%s" % (head, label, " " if comment else "", comment))

    lines = []
    if pool:
        lines.append("\t.balign 4")
        for label, expr in pool:
            lines.append("%s: .word %s" % (label, expr))
    return out, lines


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--base", default="0x02000000",
                    help="synthetic guest base address for the emitted functions")
    ap.add_argument("--thumb", action="store_true",
                    help="emit thumb_func_start instead of arm_func_start")
    args = ap.parse_args()

    with open(args.input, "r", errors="replace") as fh:
        funcs = extract(fh.readlines(), args.input)

    addr = int(args.base, 0)
    start = "thumb_func_start" if args.thumb else "arm_func_start"
    end = "thumb_func_end" if args.thumb else "arm_func_end"

    with open(args.output, "w") as fh:
        fh.write("/* Generated by tools/armrec/extract_asm.py from %s.\n"
                 " * Addresses are synthetic; these functions are reached by\n"
                 " * name, not through the dispatch table. */\n"
                 % args.input)
        fh.write("\t.section .text\n\n")
        for name, body, subst in funcs:
            body, pool = pool_literals(name, jump_tables(body))
            if subst:
                # Recorded in the output rather than only in this script: the
                # substitution is the one step here that changes what a register
                # operand *means*, so anyone reading the recompiled C should be
                # able to see it without re-deriving it.
                fh.write("\t/* %s names its arguments: %s */\n"
                         % (name, ", ".join("%s=%s" % (k, v) for k, v in subst)))
            fh.write("\t%s %s\n" % (start, name))
            fh.write("%s: ; 0x%08X\n" % (name, addr))
            for line in body + pool:
                fh.write("%s\n" % line)
            fh.write("\t%s %s\n\n" % (end, name))
            # Leave room so no two functions share an address.
            addr += max(0x100, (len(body) + len(pool) + 4) * 4)
    return 0


if __name__ == "__main__":
    sys.exit(main())
