#!/usr/bin/env python3
"""pc/tools/dp_retwidth_lint.py: C calls into assembly through a too-narrow return type.

    dp_retwidth_lint.py [--allow FILE] [-v] OBJDIR ROMBUILD ASMDIR...

Exits 1 on a call site that is a hit or an unknown and is not named in the
allow file (`<callee> <caller TU basename>` or `<callee> *` per line, `#`
comments: each entry says why the call cannot matter). -v also lists the
sites proven harmless.

mwcc let decompiled C call an assembly function through a prototype whose
return type is narrower than the value the assembly leaves in r0
(`extern u16 ov05_021E7184(...)` for a function returning a SysTask*). The
ARM ABI makes the callee narrow, so mwcc's caller uses r0 as it comes; when
the assembly never narrowed, the ROM keeps the upper bits. The typed wasm
bridge (games/platinum/tools/armrec/irbridge.py, gen_bridge.py) narrows r0
to the prototype's type at the call, so such a call loses them. That was
the start-menu SAVE fault in Heap_Free(NULL).

A site matters when both halves hold:

  callee  some return path of the assembly function leaves r0 wider than
          the declared type. Each return (bx lr, pop {pc}, ldm {pc}, a tail
          call) is traced back through the function's branches to the
          definitions of r0 that reach it, following register copies, the
          literal pool and calls (an assembly callee recursively, a C
          callee by its definition's signature in the .sigs). ldrb/ldrh,
          lsr/asr, masks and in-range constants are narrow; a pointer
          (literal-pool symbol, sp/pc-relative add, pointer-returning
          call) or an out-of-range constant is wide; a word load, an int
          return or arithmetic that can overflow is "maybe". The call
          site's narrow parameters are known on entry (mwcc and the bridge
          both pass them narrowed), and an assembly callee's narrow
          arguments likewise.
  caller  the ROM's own caller (mwcc's object under ROMBUILD) lets the
          upper bits of r0 reach something observable. The value is
          followed forward from the `bl` through copies and the
          low-bit-preserving ALU ops; lsl by >= 32 - width and masks
          clean it, strb/strh store only the low bits, a call clobbers
          r0-r3. A word store, a compare, an address, a right shift or
          a return as int is a use of the upper bits; a pass to another
          call or a return through a narrow type is reported as unknown.
          Only Thumb callers are decoded (mwcc's C is Thumb); an ARM one is
          an unknown.

A narrow callee or a ROM caller that narrows makes a site harmless: both
builds then agree on every bit the program looks at.

hit      callee wide, caller uses the upper bits: fix the prototype.
unknown  either half undecided (callee "maybe", or a caller use the trace
         cannot classify): reviewed by hand, listed in the allow file.
"""
import collections, glob, os, re, struct, sys

DECL = {"b": (0, 1), "h": (0, 0xFF), "c": (-0x80, 0x7F), "a": (-0x80, 0x7F),
        "t": (0, 0xFFFF), "w": (0, 0xFFFF), "s": (-0x8000, 0x7FFF)}
DECL_BITS = {"b": 8, "h": 8, "c": 8, "a": 8, "t": 16, "w": 16, "s": 16}
U32 = 1 << 32

# ---------------------------------------------------------------------------
# Abstract values: a frozenset of atoms
#   ("r", lo, hi)   an integer interval (negative = upper bits set)
#   ("ptr", why)    an address: wide
#   ("word", why)   a 32-bit value nothing narrowed (word load, int return)
#   ("unk", why)    the trace could not tell
# ---------------------------------------------------------------------------

def rng(lo, hi):
    if lo < -(1 << 31) or hi > U32 - 1:
        return frozenset([("word", "overflow")])
    return frozenset([("r", lo, hi)])

def ranges(v):
    """[(lo, hi)] when every atom is an interval, else None."""
    if not v or any(a[0] != "r" for a in v):
        return None
    return [(a[1], a[2]) for a in v]

def span(v):
    rs = ranges(v)
    return (min(r[0] for r in rs), max(r[1] for r in rs)) if rs else None

def binop(a, b, f, why):
    sa, sb = span(a), span(b)
    if any(x[0] == "ptr" for x in a | b):
        return frozenset([("ptr", why)])
    if sa is None or sb is None:
        return frozenset([("word", why)])
    vals = [f(x, y) for x in sa for y in sb]
    return rng(min(vals), max(vals))

def unop(a, f, why):
    s = span(a)
    if s is None:
        return frozenset([("word", why)]) if not any(x[0] == "ptr" for x in a) else a
    vals = [f(s[0]), f(s[1])]
    return rng(min(vals), max(vals))

def verdict(v, code):
    """narrow | wide | maybe, and the atoms that decided it."""
    lo, hi = DECL[code]
    wide, maybe = [], []
    for a in sorted(v, key=str):
        if a[0] == "ptr":
            wide.append(a)
        elif a[0] == "r":
            if a[1] >= lo and a[2] <= hi:
                continue
            if a[1] == a[2]:
                wide.append(a)          # a constant out of range
            else:
                maybe.append(a)
        else:
            maybe.append(a)
    if wide:
        return "wide", wide
    if maybe:
        return "maybe", maybe
    return "narrow", []

def describe(atoms):
    out = []
    for a in atoms:
        if a[0] == "r":
            out.append("%#x" % (a[1] & 0xFFFFFFFF) if a[1] == a[2] else "[%d,%d]" % (a[1], a[2]))
        else:
            out.append("%s(%s)" % a)
    return ", ".join(out)

