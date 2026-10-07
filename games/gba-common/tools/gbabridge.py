#!/usr/bin/env python3
"""
gbabridge: place one decomp translation unit's LLVM IR at the cartridge's
addresses (wasm32 guest of a GBA game).

    gbabridge.py SYMS.json OBJNAME IN.ll OUT.ll OUT.sigs

SYMS.json comes from elfsyms.py; OBJNAME is the TU's object basename in the
ELF ("main.o"), whose FILE group holds its statics ("" for port TUs, which
have none). Input: clang -S -emit-llvm with -Xclang -disable-llvm-passes (no
function removed or inlined yet; OUT.ll is optimised when it is compiled).

The game's data lives in the player's ROM at 0x08000000 and its RAM at the
GBA addresses, because ROM tables point at RAM (sSpecialVars) and at code
(callbacks, jump tables) by address. So:

 1. A global variable with an ELF address (a static by OBJNAME's FILE group,
    a function-scope static `Func.var` by `var.N` in it, anything else by
    the ELF's globals) loses its definition (or its `external` declaration),
    and every use becomes `inttoptr (i32 addr to ptr)`. Nothing of its
    initialiser reaches the module. Sizes are compared with the ELF and a
    mismatch is reported (`M` records): it means agbcc and clang disagree
    on the layout.
 2. Every use of a function other than as a direct callee becomes its GBA
    code pointer: the ELF address (Thumb bit included), or for code the ELF
    does not have (the port's own, or a static agbcc dropped) a synthetic
    0x0F000000-0x0FFFFFFF address from the name (`static` ones salted with
    OBJNAME). Every defined function gets a word wrapper
    `i32 gw$<addr>(i32 x8)` that converts the words to its parameters, and a
    `F <addr> <name>` record, from which gen_dispatch.py builds the table
    behind gba_dispatch().
 3. An indirect call `call T %p(args)` becomes `call T @ga$<sig>(ptr %p,
    args)`, a TU-local adapter that packs the arguments into words and calls
    gba_dispatch(p, w0..w7).
 4. `load volatile` / `store volatile` of i8/i16/i32/ptr become calls to
    gba_vload{8,16,32} / gba_vstore{8,16,32}: I/O registers have side effects
    (DMA, timers, IF acknowledge, VCOUNT advancing while polled).
 5. Every function of a decomp TU starts with a call to gba_tick(), the
    machine's stand-in for CPU time: every so many calls a scanline passes,
    so interrupts arrive while the game computes, as they do on the console
    (DoMapLoadLoop spins until the VBlank handler has run its DMA queue).
"""
import hashlib
import json
import re
import sys

NAME = r'(?:[-a-zA-Z$._][-a-zA-Z$._0-9]*|"[^"]*")'
GREF = re.compile(r'@(' + NAME + r')')
LOCAL = r'%(?:[0-9]+|' + NAME + r')'
NWORDS = 8
SYNTH_BASE = 0x0F000000

PARAM_ATTRS = {"noundef", "zeroext", "signext", "nonnull", "noalias", "nocapture", "readonly",
               "writeonly", "returned", "inreg", "nest", "immarg", "noalias", "writable",
               "dead_on_unwind", "nofree", "swiftself", "noundef"}
PARAM_ATTRS_ARG = ("align", "dereferenceable", "dereferenceable_or_null", "byval", "sret",
                   "byref", "preallocated", "inalloca", "elementtype", "captures", "range",
                   "nofpclass", "initializes")


def unq(n):
    return n[1:-1] if n.startswith('"') else n


def synth(key):
    h = int.from_bytes(hashlib.sha1(key.encode()).digest()[:4], "little")
    return SYNTH_BASE | (h & 0x00FFFFFC) | 1


# ------------------------------------------------------------ IR text utils
def split_top(s, sep=","):
    parts, depth, cur, i = [], 0, [], 0
    while i < len(s):
        c = s[i]
        if c == '"':
            j = s.index('"', i + 1)
            cur.append(s[i:j + 1])
            i = j + 1
            continue
        if c in "([{<":
            depth += 1
        elif c in ")]}>":
            depth -= 1
        if c == sep and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        parts.append("".join(cur).strip())
    return parts


