#!/usr/bin/env python3
"""pc/tools/dp_arity_lint.py: C calls into assembly that pass too few arguments.

    dp_arity_lint.py [--allow FILE] OBJDIR ASMDIR...

Exits 1 on a short call site not named in the allow file (one name per
line, `#` comments: each entry says why the call cannot matter).

mwcc let decompiled C call an assembly function through a prototype with
fewer parameters than the function reads (`sub_0201B6C8()` for a function
that loads `[r0, #0x10]`): on the ROM the missing argument is whatever the
register still held. The typed wasm bridge (games/platinum/tools/armrec/
irbridge.py) passes zero for every word the call site does not supply, so
such a call reads a NULL-based pointer instead. That was the heap
corruption behind the first Diamond list-menu hang.

Two inputs: every `W <sig> <name>` record the bridge wrote beside each C
object (the call-site signature of a direct C -> assembly call), and, for
each assembly function, the argument registers r0-r3 it reads before it
writes them on its entry path (a linear scan to the first branch, call or
return; a conservative under-approximation, so a hit is real and a miss
proves nothing). A call site supplying fewer argument words than the
callee reads is reported with the TUs that make it.
"""
import collections, glob, os, re, sys

WORD = {"v": 0, "b": 1, "h": 1, "a": 1, "c": 1, "t": 1, "s": 1, "w": 1,
        "i": 1, "p": 1, "f": 1, "l": 2, "d": 2}

def sig_words(sig):
    ret, _, params = sig.partition("_")
    n = 0
    for tok in re.findall(r"[yr]\d+|v\d+|[a-z]", params):
        if tok[0] in "yr":
            n += (int(tok[1:]) + 3) // 4 if tok[0] == "y" else 1
        elif tok[0] == "v" and len(tok) > 1:
            continue
        else:
            n += WORD.get(tok, 1)
    if ret.startswith("r"):
        n += 1
    return n

REG = re.compile(r"\b(r[0-3])\b")
START = re.compile(r"^\s*(?:arm|thumb|non_word_aligned_thumb)_func_start\s+(\w+)")
STOREISH = ("str", "strb", "strh", "cmp", "cmn", "tst", "teq", "push", "stmdb", "stmfd", "stmia", "stm")

def entry_reads(lines):
    written, read = set(), set()
    for raw in lines:
        s = raw.split(";")[0].split("@")[0].strip()
        if not s or s.endswith(":") or s.startswith("."):
            if s.endswith(":") and not s.startswith("_0") and False:
                break
            continue
        m = s.split(None, 1)
        mn = m[0].lower()
        ops = m[1] if len(m) > 1 else ""
        base = re.sub(r"(eq|ne|cs|cc|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)$", "", mn)
        if base in ("b", "bx", "bl", "blx") or mn.startswith("b") and base in ("beq", "bne") \
           or "pc" in ops.split(",")[0] or (base.startswith("pop") and "pc" in ops) \
           or (base.startswith("ldm") and "pc" in ops):
            regs = REG.findall(ops)
            for r in regs:
                if r not in written:
                    read.add(r)
            break
        regs = REG.findall(ops)
        if not regs:
            continue
        if base.startswith(STOREISH) or base.startswith("str"):
            srcs, dsts = regs, []
            if base in ("push", "stmdb", "stmfd") or base.startswith("stm"):
                srcs = []          # spilling a register is not a use of its value
        else:
            first = ops.split(",")[0]
            dsts = REG.findall(first)
            srcs = REG.findall(",".join(ops.split(",")[1:]))
            if base.startswith("ldm") or base.startswith("pop"):
                dsts, srcs = REG.findall(ops), []
        for r in srcs:
            if r not in written:
                read.add(r)
        written.update(dsts)
    return read

def main(argv):
    argv = list(argv[1:])
    allow = set()
    if argv and argv[0] == "--allow":
        for ln in open(argv[1]):
            ln = ln.split("#", 1)[0].strip()
            if ln:
                allow.add(ln)
        argv = argv[2:]
    objdir, asmdirs = argv[0], [d for d in argv[1:] if os.path.isdir(d)]
    funcs = {}
    for d in asmdirs:
        for p in glob.glob(os.path.join(d, "**", "*.s"), recursive=True):
            lines = open(p, errors="replace").read().split("\n")
            for i, ln in enumerate(lines):
                m = START.match(ln)
                if m:
                    body = []
                    for l2 in lines[i + 1:]:
                        if START.match(l2):
                            break
                        body.append(l2)
                    reads = entry_reads(body[1:] if body and body[0].strip().startswith(m.group(1)) else body)
                    funcs[m.group(1)] = max([int(r[1]) + 1 for r in reads] or [0])
    calls = collections.defaultdict(lambda: collections.defaultdict(set))
    for p in glob.glob(os.path.join(objdir, "**", "*.sigs"), recursive=True):
        for ln in open(p):
            r = ln.split()
            if r and r[0] == "W":
                calls[r[2]][r[1]].add(os.path.relpath(p, objdir)[:-7])
    hits = 0
    for name in sorted(calls):
        need = funcs.get(name)
        if need is None:
            continue
        for sig, tus in sorted(calls[name].items()):
            if sig_words(sig) < need:
                if name in allow:
                    continue
                hits += 1
                print("dp_arity_lint: " + "%s reads r0-r%d on entry; called as %s (%d words) from %s"
                      % (name, need - 1, sig, sig_words(sig), ", ".join(sorted(tus))))
    print("  ARITY   %d unexplained short call sites over %d asm functions called from C"
          % (hits, len(calls)))
    return 1 if hits else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
