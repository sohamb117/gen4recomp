#!/usr/bin/env python3
"""
irbridge: the IR half of the typed C <-> recompiled bridge (wasm32 only).

Usage:
    irbridge.py --classes classes.txt [--tu-id ID] IN.ll OUT.ll OUT.sigs

On i386 decompiled C and armrec's recompiled C called each other with
mismatched prototypes and cdecl absorbed the difference. wasm calls are typed,
so every crossing has to be made explicit. This rewrites one translation
unit's textual LLVM IR (clang -S -emit-llvm, opaque pointers) against the
classification armrec produces (classes.txt: `F name 0xaddr` for every
function a .s defines, `D name 0xaddr` for every asm data label):

 1. A direct call to an asm function F (not defined in this TU) becomes a call
    to `aw$<sig>$F`, a generated wrapper with exactly the call site's types
    that marshals them into r0..r3 + emulated-stack words and calls F.
 2. Any other use of an asm function `@F` becomes its guest address,
    `inttoptr (i32 0xaddr|thumb to ptr)`, which is a constant and so is valid
    in a global initializer too.
 3. Any use of an asm data label `@D` likewise becomes its guest address; its
    `external global` declaration is dropped.
 4. An indirect call `call T %p(args)` becomes `call T @ai$<sig>(ptr %p,
    args)`; the generated adapter calls p natively when it is a wasm table
    index (< 0x00100000) and dispatches it as a guest code address otherwise.
 5. The TU's defined functions and data, every C function whose address it
    takes, and the wrappers/adapters it needs are written to OUT.sigs, from
    which gen_bridge.py generates the C side. An address-taken function with
    internal linkage gets a hidden alias `ab$<tu-id>$<name>` so the generated
    table can name it.

Intrinsics (`@llvm.*`), inline asm and calls to functions defined in this TU
are never touched.

Signature tokens (one per IR value, see sig_token()):
    v void   b i1   h/a/c i8 zeroext/signext/plain   t/s/w i16 likewise
    i i32    l i64  p ptr   f float   d double
    y<N> byval aggregate of N bytes     r<N> sret aggregate of N bytes
A signature is `<ret>_<params>`, plus `_v<n>` for a variadic function type
whose first n parameters are fixed (only indirect calls and definitions
carry it: a direct call into recompiled code marshals the call site's words
whatever the prototype says).

.sigs records (one per line, space separated):
    D <name> <sig> <strong|weak>   a function this TU defines (external linkage)
    G <name>                       data this TU defines (external linkage)
    T <sym> <sig> <name>           a C function whose address this TU takes;
                                   <sym> is the alias for an internal one
    W <sig> <name>                 aw$ wrapper this TU calls
    I <sig>                        ai$ adapter this TU calls
    E <name> <sig>                 a non-asm function this TU only declares
"""

import argparse
import hashlib
import os
import re
import sys

FNPTR_LIMIT = 0x00100000

# ---------------------------------------------------------------------------
# Lexing
# ---------------------------------------------------------------------------

OPEN = {"(": ")", "[": "]", "{": "}", "<": ">"}
UNQUOTED_NAME = re.compile(r"[-A-Za-z$._][-A-Za-z0-9$._]*|[0-9]+")
WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_.]*")


class IRError(Exception):
    pass


def skip_string(s, i):
    """s[i] == '"': index just past the closing quote (IR strings use \\XX
    hex escapes, so a backslash never precedes the closing quote)."""
    j = s.index('"', i + 1)
    return j + 1


def match_close(s, i):
    """s[i] is an opener; index just past its matching closer."""
    stack = [OPEN[s[i]]]
    j = i + 1
    n = len(s)
    while j < n:
        c = s[j]
        if c == '"':
            j = skip_string(s, j)
            continue
        if c in OPEN:
            # `<` only opens a vector/packed-struct type; `->` never occurs.
            stack.append(OPEN[c])
        elif c == stack[-1]:
            stack.pop()
            if not stack:
                return j + 1
        elif c in ")]}>":
            raise IRError("unbalanced %r at %d in: %s" % (c, j, s))
        j += 1
    raise IRError("unterminated %r in: %s" % (s[i], s))


def skip_ws(s, i):
    n = len(s)
    while i < n and s[i] in " \t":
        i += 1
    return i