# ---------------------------------------------------------------------------
# The assembly side: .s text
# ---------------------------------------------------------------------------

START = re.compile(r"^\s*(?:arm|thumb|non_word_aligned_thumb)_func_start\s+(\w+)")
LABEL = re.compile(r"^\s*([A-Za-z_.$][\w.$]*):(.*)$")
REGN = {"sb": "r9", "sl": "r10", "fp": "r11", "ip": "r12", "sp": "r13", "lr": "r14", "pc": "r15"}
CONDS = ("eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt",
         "gt", "le", "al")
# base mnemonic -> the suffixes it takes after the (pre-UAL) condition
LDST = {"", "b", "h", "sb", "sh", "t", "bt", "d"}
MULTI = {"", "ia", "ib", "da", "db", "fd", "ed", "fa", "ea"}
SUFFIXES = {"ldr": LDST, "str": LDST, "ldm": MULTI, "stm": MULTI, "b": {""}, "bl": {""},
            "bx": {""}, "blx": {""}}
BASES = ["ldm", "stm", "push", "pop", "ldr", "str", "mov", "mvn", "add", "adc", "sub", "sbc",
         "rsb", "rsc", "and", "orr", "eor", "bic", "mul", "mla", "umull", "smull", "umlal",
         "smlal", "lsl", "lsr", "asr", "ror", "neg", "cmp", "cmn", "tst", "teq", "blx", "bx",
         "bl", "b", "swi", "svc", "clz", "mrs", "msr", "mrc", "mcr", "nop", "swp", "smul",
         "smla", "qadd", "qsub", "qdadd", "qdsub", "cpy", "ldc", "stc", "cdp", "pld", "bkpt"]

def parse_mn(mn):
    """(base, cond or None, suffix): `ldreqh` -> ldr eq h, `bls` -> b ls."""
    for base in BASES:
        if not mn.startswith(base):
            continue
        rest, ok = mn[len(base):], SUFFIXES.get(base, {"", "s"})
        for cond in CONDS:
            if rest.startswith(cond) and rest[len(cond):] in ok:
                return base, (None if cond == "al" else cond), rest[len(cond):]
        if rest in ok:
            return base, None, rest
    return mn, None, ""

def reg(tok):
    tok = tok.strip().lower().rstrip("!")
    tok = REGN.get(tok, tok)
    return tok if re.fullmatch(r"r\d+", tok) else None

def split_ops(ops):
    out, depth, cur = [], 0, ""
    for ch in ops:
        if ch in "[{":
            depth += 1
        elif ch in "]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out

def imm(tok):
    tok = tok.strip().lstrip("#")
    try:
        return int(tok, 0)
    except ValueError:
        return None

def reglist(tok):
    regs = set()
    for part in tok.strip("{}^ ").split(","):
        part = part.strip()
        if "-" in part:
            a, b = (reg(x) for x in part.split("-"))
            if a and b:
                regs.update("r%d" % i for i in range(int(a[1:]), int(b[1:]) + 1))
        elif reg(part):
            regs.add(reg(part))
    return regs

class Insn:
    __slots__ = ("base", "cond", "suf", "ops", "text")

    def __init__(self, mn, ops, text):
        self.base, self.cond, self.suf = parse_mn(mn)
        self.ops = split_ops(ops)
        self.text = text

class AsmFunc:
    def __init__(self, name, thumb):
        self.name, self.thumb = name, thumb
        self.insns, self.labels = [], {}

def parse_asm(paths):
    funcs, words = {}, {}
    for p in paths:
        cur = None
        for raw in open(p, errors="replace"):
            m = START.match(raw)
            if m:
                cur = AsmFunc(m.group(1), "thumb" in raw.split()[0])
                funcs[cur.name] = cur
                continue
            s = raw.split(";")[0].split("@")[0].strip()
            labels = []
            while True:
                lm = LABEL.match(s)
                if not lm:
                    break
                labels.append(lm.group(1))
                s = lm.group(2).strip()
            if s.startswith(".word") and labels:
                val = s.split(None, 1)[1].strip() if len(s.split(None, 1)) > 1 else ""
                for l in labels:
                    words[l] = val
            if cur is None:
                continue
            for l in labels:
                cur.labels[l] = len(cur.insns)
            if not s or s.startswith("."):
                continue
            parts = s.split(None, 1)
            cur.insns.append(Insn(parts[0].lower(), parts[1] if len(parts) > 1 else "", s))
    return funcs, words

class Callee:
    """Return-value analysis of the assembly functions."""

    def __init__(self, funcs, words, c_ret):
        self.funcs, self.words, self.c_ret = funcs, words, c_ret
        self.memo, self.busy = {}, set()

    def literal(self, tok):
        tok = tok.strip()
        if tok.startswith("="):
            tgt = tok[1:].strip()
        else:
            tgt = self.words.get(tok)
            if tgt is None:
                return None
        n = imm(tgt)
        if n is not None:
            n &= 0xFFFFFFFF
            return ("const", n - U32 if n >= 1 << 31 else n)
        return ("sym", tgt.split("+")[0].strip())

    def call(self, name, entry=()):
        """r0 after a call to `name` (entry: known argument registers)."""
        if name in self.funcs:
            return self.ret(name, entry)[0]
        code = self.c_ret.get(name)
        if code is None:
            return frozenset([("unk", "call " + name)])
        if code in DECL:
            return rng(*DECL[code])
        if code == "p":
            return frozenset([("ptr", "call " + name)])
        if code == "v":
            return frozenset([("unk", "void " + name)])
        return frozenset([("word", "%s returns %s" % (name, code))])

    def ret(self, name, entry=()):
        """(value, [return-path notes]) of an assembly function's r0.
        entry: ((reg, value), ...) known on entry (the call site's narrow
        parameters: mwcc and the bridge both pass them narrowed)."""
        key = (name, entry)
        if key in self.memo:
            return self.memo[key]
        if key in self.busy:
            return frozenset([("unk", "recursion " + name)]), []
        self.busy.add(key)
        f = self.funcs[name]
        st = _Flow(self, f, dict(entry))
        val, notes = frozenset(), []
        for i, ins in enumerate(f.insns):
            kind = st.exit_kind(i)
            if kind is None:
                continue
            if kind[0] == "ret":
                v = st.before("r0", i)
            else:
                v = self.call(kind[1], st.args_at(kind[1], i))
            val |= v
            notes.append((i, v))
        if not notes:
            val = frozenset([("unk", "no return found in " + name)])
        self.busy.discard(key)
        self.memo[key] = (val, notes)
        return val, notes

