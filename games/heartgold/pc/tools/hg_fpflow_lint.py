#!/usr/bin/env python3
"""pc/tools/hg_fpflow_lint.py: C indirect calls whose wasm type differs from a callee's.

    hg_fpflow_lint.py facts TU IR OUT          one TU's flow facts (JSON)
    hg_fpflow_lint.py solve [--allow FILE] FACTS...

mwcc let decompiled C call a function pointer through a type that differs
from the function stored in it (`void (*)()` called with four arguments,
a two-parameter callback declared with none): on ARM the extra arguments
are registers the callee ignores. In wasm an indirect call is
call_indirect with the call site's type, and a callee of another type
traps (the communication club and the custom Safari Zone, both
pc/patches/src/overlay_03).

`facts` (pc/mk/game.mk runs it on every game TU's clang -O0 IR, written
beside the object as <obj>.fpflow) records the TU's C definitions with
their wasm types, the flow edges of function constants and every indirect
call site. The flow is field-based: a struct field is one slot per
(struct type, field index), shared by every object of the type; a global,
an alloca, a parameter and a return are slots of their own. Function
constants flow through stores, loads, direct calls (arguments to parameter
slots, returns to the call's value), memcpy and global initializers.

`solve` (at link, pc/Makefile.wasm) runs the flow to a fixpoint over every
TU and reports each indirect call site that can reach a C definition of
another wasm type. Assembly callees are not checked: armrec reaches them
through their c2u$ adapters, which take every call. Exits 1 on a site with
a mismatched callee not named in the allow file (`<calling function>
<callee>` or `<calling function> *` per line, `#` comments: each entry says
why the call never reaches that callee).

The flow over-approximates (a hit may be unreachable, which the allow file
records); an assignment it does not model (a pointer built by integer
arithmetic, a value passed through assembly) is a miss.
"""
import json, os, re, sys

TOK = re.compile(r'''\s*(c"[^"]*"|"[^"]*"|[@%][-A-Za-z0-9$._"]+|<\{|\}>|[\[\]{}(),<>=*]|[-A-Za-z0-9_.$:]+|\S)''')

ATTRS = {"noundef", "zeroext", "signext", "inreg", "nonnull", "noalias", "nocapture",
         "readonly", "writeonly", "returned", "immarg", "dereferenceable", "align",
         "noinline", "dso_local", "hidden", "internal", "private", "weak", "extern_weak",
         "linkonce_odr", "local_unnamed_addr", "unnamed_addr", "nofree", "nest", "swiftself"}


def lex(s):
    out, i = [], 0
    while i < len(s):
        m = TOK.match(s, i)
        if not m:
            break
        out.append(m.group(1))
        i = m.end()
    return out


def split_top(s):
    """s split at its top-level commas."""
    parts, depth, cur, q = [], 0, [], False
    for ch in s:
        if ch == '"':
            q = not q
        if not q:
            if ch in "([{<":
                depth += 1
            elif ch in ")]}>":
                depth -= 1
            elif ch == "," and depth == 0:
                parts.append("".join(cur))
                cur = []
                continue
        cur.append(ch)
    if "".join(cur).strip():
        parts.append("".join(cur))
    return [p.strip() for p in parts]


def wtype(t):
    """An IR type's wasm32 value type: v(oid), l (i64), f, d, else i (i32)."""
    return {"void": "v", "i64": "l", "float": "f", "double": "d"}.get(t.strip(), "i")


def strip_attrs(p):
    """'ptr noundef byval(%struct.X) align 4 %0' -> 'ptr'"""
    p = re.sub(r"\b(byval|sret|elementtype|dereferenceable|dereferenceable_or_null|align|range)\s*\([^)]*\)", "", p)
    p = re.sub(r"\balign \d+", "", p)
    toks = [t for t in p.split() if t not in ATTRS]
    return toks[0] if toks else "?"


def sig_of(ret, params, variadic):
    s = wtype(ret) + "_" + "".join(wtype(strip_attrs(p)) for p in params)
    if variadic:
        s += "i"   # wasm32 varargs: one extra i32, the argument buffer
    return s