def split_top(s, sep=","):
    """Split at top-level separators (outside brackets and strings)."""
    out = []
    depth = 0
    start = 0
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == '"':
            i = skip_string(s, i)
            continue
        if c in "([{<":
            depth += 1
        elif c in ")]}>":
            depth -= 1
        elif c == sep and depth == 0:
            out.append(s[start:i])
            start = i + 1
        i += 1
    tail = s[start:]
    if tail.strip() or out:
        out.append(tail)
    return [x.strip() for x in out]


def unescape_name(q):
    """The body of a quoted IR name -> the symbol it spells."""
    return re.sub(r"\\([0-9A-Fa-f]{2})", lambda m: chr(int(m.group(1), 16)), q)


def ir_global(name):
    """A symbol -> its IR spelling."""
    if re.fullmatch(r"[-A-Za-z$._][-A-Za-z0-9$._]*", name):
        return "@" + name
    esc = "".join(c if (c.isprintable() and c not in '"\\') else "\\%02X" % ord(c)
                  for c in name)
    return '@"%s"' % esc


def parse_global_name(s, i):
    """s[i] == '@': (symbol, end)."""
    j = i + 1
    if j < len(s) and s[j] == '"':
        k = skip_string(s, j)
        return unescape_name(s[j + 1:k - 1]), k
    m = UNQUOTED_NAME.match(s, j)
    if not m:
        raise IRError("bad global name at %d: %s" % (i, s))
    return m.group(0), m.end()


def global_refs(s, start=0):
    """Every `@name` reference in s[start:] outside strings and comments:
    [(begin, end, symbol)]."""
    out = []
    i = start
    n = len(s)
    while i < n:
        c = s[i]
        if c == '"':
            i = skip_string(s, i)
        elif c == ";":
            break
        elif c == "@":
            name, j = parse_global_name(s, i)
            out.append((i, j, name))
            i = j
        elif c == "%" and i + 1 < n and s[i + 1] == '"':
            i = skip_string(s, i + 1)
        else:
            i += 1
    return out


# ---------------------------------------------------------------------------
# Types
# ---------------------------------------------------------------------------

SIMPLE_TYPES = {"void", "half", "bfloat", "float", "double", "fp128",
                "x86_fp80", "ppc_fp128", "label", "metadata", "token",
                "x86_amx", "ptr"}


def parse_type(s, i):
    """A type starting at s[i] (after whitespace): (text, end). Does not
    consume a trailing function-type parameter list."""
    i = skip_ws(s, i)
    if i >= len(s):
        raise IRError("expected a type: %s" % s)
    c = s[i]
    if c in "{[<":
        j = match_close(s, i)
        return s[i:j], j
    if c == "%":
        if s[i + 1] == '"':
            j = skip_string(s, i + 1)
        else:
            m = UNQUOTED_NAME.match(s, i + 1)
            j = m.end()
        return s[i:j], j
    m = WORD.match(s, i)
    if not m:
        raise IRError("expected a type at %d: %s" % (i, s))
    w = m.group(0)
    j = m.end()
    if re.fullmatch(r"i\d+", w) or w in SIMPLE_TYPES:
        if w == "ptr":
            k = skip_ws(s, j)
            if s.startswith("addrspace(", k):
                j = match_close(s, k + len("addrspace"))
        return s[i:j], j
    if w == "target" and j < len(s) and s[j] == "(":
        j = match_close(s, j)
        return s[i:j], j
    raise IRError("unknown type %r in: %s" % (w, s))