def entry_values(sig):
    """The narrow parameters of a call-site signature, by register."""
    out, n = [], 0
    for tok in re.findall(r"[yr]\d+|v\d+|[a-z]", sig.partition("_")[2]):
        if n > 3 or tok[0] in "yrvld":
            break
        if tok in DECL:
            out.append(("r%d" % n, rng(*DECL[tok])))
        n += 1
    return tuple(out)

class _Flow:
    def __init__(self, callee, f, entry):
        self.c, self.f, self.entry = callee, f, entry
        self.ins = f.insns
        self.preds = collections.defaultdict(list)
        for i, ins in enumerate(self.ins):
            if self.falls(i) and i + 1 < len(self.ins):
                self.preds[i + 1].append(i)
            t = self.branch_target(i)
            if t is not None:
                self.preds[t].append(i)
        self.memo, self.active = {}, set()

    def branch_target(self, i):
        ins = self.ins[i]
        if ins.base == "b" and ins.ops:
            return self.f.labels.get(ins.ops[0])
        return None

    def is_return(self, i):
        ins = self.ins[i]
        if ins.base in ("pop", "ldm") and "r15" in reglist(ins.ops[-1] if ins.ops else ""):
            return True
        if ins.base == "bx":
            r = reg(ins.ops[0]) if ins.ops else None
            if r == "r14":
                return True
            if r is not None and self.tail_target(i) is None:
                return True
        if ins.base in ("mov", "ldr") and ins.ops and reg(ins.ops[0]) == "r15":
            return True
        return False

    def tail_target(self, i):
        ins = self.ins[i]
        if ins.base == "b" and ins.ops and ins.ops[0] not in self.f.labels:
            return ins.ops[0]
        if ins.base == "bx" and ins.ops and reg(ins.ops[0]) not in (None, "r14"):
            for v in self.def_sources(reg(ins.ops[0]), i):
                if v is not None and v[0] == "sym":
                    return v[1]
        return None

    def def_sources(self, r, i):
        """Literal-pool definitions of r reaching i (for `ldr r3, =f; bx r3`)."""
        out, seen, work = [], set(), list(self.preds[i])
        while work:
            p = work.pop()
            if p in seen:
                continue
            seen.add(p)
            ins = self.ins[p]
            if self.defs(ins, r):
                if ins.base == "ldr" and not ins.suf and len(ins.ops) == 2 and "[" not in ins.ops[1]:
                    out.append(self.c.literal(ins.ops[1]))
                else:
                    out.append(None)
                continue
            work.extend(self.preds[p])
        return out

    def falls(self, i):
        ins = self.ins[i]
        if ins.cond:
            return True
        if ins.base == "b" or self.is_return(i):
            return False
        if ins.base == "bx":
            return False
        return True

    def exit_kind(self, i):
        tt = self.tail_target(i)
        if tt is not None:
            return ("tail", tt)
        if self.is_return(i):
            return ("ret",)
        return None

    def defs(self, ins, r):
        b = ins.base
        if b in ("str", "stm", "push", "cmp", "cmn", "tst", "teq", "b", "bx", "nop",
                 "msr", "mcr", "pld", "stc", "bkpt"):
            return False
        if b in ("bl", "blx", "swi", "svc"):
            return r in ("r0", "r1", "r2", "r3", "r12")
        if b in ("pop",):
            return r in reglist(ins.ops[0]) if ins.ops else False
        if b == "ldm":
            return r in reglist(ins.ops[-1]) or (len(ins.ops) > 1 and ins.ops[0].endswith("!") and reg(ins.ops[0]) == r)
        if b in ("umull", "smull", "umlal", "smlal"):
            return r in (reg(ins.ops[0]), reg(ins.ops[1]))
        return bool(ins.ops) and reg(ins.ops[0]) == r

    def args_at(self, name, i):
        """Argument registers known to be in range on entry to the call at i
        (only an assembly callee can use them; a tail `bx rN` names its
        target in rN)."""
        if name not in self.c.funcs:
            return ()
        ins = self.ins[i]
        skip = reg(ins.ops[0]) if ins.base == "bx" and ins.ops else None
        out = []
        for k in range(4):
            r = "r%d" % k
            if r == skip:
                continue
            v = self.before(r, i)
            if ranges(v) is not None:
                out.append((r, v))
        return tuple(out)

    def before(self, r, i, depth=0):
        """Value of register r on entry to instruction i: the join of every
        definition that reaches it (a conditional ARM definition also lets the
        older one through), or the value on function entry."""
        key = (r, i)
        if key in self.memo:
            return self.memo[key]
        if key in self.active:
            return frozenset([("unk", "loop-carried %s" % r)])
        self.active.add(key)
        defs, entry, seen = [], i == 0, set()
        work = list(self.preds[i])
        while work:
            p = work.pop()
            if p in seen:
                continue
            seen.add(p)
            ins = self.ins[p]
            if self.defs(ins, r):
                defs.append(p)
                if not (ins.cond and ins.base not in ("bl", "blx")):
                    continue
            if p == 0:
                entry = True
            work.extend(self.preds[p])
        v = frozenset()
        for p in defs:
            v |= self.eval_def(self.ins[p], r, p, depth)
        if entry:
            v |= self.entry.get(r, frozenset([("unk", "%s on entry to %s" % (r, self.f.name))]))
        self.active.discard(key)
        self.memo[key] = v
        return v

    def operand(self, tok, p, depth):
        r = reg(tok)
        if r is not None:
            if r == "r13":
                return frozenset([("ptr", "sp")])
            if r == "r15":
                return frozenset([("ptr", "pc")])
            return self.before(r, p, depth + 1)
        n = imm(tok)
        if n is not None:
            return rng(n, n)
        return frozenset([("unk", "operand " + tok)])

    def eval_def(self, ins, r, p, depth):
        b, ops, suf = ins.base, ins.ops, ins.suf
        W = lambda why: frozenset([("word", why)])
        if b in ("bl",):
            if r != "r0":
                return frozenset([("unk", "clobbered by call")])
            return self.c.call(ops[0], self.args_at(ops[0], p))
        if b in ("blx", "swi", "svc"):
            return frozenset([("unk", ins.text)])
        if b in ("pop", "ldm"):
            return W("restored " + ins.text)
        if b == "ldr":
            if len(ops) == 2 and "[" not in ops[1]:
                lit = self.c.literal(ops[1])
                if lit is None:
                    return frozenset([("unk", ins.text)])
                if lit[0] == "const":
                    return rng(lit[1], lit[1])
                return frozenset([("ptr", "=" + lit[1])])
            size = suf.replace("t", "")
            if size in ("b",):
                return rng(0, 0xFF)
            if size in ("h",):
                return rng(0, 0xFFFF)
            if size in ("sb",):
                return rng(-0x80, 0x7F)
            if size in ("sh",):
                return rng(-0x8000, 0x7FFF)
            if size == "d":
                return W(ins.text)
            return W("ldr " + ",".join(ops[1:]))
        if b in ("mov", "cpy"):
            if len(ops) >= 3:              # ARM shifted operand: mov r0, r1, lsr #16
                return self.shifted(ops[1], ops[2], p, depth)
            return self.operand(ops[1], p, depth)
        if b == "mvn":
            if len(ops) == 2 and imm(ops[1]) is not None:
                n = ~imm(ops[1]) & 0xFFFFFFFF
                n = n - U32 if n >= 1 << 31 else n
                return rng(n, n)
            return W(ins.text)
        if b in ("lsl", "lsr", "asr", "ror"):
            src, amt = (ops[0], ops[1]) if len(ops) == 2 else (ops[1], ops[2])
            return self.shifted(src, "%s %s" % (b, amt), p, depth)
        if b == "neg" or (b == "rsb" and len(ops) == 3 and imm(ops[2]) == 0):
            return unop(self.operand(ops[1], p, depth), lambda x: -x, ins.text)
        if b in ("add", "sub", "adc", "sbc", "rsb", "mul", "and", "orr", "eor", "bic"):
            if len(ops) == 2:
                a, c = self.operand(ops[0], p, depth), self.operand(ops[1], p, depth)
            else:
                a = self.operand(ops[1], p, depth)
                c = self.shifted(ops[2], ops[3], p, depth) if len(ops) > 3 else self.operand(ops[2], p, depth)
            zero = frozenset([("r", 0, 0)])
            if b in ("add", "sub", "orr", "eor") and c == zero:
                return a                    # Thumb's `add rd, rs, #0` is a move
            if b == "add":
                if any(x[0] == "ptr" for x in a | c):
                    return frozenset([("ptr", ins.text)])
                return binop(a, c, lambda x, y: x + y, ins.text)
            if b == "sub":
                if any(x[0] == "ptr" for x in a):
                    return frozenset([("ptr", ins.text)])
                return binop(a, c, lambda x, y: x - y, ins.text)
            if b == "rsb":
                return binop(a, c, lambda x, y: y - x, ins.text)
            if b == "mul":
                return binop(a, c, lambda x, y: x * y, ins.text)
            if b == "and":
                cand = [s for s in (span(a), span(c)) if s is not None and s[0] >= 0]
                if cand:
                    return rng(0, min(s[1] for s in cand))
                return W(ins.text)
            if b == "bic":
                s = span(a)
                return rng(0, s[1]) if s is not None and s[0] >= 0 else W(ins.text)
            if b in ("orr", "eor"):
                sa, sc = span(a), span(c)
                if sa and sc and sa[0] >= 0 and sc[0] >= 0:
                    return rng(0, (1 << max(sa[1], sc[1]).bit_length()) - 1)
                return W(ins.text)
            return W(ins.text)
        if b == "clz":
            return rng(0, 32)
        return frozenset([("unk", ins.text)])

    def shifted(self, src, shift, p, depth):
        parts = shift.replace("#", " ").split()
        v = self.operand(src, p, depth)
        if len(parts) != 2 or imm(parts[1]) is None:
            return frozenset([("word", "shift " + shift)])
        op, n = parts[0].lower(), imm(parts[1])
        s = span(v)
        if op == "lsr":
            if s is not None and s[0] >= 0:
                return rng(s[0] >> n, s[1] >> n)
            return rng(0, (U32 - 1) >> n)
        if op == "asr":
            if s is not None and s[0] >= -(1 << 31) and s[1] < 1 << 31:
                return rng(s[0] >> n, s[1] >> n)
            return rng(-(1 << (31 - n)), (1 << (31 - n)) - 1)
        if op == "lsl":
            if s is not None and s[0] >= 0 and s[1] << n < U32:
                return rng(s[0] << n, s[1] << n)
            return frozenset([("word", "lsl #%d" % n)])
        return frozenset([("word", "shift " + shift)])