def match_close(s, i):
    """s[i] is an opener; index just past its closer."""
    depth = 0
    while i < len(s):
        c = s[i]
        if c == '"':
            i = s.index('"', i + 1) + 1
            continue
        if c in "([{<":
            depth += 1
        elif c in ")]}>":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced: " + s)


def read_type(s, i=0):
    """(type text, index after it) for the IR type starting at s[i]."""
    while s[i] == " ":
        i += 1
    if s[i] in "[{<":
        j = match_close(s, i)
    else:
        m = re.match(r'%' + NAME + r'|[a-z][a-z0-9]*', s[i:])
        if not m:
            raise ValueError("no type at: " + s[i:i + 40])
        j = i + m.end()
        if s[i:j] == "ptr" and s.startswith(" addrspace(", j):
            j = match_close(s, j + len(" addrspace"))
    return s[i:j], j


def parse_operand(spec):
    """'TYPE attrs... VALUE' -> (type, [attrs], value)."""
    t, j = read_type(spec)
    rest = spec[j:].strip()
    attrs = []
    while rest:
        m = re.match(r'([a-z_]+)', rest)
        if not m:
            break
        w = m.group(1)
        if w in PARAM_ATTRS:
            attrs.append(w)
            rest = rest[m.end():].strip()
        elif w in PARAM_ATTRS_ARG:
            k = m.end()
            if k < len(rest) and rest[k] == "(":
                k = match_close(rest, k)
            elif w == "align":
                k = re.match(r'align\s+\d+', rest).end()
            attrs.append(rest[:k])
            rest = rest[k:].strip()
        else:
            break
    return t, attrs, rest


class Types:
    def __init__(self):
        self.defs = {}

    def size_align(self, t):
        t = t.strip()
        if t.startswith("%"):
            body = self.defs.get(t)
            if body is None or body == "opaque":
                return None
            return self.size_align(body)
        m = re.fullmatch(r'i(\d+)', t)
        if m:
            b = int(m.group(1))
            n = 1 if b <= 8 else 2 if b <= 16 else 4 if b <= 32 else 8 if b <= 64 else 16
            return n, n
        if t == "ptr" or t.startswith("ptr "):
            return 4, 4
        if t == "float":
            return 4, 4
        if t == "double":
            return 8, 8
        if t.startswith("["):
            m = re.match(r'\[\s*(\d+)\s+x\s+(.*)\]$', t, re.S)
            sa = self.size_align(m.group(2))
            return None if sa is None else (int(m.group(1)) * sa[0], sa[1])
        packed = t.startswith("<{")
        if t.startswith("{") or packed:
            inner = t[2:-2] if packed else t[1:-1]
            off, al = 0, 1
            for f in split_top(inner):
                sa = self.size_align(f)
                if sa is None:
                    return None
                a = 1 if packed else sa[1]
                off = (off + a - 1) // a * a + sa[0]
                al = max(al, a)
            off = (off + al - 1) // al * al
            return off, al
        return None