class Layout(object):
    """Aggregate sizes under the wasm32 datalayout
    (e-m:e-p:32:32-i64:64-i128:128-n32:64-S128)."""

    def __init__(self, typedefs):
        self.typedefs = typedefs   # "%struct.X" -> body text
        self.memo = {}

    def size_align(self, t):
        t = t.strip()
        if t in self.memo:
            return self.memo[t]
        r = self._size_align(t)
        self.memo[t] = r
        return r

    def _size_align(self, t):
        m = re.fullmatch(r"i(\d+)", t)
        if m:
            bits = int(m.group(1))
            if bits <= 8:
                return 1, 1
            if bits <= 16:
                return 2, 2
            if bits <= 32:
                return 4, 4
            if bits <= 64:
                return 8, 8
            return ((bits + 127) // 128) * 16, 16
        if t == "ptr" or t.startswith("ptr "):
            return 4, 4
        if t in ("float",):
            return 4, 4
        if t in ("double",):
            return 8, 8
        if t in ("half", "bfloat"):
            return 2, 2
        if t in ("fp128",):
            return 16, 16
        if t.startswith("%"):
            body = self.typedefs.get(t)
            if body is None or body == "opaque":
                raise IRError("size of undefined or opaque type %s" % t)
            return self.size_align(body)
        if t.startswith("<{"):
            size = 0
            for e in split_top(t[2:-2]):
                size += self.size_align(e)[0]
            return size, 1
        if t.startswith("{"):
            size, align = 0, 1
            for e in split_top(t[1:-1]):
                es, ea = self.size_align(e)
                size = (size + ea - 1) // ea * ea + es
                align = max(align, ea)
            return (size + align - 1) // align * align, align
        if t.startswith("["):
            m = re.fullmatch(r"\[\s*(\d+)\s+x\s+(.*)\]", t, re.S)
            n, et = int(m.group(1)), m.group(2)
            es, ea = self.size_align(et)
            return n * es, ea
        if t.startswith("<"):
            m = re.fullmatch(r"<\s*(\d+)\s+x\s+(.*)>", t, re.S)
            n, et = int(m.group(1)), m.group(2)
            es = self.size_align(et)[0] * n
            a = 1
            while a < es:
                a *= 2
            return es, a
        raise IRError("cannot size type %s" % t)


# ---------------------------------------------------------------------------
# Attributes and parameters
# ---------------------------------------------------------------------------

# Words that may precede the value (call args) or follow the type (params).
PARAM_ATTRS = set("""
    zeroext signext noext inreg byval byref preallocated inalloca sret
    elementtype align noalias captures nocapture nofree nest returned nonnull
    dereferenceable dereferenceable_or_null swiftself swiftasync swifterror
    immarg noundef nofpclass alignstack allocalign allocptr readnone readonly
    writeonly writable dead_on_unwind dead_on_return initializes range
""".split())
# Words between `call` and the return type, and before a define's type.
PRE_TYPE_WORDS = PARAM_ATTRS | set("""
    tail musttail notail call
    nnan ninf nsz arcp contract afn reassoc fast
    ccc fastcc coldcc cc webkit_jscc anyregcc preserve_mostcc preserve_allcc
    preserve_nonecc cxx_fast_tlscc swiftcc swifttailcc tailcc cfguard_checkcc
    private internal available_externally linkonce weak common appending
    extern_weak linkonce_odr weak_odr external
    default hidden protected dllimport dllexport
    dso_local dso_preemptable thread_local unnamed_addr local_unnamed_addr
    addrspace
""".split())


def skip_attr_words(s, i, words, attrs=None):
    """Consume attribute-like words (with a parenthesized argument or the
    number after `align`/`cc`/`alignstack`) from s[i]. Records name -> raw
    argument in `attrs`. Returns the new index."""
    while True:
        i = skip_ws(s, i)
        m = WORD.match(s, i)
        if not m or m.group(0) not in words:
            return i
        w = m.group(0)
        j = m.end()
        arg = None
        k = skip_ws(s, j)
        if k < len(s) and s[k] == "(" and w != "call":
            e = match_close(s, k)
            arg = s[k + 1:e - 1].strip()
            j = e
        elif w in ("align", "cc", "alignstack", "dereferenceable"):
            mm = re.compile(r"\d+").match(s, k)
            if mm:
                arg = mm.group(0)
                j = mm.end()
        if attrs is not None:
            attrs[w] = arg
        i = j


class Param(object):
    __slots__ = ("type", "attrs", "value", "text")

    def __init__(self, type_, attrs, value, text):
        self.type = type_
        self.attrs = attrs
        self.value = value
        self.text = text


def parse_param(text):
    """`<type> <attrs> [value]` -> Param. `...` -> None."""
    text = text.strip()
    if text == "...":
        return None
    t, j = parse_type(text, 0)
    attrs = {}
    j = skip_attr_words(text, j, PARAM_ATTRS, attrs)
    return Param(t, attrs, text[j:].strip(), text)


class Sig(object):
    """A function signature in IR terms: return type/attrs, params, varargs."""

    def __init__(self, ret, ret_attrs, params, nfixed=None):
        self.ret = ret
        self.ret_attrs = ret_attrs
        self.params = params          # [Param]
        self.nfixed = nfixed          # None, or count of fixed params


def int_token(attrs, kinds):
    if "zeroext" in attrs:
        return kinds[0]
    if "signext" in attrs:
        return kinds[1]
    return kinds[2]


def sig_token(t, attrs, layout, where):
    if "sret" in attrs:
        return "r%d" % layout.size_align(attrs["sret"])[0]
    if "byval" in attrs:
        return "y%d" % layout.size_align(attrs["byval"])[0]
    if t == "void":
        return "v"
    if t == "i1":
        return "b"
    if t == "i8":
        return int_token(attrs, "hac")
    if t == "i16":
        return int_token(attrs, "tsw")
    if t == "i32":
        return "i"
    if t == "i64":
        return "l"
    if t == "ptr":
        return "p"
    if t == "float":
        return "f"
    if t == "double":
        return "d"
    raise IRError("%s: no bridge token for IR type %r" % (where, t))


def mangle(sig, layout, where, variadic_mark):
    r = sig_token(sig.ret, sig.ret_attrs, layout, where)
    ps = "".join(sig_token(p.type, p.attrs, layout, where) for p in sig.params)
    s = "%s_%s" % (r, ps)
    if variadic_mark and sig.nfixed is not None:
        s += "_v%d" % sig.nfixed
    return s


# ---------------------------------------------------------------------------
# Function headers (define/declare)
# ---------------------------------------------------------------------------

class FuncHeader(object):
    __slots__ = ("kind", "name", "linkage", "sig", "name_span")


LINKAGES = ("private", "internal", "available_externally", "linkonce", "weak",
            "common", "appending", "extern_weak", "linkonce_odr", "weak_odr",
            "external")


def parse_func_header(line):
    """A `define`/`declare` line -> FuncHeader."""
    m = re.match(r"\s*(define|declare)\b", line)
    kind = m.group(1)
    i = m.end()
    linkage = "external"
    ret_attrs = {}
    # Words before the type: linkage, visibility, cconv, return attributes.
    while True:
        i = skip_ws(line, i)
        mw = WORD.match(line, i)
        if not mw or mw.group(0) not in PRE_TYPE_WORDS:
            break
        w = mw.group(0)
        if w in LINKAGES:
            linkage = w
        before = i
        i = skip_attr_words(line, i, {w}, ret_attrs)
        if i == before:
            break
    ret, i = parse_type(line, i)
    i = skip_ws(line, i)
    if line[i] != "@":
        raise IRError("expected function name: %s" % line)
    name, j = parse_global_name(line, i)
    span = (i, j)
    if line[j] != "(":
        raise IRError("expected parameter list: %s" % line)
    e = match_close(line, j)
    params = []
    nfixed = None
    for p in split_top(line[j + 1:e - 1]):
        pp = parse_param(p)
        if pp is None:
            nfixed = len(params)
        else:
            params.append(pp)
    h = FuncHeader()
    h.kind = kind
    h.name = name
    h.linkage = linkage
    h.sig = Sig(ret, ret_attrs, params, nfixed)
    h.name_span = span
    return h


# ---------------------------------------------------------------------------
# Calls
# ---------------------------------------------------------------------------

CALL_RE = re.compile(r"^(\s*(?:%(?:\"[^\"]*\"|[-A-Za-z0-9$._]+)\s*=\s*)?)"
                     r"((?:(?:tail|musttail|notail)\s+)?)call\b")


class Call(object):
    __slots__ = ("prefix", "tailkind", "ret", "ret_attrs", "pre_text",
                 "fnty_params", "callee", "callee_span", "args", "args_span",
                 "rest")


def parse_call(line):
    """A `call` instruction -> Call, or None if the line is not one (or the
    callee is inline asm)."""
    m = CALL_RE.match(line)
    if not m:
        return None
    c = Call()
    c.prefix = m.group(1)
    c.tailkind = m.group(2).strip()
    i = m.end()
    ret_attrs = {}
    i = skip_attr_words(line, i, PRE_TYPE_WORDS, ret_attrs)
    pre_start = m.end()
    ret, j = parse_type(line, i)
    c.pre_text = line[pre_start:i]
    c.ret = ret
    c.ret_attrs = ret_attrs
    j = skip_ws(line, j)
    c.fnty_params = None
    if line[j] == "(":
        e = match_close(line, j)
        c.fnty_params = line[j + 1:e - 1]
        j = skip_ws(line, e)
    if line.startswith("addrspace(", j):
        j = skip_ws(line, match_close(line, j + len("addrspace")))
    if re.match(r"asm\b", line[j:]):
        return None
    k = j
    if line[j] == "@":
        name, k = parse_global_name(line, j)
        c.callee = ("global", name)
    elif line[j] == "%":
        if line[j + 1] == '"':
            k = skip_string(line, j + 1)
        else:
            k = UNQUOTED_NAME.match(line, j + 1).end()
        c.callee = ("local", line[j:k])
    else:
        # A constant expression: keywords then one parenthesized operand
        # list, e.g. `inttoptr (i32 33556480 to ptr)`.
        while True:
            k = skip_ws(line, k)
            if line[k] == "(":
                k = match_close(line, k)
                break
            mw = WORD.match(line, k)
            if not mw:
                raise IRError("cannot parse callee: %s" % line)
            k = mw.end()
        c.callee = ("const", line[j:k].strip())
    c.callee_span = (j, k)
    a = skip_ws(line, k)
    if line[a] != "(":
        raise IRError("expected call arguments: %s" % line)
    e = match_close(line, a)
    c.args_span = (a, e)
    c.args = [parse_param(x) for x in split_top(line[a + 1:e - 1])]
    c.rest = line[e:]
    return c


def call_sig(c):
    """The call site's signature. nfixed comes from an explicit variadic
    function type."""
    nfixed = None
    if c.fnty_params is not None:
        ps = split_top(c.fnty_params)
        if ps and ps[-1] == "...":
            nfixed = len(ps) - 1
    return Sig(c.ret, c.ret_attrs, list(c.args), nfixed)


# Attributes the wrapper declarations keep: the ones that change the wasm
# lowering or the value's meaning.
KEEP_DECL_ATTRS = ("zeroext", "signext", "byval", "sret", "inreg")


def decl_param(p):
    out = [p.type]
    for a in KEEP_DECL_ATTRS:
        if a in p.attrs:
            out.append(a if p.attrs[a] is None else "%s(%s)" % (a, p.attrs[a]))
    if ("byval" in p.attrs or "sret" in p.attrs) and "align" in p.attrs:
        out.append("align %s" % p.attrs["align"])
    return " ".join(out)


def decl_ret(sig):
    out = [a for a in ("zeroext", "signext") if a in sig.ret_attrs]
    out.append(sig.ret)
    return " ".join(out)


# ---------------------------------------------------------------------------
# The rewrite
# ---------------------------------------------------------------------------

def load_classes(path):
    funcs, data = {}, {}
    with open(path) as f:
        for ln in f:
            parts = ln.split()
            if not parts or parts[0].startswith("#"):
                continue
            if parts[0] == "F":
                funcs[parts[1]] = int(parts[2], 16)
            elif parts[0] == "D":
                data[parts[1]] = int(parts[2], 16)
    return funcs, data


GLOBAL_DEF_RE = re.compile(r"^@(\"[^\"]*\"|[-A-Za-z0-9$._]+)\s*=\s*(.*)$")


def guest_ptr(addr):
    return "inttoptr (i32 %d to ptr)" % addr


def rewrite(lines, asm_funcs, asm_data, tu_id, where):
    # Pass 1: what this TU defines and declares, and the type table.
    typedefs = {}
    defined_funcs = {}     # name -> FuncHeader
    declared_funcs = {}    # name -> FuncHeader
    defined_data = {}      # name -> linkage
    declared_data = set()
    for ln in lines:
        if ln.startswith("%"):
            m = re.match(r"(%(?:\"[^\"]*\"|[-A-Za-z0-9$._]+))\s*=\s*type\s+(.*)$", ln)
            if m:
                typedefs[m.group(1)] = m.group(2).strip()
        elif ln.startswith("define"):
            h = parse_func_header(ln)
            defined_funcs[h.name] = h
        elif ln.startswith("declare"):
            h = parse_func_header(ln)
            declared_funcs[h.name] = h
        elif ln.startswith("@"):
            m = GLOBAL_DEF_RE.match(ln)
            if not m:
                continue
            name, _ = parse_global_name(ln, 0)
            body = m.group(2)
            words = body.split()
            if "alias" in words[:6] or "ifunc" in words[:6]:
                # An alias of a function is a function this TU defines.
                continue
            if words and words[0] in ("external", "extern_weak"):
                declared_data.add(name)
            else:
                linkage = words[0] if words and words[0] in LINKAGES else "external"
                defined_data[name] = linkage
    layout = Layout(typedefs)

    def is_func(name):
        return name in defined_funcs or name in declared_funcs

    def asm_fn(name):
        return (name in asm_funcs and name not in defined_funcs
                and name not in defined_data)

    def asm_dat(name):
        return (name in asm_data and name not in defined_data
                and name not in defined_funcs and name not in asm_funcs)

    def replacement(name):
        if asm_fn(name):
            return guest_ptr(asm_funcs[name])
        if asm_dat(name):
            return guest_ptr(asm_data[name])
        return None

    wrappers = {}      # (sig, name) -> decl line
    indirects = {}     # sig -> decl line
    taken = {}         # symbol -> True
    out = []

    def note_taken(name):
        if name.startswith("llvm."):
            return
        if is_func(name) and not asm_fn(name):
            taken[name] = True

    def subst_refs(text, start=0):
        """Rewrites 2 and 3 on every @name in text[start:], noting
        address-taken C functions."""
        refs = global_refs(text, start)
        if not refs:
            return text
        parts = []
        last = 0
        for b, e, name in refs:
            r = replacement(name)
            if r is None:
                note_taken(name)
                continue
            parts.append(text[last:b])
            parts.append(r)
            last = e
        parts.append(text[last:])
        return "".join(parts)

    def rewrite_call(ln, c):
        kind, cal = c.callee
        if kind == "global" and cal.startswith("llvm."):
            return subst_refs(ln, c.args_span[0])
        if kind == "global" and asm_dat(cal):
            kind, cal = "const", guest_ptr(asm_data[cal])
        if kind == "global" and not asm_fn(cal):
            # C -> C (or C -> this TU): untouched; arguments may still name
            # asm symbols.
            head = ln[:c.args_span[0]]
            return head + subst_refs(ln, c.args_span[0])[c.args_span[0]:]
        args_text = subst_refs(ln[c.args_span[0]:c.args_span[1]])
        rest = subst_refs(c.rest)
        sig = call_sig(c)
        tail = c.tailkind
        if tail == "musttail":
            tail = "tail"
        head = c.prefix + (tail + " " if tail else "") + "call" + c.pre_text
        if kind == "global":
            s = mangle(sig, layout, "%s: call to %s" % (where, cal), False)
            wname = "aw$%s$%s" % (s, cal)
            if (s, cal) not in wrappers:
                wrappers[(s, cal)] = "declare %s %s(%s)" % (
                    decl_ret(sig), ir_global(wname),
                    ", ".join(decl_param(p) for p in sig.params))
            return "%s%s %s%s%s" % (head, c.ret, ir_global(wname), args_text, rest)
        # Indirect: prepend the callee.
        s = mangle(sig, layout, "%s: indirect call" % where, True)
        aname = "ai$%s" % s
        if s not in indirects:
            indirects[s] = "declare %s %s(%s)" % (
                decl_ret(sig), ir_global(aname),
                ", ".join(["ptr"] + [decl_param(p) for p in sig.params]))
        callee = cal if kind == "local" else subst_refs(cal)
        inner = args_text[1:-1].strip()
        new_args = "(ptr %s%s)" % (callee, (", " + inner) if inner else "")
        return "%s%s %s%s%s" % (head, c.ret, ir_global(aname), new_args, rest)

    drop = set()
    for name in declared_funcs:
        if asm_fn(name) or asm_dat(name):
            drop.add(name)

    for ln in lines:
        if ln.startswith("declare"):
            h = declared_funcs.get(parse_global_name(ln, ln.index("@"))[0])
            if h is not None and h.name in drop:
                continue
            out.append(ln)
            continue
        if ln.startswith("define") or ln.startswith("attributes ") or \
                ln.startswith("!") or ln.startswith(";") or \
                ln.startswith("%") or ln.startswith("source_filename") or \
                ln.startswith("target ") or ln.startswith("module asm"):
            out.append(ln)
            continue
        if ln.startswith("@"):
            name, j = parse_global_name(ln, 0)
            if name in declared_data and asm_dat(name):
                continue
            if name in ("llvm.used", "llvm.compiler.used"):
                out.append(ln)
                continue
            eq = ln.index("=", j)
            if re.search(r"=\s*(?:\w+\s+)*alias\b", ln):
                out.append(ln)
                for _b, _e, n in global_refs(ln, eq + 1):
                    note_taken(n)
                continue
            out.append(ln[:eq + 1] + subst_refs(ln, eq + 1)[eq + 1:])
            continue
        c = parse_call(ln) if "call" in ln else None
        if c is not None:
            out.append(rewrite_call(ln, c))
        else:
            out.append(subst_refs(ln))

    # Aliases for address-taken internal functions.
    aliases = {}
    used_alias_names = set()
    for name in sorted(taken):
        h = defined_funcs.get(name)
        if h is not None and h.linkage in ("internal", "private"):
            base = "ab$%s$%s" % (tu_id, re.sub(r"[^A-Za-z0-9_$]", "_", name))
            a = base
            k = 1
            while a in used_alias_names:
                k += 1
                a = "%s$%d" % (base, k)
            used_alias_names.add(a)
            aliases[name] = a
    for name, a in sorted(aliases.items()):
        out.append("%s = hidden alias %s (%s), ptr %s" % (
            ir_global(a), defined_funcs[name].sig.ret,
            ", ".join([p.type for p in defined_funcs[name].sig.params] +
                      (["..."] if defined_funcs[name].sig.nfixed is not None else [])),
            ir_global(name)))
    for k in sorted(wrappers):
        out.append(wrappers[k])
    for k in sorted(indirects):
        out.append(indirects[k])

    # Records.
    recs = []
    for name, h in sorted(defined_funcs.items()):
        if h.linkage in ("internal", "private", "available_externally"):
            continue
        strength = "weak" if h.linkage.startswith(("weak", "linkonce")) or \
            h.linkage in ("common", "extern_weak") else "strong"
        recs.append("D %s %s %s" % (name, mangle(h.sig, layout, "%s: define %s"
                                                 % (where, name), True), strength))
    for name, linkage in sorted(defined_data.items()):
        if linkage in ("internal", "private", "available_externally", "appending"):
            continue
        if name.startswith("llvm."):
            continue
        recs.append("G %s" % name)
    for name in sorted(taken):
        h = defined_funcs.get(name) or declared_funcs.get(name)
        try:
            s = mangle(h.sig, layout, "%s: address of %s" % (where, name), True)
        except IRError as e:
            sys.stderr.write("irbridge: warning: %s\n" % e)
            continue
        recs.append("T %s %s %s" % (aliases.get(name, name), s, name))
    for s, name in sorted(wrappers):
        recs.append("W %s %s" % (s, name))
    for s in sorted(indirects):
        recs.append("I %s" % s)
    for name, h in sorted(declared_funcs.items()):
        if name.startswith("llvm.") or name in drop:
            continue
        try:
            recs.append("E %s %s" % (name, mangle(h.sig, layout, name, True)))
        except IRError:
            pass
    return out, recs


RETADDR_RE = re.compile(
    r"(?:tail |musttail |notail )?call ptr @llvm\.returnaddress(?:\.p0)?\(i32 0\)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--classes", required=True)
    ap.add_argument("--tu-id", help="unique id for alias names (default: a "
                    "hash of OUT.ll's absolute path)")
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("sigs")
    args = ap.parse_args()

    asm_funcs, asm_data = load_classes(args.classes)
    tu_id = args.tu_id or hashlib.sha1(
        os.path.abspath(args.out).encode()).hexdigest()[:10]
    with open(args.inp) as f:
        text = f.read()
    # -finstrument-functions (the VRAMCNT writers, see D/pc/mk/game.mk) passes
    # __cyg_profile_func_*(fn, llvm.returnaddress(0)), and the wasm backend
    # refuses that intrinsic outside Emscripten. armrec_rt.c's hooks ignore
    # the argument, so it becomes null (games/platinum/pc/wasm/
    # cc_instrument.sh does the same for Platinum's plain compile).
    text = RETADDR_RE.sub("getelementptr i8, ptr null, i32 0", text)
    lines = text.split("\n")
    try:
        out, recs = rewrite(lines, asm_funcs, asm_data, tu_id, args.inp)
    except IRError as e:
        sys.stderr.write("irbridge: %s: %s\n" % (args.inp, e))
        return 1
    with open(args.out, "w") as f:
        f.write("\n".join(out))
        if not out or out[-1] != "":
            f.write("\n")
    with open(args.sigs, "w") as f:
        for r in recs:
            f.write(r + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
