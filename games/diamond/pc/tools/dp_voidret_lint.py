#!/usr/bin/env python3
"""pc/tools/dp_voidret_lint.py: assembly reading r0 after calling C that returns void.

    dp_voidret_lint.py [--allow FILE] [-v] OBJDIR ASM...

ASM: directories searched for *.s, or .s files (HG/SS pass the exact list
armrec translates, staged overlay copies included).

Exits 1 on a call site not named in the allow file (`<callee> <caller>` or
`<callee> *` per line, `#` comments: each entry says why the read cannot
matter).

The decompiled C matches the ROM byte for byte, which says nothing about a
return type mwcc did not have to emit code for: `void sub_0200C630(u32 *p)
{ sub_0200C628(*p); }` compiles to the same `ldr; bl; pop {pc}` as a
function returning the callee's value, and the ROM's assembly callers rely
on r0 still holding it (the battle send-out read the trainer's animation
frame through it). The typed wasm bridge returns 0 from a void C function
(games/platinum/tools/armrec/irbridge.py, the c2u$ adapters), so such a call
reads 0 instead.

mwcc never reads r0 after a call it compiled as void, so assembly that does
was compiled against a prototype returning a value: the decompiled
prototype is wrong. Inputs: every `D <name> <sig>` record the bridge wrote
beside each C object (return code `v`), and each assembly function's calls
to those names. From each call the instructions that follow are walked
(through both arms of a conditional branch and through unconditional ones)
until r0 is written; a read before that is a hit. A call on the way passes
r0 as an argument when the callee reads r0 (a C callee's parameter count, an
assembly callee's entry reads); a return with r0 unwritten hands the value
to the function's own callers and is reported as `returns` (a hit when one
of those callers reads it, which this lint then reports in its own right
if that function is C-visible only through assembly; listed for review).
"""
import collections, glob, os, re, sys

REG = re.compile(r"\b(r[0-3])\b")
START = re.compile(r"^\s*(?:arm|thumb|non_word_aligned_thumb)_func_start\s+(\w+)")
LABEL = re.compile(r"^\s*(\w+):")
COND = r"(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)"
# Thumb two-operand ALU forms read their destination (`and r0, r1` is
# r0 = r0 & r1, `add r0, #4` is r0 += 4).
TWO_OP_READS = {"and", "eor", "lsl", "lsr", "asr", "adc", "sbc", "ror", "orr",
                "mul", "bic", "add", "sub", "ands", "eors", "lsls", "lsrs",
                "asrs", "adcs", "sbcs", "rors", "orrs", "muls", "bics", "adds",
                "subs"}
STOREISH = ("str", "cmp", "cmn", "tst", "teq")


def parse(paths):
    """{func: [(kind, a, b)]}: instructions as ('lab', name) / ('ins', mn, ops)."""
    funcs = {}
    for p in paths:
        cur = None
        for raw in open(p, errors="replace"):
            m = START.match(raw)
            if m:
                cur = funcs.setdefault(m.group(1), [])
                continue
            if cur is None:
                continue
            s = raw.split(";")[0].split("@")[0].strip()
            if not s:
                continue
            lm = LABEL.match(s)
            if lm:
                cur.append(("lab", lm.group(1), None))
                s = s[lm.end():].strip()
                if not s:
                    continue
            if s.startswith("."):
                cur.append(("dir", s, None))
                continue
            parts = s.split(None, 1)
            cur.append(("ins", parts[0].lower(), parts[1] if len(parts) > 1 else ""))
    return funcs


def effect(mn, ops):
    """(reads, writes) of r0-r3 for one instruction (not a branch)."""
    base = mn
    regs = REG.findall(ops)
    opl = [o.strip() for o in ops.split(",")]
    if base.startswith(("push", "stmdb", "stmfd")):
        return set(), set()
    if base.startswith("stm"):
        return set(REG.findall(opl[0])), set()
    if base.startswith(("pop", "ldm")):
        if base.startswith("ldm"):
            return set(REG.findall(opl[0])), set(REG.findall(ops.split("{", 1)[1] if "{" in ops else ""))
        return set(), set(regs)
    if base.startswith(STOREISH):
        return set(regs), set()
    dst = set(REG.findall(opl[0]))
    src = set(REG.findall(",".join(opl[1:])))
    if re.sub(COND + "$", "", base) in TWO_OP_READS and len(opl) == 2:
        src |= dst
    return src, dst


def entry_reads_r0(body):
    """Whether an assembly function reads r0 before writing it on its entry path."""
    for kind, mn, ops in body:
        if kind != "ins":
            continue
        if re.match(r"b(l|lx|x)?" + COND + r"?$", mn) or "pc" in (ops or ""):
            return "r0" in REG.findall(ops)
        r, w = effect(mn, ops)
        if "r0" in r:
            return True
        if "r0" in w:
            return False
    return False