# ------------------------------------------------------------------ bridge
class Bridge:
    def __init__(self, syms, objname):
        self.g = syms["globals"]
        self.loc = syms["locals"].get(objname, {}) if objname else {}
        self.locals_all = syms["locals"]
        self.obj = objname
        self.types = Types()
        self.adapters = {}
        self.records = []
        self.cur_func = "?"
        self.site_serial = 0
        self.site_base = (int.from_bytes(hashlib.sha1((objname or "port").encode()).digest()[:2], "little") & 0xFFF) << 20

    def data_addr(self, name, internal):
        if name.startswith("__gba_local__"):
            # port code naming a decomp static: __gba_local__<file>__<name>
            f, _, n = name[len("__gba_local__"):].partition("__")
            return self.locals_all.get(f + ".o", {}).get(n)
        if "." in name and not name.startswith("."):
            # function-scope static Func.var -> var.N in this object
            var = name.split(".", 1)[1]
            hits = [v for k, v in self.loc.items() if v[2] == "O" and re.fullmatch(re.escape(var) + r"\.\d+", k)]
            if len(hits) == 1:
                return hits[0]
            if len(hits) > 1:
                # several functions have a static of this name: agbcc
                # numbers them in source order, which is also the order
                # clang emits them (self.static_rank, set by run())
                peers = self.static_peers.get(var, [])
                cands = sorted((int(k.rsplit(".", 1)[1]), v) for k, v in self.loc.items()
                               if v[2] == "O" and re.fullmatch(re.escape(var) + r"\.\d+", k))
                if len(peers) == len(cands):
                    return cands[peers.index(name)][1]
                self.records.append(f"A {name} {len(hits)}")
            return None
        if name.startswith(".") or name.startswith("__const") or name.startswith("llvm."):
            return None
        if internal:
            return self.loc.get(name)
        return self.g.get(name)

    def func_addr(self, name, internal):
        v = (self.loc if internal else self.g).get(name)
        if v and v[2] == "F":
            return v[0]
        return synth((self.obj + ":" if internal else "") + name)

    def run(self, lines):
        # pass A: types, globals, functions
        gdefs = {}   # name -> (line index, internal, type or None)
        fdefs = {}   # name -> (line index, internal)
        fdecls = set()
        for i, ln in enumerate(lines):
            if ln.startswith("%") and " = type " in ln:
                k, v = ln.split(" = type ", 1)
                self.types.defs[k] = v.strip()
            elif ln.startswith("@"):
                m = re.match(r'@(' + NAME + r') = (.*)$', ln)
                if not m:
                    continue
                name, rest = unq(m.group(1)), m.group(2)
                if re.match(r'(?:[a-z_]+ )*alias ', rest) or re.match(r'(?:[a-z_]+ )*ifunc ', rest):
                    continue
                internal = bool(re.match(r'(internal|private) ', rest))
                km = re.search(r'\b(global|constant) ', rest)
                ty = None
                if km and not rest.startswith("external "):
                    try:
                        ty, _ = read_type(rest, km.end())
                    except ValueError:
                        ty = None
                gdefs[name] = (i, internal, ty, rest.startswith("external "))
            elif ln.startswith("define "):
                m = re.search(r'@(' + NAME + r')\(', ln)
                internal = bool(re.match(r'define (internal|private) ', ln))
                fdefs[unq(m.group(1))] = (i, internal)
            elif ln.startswith("declare "):
                m = re.search(r'@(' + NAME + r')\(', ln)
                fdecls.add(unq(m.group(1)))

        self.static_peers = {}
        for name, (i, _int, _ty, _ext) in sorted(gdefs.items(), key=lambda kv: kv[1][0]):
            if "." in name and not name.startswith("."):
                self.static_peers.setdefault(name.split(".", 1)[1], []).append(name)
        # pass B: addresses
        dmap, drop = {}, set()
        for name, (i, internal, ty, external) in gdefs.items():
            v = self.data_addr(name, internal)
            if v is None:
                if not external and not internal and not name.startswith(("llvm.", ".", "__")):
                    self.records.append(f"U {name}")
                continue
            dmap[name] = v[0]
            drop.add(i)
            if ty is not None and v[1]:
                sa = self.types.size_align(ty)
                if sa and sa[0] != v[1]:
                    self.records.append(f"M {name} {sa[0]} {v[1]}")
        fmap = {}
        for name, (i, internal) in fdefs.items():
            if name.startswith("llvm."):
                continue
            fmap[name] = self.func_addr(name, internal)
        for name in fdecls:
            if not name.startswith("llvm.") and name not in fmap:
                fmap[name] = self.func_addr(name, False)

        # pass C: rewrite
        out = []
        fsig = {}  # defined function -> (ret, [(type, attrs)], variadic)
        for i, ln in enumerate(lines):
            if i in drop:
                continue
            if ln.startswith("define ") or ln.startswith("declare "):
                if ln.startswith("define "):
                    name = unq(re.search(r'@(' + NAME + r')\(', ln).group(1))
                    fsig[name] = self.parse_define(ln)
                    self.cur_func = name
                    out.append(ln)
                    if self.obj and ln.rstrip().endswith("{"):
                        out.append("  call void @gba_tick()")
                    continue
                out.append(ln)
                continue
            if ln.startswith("@") and re.match(r'@' + NAME + r' = (?:[a-z_]+ )*(?:alias|ifunc) ', ln):
                out.append(ln)  # aliases name a symbol, never an address
                continue
            if ln.startswith("@llvm.used") or ln.startswith("@llvm.compiler.used"):
                ln = self.fix_used(ln, dmap, fmap)
                if ln is None:
                    continue
            if ln.startswith("@") and ' section "' in ln:
                ln = re.sub(r', section "[^"]*"', "", ln)
            if "@" in ln:
                ln = self.subst_refs(ln, dmap, fmap)
            if " volatile " in ln:
                ln = self.volatile(ln)
            if " call " in ln or ln.lstrip().startswith("call "):
                ln = self.indirect(ln)
            out.append(ln)

        # wrappers + adapters + records
        seen = set()
        for name, (ret, params, variadic) in fsig.items():
            if name.startswith("llvm.") or variadic or len(params) > NWORDS:
                continue
            addr = fmap[name]
            if addr in seen:  # the ELF names one function twice
                continue
            seen.add(addr)
            out.extend(self.wrapper(name, addr, ret, params))
            self.records.append(f"F {addr:#010x} {name}")
        out.extend(self.adapters.values())
        have = set(fdefs) | fdecls
        if not any(l.startswith("@gba_icall_site ") for l in lines):
            out.append("@gba_icall_site = external global i32, align 4")
        if self.obj and "gba_tick" not in have:
            out.append("declare void @gba_tick()")
        if "gba_dispatch" not in have:
            out.append("declare i32 @gba_dispatch(i32, i32, i32, i32, i32, i32, i32, i32, i32)")
        for w in (8, 16, 32):
            if f"gba_vload{w}" not in have:
                out.append(f"declare i{w} @gba_vload{w}(ptr)")
            if f"gba_vstore{w}" not in have:
                out.append(f"declare void @gba_vstore{w}(ptr, i{w})")
        return out

    def parse_define(self, ln):
        m = re.search(r'@(' + NAME + r')\(', ln)
        head = ln[:m.start()]
        # return type: last type before the name, after linkage/attrs
        rt = re.sub(r'^define\s+', "", head).strip()
        toks = rt.split()
        # strip leading keywords until a type parses at the end
        ret = toks[-1] if toks else "void"
        if rt.endswith("}"):
            ret = rt[rt.rindex("{"):]
        pstart = m.end() - 1
        pend = match_close(ln, pstart)
        params, variadic = [], False
        for p in split_top(ln[pstart + 1:pend - 1]):
            if p == "...":
                variadic = True
                continue
            t, attrs, _ = parse_operand(p)
            params.append((t, attrs))
        return ret, params, variadic

    def fix_used(self, ln, dmap, fmap):
        m = re.match(r'(@llvm\.(?:compiler\.)?used = appending global )\[\d+ x ptr\] \[(.*)\](.*)$', ln)
        if not m:
            return ln
        keep = [e for e in split_top(m.group(2)) if unq(e.split("@", 1)[1]) not in dmap and unq(e.split("@", 1)[1]) not in fmap]
        if not keep:
            return None
        return f"{m.group(1)}[{len(keep)} x ptr] [{', '.join(keep)}]{m.group(3)}"

    def subst_refs(self, ln, dmap, fmap):
        def repl(m):
            name = unq(m.group(1))
            if name in dmap:
                return f"inttoptr (i32 {dmap[name]} to ptr)"
            if name in fmap:
                # direct callee: `call <ret> @f(` with nothing but the
                # return type between `call` and the name
                after = ln[m.end():m.end() + 1]
                before = ln[:m.start()]
                k = before.rfind("call ")
                head = re.sub(r'%(?:struct|union)\.[\w.]+', "", before[k:]) if k >= 0 else "@"
                if after == "(" and "@" not in head and "%" not in head:
                    return m.group(0)
                return f"inttoptr (i32 {fmap[name]} to ptr)"
            return m.group(0)
        return GREF.sub(repl, ln)

    def volatile(self, ln):
        m = re.match(r'(\s*' + LOCAL + r') = load volatile (i8|i16|i32|ptr), (.*)$', ln)
        if m:
            parts = split_top(m.group(3))
            t, _, ptrv = parse_operand(parts[0])
            ty = m.group(2)
            if ty == "ptr":
                tmp = "%vp." + m.group(1).strip()[1:]
                return (f"{tmp} = call i32 @gba_vload32(ptr {ptrv})\n"
                        f"{m.group(1)} = inttoptr i32 {tmp} to ptr")
            return f"{m.group(1)} = call {ty} @gba_vload{ty[1:]}(ptr {ptrv})"
        m = re.match(r'(\s*)store volatile (.*)$', ln)
        if m:
            parts = split_top(m.group(2))
            vt, _, vv = parse_operand(parts[0])
            _, _, ptrv = parse_operand(parts[1])
            if vt in ("i8", "i16", "i32"):
                return f"{m.group(1)}call void @gba_vstore{vt[1:]}(ptr {ptrv}, {vt} {vv})"
            if vt == "ptr":
                return f"{m.group(1)}call void @gba_vstore32(ptr {ptrv}, i32 ptrtoint (ptr {vv} to i32))" \
                    if not vv.startswith("%") else \
                    f"{m.group(1)}%vs.{abs(hash(ln)) & 0xFFFFFF} = ptrtoint ptr {vv} to i32\n" \
                    f"{m.group(1)}call void @gba_vstore32(ptr {ptrv}, i32 %vs.{abs(hash(ln)) & 0xFFFFFF})"
        return ln

    def indirect(self, ln):
        m = re.match(r'(\s*(?:' + LOCAL + r' = )?(?:tail |musttail |notail )?call )(.*)$', ln)
        if not m:
            return ln
        prefix, rest = m.group(1), m.group(2)
        # callee: first `%x(` or `inttoptr (...)(` at depth 0
        cm = re.search(r'(' + LOCAL + r')\(', rest)
        im = rest.find("inttoptr (")
        if im >= 0 and (cm is None or im < cm.start()):
            cend = match_close(rest, im + len("inttoptr "))
            if cend >= len(rest) or rest[cend] != "(":
                return ln
            callee, cstart, astart = rest[im:cend], im, cend
        elif cm:
            callee, cstart, astart = cm.group(1), cm.start(), cm.end() - 1
        else:
            return ln
        head = rest[:cstart].strip()
        if "@" in head:
            return ln
        aend = match_close(rest, astart)
        args = split_top(rest[astart + 1:aend - 1])
        tail = rest[aend:]
        # head: [cconv] [ret attrs] ret-type [fn type]
        if head.endswith(")") and "(" in head:  # explicit function type: variadic
            raise SystemExit(f"gbabridge: variadic indirect call not supported: {ln.strip()}")
        rattrs = []
        htoks = head
        while True:
            mm = re.match(r'(noundef|zeroext|signext|nonnull|noalias|fastcc|ccc|align \d+|dereferenceable\(\d+\)) ', htoks)
            if not mm:
                break
            rattrs.append(mm.group(1))
            htoks = htoks[mm.end():]
        ret = htoks.strip()
        ops = [parse_operand(a) for a in args]
        sig = self.sig(ret, rattrs, [(t, at) for t, at, _ in ops])
        name = "ga$" + sig
        if name not in self.adapters:
            self.adapters[name] = self.adapter(name, ret, rattrs, [(t, at) for t, at, _ in ops])
        # record the call site for gba_bad_call's report (S records)
        self.site_serial += 1
        site = (self.site_base | self.site_serial) & 0xFFFFFFFF
        self.records.append(f"S {site:#010x} {self.cur_func}")
        indent = re.match(r'\s*', ln).group(0)
        store = f"{indent}store i32 {site - (1 << 32) if site >= 1 << 31 else site}, ptr @gba_icall_site, align 4\n"
        newargs = ", ".join([f"ptr {callee}"] + [f"{t} {' '.join(at) + ' ' if at else ''}{v}" for t, at, v in ops])
        return f"{store}{prefix}{' '.join(rattrs) + ' ' if rattrs else ''}{ret} @\"{name}\"({newargs}){tail}"

    @staticmethod
    def sig(ret, rattrs, params):
        def tok(t, attrs):
            s = "sx" if "signext" in attrs else ""
            if any(a.startswith(("byval", "sret")) for a in attrs):
                s += "b"
            return re.sub(r'[^A-Za-z0-9]', "_", t) + s
        return tok(ret, rattrs) + "_" + "_".join(tok(t, a) for t, a in params)

    @staticmethod
    def to_word(t, attrs, v, tmp, out):
        if t == "i32":
            return v
        if t in ("i1", "i8", "i16"):
            op = "sext" if "signext" in attrs else "zext"
            out.append(f"  {tmp} = {op} {t} {v} to i32")
        elif t == "ptr":
            out.append(f"  {tmp} = ptrtoint ptr {v} to i32")
        elif t == "i64":
            out.append(f"  {tmp} = trunc i64 {v} to i32")
        elif t == "float":
            out.append(f"  {tmp} = bitcast float {v} to i32")
        else:
            raise SystemExit(f"gbabridge: cannot pass {t} through gba_dispatch")
        return tmp

    @staticmethod
    def from_word(t, attrs, w, tmp, out):
        if t == "i32":
            return w
        if t in ("i1", "i8", "i16"):
            out.append(f"  {tmp} = trunc i32 {w} to {t}")
        elif t == "ptr":
            out.append(f"  {tmp} = inttoptr i32 {w} to ptr")
        elif t == "i64":
            out.append(f"  {tmp} = zext i32 {w} to i64")
        elif t == "float":
            out.append(f"  {tmp} = bitcast i32 {w} to float")
        else:
            raise SystemExit(f"gbabridge: cannot return {t} through gba_dispatch")
        return tmp

    def adapter(self, name, ret, rattrs, params):
        if len(params) > NWORDS:
            raise SystemExit(f"gbabridge: indirect call with {len(params)} arguments")
        body = []
        ps = ", ".join(["ptr %fp"] + [f"{t} %p{k}" for k, (t, _) in enumerate(params)])
        body.append(f"define internal {ret} @\"{name}\"({ps}) {{")
        body.append("  %fpw = ptrtoint ptr %fp to i32")
        words = []
        for k, (t, at) in enumerate(params):
            words.append(self.to_word(t, at, f"%p{k}", f"%w{k}", body))
        words += ["0"] * (NWORDS - len(words))
        body.append("  %r = call i32 @gba_dispatch(i32 %fpw, " + ", ".join(f"i32 {w}" for w in words) + ")")
        if ret == "void":
            body.append("  ret void")
        else:
            r = self.from_word(ret, rattrs, "%r", "%rv", body)
            body.append(f"  ret {ret} {r}")
        body.append("}")
        return "\n".join(body)

    def wrapper(self, name, addr, ret, params):
        body = [f"define hidden i32 @\"gw${addr:08x}\"(" + ", ".join(f"i32 %w{k}" for k in range(NWORDS)) + ") {"]
        args = []
        for k, (t, at) in enumerate(params):
            v = self.from_word(t, at, f"%w{k}", f"%a{k}", body)
            keep = [a for a in at if a.startswith(("byval", "sret", "zeroext", "signext"))]
            args.append(f"{t} {' '.join(keep) + ' ' if keep else ''}{v}")
        callee = '@"' + name + '"' if not re.fullmatch(r'[-a-zA-Z$._][-a-zA-Z$._0-9]*', name) else "@" + name
        if ret == "void":
            body.append(f"  call void {callee}({', '.join(args)})")
            body.append("  ret i32 0")
        else:
            body.append(f"  %r = call {ret} {callee}({', '.join(args)})")
            r = self.to_word(ret, [], "%r", "%rw", body)
            body.append(f"  ret i32 {r}")
        body.append("}")
        return body


def main():
    if len(sys.argv) != 6:
        raise SystemExit(__doc__)
    syms = json.load(open(sys.argv[1]))
    b = Bridge(syms, sys.argv[2])
    lines = open(sys.argv[3], encoding="latin-1").read().split("\n")
    out = b.run(lines)
    open(sys.argv[4], "w", encoding="latin-1").write("\n".join(out) + "\n")
    open(sys.argv[5], "w").write("\n".join(b.records) + ("\n" if b.records else ""))


if __name__ == "__main__":
    main()