class TU:
    """One TU's facts from its clang -O0 IR."""

    def __init__(self, name, text):
        self.name = name
        self.types = {}      # %struct.X -> [field types]
        self.defs = {}       # function -> wasm sig
        self.decls = set()
        self.edges = []      # (source, slot): source ("fn", f) | ("slot", S)
        self.sites = []      # (function, line, call sig, [sources])
        self.lines = text.split("\n")
        self.parse()

    def gname(self, g):
        """A global's name, TU-qualified when it is internal."""
        g = g[1:]
        return self.name + "::" + g if g in self.local_syms else g

    def parse(self):
        self.local_syms = set()
        self.dbg = {}
        for l in self.lines:
            m = re.match(r'^!(\d+) = !DILocation\(line: (\d+)', l)
            if m:
                self.dbg[m.group(1)] = int(m.group(2))
            m = re.match(r'^(%[-\w.$"]+) = type (.*)$', l)
            if m:
                body = m.group(2).strip()
                if body.startswith("<{"):
                    body = body[2:-2]
                elif body.startswith("{"):
                    body = body[1:-1]
                else:
                    continue
                self.types[m.group(1)] = split_top(body)
            m = re.match(r'^@([-\w.$]+) = (internal|private) ', l)
            if m:
                self.local_syms.add(m.group(1))
            m = re.match(r'^define (internal|private) .*? @([-\w.$]+)\(', l)
            if m:
                self.local_syms.add(m.group(2))
        for l in self.lines:
            m = re.match(r'^define (.*?)@([-\w.$]+)\((.*)\)[^)]*\{$', l)
            if m:
                pre = [t for t in m.group(1).split() if t not in ATTRS and not t.startswith("#")]
                ret = pre[-1] if pre else "void"
                params = split_top(m.group(3))
                variadic = bool(params) and params[-1] == "..."
                if variadic:
                    params = params[:-1]
                self.defs[self.gname("@" + m.group(2))] = sig_of(ret, params, variadic)
            m = re.match(r'^declare .*?@([-\w.$]+)\(', l)
            if m:
                self.decls.add(m.group(1))
            m = re.match(r'^@([-\w.$]+) = (.*?)(global|constant) (.*)$', l)
            if m:
                g = self.gname("@" + m.group(1))
                rest = re.sub(r",\s*(align \d+|section .*|!.*)$", "", m.group(4))
                rest = re.sub(r",\s*align \d+.*$", "", rest)
                self.init(g, rest)
        self.funcs()

    def is_fn(self, g):
        return g[1:] in self.decls or self.gname(g) in self.defs

    # ---- global initializers
    def init(self, g, text):
        toks = lex(text)
        pos = [0]

        def peek():
            return toks[pos[0]] if pos[0] < len(toks) else ""

        def take():
            t = peek()
            pos[0] += 1
            return t

        def before(close):
            """Whether a list continues before `close` (the lexer losing its
            place is an error: the fallback below takes the initializer)."""
            t = peek()
            if t == "":
                raise ValueError("initializer of %s: no %s" % (g, close))
            return t != close

        def parse_type():
            t = take()
            if t == "[":
                take()
                take()  # N x
                el = parse_type()
                take()  # ]
                return ("arr", el)
            if t in ("{", "<{"):
                fields = []
                close = "}" if t == "{" else "}>"
                while before(close):
                    fields.append(parse_type())
                    if peek() == ",":
                        take()
                take()
                return ("lit", fields)
            return t

        def fn_edge(x, slot):
            self.edges.append((("fn", self.gname(x)), slot))
            self.edges.append((("fn", self.gname(x)), ("G", g)))

        def value(ty, slot):
            """A constant of type ty, stored to slot (the field it initializes)."""
            t = peek()
            if t in ("zeroinitializer", "null", "undef", "poison", "true", "false") or t.startswith('c"'):
                take()
                return
            if t == "[":
                take()
                while before("]"):
                    value(parse_type(), slot)
                    if peek() == ",":
                        take()
                take()
                return
            if t in ("{", "<{"):
                take()
                close = "}" if t == "{" else "}>"
                k = 0
                while before(close):
                    fty = parse_type()
                    fslot = ("F", ty, k) if isinstance(ty, str) and ty.startswith(("%struct.", "%union.")) else slot
                    value(fty, fslot)
                    if peek() == ",":
                        take()
                    k += 1
                take()
                return
            if t.startswith("@"):
                take()
                if self.is_fn(t):
                    fn_edge(t, slot)
                return
            if t in ("getelementptr", "ptrtoint", "inttoptr", "bitcast", "addrspacecast", "add", "sub", "or", "and"):
                take()
                depth = 0
                while True:
                    x = take()
                    if x == "(":
                        depth += 1
                    elif x == ")":
                        depth -= 1
                        if depth == 0:
                            break
                    elif x.startswith("@") and self.is_fn(x):
                        fn_edge(x, slot)
                    elif x == "":
                        break
                return
            take()  # a number

        try:
            value(parse_type(), ("G", g))
        except Exception:
            for x in re.findall(r"@[-\w.$]+", text):
                if self.is_fn(x):
                    self.edges.append((("fn", self.gname(x)), ("G", g)))

    # ---- function bodies
    def funcs(self):
        cur = None
        for l in self.lines:
            m = re.match(r'^define (.*?)@([-\w.$]+)\((.*)\)[^)]*\{$', l)
            if m:
                cur = self.gname("@" + m.group(2))
                self.vals = {}
                self.addr = {}
                for i, p in enumerate(split_top(m.group(3))):
                    r = re.search(r"(%[-\w.]+)$", p)
                    if r:
                        self.vals[r.group(1)] = {("slot", ("P", cur, i))}
                continue
            if l.startswith("}"):
                cur = None
                continue
            if cur is None or not l.startswith("  "):
                continue
            s = l.strip()
            md = re.search(r", !dbg !(\d+)", s)
            line = self.dbg.get(md.group(1), 0) if md else 0
            s = re.sub(r"(, ![\w.]+ !\d+)+$", "", s)
            self.inst(cur, line, s)

    def val(self, v):
        """The sources of a value operand."""
        out = set()
        for x in re.findall(r"@[-\w.$]+", v):
            if self.is_fn(x):
                out.add(("fn", self.gname(x)))
        for x in re.findall(r"%[-\w.]+", v):
            out |= self.vals.get(x, set())
        return out

    def address(self, a, cur):
        """The slots an address operand denotes."""
        a = a.strip()
        m = re.match(r"^getelementptr (?:inbounds )?(?:nuw )?(?:nusw )?\((.*)\)$", a)
        if m:
            return self.gep(split_top(m.group(1)), cur)
        if a.startswith("@"):
            return {("G", self.gname(a))}
        if a.startswith("%"):
            return self.addr.get(a, set())
        return set()

    def gep(self, parts, cur):
        t = parts[0]
        base = parts[1].split()[-1] if not parts[1].strip().startswith("ptr getelementptr") else parts[1][4:]
        bslots = self.address(base, cur)
        last = None
        for i in [p.split()[-1] for p in parts[3:]]:   # parts[2] is the pointer step
            if t in self.types:
                try:
                    k = int(i)
                except ValueError:
                    break
                last = ("F", t, k)
                t = self.types[t][k] if k < len(self.types[t]) else "?"
            elif t.startswith("["):
                m = re.match(r"\[\d+ x (.*)\]$", t)
                t = m.group(1) if m else "?"
            else:
                break
        out = set(s for s in bslots if s[0] == "G")
        if last:
            out.add(last)
        else:
            out |= bslots
        return out

    def inst(self, cur, ln, s):
        m = re.match(r"^(%[-\w.]+) = (.*)$", s)
        lhs, rhs = (m.group(1), m.group(2)) if m else (None, s)
        op = rhs.split()[0] if rhs else ""
        if op == "alloca":
            self.addr[lhs] = {("A", cur, lhs)}
        elif op == "getelementptr":
            body = split_top(re.sub(r"^getelementptr (inbounds )?(nuw )?(nusw )?", "", rhs))
            self.addr[lhs] = self.gep(body, cur)
            self.vals[lhs] = self.val(body[1])
        elif op == "store":
            body = re.sub(r", align \d+$", "", re.sub(r"^store (volatile )?", "", rhs))
            parts = split_top(body)
            v = parts[0].split(None, 1)[1] if " " in parts[0] else parts[0]
            srcs = self.val(v)
            for d in self.address(parts[1].split(None, 1)[1], cur):
                for src in srcs:
                    self.edges.append((src, d))
        elif op == "load":
            body = re.sub(r", align \d+.*$", "", re.sub(r"^load (volatile )?", "", rhs))
            a = split_top(body)[1].split(None, 1)[1]
            self.vals[lhs] = {("slot", d) for d in self.address(a, cur)}
        elif op in ("bitcast", "ptrtoint", "inttoptr", "zext", "sext", "trunc", "freeze", "addrspacecast"):
            mm = re.match(r"^\w+ \S+ (.*) to .*$", rhs)
            if mm:
                self.vals[lhs] = self.val(mm.group(1))
                self.addr[lhs] = self.addr.get(mm.group(1).strip(), set())
        elif op in ("phi", "select"):
            vs = set()
            for x in re.findall(r"%[-\w.]+|@[-\w.$]+", rhs):
                if x.startswith("@") and self.is_fn(x):
                    vs.add(("fn", self.gname(x)))
                elif x in self.vals:
                    vs |= self.vals[x]
                if x in self.addr:
                    self.addr.setdefault(lhs, set()).update(self.addr[x])
            self.vals[lhs] = vs
        elif op == "ret":
            if rhs != "ret void":
                for src in self.val(rhs[4:]):
                    self.edges.append((src, ("R", cur)))
        else:
            mc = re.match(r"^(?:tail |musttail |notail )?call (.*)$", rhs)
            if mc:
                self.call(cur, ln, lhs, mc.group(1))

    def call(self, cur, ln, lhs, body):
        mm = re.match(r"^(.*?)([@%][-\w.$]+)\((.*)\)(?:\s*#\d+)?$", body)
        if not mm:
            return
        pre, callee, args = mm.group(1), mm.group(2), split_top(mm.group(3))
        if callee.startswith(("@llvm.memcpy", "@llvm.memmove")):
            for d in self.address(args[0].split()[-1], cur):
                for sv in self.address(args[1].split()[-1], cur):
                    self.edges.append((("slot", sv), d))
            return
        if callee.startswith("@llvm."):
            return
        arg_vals = []
        for a in args:
            v = a.split()[-1] if a.split() else ""
            arg_vals.append(self.val(v))
        if callee.startswith("@"):
            g = self.gname(callee)
            for i, vs in enumerate(arg_vals):
                for src in vs:
                    self.edges.append((src, ("P", g, i)))
            if lhs:
                self.vals[lhs] = {("slot", ("R", g))}
            return
        # Indirect: the call's own function type.
        pre_t = " ".join(t for t in pre.split() if t not in ATTRS)
        mt = re.match(r"^(.*?)\s*\((.*)\)\s*$", pre_t)
        if mt and "(" in pre_t:
            ps = split_top(mt.group(2))
            var = bool(ps) and ps[-1] == "..."
            csig = sig_of(mt.group(1).split()[-1], ps[:-1] if var else ps, var)
        else:
            csig = sig_of(pre_t.split()[-1] if pre_t else "void", args, False)
        self.sites.append((cur, ln, csig, sorted(self.vals.get(callee, set()))))
        if lhs:
            self.vals[lhs] = {("slot", ("RI", self.name, ln))}