def walk(body, start, labels, c_params, asm_reads):
    """From index start (just after a void call), the first use of r0: (kind, where) or None."""
    seen, todo, found = set(), [start], []
    while todo:
        i = todo.pop()
        while i < len(body) and i not in seen:
            seen.add(i)
            kind, mn, ops = body[i]
            if kind != "ins":
                i += 1
                continue
            bm = re.match(r"(b|bl|blx|bx)" + COND + r"?$", mn)
            if mn in ("bl", "blx") and not REG.search(ops):
                tgt = ops.strip()
                n = c_params.get(tgt)
                if (n is not None and n > 0) or (n is None and asm_reads.get(tgt)):
                    found.append(("passed to " + tgt, i))
                break                        # the call writes r0 (or the trace ends)
            if mn == "bx" or (mn.startswith("pop") and "pc" in ops) or \
               (mn.startswith("ldm") and "pc" in ops and "r0" not in REG.findall(ops)):
                if mn == "bx" and ops.strip() == "r0":
                    found.append(("branch through r0", i))
                else:
                    found.append(("returns", i))
                break
            if bm and mn not in ("bl", "blx", "bx"):
                tgt = labels.get(ops.strip())
                cond = bm.group(2) not in (None, "al")
                if tgt is not None:
                    todo.append(tgt)
                if not cond:
                    break
                i += 1
                continue
            r, w = effect(mn, ops)
            if "r0" in r:
                found.append(("read by `%s %s`" % (mn, ops), i))
                break
            if "r0" in w:
                break
            i += 1
    return found


def main(argv):
    argv = list(argv[1:])
    allow, verbose = set(), False
    while argv and argv[0].startswith("-"):
        if argv[0] == "--allow":
            for ln in open(argv[1]):
                ln = ln.split("#", 1)[0].split()
                if ln:
                    allow.add(tuple(ln[:2]))
            argv = argv[2:]
        elif argv[0] == "-v":
            verbose, argv = True, argv[1:]
        else:
            break
    objdir, asmdirs = argv[0], argv[1:]
    paths = []
    for d in asmdirs:
        if os.path.isdir(d):
            paths.extend(sorted(glob.glob(os.path.join(d, "**", "*.s"), recursive=True)))
        elif d.endswith(".s") and os.path.isfile(d):
            paths.append(d)
    funcs = parse(paths)
    c_ret, c_params = {}, {}
    for p in glob.glob(os.path.join(objdir, "**", "*.sigs"), recursive=True):
        for ln in open(p):
            r = ln.split()
            if len(r) >= 3 and r[0] == "D":
                ret, _, params = r[2].partition("_")
                toks = re.findall(r"[yr]\d+|[a-z]", params)
                # An aggregate return (sret, `r<N>`) is void in the wasm
                # prototype, but the bridge's c2u$ adapter hands the assembly
                # the aggregate (<= 4 bytes) or its address in r0, as mwcc did.
                c_ret[r[1]] = "r" if ret == "v" and any(t[0] == "r" for t in toks) else ret
                c_params[r[1]] = len(toks)
    asm_reads = {f: entry_reads_r0(b) for f, b in funcs.items() if f not in c_ret}
    labels = {f: {mn: i for i, (k, mn, _) in enumerate(b) if k == "lab"} for f, b in funcs.items()}
    # void: C functions returning void, then assembly functions with a path
    # that returns r0 as a void callee left it (`bl voidC; pop {pc}`): their
    # callers get the same nothing (name -> the chain, for the report).
    void = {f: f for f, r in c_ret.items() if r == "v"}
    uses = collections.defaultdict(list)     # callee -> [(caller, why)]
    todo = set(void)
    while todo:
        new = set()
        for fname, body in funcs.items():
            if fname in c_ret:
                continue                     # a C definition shadows the asm one
            for i, (kind, mn, ops) in enumerate(body):
                if kind != "ins" or mn not in ("bl", "blx") or ops.strip() not in todo:
                    continue
                callee = ops.strip()
                for why, _ in walk(body, i + 1, labels[fname], c_params, asm_reads):
                    if why != "returns":
                        uses[callee].append((fname, why))
                    elif fname not in void:
                        void[fname] = fname + " <- " + void[callee]
                        new.add(fname)
        todo = new
    # C callers that take a value from a tainted assembly function.
    for p in glob.glob(os.path.join(objdir, "**", "*.sigs"), recursive=True):
        tu = os.path.basename(p)[:-7]
        for ln in open(p):
            r = ln.split()
            if len(r) >= 3 and r[0] == "W" and r[2] in void and r[2] not in c_ret \
               and not r[1].startswith("v"):
                uses[r[2]].append((tu, "taken as '%s' by C" % r[1].partition("_")[0]))
    hits = 0
    for callee in sorted(uses):
        for caller, why in sorted(set(uses[callee])):
            if (callee, caller) in allow or (callee, "*") in allow:
                continue
            hits += 1
            print("dp_voidret_lint: HIT %s calls %s, then r0 is %s" % (caller, void[callee], why))
    if verbose:
        for f in sorted(void):
            if f not in c_ret:
                print("dp_voidret_lint: returns what void C left: " + void[f])
    print("  VOIDRET %d unexplained r0 uses after a void C call (%d asm functions pass one on)"
          % (hits, sum(1 for f in void if f not in c_ret)))
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