# ---------------------------------------------------------------------------
# The ROM caller: mwcc's ELF object, Thumb (or ARM) machine code
# ---------------------------------------------------------------------------

R_ARM_ABS32, R_ARM_PC24, R_ARM_THM_CALL, R_ARM_CALL = 2, 1, 10, 28

def load_elf(path):
    d = open(path, "rb").read()
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]

    def cstr(sec, off):
        s = secs[sec][4] + off
        return d[s:d.index(b"\0", s)].decode("latin-1")
    names = [cstr(shstrndx, s[0]) for s in secs]
    symsec = next(i for i, s in enumerate(secs) if s[1] == 2)
    syms = []
    for k in range(secs[symsec][5] // 16):
        nm, val, sz, info, oth, shn = struct.unpack_from("<IIIBBH", d, secs[symsec][4] + k * 16)
        syms.append((cstr(secs[symsec][6], nm), val, sz, info, shn))
    texts = {}
    for i, (s, n) in enumerate(zip(secs, names)):
        if s[1] == 1 and n.startswith(".text") or (s[1] == 1 and n in (".itcm", ".dtcm")):
            texts[i] = {"data": d[s[4]:s[4] + s[5]], "rel": {}, "syms": [], "map": []}
    for s in secs:
        if s[1] in (4, 9) and s[7] in texts:
            ent = 12 if s[1] == 4 else 8
            for k in range(s[5] // ent):
                off, info = struct.unpack_from("<II", d, s[4] + k * ent)
                texts[s[7]]["rel"][off] = (syms[info >> 8][0], info & 0xFF)
    for nm, val, sz, info, shn in syms:
        if shn in texts and nm:
            if nm in ("$t", "$a", "$d") or nm.startswith(("$t.", "$a.", "$d.")):
                texts[shn]["map"].append((val, nm[1]))
            elif info & 0xF == 2:
                texts[shn]["syms"].append((nm, val, sz))
    return texts

class Op:
    """One decoded instruction, reduced to what the forward trace needs."""
    __slots__ = ("kind", "rd", "srcs", "imm", "size", "regs", "target", "cond", "length", "text", "sym")

    def __init__(self, kind, length, **kw):
        self.kind, self.length = kind, length
        self.rd = kw.get("rd")
        self.srcs = kw.get("srcs", ())
        self.imm = kw.get("imm")
        self.size = kw.get("size")
        self.regs = kw.get("regs", ())
        self.target = kw.get("target")
        self.cond = kw.get("cond", False)
        self.text = kw.get("text", kind)
        self.sym = kw.get("sym")

def thumb_decode(data, off, rel):
    """Decode the Thumb instruction at off. kinds:
       mov(rd, srcs=(rs,) or imm)  alu(rd, srcs)  shl/shr(rd, srcs, imm)  and(rd, srcs)
       cmp(srcs, imm)  ld(rd, srcs=address regs, size)  st(srcs=(value, addr...), size)
       lit(rd, imm/sym)  addr(rd)  push/pop(regs)  b(target, cond)  bl(sym)  ret  other"""
    h, = struct.unpack_from("<H", data, off)
    r = lambda n: "r%d" % n
    top3, top5 = h >> 13, h >> 11
    if top3 == 0:
        op = (h >> 11) & 3
        if op < 3:
            rd, rs, n = r(h & 7), r((h >> 3) & 7), (h >> 6) & 31
            kind = ("lsl", "lsr", "asr")[op]
            if kind != "lsl" and n == 0:
                n = 32
            return Op("shift", 2, rd=rd, srcs=(rs,), imm=n, size=kind, text="%s %s,%s,#%d" % (kind, rd, rs, n))
        rd, rs = r(h & 7), r((h >> 3) & 7)
        isimm, sub = (h >> 10) & 1, (h >> 9) & 1
        x = (h >> 6) & 7
        if isimm and x == 0:
            return Op("mov", 2, rd=rd, srcs=(rs,), text="mov %s,%s" % (rd, rs))
        srcs = (rs,) if isimm else (rs, r(x))
        return Op("alu", 2, rd=rd, srcs=srcs, text=("sub" if sub else "add") + " " + rd)
    if top3 == 1:
        op, rd, n = (h >> 11) & 3, r((h >> 8) & 7), h & 0xFF
        if op == 0:
            return Op("movi", 2, rd=rd, imm=n, text="mov %s,#%d" % (rd, n))
        if op == 1:
            return Op("cmp", 2, srcs=(rd,), imm=n, text="cmp %s,#%d" % (rd, n))
        return Op("alu", 2, rd=rd, srcs=(rd,), imm=n, text=("add" if op == 2 else "sub") + " %s,#%d" % (rd, n))
    if h >> 10 == 0x10:
        op, rd, rs = (h >> 6) & 15, r(h & 7), r((h >> 3) & 7)
        name = ["and", "eor", "lsl", "lsr", "asr", "adc", "sbc", "ror", "tst", "neg", "cmp",
                "cmn", "orr", "mul", "bic", "mvn"][op]
        text = "%s %s,%s" % (name, rd, rs)
        if name in ("tst", "cmp", "cmn"):
            return Op("cmp", 2, srcs=(rd, rs), size=name, text=text)
        if name == "and":
            return Op("and", 2, rd=rd, srcs=(rd, rs), text=text)
        if name in ("lsr", "asr", "ror"):
            return Op("mix", 2, rd=rd, srcs=(rd, rs), text=text)
        if name == "lsl":
            return Op("mix", 2, rd=rd, srcs=(rd, rs), text=text)
        if name in ("neg", "mvn"):
            return Op("alu", 2, rd=rd, srcs=(rs,), text=text)
        if name == "bic":
            return Op("alu", 2, rd=rd, srcs=(rd, rs), text=text)
        return Op("alu", 2, rd=rd, srcs=(rd, rs), text=text)
    if h >> 10 == 0x11:
        op = (h >> 8) & 3
        rd = r((h & 7) | ((h >> 4) & 8))
        rs = r((h >> 3) & 15)
        if op == 0:
            return Op("alu", 2, rd=rd, srcs=(rd, rs), text="add %s,%s" % (rd, rs))
        if op == 1:
            return Op("cmp", 2, srcs=(rd, rs), size="cmp", text="cmp %s,%s" % (rd, rs))
        if op == 2:
            if rd == "r15":
                return Op("ret", 2, srcs=(rs,), text="mov pc,%s" % rs)
            return Op("mov", 2, rd=rd, srcs=(rs,), text="mov %s,%s" % (rd, rs))
        if (h >> 7) & 1:
            return Op("blxr", 2, srcs=(rs,), text="blx " + rs)
        return Op("ret", 2, srcs=(rs,), text="bx " + rs)
    if top5 == 9:
        rd = r((h >> 8) & 7)
        lo = ((off + 4) & ~3) + (h & 0xFF) * 4
        if lo in rel:
            return Op("lit", 2, rd=rd, sym=rel[lo][0], text="ldr %s,=%s" % (rd, rel[lo][0]))
        val = struct.unpack_from("<I", data, lo)[0] if lo + 4 <= len(data) else None
        return Op("lit", 2, rd=rd, imm=val, text="ldr %s,=%r" % (rd, val))
    if h >> 12 == 5:
        ro, rb, rd = r((h >> 6) & 7), r((h >> 3) & 7), r(h & 7)
        if (h >> 9) & 1 == 0:
            load, byte = (h >> 11) & 1, (h >> 10) & 1
            size = "b" if byte else "w"
        else:
            hs = (h >> 10) & 3
            load, size = (0, "h") if hs == 0 else (1, ("sb", "h", "sh")[hs - 1])
        if load:
            return Op("ld", 2, rd=rd, srcs=(rb, ro), size=size, text="ldr%s %s,[%s,%s]" % (size.strip("w"), rd, rb, ro))
        return Op("st", 2, srcs=(rd, rb, ro), size=size, text="str%s %s,[%s,%s]" % (size.strip("w"), rd, rb, ro))
    if top3 == 3 or h >> 12 == 8:
        rb, rd, n = r((h >> 3) & 7), r(h & 7), (h >> 6) & 31
        load = (h >> 11) & 1
        size = "h" if h >> 12 == 8 else ("b" if (h >> 12) & 1 else "w")
        n *= {"w": 4, "h": 2, "b": 1}[size]
        if load:
            return Op("ld", 2, rd=rd, srcs=(rb,), size=size, text="ldr%s %s,[%s,#%#x]" % (size.strip("w"), rd, rb, n))
        return Op("st", 2, srcs=(rd, rb), size=size, text="str%s %s,[%s,#%#x]" % (size.strip("w"), rd, rb, n))
    if h >> 12 == 9:
        rd, load = r((h >> 8) & 7), (h >> 11) & 1
        if load:
            return Op("ld", 2, rd=rd, srcs=("r13",), size="w", imm=(h & 0xFF) * 4, text="ldr %s,[sp,#%d]" % (rd, (h & 0xFF) * 4))
        return Op("st", 2, srcs=(rd, "r13"), size="w", imm=(h & 0xFF) * 4, text="str %s,[sp,#%d]" % (rd, (h & 0xFF) * 4))
    if h >> 12 == 10:
        rd = r((h >> 8) & 7)
        return Op("addr", 2, rd=rd, text="add %s,%s" % (rd, "sp" if (h >> 11) & 1 else "pc"))
    if h >> 8 == 0xB0:
        return Op("other", 2, text="add sp")
    if h >> 12 == 11 and (h >> 9) & 3 == 2:
        regs = tuple(r(i) for i in range(8) if h >> i & 1)
        if (h >> 11) & 1:
            if (h >> 8) & 1:
                return Op("ret", 2, regs=regs, text="pop {pc}")
            return Op("pop", 2, regs=regs, text="pop")
        return Op("push", 2, regs=regs + (("r14",) if (h >> 8) & 1 else ()), text="push")
    if h >> 12 == 12:
        rb = r((h >> 8) & 7)
        regs = tuple(r(i) for i in range(8) if h >> i & 1)
        if (h >> 11) & 1:
            return Op("pop", 2, regs=regs, srcs=(rb,), text="ldmia")
        return Op("st", 2, srcs=regs + (rb,), size="m", text="stmia")
    if h >> 12 == 13:
        cond = (h >> 8) & 15
        if cond == 15:
            return Op("other", 2, text="swi")
        o = h & 0xFF
        o = o - 256 if o & 0x80 else o
        return Op("b", 2, target=off + 4 + o * 2, cond=True, text="b<cond>")
    if top5 == 0x1C:
        o = h & 0x7FF
        o = o - 0x800 if o & 0x400 else o
        return Op("b", 2, target=off + 4 + o * 2, text="b")
    if top5 == 0x1E and off + 4 <= len(data):
        sym = rel.get(off, (None,))[0]
        return Op("bl", 4, sym=sym, text="bl %s" % sym)
    return Op("undef", 2, text="%04x" % h)

class Caller:
    """Forward trace of r0 after a ROM `bl`."""

    def __init__(self, c_params):
        self.c_params = c_params

    def trace(self, data, rel, start, bits, own_ret):
        """[(kind, detail)] of the uses that see the upper bits; [] = clean."""
        uses, seen = [], set()
        work = [(start, frozenset(["r0"]), ())]
        steps = 0
        while work and steps < 4000:
            off, taint, consts = work.pop()
            consts = dict(consts)
            while taint and off + 2 <= len(data) and steps < 4000:
                steps += 1
                key = (off, taint, tuple(sorted(consts.items())))
                if key in seen:
                    break
                seen.add(key)
                op = thumb_decode(data, off, rel)
                nxt = off + op.length
                k = op.kind
                tset = set(taint)
                hit = lambda why: uses.append((why, "%#x %s" % (off, op.text)))
                if k == "shift":
                    if op.srcs[0] in taint:
                        if op.size == "lsl" and op.imm >= 32 - bits:
                            tset.discard(op.rd)
                        elif op.size == "lsl":
                            tset.add(op.rd)
                        else:
                            hit("right shift")
                            tset.discard(op.rd)
                    else:
                        tset.discard(op.rd)
                    consts.pop(op.rd, None)
                elif k == "mov":
                    (tset.add if op.srcs[0] in taint else tset.discard)(op.rd)
                    if op.srcs[0] in consts:
                        consts[op.rd] = consts[op.srcs[0]]
                    else:
                        consts.pop(op.rd, None)
                elif k == "movi":
                    tset.discard(op.rd)
                    consts[op.rd] = op.imm
                elif k == "lit":
                    tset.discard(op.rd)
                    if op.imm is not None:
                        consts[op.rd] = op.imm
                    else:
                        consts.pop(op.rd, None)
                elif k == "and":
                    other = [s for s in op.srcs if s not in taint]
                    tainted = [s for s in op.srcs if s in taint]
                    if tainted and other and consts.get(other[0], U32) < (1 << bits):
                        tset.discard(op.rd)
                    elif tainted:
                        tset.add(op.rd)
                    else:
                        tset.discard(op.rd)
                    consts.pop(op.rd, None)
                elif k == "alu":
                    (tset.add if any(s in taint for s in op.srcs) else tset.discard)(op.rd)
                    consts.pop(op.rd, None)
                elif k == "mix":
                    if any(s in taint for s in op.srcs):
                        hit("shift by register")
                    tset.discard(op.rd)
                    consts.pop(op.rd, None)
                elif k == "cmp":
                    ts = [s for s in op.srcs if s in taint]
                    if ts:
                        other = [s for s in op.srcs if s not in taint]
                        if op.size == "tst" and other and consts.get(other[0], U32) < (1 << bits):
                            pass
                        else:
                            hit("compare")
                elif k == "ld":
                    if any(s in taint for s in op.srcs if s != "r13"):
                        hit("address")
                    # a reload of a spilled value carries its taint
                    (tset.add if op.srcs == ("r13",) and "s%d" % op.imm in taint else tset.discard)(op.rd)
                    consts.pop(op.rd, None)
                elif k == "st" and op.size == "m":
                    if op.srcs[-1] in taint:
                        hit("address")
                    if any(s in taint for s in op.srcs[:-1]):
                        hit("word store")
                elif k == "st":
                    if any(s in taint for s in op.srcs[1:] if s != "r13"):
                        hit("address")
                    if op.srcs[1:] == ("r13",):
                        slot = "s%d" % op.imm
                        (tset.add if op.srcs[0] in taint else tset.discard)(slot)
                    elif op.srcs[0] in taint:
                        if op.size == "w":
                            hit("word store")
                        elif op.size == "h" and bits < 16:
                            hit("halfword store")
                elif k == "addr":
                    tset.discard(op.rd)
                    consts.pop(op.rd, None)
                elif k == "push":
                    if any(s in taint for s in op.regs):
                        hit("push")
                elif k == "pop":
                    for s in op.regs:
                        tset.discard(s)
                        consts.pop(s, None)
                elif k in ("bl", "blxr"):
                    callee = op.sym if k == "bl" else None
                    nargs = self.c_params.get(callee, 4) if callee else 4
                    for i in range(min(nargs, 4)):
                        if "r%d" % i in taint:
                            uses.append(("argument %d of %s" % (i, callee or "an indirect call"),
                                         "%#x %s" % (off, op.text)))
                    if any(s[0] == "s" and int(s[1:]) < 4 * (nargs - 4) for s in taint):
                        uses.append(("stack argument of %s" % callee, "%#x %s" % (off, op.text)))
                    for s in ("r0", "r1", "r2", "r3", "r12"):
                        tset.discard(s)
                        consts.pop(s, None)
                elif k == "ret":
                    if "r0" in taint and own_ret is not None:
                        if own_ret in ("v",):
                            pass
                        elif own_ret in DECL and DECL_BITS[own_ret] <= bits:
                            uses.append(("returned as narrow '%s'" % own_ret, "%#x %s" % (off, op.text)))
                        else:
                            hit("returned as '%s'" % own_ret)
                    elif "r0" in taint:
                        uses.append(("returned (unknown type)", "%#x %s" % (off, op.text)))
                    break
                elif k == "b":
                    if op.cond:
                        work.append((op.target, frozenset(tset), tuple(sorted(consts.items()))))
                    else:
                        nxt = op.target
                elif k == "undef":
                    uses.append(("undecoded", "%#x %s" % (off, op.text)))
                    break
                taint = frozenset(tset)
                off = nxt
            if taint and off + 2 > len(data):
                uses.append(("ran off the function", "%#x" % off))
        if work or steps >= 4000:
            uses.append(("trace limit", "%d steps" % steps))
        return uses

DEFINITE = ("word store", "compare", "address", "right shift", "shift by register", "returned as '",
            "halfword store", "push")

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
            verbose = True
            argv = argv[1:]
        else:
            break
    objdir, rombuild, asmdirs = argv[0], argv[1], [d for d in argv[2:] if os.path.isdir(d)]
    paths = []
    for d in asmdirs:
        paths.extend(sorted(glob.glob(os.path.join(d, "**", "*.s"), recursive=True)))
    funcs, words = parse_asm(paths)
    calls = collections.defaultdict(set)        # (name, sig) -> {tu}
    c_ret, c_params = {}, {}
    for p in glob.glob(os.path.join(objdir, "**", "*.sigs"), recursive=True):
        tu = os.path.relpath(p, objdir)[:-7]
        for ln in open(p):
            r = ln.split()
            if not r:
                continue
            if r[0] == "W" and r[1].split("_")[0] in DECL:
                calls[(r[2], r[1])].add(tu)
            elif r[0] == "D":
                c_ret[r[1]] = r[2].split("_")[0]
                c_params[r[1]] = len(re.findall(r"[yr]\d+|[a-z]", r[2].partition("_")[2]))
    callee = Callee(funcs, words, c_ret)
    caller = Caller(c_params)
    hits = unknowns = clean = 0
    report, elfs = [], {}
    for (name, sig), tus in sorted(calls.items()):
        code = sig.split("_")[0]
        if name not in funcs:
            report.append(("unknown", name, code, ",".join(sorted(tus)), "no assembly body found"))
            continue
        val, _ = callee.ret(name, entry_values(sig))
        cv, atoms = verdict(val, code)
        if cv == "narrow":
            clean += 1
            if verbose:
                report.append(("ok", name, code, ",".join(sorted(tus)), "callee narrow: " + describe(sorted(val, key=str))))
            continue
        for tu in sorted(tus):
            rom = os.path.join(rombuild, tu.split("/", 1)[1] + ".o")
            if not os.path.exists(rom):
                report.append(("unknown", name, code, tu, "callee %s (%s); no ROM object %s" % (cv, describe(atoms), rom)))
                continue
            if rom not in elfs:
                elfs[rom] = load_elf(rom)
            texts = elfs[rom]
            sites = []
            for t in texts.values():
                for off, (sym, typ) in sorted(t["rel"].items()):
                    if sym == name and typ in (R_ARM_THM_CALL, R_ARM_CALL, R_ARM_PC24):
                        fn = [s for s in t["syms"] if s[1] <= off < s[1] + max(s[2], 1)]
                        fname = fn[0][0] if fn else "?"
                        arm = typ != R_ARM_THM_CALL
                        sites.append((fname, off, t, arm))
            if not sites:
                report.append(("unknown", name, code, tu, "callee %s; no `bl %s` in %s" % (cv, name, rom)))
                continue
            for fname, off, t, arm in sites:
                if arm:
                    report.append(("unknown", name, code, tu, "%s: ARM-mode caller not traced" % fname))
                    continue
                base = [s for s in t["syms"] if s[0] == fname]
                start = base[0][1] if base else 0
                uses = caller.trace(t["data"], t["rel"], off + 4, DECL_BITS[code], c_ret.get(fname))
                definite = [u for u in uses if u[0].startswith(DEFINITE)]
                where = "%s+%#x" % (fname, off - start)
                if not uses:
                    clean += 1
                    if verbose:
                        report.append(("ok", name, code, tu, "%s: ROM caller narrows (callee %s: %s)" % (where, cv, describe(atoms))))
                    continue
                kind = "HIT" if cv == "wide" and definite else "unknown"
                report.append((kind, name, code, tu, "%s: callee %s (%s); ROM caller: %s" % (
                    where, cv, describe(atoms), "; ".join("%s at %s" % u for u in uses))))
    for kind, name, code, tu, why in report:
        tu_short = tu.split("/")[-1]
        if kind != "ok" and ((name, tu_short) in allow or (name, "*") in allow):
            continue
        if kind == "HIT":
            hits += 1
        elif kind == "unknown":
            unknowns += 1
        print("dp_retwidth_lint: %-7s %s as '%s' from %s: %s" % (kind, name, code, tu_short, why))
    print("  RETWIDTH %d unexplained hits, %d unexplained unknowns over %d narrow-return asm calls"
          % (hits, unknowns, len(calls)))
    return 1 if hits or unknowns else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