def facts(tu, ir, out):
    t = TU(tu, open(ir).read())
    with open(out + ".tmp", "w") as f:
        json.dump({"tu": tu, "defs": t.defs, "edges": t.edges, "sites": t.sites}, f)
    os.replace(out + ".tmp", out)
    return 0


def key(x):
    return json.dumps(x)


def solve(argv):
    allow = set()
    if argv[:1] == ["--allow"]:
        for ln in open(argv[1]):
            ln = ln.split("#", 1)[0].split()
            if len(ln) == 2:
                allow.add(tuple(ln))
        argv = argv[2:]
    defs, edges, sites = {}, [], []
    for p in argv:
        d = json.load(open(p))
        defs.update(d["defs"])
        edges += d["edges"]
        sites += [(d["tu"],) + tuple(s) for s in d["sites"]]
    contents, succ = {}, {}       # slot -> functions; slot -> slots it flows to
    for src, dst in edges:
        if src[0] == "fn":
            contents.setdefault(key(dst), set()).add(src[1])
        else:
            succ.setdefault(key(src[1]), set()).add(key(dst))
    work = list(contents)
    while work:
        k = work.pop()
        fs = contents.get(k, set())
        for d in succ.get(k, ()):
            cur = contents.setdefault(d, set())
            if not fs <= cur:
                cur |= fs
                work.append(d)
    hits = 0
    for tu, fn, ln, csig, srcs in sites:
        callees = set()
        for s in srcs:
            callees |= {s[1]} if s[0] == "fn" else contents.get(key(s[1]), set())
        bad = sorted((c, defs[c]) for c in callees if c in defs and defs[c] != csig
                     and (fn.split("::")[-1], c.split("::")[-1]) not in allow
                     and (fn.split("::")[-1], "*") not in allow)
        if bad:
            hits += 1
            print("hg_fpflow_lint: %s:%d: %s calls through %s; reaches %s" % (
                tu, ln, fn.split("::")[-1], csig,
                ", ".join("%s (%s)" % (c.split("::")[-1], s) for c, s in bad)))
    print("  FPFLOW  %d unexplained indirect calls reaching C of another wasm type "
          "(%d indirect call sites, %d TUs)" % (hits, len(sites), len(argv)))
    return 1 if hits else 0


def main(argv):
    if len(argv) == 5 and argv[1] == "facts":
        return facts(argv[2], argv[3], argv[4])
    if len(argv) >= 2 and argv[1] == "solve":
        return solve(argv[2:])
    sys.exit(__doc__.split("\n\n")[1])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
