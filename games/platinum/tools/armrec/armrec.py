#!/usr/bin/env python3
"""
armrec, a static recompiler for the DS assembly in this tree.

Reads the .s files, which carry function boundaries, symbol names and guest
addresses, and emits C that reproduces the instruction semantics against the
runtime in armrec_rt.h.

Usage:
    armrec.py --out DIR [--symbols FILE] FILE.s [FILE.s ...]
    armrec.py --scan FILE.s [...]        # report coverage, emit nothing

r0 to r12 and lr are C locals and r13 is the global armrec_sp, so recompiled
functions share one emulated stack as they did on hardware. Every recompiled
function is uint64_t f(uint32_t, uint32_t, uint32_t, uint32_t), packing r0 and
r1 into the return value, which is close enough to AAPCS that calls to and
from hand-decompiled C need no thunks. Guest memory is identity mapped, so a
guest pointer is a host pointer.
"""

import argparse
import ast
import copy
import os
import re
import subprocess
import sys
from collections import Counter, OrderedDict

# --------------------------------------------------------------------------
# Registers and conditions
# --------------------------------------------------------------------------

REG_ALIASES = {
    "sp": 13, "lr": 14, "pc": 15, "ip": 12, "fp": 11, "sl": 10, "sb": 9,
}
for _i in range(16):
    REG_ALIASES["r%d" % _i] = _i

CONDS = OrderedDict([
    ("eq", "(zf)"),
    ("ne", "(!zf)"),
    ("cs", "(cf)"),
    ("hs", "(cf)"),
    ("cc", "(!cf)"),
    ("lo", "(!cf)"),
    ("mi", "(nf)"),
    ("pl", "(!nf)"),
    ("vs", "(vf)"),
    ("vc", "(!vf)"),
    ("hi", "(cf && !zf)"),
    ("ls", "(!cf || zf)"),
    ("ge", "(nf == vf)"),
    ("lt", "(nf != vf)"),
    ("gt", "(!zf && nf == vf)"),
    ("le", "(zf || nf != vf)"),
    ("al", None),
])

# Data-processing opcodes: name -> (has_rd, has_rn, template kind)
DP_OPS = {
    "and": "log", "eor": "log", "orr": "log", "bic": "log",
    "sub": "sub", "rsb": "rsb", "add": "add", "adc": "adc",
    "sbc": "sbc", "rsc": "rsc",
    "mov": "mov", "mvn": "mvn",
    "tst": "test_log", "teq": "test_log", "cmp": "test_cmp", "cmn": "test_cmn",
}

SHIFT_OPS = ("lsl", "lsr", "asr", "ror")

LDM_MODES = {
    "ia": "ia", "ib": "ib", "da": "da", "db": "db",
    "fd": "ia", "ed": "ib", "fa": "da", "ea": "db",  # LDM aliases
}
STM_MODES = {
    "ia": "ia", "ib": "ib", "da": "da", "db": "db",
    "ea": "ia", "fa": "ib", "ed": "da", "fd": "db",  # STM aliases
}


class Unsupported(Exception):
    """Raised for a construct the translator does not implement."""


# --------------------------------------------------------------------------
# Source items
# --------------------------------------------------------------------------

class Insn(object):
    __slots__ = ("mnem", "cond", "sflag", "ops", "raw", "cmt", "thumb", "addr", "lineno")

    def __init__(self, mnem, cond, sflag, ops, raw, cmt, thumb, lineno):
        self.mnem = mnem
        self.cond = cond
        self.sflag = sflag
        self.ops = ops
        self.raw = raw
        self.cmt = cmt
        self.thumb = thumb
        self.addr = None
        self.lineno = lineno


class Label(object):
    __slots__ = ("name", "addr", "lineno")

    def __init__(self, name, addr, lineno):
        self.name = name
        self.addr = addr
        self.lineno = lineno


class Directive(object):
    __slots__ = ("name", "args", "lineno")

    def __init__(self, name, args, lineno):
        self.name = name
        self.args = args
        self.lineno = lineno


class Func(object):
    def __init__(self, name, thumb, addr, is_global, markerless=False,
                 attested=True, file_local=False):
        self.name = name
        self.thumb = thumb
        self.addr = addr
        self.is_global = is_global
        self.markerless = markerless
        # The source said local_arm_func_start, so mwcc compiled this static.
        # Not the same as `not is_global`, which is also true of a label
        # split_call_targets() promoted to a function.
        self.file_local = file_local
        # True when the source names this a function rather than armrec
        # guessing. A marker macro says so, and so does a trailing
        # `arm_func_end NAME` with no matching start. A bare `.global` does
        # not: that shape also spells data.
        self.attested = attested
        self.items = []          # Insn / Label / Directive in order
        self.labels = set()      # local label names
        # Entry points reachable in this body. Grows when merge_multi_entry()
        # copies a sibling entry point's code in, and it is what tells a BL to
        # a real function apart from a BL used as a long branch.
        self.entries = set([name])


# --------------------------------------------------------------------------
# Address helpers
# --------------------------------------------------------------------------

ADDR_IN_NAME = re.compile(r"^_([0-9A-Fa-f]{6,8})$")
OV_ADDR_IN_NAME = re.compile(r"^(?:ov\d+_|sub_|FUN_)([0-9A-Fa-f]{6,8})$")
ADDR_COMMENT = re.compile(r";\s*0x([0-9A-Fa-f]+)")
# A call target spelled as a guest address to dispatch at run time; see
# emit_branch().
DISPATCH_TARGET = re.compile(r"^armrec_dispatch_([0-9A-Fa-f]{8})$")

# A label states its address twice: in its own `; 0x...` comment, and in the
# assembler's location counter. Where they disagree the counter wins, and the
# disagreement is counted into the build report. The counter only wins while
# it is live; a directive armrec cannot size calls advance(None), so an
# unknown construct gives no verdict and the comment re-anchors at the next
# label.
ADDR_CONTRA_TAG = "address comment the location counter contradicts: "


def addr_contradiction(name, claimed, counted):
    """The report line for a label the assembler disagrees with."""
    return (ADDR_CONTRA_TAG + "%s says 0x%08X, the assembler is at 0x%08X "
            "(%+d)" % (name, claimed, counted, claimed - counted))


def addr_from_name(name):
    m = ADDR_IN_NAME.match(name) or OV_ADDR_IN_NAME.match(name)
    if m:
        try:
            return int(m.group(1), 16)
        except ValueError:
            return None
    return None


# What a decompiled symbol is: Thumb code, ARM code, or not code at all.
# Decompiling deletes the .s and its arm_func_start marker, so a `.word`
# naming one has an address armrec can resolve and a state it cannot. Bit 0
# of a stored ARM function pointer is that state. Generated by
# tools/armrec/gen_decomp_thumb.py.
DECOMP_STATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "decomp_thumb.txt")

# A `.word` operand that is nothing but a symbol name, so `Thumbfn + 4` (an
# offset into code rather than a pointer to it) is not mistaken for one.
BARE_NAME = re.compile(r"^[A-Za-z_.$][\w.$]*$")


def load_decomp_state(path):
    """
    {name: 'arm'|'thumb'|'data'} from the generated table; missing file is empty.

    `data` is not a third instruction set, it is the answer "this is not code".
    A `.word` can name a decompiled object, and bit 0 of a pointer to one is
    part of the address, so it must stay clear.
    """
    out = {}
    try:
        fh = open(path)
    except IOError:
        return out
    with fh:
        for line in fh:
            line = line.split("#", 1)[0].split()
            if len(line) >= 2 and line[1] in ("arm", "thumb", "data"):
                out[line[0]] = line[1]
    return out


_EXPR_BINOPS = {
    ast.Add: lambda a, b: a + b,
    ast.Sub: lambda a, b: a - b,
    ast.Mult: lambda a, b: a * b,
    ast.FloorDiv: lambda a, b: a // b,
    ast.Div: lambda a, b: a // b,
    ast.Mod: lambda a, b: a % b,
    ast.LShift: lambda a, b: a << b,
    ast.RShift: lambda a, b: a >> b,
    ast.BitAnd: lambda a, b: a & b,
    ast.BitOr: lambda a, b: a | b,
    ast.BitXor: lambda a, b: a ^ b,
}


# A symbol as the assembler spells it. Wider than a Python identifier: mwcc
# spells a function-local static "fp_str$6E88", and names here may also carry
# "." and "?". The lookbehind stops a match starting inside another name.
ASM_NAME_RE = re.compile(r"(?<![\w.$?])[A-Za-z_.$?][\w.$?]*")


def mangle_asm_names(tok):
    """
    Rewrite every symbol in an assembler expression to a Python-legal name.

    Returns (source, {placeholder: real name}).

    Python's parser evaluates these expressions and rejects "$" outright, so a
    name containing one used to raise SyntaxError and be reported as "cannot
    parse expression". Names are mangled unconditionally rather than only when
    they contain a "$", so a leading "." cannot parse as attribute access and a
    symbol spelled like a Python keyword cannot become a syntax error.
    """
    names = {}

    def sub(m):
        key = "_asmsym%d" % len(names)
        names[key] = m.group(0)
        return key

    return ASM_NAME_RE.sub(sub, tok), names


def eval_asm_expr(tok, symtab=None):
    """
    Evaluate an assembler constant expression.

    Directives carry things like ".space 624 * 4" and ".word sym - 0xC", so a
    bare int() is not enough. Python's parser gives us the grammar for free;
    the node whitelist keeps this from evaluating anything but arithmetic.
    """
    tok = tok.strip()
    if re.match(r"^'.'$", tok):
        return ord(tok[1])

    src, names = mangle_asm_names(tok)

    def walk(node):
        if isinstance(node, ast.Expression):
            return walk(node.body)
        if isinstance(node, ast.Constant):
            if isinstance(node.value, int):
                return node.value
            raise Unsupported("non-integer constant in %r" % tok)
        if isinstance(node, ast.BinOp) and type(node.op) in _EXPR_BINOPS:
            return _EXPR_BINOPS[type(node.op)](walk(node.left), walk(node.right))
        if isinstance(node, ast.UnaryOp):
            if isinstance(node.op, ast.USub):
                return -walk(node.operand)
            if isinstance(node.op, ast.UAdd):
                return walk(node.operand)
            if isinstance(node.op, ast.Invert):
                return ~walk(node.operand)
        if isinstance(node, ast.Name):
            name = names.get(node.id, node.id)
            if symtab is not None and name in symtab:
                return symtab[name]
            # Local labels encode their own guest address (_020424D0), which is
            # how the Thumb branch tables express their offsets.
            a = addr_from_name(name)
            if a is not None:
                if (WASM_ASM_NAMES is not None and name not in WASM_ASM_NAMES
                        and not ADDR_IN_NAME.match(name)):
                    # sub_/FUN_/ovNN_ that no .s defines is decompiled C. On
                    # wasm C does not live at its guest address, so it is
                    # an extern like any other C symbol (see C2U_PREFIX).
                    raise Unsupported("C symbol %r" % name)
                return a
            raise Unsupported("unresolved symbol %r" % name)
        raise Unsupported("unsupported expression %r" % tok)

    # 0x/0b literals are already Python syntax; a bare 0-prefixed decimal is not.
    # Safe to do after mangling: every symbol is a _asmsymN placeholder by now,
    # so this can only see numeric literals. Before mangling it would have
    # rewritten the digits inside a name like "wstr$0100".
    src = re.sub(r"\b0+(\d+)\b", r"\1", src)
    try:
        tree = ast.parse(src, mode="eval")
    except SyntaxError:
        raise Unsupported("cannot parse expression %r" % tok)
    return walk(tree)


def parse_int(tok):
    """Parse an assembler integer literal or constant expression."""
    return eval_asm_expr(tok)


# --------------------------------------------------------------------------
# Lexer / parser
# --------------------------------------------------------------------------

def split_comment(line):
    """Split off an asm comment, respecting quotes. Returns (code, comment)."""
    out = []
    i = 0
    n = len(line)
    in_str = None
    while i < n:
        c = line[i]
        if in_str:
            if c == "\\":
                out.append(c)
                i += 1
                if i < n:
                    out.append(line[i])
                    i += 1
                continue
            if c == in_str:
                in_str = None
            out.append(c)
            i += 1
            continue
        if c in "\"'":
            in_str = c
            out.append(c)
            i += 1
            continue
        if c == ";" or c == "@":
            return "".join(out), line[i:]
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            return "".join(out), line[i:]
        out.append(c)
        i += 1
    return "".join(out), ""


def split_mnemonic(word, thumb):
    """Split a mnemonic into (base, cond, sflag, lsm_mode)."""
    w = word.lower()

    # push/pop are their own thing
    if w in ("push", "pop"):
        return w, None, False, None

    # LDM/STM: ldm{cond}{mode} or ldm{mode}{cond}
    m = re.match(r"^(ldm|stm)(.*)$", w)
    if m:
        base, rest = m.group(1), m.group(2)
        cond = None
        mode = None
        # try cond first (ldmeqia), then mode first (ldmiaeq)
        for c in CONDS:
            if rest.startswith(c) and rest[len(c):] in LDM_MODES:
                cond, mode = c, rest[len(c):]
                break
        if mode is None:
            for md in LDM_MODES:
                if rest.startswith(md):
                    mode = md
                    tail = rest[len(md):]
                    if tail:
                        if tail in CONDS:
                            cond = tail
                        else:
                            raise Unsupported("bad LDM/STM suffix %r" % word)
                    break
        if mode is None:
            if rest in CONDS:
                cond, mode = rest, "ia"
            elif rest == "":
                mode = "ia"
            else:
                raise Unsupported("bad LDM/STM suffix %r" % word)
        return base, cond, False, mode

    # Loads/stores with a size suffix: ldr{cond}{b,h,sb,sh}
    m = re.match(r"^(ldr|str)(.*)$", w)
    if m:
        base, rest = m.group(1), m.group(2)
        cond = None
        for c in CONDS:
            if rest.startswith(c):
                cond = c
                rest = rest[len(c):]
                break
        size = rest
        if size not in ("", "b", "h", "sb", "sh", "d", "bt", "t"):
            # condition may trail instead (e.g. "streqb" handled above, "strbeq")
            for c in CONDS:
                if size.endswith(c):
                    cond = c
                    size = size[: -len(c)]
                    break
        if size not in ("", "b", "h", "sb", "sh", "d"):
            raise Unsupported("unsupported %s size suffix %r" % (base, word))
        return base + size, cond, False, None

    # Everything else: base{cond}{s}
    for base in sorted(
        list(DP_OPS) + list(SHIFT_OPS) + [
            "b", "bl", "bx", "blx", "swi", "svc", "nop", "mul", "mla",
            "umull", "umlal", "smull", "smlal", "clz", "mrs", "msr",
            "mrc", "mcr", "neg", "qadd", "qsub", "smulbb", "smulbt",
            "smultb", "smultt", "smlabb", "smlabt", "smlatb", "smlatt",
            "smulwb", "smulwt", "smlawb", "smlawt", "smlalbb", "bkpt", "adr",
            "swp", "swpb",
        ],
        key=len,
        reverse=True,
    ):
        if not w.startswith(base):
            continue
        rest = w[len(base):]
        # "b" is a prefix of every b<cond>; make sure we do not mis-split e.g. "bic"
        cond = None
        sflag = False
        if rest == "":
            return base, None, False, None
        if rest == "s" and base not in ("b", "bl", "bx", "blx"):
            return base, None, True, None
        if rest in CONDS:
            return base, rest, False, None
        if rest.endswith("s") and rest[:-1] in CONDS and base not in ("b", "bl", "bx", "blx"):
            return base, rest[:-1], True, None
        if len(rest) >= 2 and rest[:2] in CONDS and rest[2:] == "s" and base not in ("b", "bl", "bx", "blx"):
            return base, rest[:2], True, None
        continue

    raise Unsupported("unknown mnemonic %r" % word)


# The condition field of an ARM encoding, in encoding order. 0xF is not a
# condition but a separate instruction space (BLX immediate, PLD), so it is
# absent and decode_arm_word() declines it.
ARM_COND_NAMES = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                  "hi", "ls", "ge", "lt", "gt", "le", ""]


def decode_arm_word(word):
    """
    Disassemble one ARM encoding that the disassembler left as a `.word`.

    A `.word` in the middle of a function's instruction stream is not data, it
    is an instruction the tool that produced these files could not spell. There
    are eight in this tree, all `blx rN` in one overlay.

    Returns a mnemonic string for the caller to re-parse through the ordinary
    instruction path, or None when armrec cannot spell this word either, which
    the caller counts.
    """
    cond = (word >> 28) & 0xF
    if cond == 0xF:
        return None
    suffix = ARM_COND_NAMES[cond]
    # BLX (register): cond 0001 0010 1111 1111 1111 0011 Rm
    if (word & 0x0FFFFFF0) == 0x012FFF30:
        return "blx%s r%d" % (suffix, word & 0xF)
    # BX (register): cond 0001 0010 1111 1111 1111 0001 Rm
    if (word & 0x0FFFFFF0) == 0x012FFF10:
        return "bx%s r%d" % (suffix, word & 0xF)
    return None


INCLUDE_RE = re.compile(r'^\s*\.include\s+"([^"]+)"\s*$')

_include_cache = {}


def asm_incdirs(path, incdirs):
    """
    The include path the assembler searches, for `.include` and `.incbin`.

    arm9/Makefile builds with `-i ../include -i ..` and cwd `arm9`, so a
    `.include "asm/macros.inc"` in arm9/overlays/83 resolves to
    arm9/asm/macros.inc. The cwd half is the source file's own CPU root; the
    rest is what --include gives.

    The root is the path's own arm9 or arm7 component wherever it sits, not the
    first component of the path as given, which is empty for an absolute path
    and wrong for a relative one built from somewhere else.
    """
    parts = os.path.normpath(path).replace(os.sep, "/").split("/")
    root = None
    for i, part in enumerate(parts[:-1]):
        if part in ("arm9", "arm7"):
            root = "/".join(parts[:i + 1]) or "/"
    dirs = [root] if root and os.path.isdir(root) else []
    return dirs + [d for d in incdirs if d not in dirs]


def read_incbin(name, src_path, incdirs):
    """
    The bytes of a `.incbin "X"`, or None if X is not on the include path.

    Same lookup as `.include`, because it is the same directive family and the
    same `-i` list. The one site in this tree reads a .4bpp that `make -f
    pc/Makefile assets` produces from a .png through the ROM build's own rule.
    """
    if src_path is None:
        return None
    for d in asm_incdirs(src_path, incdirs):
        cand = os.path.join(d, name)
        if os.path.exists(cand):
            try:
                with open(cand, "rb") as fh:
                    return fh.read()
            except OSError:
                return None
    return None


def expand_includes(path, incdirs, _depth=0):
    """
    Splice `.include "X"` textually, as the assembler does, before cpp sees it.

    `.include` is an assembler directive, so `cpp -P` leaves it alone and the
    included file contributes nothing. That is invisible until the included
    file is where the constants live, and then a 16-bit datum has no relocation
    path and comes out zero.

    Returns None if nothing was expanded, so the common path stays a plain
    cpp-over-the-file.
    """
    try:
        with open(path, "r", errors="replace") as fh:
            lines = fh.readlines()
    except OSError:
        return None
    if not any(INCLUDE_RE.match(l) for l in lines):
        return None
    if _depth > 16:
        raise Unsupported("`.include` nested more than 16 deep at %r" % path)

    out = []
    for line in lines:
        m = INCLUDE_RE.match(line)
        if not m:
            out.append(line)
            continue
        target = None
        for d in asm_incdirs(path, incdirs):
            cand = os.path.join(d, m.group(1))
            if os.path.exists(cand):
                target = cand
                break
        if target is None:
            # The assembler would fail here; armrec cannot, because it is
            # routinely pointed at a subset of the tree. Leave the directive
            # for the parser, which counts it.
            out.append(line)
            continue
        if target not in _include_cache:
            sub = expand_includes(target, incdirs, _depth + 1)
            if sub is None:
                with open(target, "r", errors="replace") as fh:
                    sub = fh.readlines()
            _include_cache[target] = sub
        out.extend(_include_cache[target])
    return out


def preprocess(path, defines, incdirs):
    """
    Run the C preprocessor over a .s file, as the real build does.

    Some assembly pulls constants in with #include, and the game-version macros
    come from include/config.h, so without this the data sections come out full
    of unresolved names. Others use the assembler's own `.include`, which cpp
    ignores; expand_includes() runs first and hands cpp the spliced text on
    stdin. Reading stdin costs an extra -I, because cpp resolves a quoted
    #include relative to the current file and stdin has no directory.

    Every option is spelled joined (`-Idir`, `-DX=X`): macOS's /usr/bin/cpp
    is a wrapper that mis-parses the separated forms (`-I dir` fails with
    "no such file or directory: 'c'"), and a failed cpp silently leaves the
    file unpreprocessed, which is what left GAME_VERSION and every
    constants/*.h name unresolved in Diamond's assembly.
    """
    cmd = ["cpp", "-P"]
    text = None
    lines = expand_includes(path, incdirs)
    if lines is None:
        try:
            with open(path, "r", errors="replace") as fh:
                raw = fh.readlines()
        except OSError:
            raw = []
        if any(INDENTED_DIRECTIVE.match(l) for l in raw):
            lines = raw
    if lines is not None:
        # macOS's cpp is a traditional-mode preprocessor, and it ignores an
        # indented `#include` on the first line of its input; mwasm does
        # not care. Moving every directive to column one costs nothing.
        text = "".join(INDENTED_DIRECTIVE.sub(r"#", l) for l in lines)
        cmd.append("-I" + (os.path.dirname(path) or "."))
    for d in incdirs:
        cmd.append("-I" + d)
    cfg = os.path.join(incdirs[0], "config.h") if incdirs else None
    if cfg and os.path.exists(cfg):
        cmd += ["-include", cfg]
    for d in sorted(defines):
        # "-D DIAMOND" makes cpp substitute DIAMOND -> 1 everywhere, including
        # inside the assembler's own ".ifdef DIAMOND", which then reads
        # ".ifdef 1" and takes the else branch. Defining the macro as itself
        # keeps #ifdef working and leaves the token intact.
        cmd.append("-D%s=%s" % (d, d))
    cmd.append("-" if text is not None else path)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, input=text)
        if r.returncode == 0 and r.stdout:
            return r.stdout.splitlines(True)
    except (OSError, subprocess.SubprocessError):
        pass
    return None


INDENTED_DIRECTIVE = re.compile(
    r"^[ \t]+#(?=\s*(?:include|if|ifdef|ifndef|elif|else|endif|define|undef)\b)")


def place_functions(funcs):
    """
    Give every instruction its address, and every entry point that has none.

    An entry point carrying no "; 0x..." comment can still be placed: functions
    in one .text section are contiguous, so it starts where its predecessor
    ended. Without that, a branch table inside such a function cannot be
    resolved at all.
    """
    for i, f in enumerate(funcs):
        end = assign_addresses(f)
        if end is None or i + 1 >= len(funcs):
            continue
        nxt = funcs[i + 1]
        # An attested entry point qualifies even when it is markerless: an
        # `arm_func_end NAME` with no start marker is still the source calling
        # it a function.
        if nxt.addr is None and (not nxt.markerless or nxt.attested):
            align = 2 if nxt.thumb else 4
            nxt.addr = (end + align - 1) & ~(align - 1)


# Thumb's unconditional B is an 11-bit signed halfword offset from pc, and pc
# reads as the instruction's own address plus four. BL is a pair of halfwords
# and reaches +-4 MB. ARM's B and BL have the same +-32 MB reach as each other,
# which is why the rule below needs no arithmetic there.
THUMB_B_REACH = (-2048, 2046)

BL_UNPLACEABLE = ("a Thumb bl to a local label with no address, so armrec "
                  "cannot tell a call from a long branch: ")


def bl_is_call(ins, target_addr, thumb):
    """
    True if this `bl` to a local label is a call, False if it is a long branch,
    None if there is not enough address information to say.

    mwcc spells a Thumb branch it cannot reach with `b` as `bl`, relying on the
    lr its own prologue pushed. That is a range decision, so range reads it
    back: a `bl` to somewhere `b` would have reached is a call. In ARM state `b`
    and `bl` reach equally far, so every `bl` is a call.

    Over the 49 sites in this tree the partition is not a knife edge. The Thumb
    calls are 42 to 1,056 bytes from their target and the long branches are
    over 2,000, either side of the 2,046-byte limit with nothing in between.
    """
    if not thumb:
        return True
    if ins.addr is None or target_addr is None:
        return None
    d = target_addr - (ins.addr + 4)
    return THUMB_B_REACH[0] <= d <= THUMB_B_REACH[1]


def split_call_targets(funcs, globals_, problems):
    """
    Promote a `bl` target that the source spells only as a label to a function.

    `arm_func_start` names an entry point and a disassembly can lose one. Where
    that happened, `bl` to the label hit the `goto` in emit_branch(): the
    label's own prologue pushed a stale lr and its epilogue returned from the
    enclosing function, skipping everything after the call.

    The discriminator is branch range; see bl_is_call() above. Two other
    readings were tried. Which entry point's range the label sits in gives
    almost the same partition but cannot see a boundary that is not in the file
    at all. Whether the target's body balances the stack is exact, but it is a
    walk over every path of every body, so it is the oracle instead:
    test_call_targets is that walk and checks this partition both ways.

    Splitting rather than emitting a second copy of the tail leaves a single
    body with a single owner, address and registration. The enclosing function
    is truncated at the label, which is what its `arm_func_end` would have said.

    Returns the number of functions promoted.
    """
    owner = {}
    for i, f in enumerate(funcs):
        for it in f.items:
            if isinstance(it, Label):
                owner.setdefault(it.name, (i, it.addr))

    cuts = {}
    for i, f in enumerate(funcs):
        for it in f.items:
            if not (isinstance(it, Insn) and it.mnem == "bl"):
                continue
            target = (it.ops or "").strip()
            got = owner.get(target)
            # An ordinary call to a marked entry point needs nothing here.
            if got is None or target == funcs[got[0]].name:
                continue
            j, taddr = got
            call = bl_is_call(it, taddr, f.thumb)
            if call is None:
                # Neither answer can be justified, so say so in the build's own
                # report rather than picking one in silence. There are none in
                # this tree and test_call_targets requires that.
                problems[BL_UNPLACEABLE + target] += 1
                continue
            if call:
                cuts.setdefault(j, set()).add(target)
    if not cuts:
        return 0

    out = []
    made = 0
    for j, f in enumerate(funcs):
        want = cuts.get(j)
        if not want:
            out.append(f)
            continue
        at = {}
        for k, it in enumerate(f.items):
            if isinstance(it, Label) and it.name in want and k > 0:
                at.setdefault(it.name, k)
        if len(at) != len(want):
            # A target that is the function's own opening label, or one this
            # walk cannot find. Say so rather than splitting some of them: the
            # `bl` keeps its old meaning and the report names the file.
            problems["a bl target label armrec could not split into a "
                     "function"] += len(want) - len(at)
        # A snapshot, because the first piece is written back into f.items
        # below and every later slice would then be taken from the truncation.
        items = f.items
        edges = [0] + sorted(at.values()) + [len(items)]
        for pi in range(len(edges) - 1):
            chunk = items[edges[pi]:edges[pi + 1]]
            names = set(it.name for it in chunk if isinstance(it, Label))
            if pi == 0:
                f.items = chunk
                f.labels = names
                out.append(f)
                continue
            lab = chunk[0]
            g = Func(lab.name, f.thumb, lab.addr, lab.name in globals_)
            g.items = chunk
            g.labels = names
            out.append(g)
            made += 1
    funcs[:] = out
    return made


def parse_file(path, defines, incdirs=(), lines=None):
    """Parse one .s file into (funcs, data_items, globals, unsupported_counter)."""
    funcs = []
    data_items = []     # (addr_or_None, label_or_None, kind, payload)
    globals_ = set()
    externs = set()
    cur_func = None
    thumb_mode = False
    section = ".text"
    skip_stack = []
    problems = Counter()
    # The assembler's own location counter, per section. Code occupies bytes,
    # so the counter advances over instructions, data and explicit alignment
    # alike, exactly as gas does. It re-anchors at every label whose address
    # is known, so a directive armrec cannot size costs the rest of one
    # label's run rather than the rest of the file.
    loc = None
    # A section's origin appears in no .s file, so the first label carrying an
    # address fixes it and everything before that label is placed by
    # subtracting. `off` counts bytes from the section start while the origin
    # is unknown, `pend` is the data emitted in that stretch, and anchor()
    # resolves both.
    off = 0
    pend = []
    anchored = False
    sec_state = {}
    # Whether the previous item was an instruction rather than a label or a
    # data directive. That tells a literal pool (always entered through a
    # label) apart from a `.word` the disassembler emitted in place of an
    # instruction it could not spell.
    prev_was_insn = False
    # --xmap: where the ROM link put each of this object's sections. Without
    # it a section's origin is its first address-bearing label, and those
    # are Diamond's addresses, which Pearl's `.ifdef`s make wrong for every
    # label after the first size difference. Not for an extracted asm-in-C
    # body, whose object's sections also hold the C around it.
    origins = {}
    if "extracted" not in os.path.normpath(path).split(os.sep):
        origins = XMAP_ORIGINS.get(os.path.basename(path)[:-2] + ".o", {})
    if ".text" in origins:
        loc, anchored = origins[".text"], True

    if lines is None:
        lines = preprocess(path, defines, list(incdirs))
    if lines is None:
        with open(path, "r", errors="replace") as fh:
            lines = fh.readlines()

    # Names this file closes with `arm_func_end NAME`. A disassembly can lose
    # the start marker and keep the end one, and an end marker naming a label
    # is the source saying that label is a function, so trust it.
    ends_named = set()
    for raw in lines:
        code, _ = split_comment(raw.rstrip("\n"))
        parts = code.strip().split(None, 1)
        if len(parts) == 2 and parts[0].lower() in ("arm_func_end",
                                                    "thumb_func_end"):
            ends_named.add(parts[1].strip())

    def enter_section(name):
        """Switch sections, remembering where the old one had got to."""
        nonlocal section, loc, off, pend, anchored
        sec_state[section] = (loc, off, pend, anchored)
        section = name
        if name not in sec_state and name in origins:
            loc, off, pend, anchored = origins[name], 0, [], True
        else:
            loc, off, pend, anchored = sec_state.get(name, (None, 0, [], False))

    def advance(n):
        """Move the counter over n bytes, or lose track of it if n is None."""
        nonlocal loc, off
        if n is None:
            loc = None
            off = None
        elif loc is not None:
            loc += n
        elif off is not None:
            off += n

    def anchor(addr):
        """
        Fix this section's origin from the first label that has an address.

        Everything ahead of that label sits at `addr` minus the bytes in
        between. It needs an unbroken walk from the section start, so a
        directive armrec cannot size gives up the prefix rather than guessing.

        A section that never anchors places nothing, not this data and not its
        functions either. That is one file, the syscall wrappers of a build
        variant this port does not link.
        """
        nonlocal loc, off, pend, anchored
        loc = addr
        if anchored:
            return
        anchored = True
        if pend and off is not None:
            origin = addr - off
            for idx, o in pend:
                _a, lab, kind, payload = data_items[idx]
                data_items[idx] = (origin + o, lab, kind, payload)
        elif pend:
            problems["data ahead of a section's first address, and the walk "
                     "to it is broken"] += len(pend)
        pend = []

    in_macro = False
    for lineno, raw in enumerate(lines, 1):
        code, cmt = split_comment(raw.rstrip("\n"))
        code = code.strip()

        # A macro definition is not content. expand_includes() splices in
        # asm/macros.inc, and cw.inc's `exception` macro body holds a `.word`.
        # armrec matches macro invocations by name rather than expanding them,
        # so the definitions carry nothing it wants.
        low = code.lower()
        if low.startswith(".macro"):
            in_macro = True
            continue
        if in_macro:
            if low.startswith(".endm"):
                in_macro = False
            continue

        # conditional assembly
        if low.startswith(".ifdef") or low.startswith(".ifndef"):
            sym = code.split(None, 1)[1].strip() if len(code.split(None, 1)) > 1 else ""
            live = (sym in defines) if low.startswith(".ifdef") else (sym not in defines)
            skip_stack.append(not live)
            continue
        if low.startswith(".else"):
            if skip_stack:
                skip_stack[-1] = not skip_stack[-1]
            continue
        if low.startswith(".endif"):
            if skip_stack:
                skip_stack.pop()
            continue
        if any(skip_stack):
            continue
        if not code:
            continue

        # label at start of line, possibly with an instruction/directive after it
        while True:
            m = re.match(r"^([A-Za-z_.$?][\w.$?]*):\s*(.*)$", code)
            if not m:
                break
            name, code = m.group(1), m.group(2).strip()
            addr = None
            am = ADDR_COMMENT.search(cmt) if cmt else None
            if am:
                addr = int(am.group(1), 16)
            if addr is None:
                addr = addr_from_name(name)
            # A `.global` label with neither an address comment nor an
            # address-derived name is still somewhere, and the location
            # counter is where. Narrowed to `.global` deliberately: a local
            # label gaining an address would also gain a case in every
            # computed branch resolving against the enclosing function's
            # labels, which changes control flow rather than a symbol table.
            if addr is None and loc is not None and name in globals_:
                addr = loc
            # Where the label says it is, against where the assembler has got
            # to. The counter wins, and the disagreement is counted into the
            # build report so a new site is something `make status` says.
            if addr is not None and loc is not None and addr != loc:
                problems[addr_contradiction(name, addr, loc)] += 1
                addr = loc
            lab = Label(name, addr, lineno)
            prev_was_insn = False
            # A file predating the arm_func_start convention marks its entry
            # points with a bare ".global NAME" / "NAME:", as both files in
            # arm9/lib/syscall do. The same shape also spells data, so the
            # promotion is undone below for any of these that no instruction
            # ever followed.
            if ((cur_func is None or cur_func.markerless)
                    and section == ".text"
                    and (name in globals_ or name in ends_named)):
                cur_func = Func(name, thumb_mode, addr, True, markerless=True,
                                attested=name in ends_named)
                funcs.append(cur_func)
            if cur_func is not None and section == ".text":
                # The address of a function is that of its own label: the
                # "; 0x…" comment, checked against the location counter just
                # above. It overrides the guess the start macro took from the
                # name, which is Diamond's address in Pearl past the first
                # `.ifdef` that changes a size; left at that guess, the xMAP
                # placement below would see the function "moved" and shift
                # every label in it, already at the counter, a second time
                # (Pearl's ov06_0224A0F0 read its jump table 12 bytes late).
                if addr is not None and name == cur_func.name:
                    cur_func.addr = addr
                cur_func.items.append(lab)
                cur_func.labels.add(name)
            # Every label anchors the data blob builder, including labels
            # inside a function. Functions contain data: Thumb branch tables
            # and literal pools both sit in .text behind a label. Skipping
            # those labels does not skip the data, it appends it to whichever
            # out-of-function symbol came last.
            data_items.append((addr, name, "label", None))
            if addr is not None:
                anchor(addr)
        if not code:
            continue

        parts = code.split(None, 1)
        head = parts[0]
        rest = parts[1].strip() if len(parts) > 1 else ""
        hl = head.lower()

        # function-boundary macros
        if hl in ("arm_func_start", "local_arm_func_start", "thumb_func_start",
                  "non_word_aligned_thumb_func_start"):
            thumb = hl.startswith("thumb") or hl.startswith("non_word")
            name = rest.strip()
            addr = addr_from_name(name)
            cur_func = Func(name, thumb, addr, hl != "local_arm_func_start",
                            file_local=hl == "local_arm_func_start")
            thumb_mode = thumb
            funcs.append(cur_func)
            if section != ".text":
                enter_section(".text")
            prev_was_insn = False
            continue
        if hl in ("arm_func_end", "thumb_func_end"):
            cur_func = None
            prev_was_insn = False
            continue
        if hl == "exception":
            continue  # CodeWarrior exception-table macro; no executable content

        # directives
        if head.startswith("."):
            d = hl[1:]
            if d in ("text",):
                enter_section(".text")
                prev_was_insn = False
                continue
            if d in ("data", "rodata", "bss"):
                enter_section("." + d)
                cur_func = None
                prev_was_insn = False
                continue
            if d == "section":
                nm = rest.split(",")[0].strip()
                enter_section(nm if nm.startswith(".") else "." + nm)
                if section not in (".text",):
                    cur_func = None
                prev_was_insn = False
                continue
            if d == "previous":
                continue
            if d == "arm":
                thumb_mode = False
                continue
            if d == "thumb":
                thumb_mode = True
                continue
            if d == "global" or d == "public":
                globals_.add(rest.strip())
                continue
            if d == "extern":
                externs.add(rest.strip())
                continue
            if d in ("align", "balign"):
                # Alignment moves the location counter and nothing else here.
                # Its padding is zeros into memory mmap already zero-filled,
                # so what matters is only that the next datum lands where gas
                # puts it.
                n = alignment_of(d, rest)
                if n is None:
                    advance(None)
                elif loc is not None:
                    loc = align_up(loc, n)
                elif off is not None:
                    # The origin is not known yet, so the padding is not
                    # either: aligning an offset only agrees with aligning the
                    # address when the origin is itself aligned. Give up the
                    # prefix rather than guess.
                    off = None
                continue
            if d in ("include", "type", "size", "ltorg", "end",
                     "hidden", "weak", "file", "loc", "ident", "syntax", "cpu",
                     "fpu", "eabi_attribute", "code", "pool", "protected"):
                continue
            if d in ("word", "long", "int", "short", "hword", "byte", "asciz",
                     "ascii", "space", "skip", "fill", "incbin"):
                # A `.word` reached while the previous item was an instruction,
                # in an ARM function, is an instruction the disassembler could
                # not spell, not data. Emitting it as data loses the call and
                # writes four bytes into the enclosing label's blob at an
                # address belonging to something else.
                #
                # Only `.word` and only in ARM state. Widening the rule would
                # turn the 261-line secure area blob into 261 failures.
                if (prev_was_insn and cur_func is not None
                        and section == ".text" and not thumb_mode
                        and d in ("word", "long", "int")):
                    text = None
                    try:
                        text = decode_arm_word(parse_int(rest) & 0xFFFFFFFF)
                    except Unsupported:
                        text = None
                    if text is not None:
                        base, cond, sflag, mode = split_mnemonic(
                            text.split()[0], thumb_mode)
                        ins = Insn(base, cond, sflag, text.split(None, 1)[1],
                                   code, cmt, thumb_mode, lineno)
                        if mode:
                            ins.mnem = base + ":" + mode
                        cur_func.items.append(ins)
                        advance(ARM_INSN_SIZE)
                        continue
                    # Not one armrec can spell. Keep it in the function so the
                    # address walk counts its four bytes, keep it out of the
                    # data blob so it cannot overwrite anything, and count it.
                    problems["undecodable .%s in instruction stream" % d] += 1
                    cur_func.items.append(Directive(d, rest, lineno))
                    advance(ARM_INSN_SIZE)
                    continue
                # Implicit alignment, before the datum is given an address: a
                # `.word` after an odd `.byte` run starts on the next 4-byte
                # boundary, which is where the console has UNK_020F767C's four
                # 12-byte entries.
                a = implicit_alignment(d)
                if a > 1 and loc is not None:
                    if loc % a and d in UNARBITRATED_ALIGN:
                        # Applied, and counted: no site in this tree exercises
                        # the halfword boundary, so the ROM has never been
                        # asked whether it is right. See IMPLICIT_ALIGN.
                        problems[UNARBITRATED_TAG + ".%s wants a %d-byte "
                                 "boundary at 0x%08X" % (d, a, loc)] += 1
                    loc = align_up(loc, a)
                elif a > 1 and off is not None:
                    # The origin is not known yet, so the padding is not
                    # either: aligning an offset only agrees with aligning the
                    # address when the origin is itself a multiple of `a`.
                    # anchor() reports the prefix it costs rather than dropping
                    # it silently. No file here reaches this.
                    off = None
                if loc is None and off is not None:
                    pend.append((len(data_items), off))
                data_items.append((loc, None, d, rest))
                if cur_func is not None and section == ".text":
                    cur_func.items.append(Directive(d, rest, lineno))
                n = directive_size(d, rest)
                if n is None and d == "incbin":
                    m = re.match(r'^\s*"([^"]+)"', rest)
                    blob = read_incbin(m.group(1), path, incdirs) if m else None
                    n = None if blob is None else len(blob)
                advance(n)
                prev_was_insn = False
                continue
            # An unrecognised directive emits an unknown number of bytes, so
            # the counter gives up rather than carrying on as if it emitted
            # none. advance(None) makes the label comment win again from here,
            # which is the honest answer when armrec does not know the
            # arithmetic. No file in this tree reaches it.
            problems["directive .%s" % d] += 1
            advance(None)
            continue

        # instruction
        try:
            base, cond, sflag, mode = split_mnemonic(head, thumb_mode)
        except Unsupported as e:
            problems[str(e)] += 1
            continue
        ins = Insn(base, cond, sflag, rest, code, cmt, thumb_mode, lineno)
        if mode:
            ins.mnem = base + ":" + mode
        # The counter advances whether or not the instruction is inside a
        # function: it occupies bytes on the console either way, and it is
        # data placed *after* it that this is for.
        advance(insn_size(base, rest, thumb_mode))
        if cur_func is None:
            # stray instruction outside a function; ignore but count
            problems["instruction outside function"] += 1
            continue
        cur_func.items.append(ins)
        prev_was_insn = True

    # A markerless label that turned out to head data, not code, is data.
    funcs = [f for f in funcs
             if not f.markerless or any(isinstance(it, Insn) for it in f.items)]

    # Twice, either side of the split, and neither is redundant. The first
    # pass gives split_call_targets() an address for every `bl` to compare
    # against; the second places the entry points it promoted.
    place_functions(funcs)
    split_call_targets(funcs, globals_, problems)
    place_functions(funcs)

    return funcs, data_items, globals_, externs, problems


# --------------------------------------------------------------------------
# Operand parsing
# --------------------------------------------------------------------------

def split_operands(s):
    """Split on commas at bracket/brace depth 0."""
    out = []
    depth = 0
    cur = ""
    for c in s:
        if c in "[{":
            depth += 1
        elif c in "]}":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


def reg_num(tok):
    t = tok.strip().lower()
    if t in REG_ALIASES:
        return REG_ALIASES[t]
    return None


class Ctx(object):
    """Per-function translation context."""

    def __init__(self, func, symtab, literals, fname, rename=None,
                 asm_funcs=None, abi_trap=None):
        self.func = func
        self.symtab = symtab
        self.literals = literals
        self.fname = fname
        # Name -> C identifier, for this file's contested file-local functions.
        self.rename = rename or {}
        # Every name any .s in this run defines as a function. A call target
        # outside it is decompiled C or pc/src/, and crosses the ABI boundary
        # described at ARMREC_CALL_EXT in armrec_rt.h. None means "assume
        # everything is recompiled", which is what the tests want.
        self.asm_funcs = asm_funcs
        self.abi_trap = abi_trap or {}
        self.ext_calls = Counter()
        self.tmp = 0
        self.used_ext = set()
        self.local_labels = {}
        self.cur_ins = None
        # id(Insn) of every `bx rN` that is a *call* rather than a tail call;
        # see call_bx_set() and emit_func().
        self.call_bx = set()

    def is_host_call(self, target):
        """Does a call to this name leave recompiled code?

        `asm_funcs` holds the names this run defines under a flat C symbol, so
        a file-local function is not in it and neither is a renamed MSL name.
        Both are still recompiled code, reachable only from a file whose own
        rename map names them, which is the test below. A --guest-libc name
        is renamed too, but its guest_ body is compiled C, so it crosses.
        """
        if self.asm_funcs is None:
            return False
        if target in GUEST_LIBC_EXT:
            return True
        return target not in self.asm_funcs and target not in self.rename

    def newtmp(self):
        self.tmp += 1
        return "t%d" % self.tmp

    def regc(self, n):
        if n == 13:
            return "ARM_SP"
        if n == 14:
            return "lr"
        if n == 15:
            # Reading PC yields a constant: the instruction's own address plus
            # the pipeline offset. Used for the "add rN, pc" sequences that
            # open a Thumb branch table.
            #
            # No word alignment. Align(PC,4) applies only to ADR, LDR literal
            # and ADD SP immediate; the forms here read the raw address plus
            # four. Rounding them down shifts every table at an odd halfword
            # by two bytes.
            if self.cur_ins is None or self.cur_ins.addr is None:
                raise Unsupported("PC operand with no known instruction address")
            if self.cur_ins.thumb:
                return "0x%08Xu" % (self.cur_ins.addr + 4)
            return "0x%08Xu" % (self.cur_ins.addr + 8)
        return "r%d" % n

    def sym_addr(self, name):
        """Guest address of a symbol, as a C expression."""
        s = self.symtab.get(name)
        if s is not None:
            return "0x%08Xu" % s
        # external symbol: the loader assigns it a synthetic address
        name = ext_name(name)
        self.used_ext.add(name)
        return "armrec_ext_%s" % sanitize(name)


def ext_name(name):
    """The C symbol an address-taken extern binds to (see --guest-libc)."""
    return (GUEST_PREFIX + name) if name in GUEST_LIBC_EXT else name


def sanitize(name):
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


# ---------------------------------------------------------------------------
# The guest has a C library and so does the host, and they must not be the
# same one.

# arm9/lib/MSL_C is Metrowerks' standard library compiled for the ARM, and
# armrec translates it like any other assembly: a weak C function under the
# real name. A weak definition in an object file satisfies a reference before
# the linker looks at any shared library, so `strcmp` in the finished binary
# was MSL's recompiled ARM body for the whole program.

# 52 names collide with the host libc, so a name the host owns is emitted with
# a prefix in both the definition and every recompiled reference. The list
# below is the C standard library's and need not be complete: test_libc_split
# derives the real collision set from the host's libc.
GUEST_PREFIX = "guest_"

# MSL names whose guest body is compiled C rather than assembly (--guest-libc).
# The C is built under the guest_ name, so a recompiled `bl rand` must reach
# guest_rand across the C boundary, not the host's rand. Set by main().
GUEST_LIBC_EXT = frozenset()

# --wasm: wasm calls are typed, so recompiled code cannot call a C function
# with its own uniform signature (wasm-ld would bind a trapping signature
# mismatch thunk), and ARMREC_CALL_EXT's twenty-word cast is the same defect.
# Every call that leaves recompiled code goes to a generated adapter
# `c2u$NAME` instead, which has the uniform signature, reads arguments five
# and up off the emulated stack itself and calls NAME with its real
# prototype. The adapters are the bridge's (games/diamond/pc/mk/bridge.mk).
# Set by main().
TARGET_WASM = False
C2U_PREFIX = "c2u$"
# --wasm: every name the assembly defines, set once the parse is done. An
# address-encoding name outside it (sub_02058ED4, ov06_...) is decompiled C,
# and eval_asm_expr() then refuses to read the address off the name.
WASM_ASM_NAMES = None
# --xmap: {object: {section: origin}} from the ROM's link map; parse_file()
# starts each section's location counter there. Set by main().
XMAP_ORIGINS = {}
# Functions that hand their result back in the condition flags: they write
# them with `msr cpsr_f` (any MSR to the CPSR's flags field) before
# returning, and their callers branch on the flags right after the call.
# The EABI soft-float comparisons are the case (TWL-SDK's runtime,
# __aeabi_cfcmple and kin: `bl cmp; bcs`, Black's angle normalisation
# loops). A function's flags are its own C locals, so after a call to one of
# these the caller reloads them from the CPSR the MSR wrote. Set by main().
FLAG_RESULT_FUNCS = frozenset()


def writes_cpsr_flags(func):
    """Does the body contain an MSR to the CPSR's flags field?"""
    for it in func.items:
        if isinstance(it, Insn) and it.mnem == "msr":
            dest = split_operands(it.ops)[0].strip().lower()
            if dest.startswith("cpsr") and ("_" not in dest or "f" in dest.split("_")[1]):
                return True
    return False

_C_STDLIB = """
    isalnum isalpha isblank iscntrl isdigit isgraph islower isprint ispunct
    isspace isupper isxdigit tolower toupper
    errno
    setlocale localeconv
    frexp ldexp modf scalbn scalbln ilogb logb nan nextafter nexttoward
    setjmp longjmp
    signal raise
    remove rename tmpfile tmpnam fclose fflush fopen freopen setbuf setvbuf
    fprintf fscanf printf scanf snprintf sprintf sscanf vfprintf vfscanf
    vprintf vscanf vsnprintf vsprintf vsscanf fgetc fgets fputc fputs getc
    getchar gets putc putchar puts ungetc fread fwrite fgetpos fseek fsetpos
    ftell rewind clearerr feof ferror perror
    atof atoi atol atoll strtod strtof strtold strtol strtoll strtoul strtoull
    rand srand calloc free malloc realloc abort atexit at_quick_exit exit
    _Exit quick_exit getenv system bsearch qsort abs labs llabs div ldiv lldiv
    mblen mbtowc wctomb mbstowcs wcstombs
    memcpy memmove strcpy strncpy strcat strncat memcmp strcmp strcoll strncmp
    strxfrm memchr strchr strcspn strpbrk strrchr strspn strstr strtok memset
    strerror strlen
    clock difftime mktime time asctime ctime gmtime localtime strftime
    fwprintf fwscanf swprintf swscanf vfwprintf vfwscanf vswprintf vswscanf
    vwprintf vwscanf wprintf wscanf fgetwc fgetws fputwc fputws fwide getwc
    getwchar putwc putwchar ungetwc wcstod wcstof wcstold wcstol wcstoll
    wcstoul wcstoull wcscpy wcsncpy wmemcpy wmemmove wcscat wcsncat wcscmp
    wcscoll wcsncmp wcsxfrm wmemcmp wcschr wcscspn wcspbrk wcsrchr wcsspn
    wcsstr wcstok wmemchr wcslen wmemset wcsftime btowc wctob mbsinit mbrlen
    mbrtowc wcrtomb mbsrtowcs wcsrtombs
    iswalnum iswalpha iswblank iswcntrl iswdigit iswgraph iswlower iswprint
    iswpunct iswspace iswupper iswxdigit iswctype wctype towlower towupper
    towctrans wctrans
"""

# <math.h> has an f and an l spelling of almost everything, so generate them
# rather than typing 200 names and getting one wrong.
_C_MATH = """
    acos asin atan atan2 cos sin tan acosh asinh atanh cosh sinh tanh exp exp2
    expm1 log log10 log1p log2 cbrt fabs hypot pow sqrt erf erfc lgamma tgamma
    ceil floor nearbyint rint lrint llrint round lround llround trunc fmod
    remainder remquo copysign fdim fmax fmin fma
"""

# Not standard C but the host libc's own: wasi-libc's strrchr calls its
# internal __memrchr(s, c, n), and MSL_C (Diamond/Pearl's recompiled
# MSL_Common_mem.s) defines a __memrchr of another shape, which won the link
# and made strrchr trap (wasm-ld: function signature mismatch).
_C_LIBC_INTERNAL = """
    __memrchr
"""

HOST_LIBC_NAMES = frozenset(
    _C_STDLIB.split()
    + _C_LIBC_INTERNAL.split()
    + [n + s for n in _C_MATH.split() for s in ("", "f", "l")])


def host_libc_collisions(names):
    """The subset of `names` the host's C library also owns."""
    return set(n for n in names if n in HOST_LIBC_NAMES)


def parse_shift(ctx, tok):
    """Parse a trailing shift spec like 'lsl #0x2' or 'asr r3'. Returns (op, amount_expr, is_imm, imm_val)."""
    tok = tok.strip()
    m = re.match(r"^(lsl|lsr|asr|ror|rrx)\b\s*(.*)$", tok, re.I)
    if not m:
        raise Unsupported("bad shift %r" % tok)
    op = m.group(1).lower()
    amt = m.group(2).strip()
    if op == "rrx":
        return op, None, True, 0
    if amt.startswith("#"):
        return op, None, True, parse_int(amt[1:])
    rn = reg_num(amt)
    if rn is None:
        raise Unsupported("bad shift amount %r" % amt)
    return op, ctx.regc(rn), False, None


def apply_shift_imm(op, val, n):
    """C expression for a constant-amount shift."""
    if op == "lsl":
        if n == 0:
            return val
        return "((uint32_t)(%s) << %d)" % (val, n)
    if op == "lsr":
        if n == 0:
            return "0u"          # LSR #0 encodes LSR #32
        return "((uint32_t)(%s) >> %d)" % (val, n)
    if op == "asr":
        if n == 0:
            return "((uint32_t)((int32_t)(%s) >> 31))" % val
        return "((uint32_t)((int32_t)(%s) >> %d))" % (val, n)
    if op == "ror":
        if n == 0:
            return "((uint32_t)(((%s) >> 1) | (cf << 31)))" % val   # RRX
        return "arm_ror(%s, %d)" % (val, n)
    raise Unsupported("shift %s" % op)


def shift_imm_carry(op, val, n):
    """C expression for the shifter carry-out of a constant-amount shift, or None."""
    if n == 0:
        if op == "lsl":
            return None                       # carry unchanged
        if op == "lsr":
            return "((uint32_t)(%s) >> 31)" % val
        if op == "asr":
            return "((uint32_t)(%s) >> 31)" % val
        if op == "ror":                       # RRX
            return "((uint32_t)(%s) & 1u)" % val
        return None
    if op == "lsl":
        return "(((uint32_t)(%s) >> (32 - %d)) & 1u)" % (val, n)
    if op in ("lsr", "asr"):
        return "(((uint32_t)(%s) >> %d) & 1u)" % (val, n - 1)
    if op == "ror":
        return "(((uint32_t)(%s) >> %d) & 1u)" % (val, (n - 1) & 31)
    return None


def parse_op2(ctx, toks):
    """
    Parse an ARM operand-2 given the remaining operand tokens.
    Returns (value_expr, carry_expr_or_None).
    """
    first = toks[0].strip()
    if first.startswith("#"):
        v = parse_int(first[1:])
        return "0x%08Xu" % (v & 0xFFFFFFFF), None
    rn = reg_num(first)
    if rn is None:
        raise Unsupported("bad operand %r" % first)
    val = ctx.regc(rn)
    if len(toks) == 1:
        return val, None
    op, amt_expr, is_imm, imm = parse_shift(ctx, toks[1])
    if is_imm:
        if op == "rrx":
            return ("((uint32_t)(((%s) >> 1) | (cf << 31)))" % val,
                    "((uint32_t)(%s) & 1u)" % val)
        return apply_shift_imm(op, val, imm), shift_imm_carry(op, val, imm)
    fn = {"lsl": "arm_lsl_r", "lsr": "arm_lsr_r", "asr": "arm_asr_r", "ror": "arm_ror_r"}[op]
    cfn = {"lsl": "arm_lsl_rc", "lsr": "arm_lsr_rc", "asr": "arm_asr_rc", "ror": None}[op]
    valexpr = "%s(%s, %s & 0xFFu)" % (fn, val, amt_expr)
    cexpr = None
    if cfn:
        cexpr = "%s(%s, %s & 0xFFu, cf)" % (cfn, val, amt_expr)
    return valexpr, cexpr


MEM_RE = re.compile(r"^\[\s*([^,\]]+?)\s*(?:,\s*(.+?))?\s*\](!?)\s*(?:,\s*(.+))?$")


def parse_mem(ctx, opstr):
    """
    Parse an addressing mode. Returns (addr_expr, writeback_stmt_or_None, base_c).
    Handles [Rn], [Rn,#i], [Rn,±Rm{,shift}], pre-indexed with !, and post-indexed.
    """
    m = MEM_RE.match(opstr.strip())
    if not m:
        raise Unsupported("bad addressing mode %r" % opstr)
    base_tok, inner, bang, post = m.group(1), m.group(2), m.group(3), m.group(4)
    bn = reg_num(base_tok)
    if bn is None:
        raise Unsupported("bad base register %r" % base_tok)
    base = ctx.regc(bn)

    def offset_expr(spec):
        spec = spec.strip()
        neg = False
        if spec.startswith("-"):
            neg = True
            spec = spec[1:].strip()
        elif spec.startswith("+"):
            spec = spec[1:].strip()
        if spec.startswith("#"):
            v = parse_int(spec[1:])
            if neg:
                v = -v
            return "0x%08Xu" % (v & 0xFFFFFFFF)
        toks = split_operands(spec)
        rn = reg_num(toks[0])
        if rn is None:
            raise Unsupported("bad offset %r" % spec)
        val = ctx.regc(rn)
        if len(toks) > 1:
            op, amt_expr, is_imm, imm = parse_shift(ctx, toks[1])
            val = apply_shift_imm(op, val, imm) if is_imm else \
                "%s(%s, %s & 0xFFu)" % (
                    {"lsl": "arm_lsl_r", "lsr": "arm_lsr_r",
                     "asr": "arm_asr_r", "ror": "arm_ror_r"}[op], val, amt_expr)
        return ("(uint32_t)(-(int32_t)(%s))" % val) if neg else val

    if post is not None:
        # post-indexed: use base, then update
        return base, "%s += %s;" % (base, offset_expr(post)), base
    if inner is None:
        return base, None, base
    off = offset_expr(inner)
    addr = "(%s + %s)" % (base, off)
    if bang:
        return addr, "%s = %s;" % (base, addr), base
    return addr, None, base


def parse_reglist(s):
    s = s.strip()
    if not (s.startswith("{") and s.endswith("}")):
        raise Unsupported("bad register list %r" % s)
    out = []
    for part in s[1:-1].split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            ra, rb = reg_num(a), reg_num(b)
            if ra is None or rb is None:
                raise Unsupported("bad register range %r" % part)
            out.extend(range(ra, rb + 1))
        else:
            r = reg_num(part)
            if r is None:
                raise Unsupported("bad register %r" % part)
            out.append(r)
    return sorted(set(out))


# --------------------------------------------------------------------------
# Instruction translation
# --------------------------------------------------------------------------

def cond_expr(cond):
    if cond is None or cond == "al":
        return None
    return CONDS[cond]


ALWAYS_FLAGS_THUMB = ("and", "eor", "adc", "sbc", "orr", "bic", "mvn",
                      "tst", "cmp", "cmn", "teq")


def thumb_sets_flags(mnem, toks, kind):
    """
    Which Thumb encodings update the condition flags.

    These files use gas divided syntax, so the flag-setting form is spelled
    without an S and has to be inferred from the operands:

      * The low-register ALU ops always set flags;
      * ADD/SUB set flags except in the SP and high-register encodings;
      * MOV sets flags for the immediate form but not the register form.

    'add rD, rS, #0' followed by BEQ is how Thumb copies a register and tests
    it at once, which the compiler emits constantly.
    """
    if mnem in ALWAYS_FLAGS_THUMB:
        return True
    regs = [reg_num(t) for t in toks]
    regs = [r for r in regs if r is not None]
    if mnem == "mov":
        # register form is the high-register MOV, which leaves flags alone
        return len(toks) > 1 and toks[-1].strip().startswith("#")
    if mnem in ("add", "sub"):
        if any(r in (13, 15) for r in regs):
            return False
        if any(r >= 8 for r in regs):
            return False
        return True
    return True


def _hold_carry(ctx, carry, sets_flags, out):
    """
    Evaluate a shifter carry-out before the destination register is written.

    The barrel shifter reads its operands and the ALU writes its result, in
    that order; C statements emitted the other way round do not. For
    `movs r1, r1, rrx` the carry-out is the old r1's bit 0, and computing it
    after the shift hands the next RRX a bit of the shifted value instead.
    `movs lr, r2, lsl lr` is the same fault with the shift amount.

    Returns the expression to assign to cf afterwards, or None.
    """
    if not carry or not sets_flags:
        return None
    t = ctx.newtmp()
    out.append("%s = %s;" % (t, carry))
    return t


def emit_dp(ctx, ins, out):
    """Data-processing instruction."""
    kind = DP_OPS[ins.mnem]
    toks = split_operands(ins.ops)

    # Thumb (and gas in ARM mode) allow the two-operand form, where the
    # destination is also the first source: "add r0, r1" == "add r0, r0, r1".
    if kind not in ("mov", "mvn") and not kind.startswith("test") and len(toks) == 2:
        toks = [toks[0], toks[0], toks[1]]

    sets_flags = ins.sflag or kind.startswith("test")
    if ins.thumb and not sets_flags:
        sets_flags = thumb_sets_flags(ins.mnem, split_operands(ins.ops), kind)

    if kind in ("mov", "mvn"):
        rd = reg_num(toks[0])
        if rd is None:
            raise Unsupported("bad destination %r" % toks[0])
        val, carry = parse_op2(ctx, toks[1:])
        if rd == 15:
            # mov pc, lr  ->  return
            src = toks[1].strip().lower()
            if src == "lr":
                out.append("ARM_RETURN();")
                return
            out.append("return armrec_dispatch(%s, r0, r1, r2, r3);" % val)
            return
        dst = ctx.regc(rd)
        expr = val if kind == "mov" else "(~(%s))" % val
        cf_pre = _hold_carry(ctx, carry, sets_flags, out)
        out.append("%s = %s;" % (dst, expr))
        if sets_flags:
            if cf_pre:
                out.append("cf = %s;" % cf_pre)
            out.append("ARM_NZ(nf, zf, %s);" % dst)
        return

    if kind.startswith("test"):
        rn = reg_num(toks[0])
        if rn is None:
            raise Unsupported("bad operand %r" % toks[0])
        a = ctx.regc(rn)
        val, carry = parse_op2(ctx, toks[1:])
        t = ctx.newtmp()
        if kind == "test_log":
            op = "&" if ins.mnem == "tst" else "^"
            out.append("%s = %s %s %s;" % (t, a, op, val))
            if carry:
                out.append("cf = %s;" % carry)
            out.append("ARM_NZ(nf, zf, %s);" % t)
        elif kind == "test_cmp":
            v = ctx.newtmp()
            out.append("%s = %s;" % (v, val))
            out.append("%s = %s - %s;" % (t, a, v))
            out.append("cf = ARM_SUB_C(%s, %s, %s);" % (a, v, t))
            out.append("vf = ARM_SUB_V(%s, %s, %s);" % (a, v, t))
            out.append("ARM_NZ(nf, zf, %s);" % t)
        else:  # cmn
            v = ctx.newtmp()
            out.append("%s = %s;" % (v, val))
            out.append("%s = %s + %s;" % (t, a, v))
            out.append("cf = ARM_ADD_C(%s, %s, %s);" % (a, v, t))
            out.append("vf = ARM_ADD_V(%s, %s, %s);" % (a, v, t))
            out.append("ARM_NZ(nf, zf, %s);" % t)
        return

    # three-operand forms
    if len(toks) < 3:
        raise Unsupported("%s needs 3 operands" % ins.mnem)
    rd = reg_num(toks[0])
    rn = reg_num(toks[1])
    if rd is None or rn is None:
        raise Unsupported("bad operands for %s" % ins.mnem)
    if rd == 15:
        return emit_pc_write(ctx, ins, toks, out)
    dst = ctx.regc(rd)
    a = ctx.regc(rn)
    val, carry = parse_op2(ctx, toks[2:])

    if kind == "log":
        op = {"and": "&", "eor": "^", "orr": "|", "bic": "& ~"}[ins.mnem]
        cf_pre = _hold_carry(ctx, carry, sets_flags, out)
        out.append("%s = %s %s (%s);" % (dst, a, op, val))
        if sets_flags:
            if cf_pre:
                out.append("cf = %s;" % cf_pre)
            out.append("ARM_NZ(nf, zf, %s);" % dst)
        return

    # arithmetic: keep the operands in temporaries because dst may alias them
    va, vb = ctx.newtmp(), ctx.newtmp()
    out.append("%s = %s;" % (va, a))
    out.append("%s = %s;" % (vb, val))
    if kind == "add":
        out.append("%s = %s + %s;" % (dst, va, vb))
        cexpr, vexpr, x, y = "ARM_ADD_C", "ARM_ADD_V", va, vb
    elif kind == "sub":
        out.append("%s = %s - %s;" % (dst, va, vb))
        cexpr, vexpr, x, y = "ARM_SUB_C", "ARM_SUB_V", va, vb
    elif kind == "rsb":
        out.append("%s = %s - %s;" % (dst, vb, va))
        cexpr, vexpr, x, y = "ARM_SUB_C", "ARM_SUB_V", vb, va
    elif kind == "adc":
        out.append("%s = %s + %s + cf;" % (dst, va, vb))
        if sets_flags:
            out.append("cf = (uint32_t)(((uint64_t)%s + (uint64_t)%s + cf) >> 32);" % (va, vb))
            out.append("vf = ARM_ADD_V(%s, %s, %s);" % (va, vb, dst))
            out.append("ARM_NZ(nf, zf, %s);" % dst)
        return
    elif kind == "sbc":
        out.append("%s = %s - %s - (1u - cf);" % (dst, va, vb))
        if sets_flags:
            out.append("cf = ((uint64_t)%s >= (uint64_t)%s + (uint64_t)(1u - cf));" % (va, vb))
            out.append("vf = ARM_SUB_V(%s, %s, %s);" % (va, vb, dst))
            out.append("ARM_NZ(nf, zf, %s);" % dst)
        return
    elif kind == "rsc":
        out.append("%s = %s - %s - (1u - cf);" % (dst, vb, va))
        if sets_flags:
            out.append("cf = ((uint64_t)%s >= (uint64_t)%s + (uint64_t)(1u - cf));" % (vb, va))
            out.append("vf = ARM_SUB_V(%s, %s, %s);" % (vb, va, dst))
            out.append("ARM_NZ(nf, zf, %s);" % dst)
        return
    else:
        raise Unsupported("dp kind %s" % kind)

    if sets_flags:
        out.append("cf = %s(%s, %s, %s);" % (cexpr, x, y, dst))
        out.append("vf = %s(%s, %s, %s);" % (vexpr, x, y, dst))
        out.append("ARM_NZ(nf, zf, %s);" % dst)


def emit_local_dispatch(ctx, expr, out):
    """
    Branch to a computed address.

    Every label in these files encodes its own guest address and the address
    pass gives each instruction one, so a computed branch resolves against the
    labels of the enclosing function. That covers both switch idioms the
    compiler emits without recognising either. Anything landing outside the
    function is treated as an indirect call.
    """
    if not ctx.local_labels:
        raise Unsupported("computed branch with no local labels to resolve against")
    t = ctx.newtmp()
    out.append("%s = %s;" % (t, expr))
    out.append("switch (%s) {" % t)
    for addr in sorted(ctx.local_labels):
        out.append("case 0x%08Xu: goto L_%s;" % (addr, sanitize(ctx.local_labels[addr])))
    out.append("default: ARM_TAILCALL_IND(%s, r0, r1, r2, r3);" % t)
    out.append("}")


def emit_pc_write(ctx, ins, toks, out):
    """Any data-processing instruction whose destination is PC."""
    kind = DP_OPS[ins.mnem]
    rn = reg_num(toks[1])
    a = ctx.regc(rn)
    val, _ = parse_op2(ctx, toks[2:])
    if kind == "add":
        expr = "(%s + %s)" % (a, val)
    elif kind == "sub":
        expr = "(%s - %s)" % (a, val)
    else:
        raise Unsupported("write to PC via %s" % ins.mnem)
    emit_local_dispatch(ctx, expr, out)


def emit_shift(ctx, ins, out):
    """Standalone LSL/LSR/ASR/ROR (Thumb form, and the gas ARM alias)."""
    toks = split_operands(ins.ops)
    rd = reg_num(toks[0])
    if rd is None:
        raise Unsupported("bad destination %r" % toks[0])
    dst = ctx.regc(rd)
    if len(toks) == 2:
        src_toks = [toks[0], toks[1]]
    else:
        src_toks = [toks[1], toks[2]]
    rs = reg_num(src_toks[0])
    if rs is None:
        raise Unsupported("bad source %r" % src_toks[0])
    src = ctx.regc(rs)
    amt = src_toks[1].strip()
    op = ins.mnem
    if amt.startswith("#"):
        n = parse_int(amt[1:])
        expr = apply_shift_imm(op, src, n)
        carry = shift_imm_carry(op, src, n)
    else:
        rn = reg_num(amt)
        if rn is None:
            raise Unsupported("bad shift amount %r" % amt)
        aexpr = ctx.regc(rn)
        fn = {"lsl": "arm_lsl_r", "lsr": "arm_lsr_r", "asr": "arm_asr_r", "ror": "arm_ror_r"}[op]
        cfn = {"lsl": "arm_lsl_rc", "lsr": "arm_lsr_rc", "asr": "arm_asr_rc", "ror": None}[op]
        expr = "%s(%s, %s & 0xFFu)" % (fn, src, aexpr)
        carry = "%s(%s, %s & 0xFFu, cf)" % (cfn, src, aexpr) if cfn else None
    # Thumb shifts always set flags; in ARM only with an explicit S.
    sets = ins.thumb or ins.sflag
    if sets and carry:
        t = ctx.newtmp()
        out.append("%s = %s;" % (t, carry))
        out.append("%s = %s;" % (dst, expr))
        out.append("cf = %s;" % t)
    else:
        out.append("%s = %s;" % (dst, expr))
    if sets:
        out.append("ARM_NZ(nf, zf, %s);" % dst)


def emit_mul(ctx, ins, out):
    toks = split_operands(ins.ops)
    m = ins.mnem
    if m == "mul":
        # Thumb spells it "mul rD, rM", meaning rD = rD * rM, and always sets
        # the flags.
        if len(toks) == 2:
            toks = [toks[0], toks[0], toks[1]]
        rd, rm, rs = (reg_num(t) for t in toks[:3])
        out.append("%s = (uint32_t)(%s * %s);" % (ctx.regc(rd), ctx.regc(rm), ctx.regc(rs)))
        if ins.sflag or ins.thumb:
            out.append("ARM_NZ(nf, zf, %s);" % ctx.regc(rd))
        return
    if m == "mla":
        rd, rm, rs, rn = (reg_num(t) for t in toks[:4])
        out.append("%s = (uint32_t)(%s * %s + %s);" %
                   (ctx.regc(rd), ctx.regc(rm), ctx.regc(rs), ctx.regc(rn)))
        if ins.sflag:
            out.append("ARM_NZ(nf, zf, %s);" % ctx.regc(rd))
        return
    if m == "neg":
        rd, rm = reg_num(toks[0]), reg_num(toks[1])
        a = ctx.newtmp()
        out.append("%s = %s;" % (a, ctx.regc(rm)))
        out.append("%s = (uint32_t)(0u - %s);" % (ctx.regc(rd), a))
        out.append("cf = ARM_SUB_C(0u, %s, %s);" % (a, ctx.regc(rd)))
        out.append("vf = ARM_SUB_V(0u, %s, %s);" % (a, ctx.regc(rd)))
        out.append("ARM_NZ(nf, zf, %s);" % ctx.regc(rd))
        return
    if m in ("umull", "smull", "umlal", "smlal"):
        rdlo, rdhi, rm, rs = (reg_num(t) for t in toks[:4])
        lo, hi = ctx.regc(rdlo), ctx.regc(rdhi)
        t = ctx.newtmp()
        if m.startswith("u"):
            prod = "((uint64_t)(uint32_t)%s * (uint64_t)(uint32_t)%s)" % (ctx.regc(rm), ctx.regc(rs))
        else:
            prod = "((uint64_t)((int64_t)(int32_t)%s * (int64_t)(int32_t)%s))" % (ctx.regc(rm), ctx.regc(rs))
        if m.endswith("mlal"):
            out.append("{ uint64_t %s = %s + (((uint64_t)%s << 32) | (uint32_t)%s);" % (t, prod, hi, lo))
        else:
            out.append("{ uint64_t %s = %s;" % (t, prod))
        out.append("  %s = (uint32_t)%s; %s = (uint32_t)(%s >> 32); }" % (lo, t, hi, t))
        if ins.sflag:
            out.append("nf = %s >> 31; zf = (%s == 0 && %s == 0);" % (hi, hi, lo))
        return
    # ARMv5TE halfword multiplies: SMULxy / SMLAxy, where x and y select the
    # bottom ("b") or top ("t") halfword of each source. The selectors are the
    # last two characters, right after the 4-character base.
    def half(reg, sel):
        if sel == "t":
            return "(int32_t)(int16_t)((%s) >> 16)" % reg
        return "(int32_t)(int16_t)(%s)" % reg

    if m in ("smulbb", "smulbt", "smultb", "smultt"):
        rd, rm, rs = (reg_num(t) for t in toks[:3])
        out.append("%s = (uint32_t)((%s) * (%s));" %
                   (ctx.regc(rd), half(ctx.regc(rm), m[4]), half(ctx.regc(rs), m[5])))
        return
    if m in ("smlabb", "smlabt", "smlatb", "smlatt"):
        rd, rm, rs, rn = (reg_num(t) for t in toks[:4])
        out.append("%s = (uint32_t)((%s) * (%s) + (int32_t)%s);" %
                   (ctx.regc(rd), half(ctx.regc(rm), m[4]), half(ctx.regc(rs), m[5]),
                    ctx.regc(rn)))
        return
    raise Unsupported("multiply form %s" % m)


LDST_KIND = {
    "ldr": ("ARM_LD32", None), "ldrb": ("ARM_LD8", None), "ldrh": ("ARM_LD16", None),
    "ldrsb": ("ARM_LD8S", None), "ldrsh": ("ARM_LD16S", None),
    "str": (None, "ARM_ST32"), "strb": (None, "ARM_ST8"), "strh": (None, "ARM_ST16"),
}


def emit_ldst(ctx, ins, out):
    toks = split_operands(ins.ops)
    rd = reg_num(toks[0])
    if rd is None:
        raise Unsupported("bad register %r" % toks[0])
    load, store = LDST_KIND[ins.mnem]
    rest = ",".join(toks[1:]).strip()

    if not rest.startswith("["):
        # PC-relative literal load: "ldr r0, _020BF568 ; =calcTexMtx_"
        if store:
            raise Unsupported("store to a literal label")
        val = ctx.literals.get(rest.strip())
        if val is None:
            raise Unsupported("unresolved literal %r" % rest)
        if rd == 15:
            # "ldr pc, <literal>" is a tail call through the literal pool.
            out.append("ARM_TAILCALL_IND(%s, r0, r1, r2, r3);" % val)
            return
        out.append("%s = %s;" % (ctx.regc(rd), val))
        return

    addr, wb, base = parse_mem(ctx, rest)
    if rd == 15:
        if store:
            raise Unsupported("store from PC")
        t = ctx.newtmp()
        out.append("%s = %s(%s);" % (t, load, addr))
        if wb:
            out.append(wb)
        if base == "ARM_SP":
            # `ldr pc, [sp], #4` is pop {pc}: the single-register spelling of
            # the return emit_block_transfer() already handles, and mwcc emits
            # it for a one-register frame. Dispatching on the value would mean
            # dispatching on a return address, which is at no registered entry
            # point. Only off the stack: `ldr pc, [r3]` elsewhere is a real
            # indirect jump.
            out.append("(void)%s; ARM_RETURN();" % t)
            return
        out.append("return armrec_dispatch(%s, r0, r1, r2, r3);" % t)
        return
    dst = ctx.regc(rd)
    if load:
        # writeback must use the pre-update address, and dst may alias base
        if wb:
            t = ctx.newtmp()
            out.append("%s = %s(%s);" % (t, load, addr))
            out.append(wb)
            out.append("%s = %s;" % (dst, t))
        else:
            out.append("%s = %s(%s);" % (dst, load, addr))
    else:
        out.append("%s(%s, %s);" % (store, addr, dst))
        if wb:
            out.append(wb)


def emit_ldm_stm(ctx, ins, out, is_load, mode):
    toks = split_operands(ins.ops)
    base_tok = toks[0].strip()
    wb = base_tok.endswith("!")
    if wb:
        base_tok = base_tok[:-1].strip()
    bn = reg_num(base_tok)
    if bn is None:
        raise Unsupported("bad LDM/STM base %r" % base_tok)
    rl = ",".join(toks[1:]).strip()
    if rl.endswith("^"):
        # The ^ suffix transfers the user-mode bank of r13/r14. The port runs
        # everything in one processor mode, so there is only one bank and the
        # transfer is the ordinary one.
        rl = rl[:-1].strip()
    regs = parse_reglist(rl)
    if is_load and wb and bn in regs and (ins.thumb or bn == regs[-1] != regs[0]):
        # A load whose base is in its own list: the ARM946E-S (ARMv5) keeps
        # the loaded value, never the written-back base, for every Thumb LDMIA
        # and for an ARM LDM whose base is the last of several registers.
        # mwcc relies on it: `ldmia r1!, {r0, r1}` loads a two-word struct
        # through r1 and passes both words on (sub_020116CC hands
        # sub_02011480 a mask and a screen this way). Writing back here would
        # turn the screen into the struct address + 8.
        wb = False
    emit_block_transfer(ctx, out, ctx.regc(bn), regs, mode, is_load, wb)


def emit_block_transfer(ctx, out, base, regs, mode, is_load, wb):
    n = len(regs)
    if n == 0:
        raise Unsupported("empty register list")
    ptr = ctx.newtmp()
    if mode == "ia":
        out.append("%s = %s;" % (ptr, base))
        newbase = "%s + %du" % (base, 4 * n)
    elif mode == "ib":
        out.append("%s = %s + 4u;" % (ptr, base))
        newbase = "%s + %du" % (base, 4 * n)
    elif mode == "da":
        out.append("%s = %s - %du;" % (ptr, base, 4 * (n - 1)))
        newbase = "%s - %du" % (base, 4 * n)
    elif mode == "db":
        out.append("%s = %s - %du;" % (ptr, base, 4 * n))
        newbase = "%s - %du" % (base, 4 * n)
    else:
        raise Unsupported("LDM/STM mode %s" % mode)

    # Compute the writeback value up front: the base register may itself be in
    # the list, and on a store ARM writes the *original* base.
    nb = None
    if wb:
        nb = ctx.newtmp()
        out.append("%s = %s;" % (nb, newbase))

    pc_loaded = False
    pcval = None
    for i, r in enumerate(regs):
        off = "" if i == 0 else " + %du" % (4 * i)
        if is_load:
            if r == 15:
                pcval = ctx.newtmp()
                out.append("%s = ARM_LD32(%s%s);" % (pcval, ptr, off))
                pc_loaded = True
            else:
                out.append("%s = ARM_LD32(%s%s);" % (ctx.regc(r), ptr, off))
        else:
            out.append("ARM_ST32(%s%s, %s);" % (ptr, off, ctx.regc(r)))
    if wb:
        out.append("%s = %s;" % (base, nb))
    if pc_loaded:
        # Loading PC off the stack is a return.
        out.append("(void)%s; ARM_RETURN();" % pcval)


def emit_branch(ctx, ins, out, func, is_call):
    target = ins.ops.strip()
    # A BL whose target is an entry point is a call: direct recursion, or a
    # call to a sibling entry point of the same code blob. A BL to any other
    # local label is not a call at all. Thumb's B reaches +-2 KB and mwcc
    # spells anything further as BL, relying on the LR the prologue pushed.

    # That holds only for a long branch inside the branching function. A `bl`
    # reaching into a sibling entry point's range is a call to a function whose
    # start marker the disassembly lost, and split_call_targets() promotes the
    # label before this runs.
    if target in func.labels and not (is_call and target in func.entries):
        out.append("goto L_%s;" % sanitize(target))
        return
    # `bl armrec_dispatch_XXXXXXXX`: a call whose target the front end could
    # not bind to a name at build time (tools/ndsrec: an address in an
    # overlay window more than one overlay can occupy beside the caller).
    # It is resolved at run time by residency, as the hardware does.
    dm = DISPATCH_TARGET.match(target)
    if dm:
        addr = int(dm.group(1), 16)
        if is_call:
            t = ctx.newtmp()
            out.append("{ uint64_t %s = armrec_dispatch(0x%08Xu, r0, r1, r2, r3);"
                       % (t, addr))
            out.append("  r0 = (uint32_t)%s; r1 = (uint32_t)(%s >> 32); }" % (t, t))
        else:
            out.append("return armrec_dispatch(0x%08Xu, r0, r1, r2, r3);" % addr)
        return
    # branch/call to another function
    sym = ctx.rename.get(target, sanitize(target))
    if ctx.symtab.get(target) is None and target not in ctx.symtab:
        ctx.used_ext.add(target)
    # Leaving recompiled code means arguments five and up move from the
    # emulated stack to the host one. See ARMREC_CALL_EXT. The test is on the
    # assembly name rather than on `sym`: the guest_ rename points a colliding
    # libc name back at MSL_C's own body, which is not a crossing at all.
    if ctx.is_host_call(target):
        why = ctx.abi_trap.get(target)
        if why is not None:
            # Do not give a trapped callee a synthetic guest address for this
            # call. Registering it would let an indirect call reach it through
            # armrec_dispatch() and corrupt the caller's stack quietly, where
            # an unregistered address aborts by name.
            ctx.used_ext.discard(target)
            t = ctx.newtmp()
            out.append('{ uint64_t %s = ARMREC_CALL_ABI_TRAP("%s", "%s");'
                       % (t, target, why))
            out.append("  r0 = (uint32_t)%s; r1 = (uint32_t)(%s >> 32); }" % (t, t))
            if not is_call:
                out.append("ARM_RETURN();")
            return
        if TARGET_WASM:
            # The adapter takes the uniform signature and reads the stack
            # words itself; see C2U_PREFIX. The boundary is keyed by the C
            # name the adapter wraps, which is what the bridge needs.
            ctx.ext_calls[sym] += 1
            sym = C2U_PREFIX + sym
            call, tail = "ARMREC_CALL", "ARM_TAILCALL"
        else:
            ctx.ext_calls[target] += 1
            call, tail = "ARMREC_CALL_EXT", "ARM_TAILCALL_EXT"
    else:
        call, tail = "ARMREC_CALL", "ARM_TAILCALL"
    if is_call:
        t = ctx.newtmp()
        out.append("{ uint64_t %s = %s(%s, r0, r1, r2, r3);" % (t, call, sym))
        out.append("  r0 = (uint32_t)%s; r1 = (uint32_t)(%s >> 32); }" % (t, t))
        if target in FLAG_RESULT_FUNCS:
            out.append("ARM_FLAGS_FROM_PSR(armrec_mrs(0));")
    else:
        out.append("%s(%s, r0, r1, r2, r3);" % (tail, sym))


def emit_insn(ctx, ins, func, out):
    m = ins.mnem

    if m == "nop" or m == "bkpt":
        out.append("/* nop */")
        return
    if m in DP_OPS:
        return emit_dp(ctx, ins, out)
    if m in SHIFT_OPS:
        return emit_shift(ctx, ins, out)
    if m in ("mul", "mla", "umull", "umlal", "smull", "smlal", "neg") or m.startswith("smul") or m.startswith("smla"):
        return emit_mul(ctx, ins, out)
    if m in LDST_KIND:
        return emit_ldst(ctx, ins, out)
    if m.startswith("ldm:") or m.startswith("stm:"):
        base, mode = m.split(":")
        modes = LDM_MODES if base == "ldm" else STM_MODES
        return emit_ldm_stm(ctx, ins, out, base == "ldm", modes[mode])
    if m == "push":
        return emit_block_transfer(ctx, out, "ARM_SP", parse_reglist(ins.ops), "db", False, True)
    if m == "pop":
        return emit_block_transfer(ctx, out, "ARM_SP", parse_reglist(ins.ops), "ia", True, True)
    if m == "b":
        return emit_branch(ctx, ins, out, func, False)
    if m in ("bl", "blx") and reg_num(ins.ops.strip()) is None:
        return emit_branch(ctx, ins, out, func, True)
    if m == "bx":
        r = reg_num(ins.ops.strip())
        if r is None:
            return emit_branch(ctx, ins, out, func, False)
        if r == 14:
            out.append("ARM_RETURN();")
        elif id(ins) in ctx.call_bx:
            # `mov lr, pc; bx rN`, which is ARMv4T's indirect call. See call_bx_set().
            t = ctx.newtmp()
            out.append("{ uint64_t %s = armrec_dispatch(%s, r0, r1, r2, r3);"
                       % (t, ctx.regc(r)))
            out.append("  r0 = (uint32_t)%s; r1 = (uint32_t)(%s >> 32); }" % (t, t))
        else:
            out.append("ARM_TAILCALL_IND(%s, r0, r1, r2, r3);" % ctx.regc(r))
        return
    if m == "blx":
        r = reg_num(ins.ops.strip())
        t = ctx.newtmp()
        out.append("{ uint64_t %s = armrec_dispatch(%s, r0, r1, r2, r3);" % (t, ctx.regc(r)))
        out.append("  r0 = (uint32_t)%s; r1 = (uint32_t)(%s >> 32); }" % (t, t))
        return
    if m in ("swp", "swpb"):
        # Atomic swap: one CPU here, so a load then a store. The old value is
        # read before the store, so `swp r0, r0, [r1]` is right.
        toks = split_operands(ins.ops)
        if len(toks) != 3:
            raise Unsupported("bad %s operands %r" % (m, ins.ops))
        rd, rm = reg_num(toks[0]), reg_num(toks[1])
        mb = re.match(r"^\[\s*(\w+)\s*\]$", toks[2].strip())
        rn = reg_num(mb.group(1)) if mb else None
        if rd is None or rm is None or rn is None or 15 in (rd, rm, rn):
            raise Unsupported("bad %s operands %r" % (m, ins.ops))
        sz = "8" if m == "swpb" else "32"
        a, v = ctx.newtmp(), ctx.newtmp()
        out.append("%s = %s; %s = ARM_LD%s(%s); ARM_ST%s(%s, %s); %s = %s;"
                   % (a, ctx.regc(rn), v, sz, a, sz, a, ctx.regc(rm),
                      ctx.regc(rd), v))
        return
    if m in ("swi", "svc"):
        v = ins.ops.strip()
        num = parse_int(v[1:]) if v.startswith("#") else parse_int(v)
        out.append("r0 = armrec_swi(0x%Xu, r0, r1, r2, r3);" % num)
        return
    if m == "adr":
        # ADR rd, label: materialise the address of a local label.
        toks = split_operands(ins.ops)
        rd = reg_num(toks[0])
        target = toks[1].strip()
        addr = ctx.symtab.get(target)
        if addr is None:
            addr = addr_from_name(target)
        if addr is None:
            raise Unsupported("ADR to unknown label %r" % target)
        out.append("%s = 0x%08Xu;" % (ctx.regc(rd), addr))
        return
    if m == "clz":
        toks = split_operands(ins.ops)
        out.append("%s = arm_clz(%s);" % (ctx.regc(reg_num(toks[0])), ctx.regc(reg_num(toks[1]))))
        return
    if m == "mrs":
        toks = split_operands(ins.ops)
        spsr = 1 if "spsr" in toks[1].lower() else 0
        if spsr:
            out.append("%s = armrec_mrs(1);" % ctx.regc(reg_num(toks[0])))
        else:
            # The flags live in this function's locals, not in the word
            # armrec_mrs keeps: NZCV come from the locals, the rest from it.
            out.append("%s = (armrec_mrs(0) & 0x0FFFFFFFu) | ((uint32_t)nf << 31) | "
                       "((uint32_t)zf << 30) | ((uint32_t)cf << 29) | ((uint32_t)vf << 28);"
                       % ctx.regc(reg_num(toks[0])))
        return
    if m == "msr":
        toks = split_operands(ins.ops)
        dest = toks[0].lower()
        spsr = 1 if dest.startswith("spsr") else 0
        mask = 0
        fields = dest.split("_")[1] if "_" in dest else "cxsf"
        for ch, bits in (("c", 0x000000FF), ("x", 0x0000FF00),
                         ("s", 0x00FF0000), ("f", 0xFF000000)):
            if ch in fields:
                mask |= bits
        src = toks[1].strip()
        if src.startswith("#"):
            val = "0x%08Xu" % (parse_int(src[1:]) & 0xFFFFFFFF)
        else:
            rn = reg_num(src)
            if rn is None:
                raise Unsupported("bad MSR source %r" % src)
            val = ctx.regc(rn)
        out.append("armrec_msr(%d, 0x%08Xu, %s);" % (spsr, mask, val))
        if not spsr and mask & 0xFF000000:
            out.append("ARM_FLAGS_FROM_PSR(%s);" % val)
        return
    if m in ("mrc", "mcr"):
        toks = [t.strip() for t in split_operands(ins.ops)]
        def cnum(t):
            t = t.lower().lstrip("pc")
            return parse_int(t) if t else 0
        cp = cnum(toks[0])
        op1 = parse_int(toks[1])
        reg = toks[2]
        crn, crm = cnum(toks[3]), cnum(toks[4])
        op2 = parse_int(toks[5]) if len(toks) > 5 else 0
        rn = reg_num(reg)
        if m == "mrc":
            out.append("%s = armrec_mrc(%d, %d, %d, %d, %d);" % (ctx.regc(rn), cp, op1, crn, crm, op2))
        else:
            out.append("armrec_mcr(%d, %d, %d, %d, %d, %s);" % (cp, op1, crn, crm, op2, ctx.regc(rn)))
        return
    raise Unsupported("mnemonic %s" % m)


# --------------------------------------------------------------------------
# Literal pools
# --------------------------------------------------------------------------

LITERAL_RE = re.compile(r"^\s*([A-Za-z_.$?][\w.$?]*):\s*\.word\s+([^;@]+)")


def collect_literals(path, lines=None):
    """
    Map a literal-pool label to the single .word it holds, for the
    'ldr rN, _020BF568 ; =calcTexMtx_' form the assembler emits.

    `lines` is the preprocessed text, and passing it is not an optimisation.
    Reading the file raw made this pass see #included constants as symbol
    names, which then reached the link undefined, and take both arms of every
    `.ifdef`.
    """
    lits = {}
    pending = None
    if lines is None:
        with open(path, "r", errors="replace") as fh:
            lines = fh.readlines()
    for raw in lines:
        code, _ = split_comment(raw.rstrip("\n"))
        code = code.strip()
        if not code:
            continue
        m = LITERAL_RE.match(code)
        if m:
            val = m.group(2).strip()
            if "," not in val:
                lits[m.group(1)] = val
            pending = None
            continue
        m = re.match(r"^([A-Za-z_.$?][\w.$?]*):\s*$", code)
        if m:
            pending = m.group(1)
            continue
        if pending:
            m = re.match(r"^\.word\s+([^;@]+)$", code)
            if m and "," not in m.group(1):
                lits[pending] = m.group(1).strip()
            pending = None
    return lits


def literal_c_value(ctx, tok):
    """
    Resolve a literal-pool word to a C expression.

    Handles a bare constant, a symbol, and a symbol with an offset expression
    such as "gMTRNG_State + 607 * 4".
    """
    tok = tok.strip()
    try:
        return "0x%08Xu" % (eval_asm_expr(tok, ctx.symtab) & 0xFFFFFFFF)
    except Unsupported:
        pass
    # Leading symbol we cannot resolve yet (it lives in decompiled C), plus an
    # optional constant displacement.
    m = re.match(r"^([A-Za-z_.$?][\w.$?]*)\s*(?:([-+])\s*(.+))?$", tok)
    if not m:
        raise Unsupported("cannot resolve literal %r" % tok)
    base = ctx.sym_addr(m.group(1))
    if m.group(2):
        return "(%s %s 0x%Xu)" % (base, m.group(2), eval_asm_expr(m.group(3), ctx.symtab))
    return base


# --------------------------------------------------------------------------
# Function emission
# --------------------------------------------------------------------------

JUMPTABLE_CASE = re.compile(r";\s*case\s+(\d+)", re.I)


def is_pc_jumptable(ins):
    """Detect  add pc, pc, Rn, lsl #2  (the compiler's switch idiom)."""
    if ins.mnem != "add":
        return None
    toks = split_operands(ins.ops)
    if len(toks) < 3:
        return None
    if reg_num(toks[0]) != 15 or reg_num(toks[1]) != 15:
        return None
    rn = reg_num(toks[2])
    if rn is None or len(toks) < 4:
        return None
    m = re.match(r"^lsl\s*#\s*(?:0x)?2$", toks[3].strip(), re.I)
    if not m:
        return None
    return rn


ARM_INSN_SIZE = 4


def insn_size(mnem, ops, thumb):
    """
    How many bytes this instruction occupies. Thumb is two except the BL/BLX
    *immediate* pair, which is a 32-bit encoding; `blx rN` is a register form
    and is two.
    """
    if not thumb:
        return ARM_INSN_SIZE
    if mnem in ("bl", "blx") and reg_num((ops or "").strip()) is None:
        return 4
    return 2


def directive_size(name, args):
    """
    How many bytes a data directive emits, or None if armrec cannot say.

    One source of truth for the two walks that need it: assign_addresses(),
    which places the instructions of one function, and parse_file()'s location
    counter, which places data between functions. A second copy of this
    arithmetic is what would let them drift.
    """
    args = args or ""
    count = len([x for x in args.split(",") if x.strip()])
    if name in ("word", "long", "int"):
        return 4 * count
    if name in ("short", "hword"):
        return 2 * count
    if name == "byte":
        return count
    if name in ("space", "skip"):
        try:
            return parse_int(args.split(",")[0])
        except Unsupported:
            return None
    if name == "asciz" or name == "ascii":
        m = re.match(r'^\s*"(.*)"\s*$', args, re.S)
        if m is None:
            return None
        s = m.group(1).encode("latin-1", "replace") \
                      .decode("unicode_escape").encode("latin-1")
        return len(s) + (1 if name == "asciz" else 0)
    return None


def align_up(addr, n):
    """gas's own rule: advance the counter to the next multiple of n."""
    if addr is None or n <= 1:
        return addr
    return (addr + n - 1) & ~(n - 1)


def alignment_of(name, args):
    """
    The boundary an explicit .align/.balign asks for, or None.

    `.align n` on an ARM target is a power of two, `.balign n` is the byte
    count itself. Both take an optional fill value and an optional maximum
    skip, which are the second and third operands and do not change where the
    counter lands.
    """
    tok = (args or "").split(",")[0].strip()
    if not tok:
        # A bare `.align` is 4 on this target. The tree spells it with an
        # operand everywhere, so this is a floor rather than a live case.
        return 4
    try:
        n = parse_int(tok)
    except Unsupported:
        return None
    if name == "align":
        if n < 0 or n > 16:
            return None
        return 1 << n
    if n <= 0 or (n & (n - 1)):
        return None
    return n


# What the assembler puts a data directive on before it emits anything, and 1
# for a directive that takes the counter as it finds it. This is ARM's own
# DCD/DCW rule: a word datum goes on a 4-byte boundary and a halfword on a
# 2-byte one, whatever the counter was on. The assembler here is mwasmarm
# rather than gas, so gas's rules are not the ones that built the ROM.

# UNK_020F767C is four 12-byte entries spelled `.word / .byte x3 / .word`,
# which is 11 bytes of directives; without the pad every entry after the first
# sits low. No `.short` here ever lands on an odd address, so the halfword rule
# has never been checked against hardware. A firing is counted into the report.
IMPLICIT_ALIGN = {"word": 4, "long": 4, "int": 4, "short": 2, "hword": 2}
UNARBITRATED_ALIGN = ("short", "hword")
# main() lifts a parse problem carrying this prefix into the build's report,
# beside the data findings, so "nothing in the tree needs the number nobody
# checked" is a claim `make status` states rather than one a run asserts.
UNARBITRATED_TAG = "implicit alignment with no oracle: "


def implicit_alignment(name):
    """The boundary a data directive starts on, whatever precedes it."""
    return IMPLICIT_ALIGN.get(name, 1)


def assign_addresses(func):
    """
    Give every instruction its guest address, and return the address just past
    the end (None if the walk lost track).

    Local labels encode their own address (_020424D0), so the walk re-anchors
    at each one and any drift is confined to a single basic block. Thumb
    instructions are two bytes except the BL/BLX pair, which is four.
    """
    addr = func.addr
    for it in func.items:
        if isinstance(it, Label):
            if it.addr is not None:
                addr = it.addr
            continue
        if isinstance(it, Directive):
            if addr is None:
                continue
            # The same implicit alignment parse_file()'s counter applies, for
            # the same reason: the two walks share directive_size() precisely
            # so that they cannot drift, and a second copy of one rule and not
            # the other is how they would.
            addr = align_up(addr, implicit_alignment(it.name))
            n = directive_size(it.name, it.args)
            addr = None if n is None else addr + n
            continue
        it.addr = addr
        if addr is not None:
            addr += insn_size(it.mnem, it.ops, it.thumb)
    return addr


def writes_pc(ins):
    """True if this data-processing instruction has PC as its destination."""
    if ins.mnem not in DP_OPS:
        return False
    toks = split_operands(ins.ops)
    return bool(toks) and reg_num(toks[0]) == 15


BRANCH_MNEMS = ("b", "bl", "blx")


def func_falls_through(items):
    """
    True if control can run off the end of this function into the next one.

    "The end of a function" is frequently not a boundary at all: mwcc lays the
    entry points of one code blob out contiguously, so ov06_0224ED7C ends on
    `add r0, r5, #0` and the hardware simply carries on into ov06_0224ED84.
    """
    last = None
    for it in items:
        # Trailing nop/bkpt is ".align 2, 0" padding sitting past the return,
        # not code. Counting it made this say "falls through" for 4,281 of the
        # tree's 4,296 candidates, nearly all of them wrong.
        if isinstance(it, Insn) and it.mnem not in ("nop", "bkpt"):
            last = it
    if last is None:
        return False
    if last.cond:
        return True
    m = last.mnem
    if m in ("b", "bx"):
        return False
    if m == "pop" or m.startswith("ldm:"):
        return not re.search(r"\bpc\b", last.ops or "")
    # `ldr pc, [sp], #4` is the one-register spelling of `pop {pc}`, and how
    # mwcc returns from a leaf that saved only lr. Missing it said sys_writec
    # and sys_readc fall through, so merge_multi_entry() gave each a copy of
    # the next entry point that nothing can reach.
    if m == "ldr" and split_operands(last.ops) and reg_num(split_operands(last.ops)[0]) == 15:
        return False
    if m in DP_OPS and writes_pc(last):
        return False
    return True


def _synth_branch(name, thumb):
    """A branch this file does not contain, standing in for a fall-through."""
    if name is None:
        return Insn("bx", None, None, "lr", "bx lr", None, thumb, 0)
    return Insn("b", None, None, name, "b " + name, None, thumb, 0)


def merge_multi_entry(funcs, path):
    """
    Give a function a private copy of the code it branches into.

    `arm_func_start` names an entry point, not a boundary. mwcc emits several
    over one blob and branches between them freely, so translating each
    function on its own turns those branches into calls to a C function nobody
    defines.

    Routing them through armrec_dispatch() would not work: it resolves against
    the registered table, which holds entry points only, and a four-argument
    call drops the callee-saved registers the target reads. Two entry points
    can differ solely in the r4 they set before jumping into a shared body.

    Copying keeps every register live, because it stays one C function with one
    set of locals. The copied region is a contiguous run in file order,
    extended over any member that falls off its own end. Returns the number of
    functions that gained a copy.
    """
    owner = {}
    for i, f in enumerate(funcs):
        for it in f.items:
            if isinstance(it, Label) and it.name != f.name:
                owner.setdefault(it.name, i)

    # Every decision below is about the file as written, so it reads a snapshot
    # rather than the functions being grown underneath it. Otherwise the
    # second member of a blob sees the first member's copy of itself and
    # refuses to merge on a label clash with its own labels.
    items0 = [list(f.items) for f in funcs]
    labels0 = [set(f.labels) for f in funcs]
    falls0 = [func_falls_through(it) for it in items0]

    merged = 0
    for i, f in enumerate(funcs):
        block = set()
        while True:
            new = set(block)
            for k in [i] + sorted(block):
                for it in items0[k]:
                    if isinstance(it, Insn) and it.mnem in BRANCH_MNEMS:
                        j = owner.get(it.ops.strip())
                        if j is not None and j != i:
                            new.add(j)
                # Running off the end into the next entry point is the same
                # thing without a branch to notice. `_u32_div_f` is
                # `cmp r1, #0 / bxeq lr` and then simply is `_u32_div_not_0_f`;
                # emitting it alone made every non-zero unsigned division
                # return its numerator. Fifteen functions here do it.
                if falls0[k] and k + 1 < len(funcs) and k + 1 != i:
                    new.add(k + 1)
            if new:
                lo, hi = min(new | set([i])), max(new | set([i]))
                new |= set(range(lo, hi + 1))
                tail = max(new)
                while tail + 1 < len(funcs) and falls0[tail]:
                    tail += 1
                    new.add(tail)
            new.discard(i)
            if new == block:
                break
            block = new
        if not block:
            continue

        clash = set()
        for k in block:
            clash |= labels0[i] & labels0[k]
        if clash:
            # Two entry points of one blob cannot share a label name, because
            # label names in these files encode their own address. If that ever
            # stops holding, say so rather than emitting a C function with two
            # identically named labels.
            sys.stderr.write("armrec: %s: %s cannot absorb %s (label clash: %s)\n"
                             % (path, f.name,
                                ", ".join(funcs[k].name for k in sorted(block)),
                                ", ".join(sorted(clash))))
            continue

        extra = []
        for k in sorted(block):
            extra.extend(copy.copy(it) for it in items0[k])
            if falls0[k]:
                nxt = funcs[k + 1].name if k + 1 < len(funcs) else None
                extra.append(_synth_branch(nxt, funcs[k].thumb))
        if falls0[i]:
            # Keep the original body out of the copy, and land where the
            # hardware would have.
            nxt = funcs[i + 1].name if i + 1 < len(funcs) else None
            f.items.append(_synth_branch(nxt, f.thumb))
        f.items.extend(extra)
        for k in block:
            f.labels |= labels0[k]
            f.entries.add(funcs[k].name)
        merged += 1
    return merged


def _entry_run(funcs, falls, i):
    """
    The self-contained body of funcs[i], as (items, labels, ok).

    Its own items, extended over any entry point it runs off its end into, with
    an explicit branch standing in for each fall-through. `ok` is False when the
    run still falls through at the end of the file, which is a body nothing can
    safely absorb.
    """
    items, labels = [], set()
    k = i
    while True:
        items.extend(copy.copy(it) for it in funcs[k].items)
        labels |= set(funcs[k].labels)
        if not falls[k]:
            break
        if k + 1 >= len(funcs):
            return items, labels, False
        items.append(_synth_branch(funcs[k + 1].name, funcs[k].thumb))
        k += 1
    # assign_addresses() re-anchors at every label that knows its own address,
    # and a function's opening label often does not. Inside its own file that
    # is harmless because the walk arrives with the right address anyway; in a
    # copy appended to another function it is not, so anchor it from the
    # function record.
    if items and isinstance(items[0], Label) and items[0].addr is None:
        items[0].addr = funcs[i].addr
    return items, labels, True


def foreign_entry_map(paths, parsed, plines):
    """
    Map every mid-function `.global` label to a copy of the body containing it.

    merge_multi_entry() fixes branches into another entry point within a file.
    Four labels here have the same shape across a file boundary: the double and
    float add/subtract runtime routines branch into the middle of each other,
    and nothing defined those names.

    Copying works here too and terminates after one level, because the copied
    body's own cross-file branch lands on a label the absorbing function
    already has. Of the 14 labels with this shape, exactly 4 are branched to
    from another file, so the four are found by evidence rather than listed.

    Emitting the mid-function label as its own C function is the other option
    and is rejected: r4 to r12 and lr are host locals that start at zero in a
    fresh function, so it would be correct only if nothing on any path read a
    callee-saved register before writing it.
    """
    out = {}
    for p in paths:
        funcs, _, globals_, _, _ = parsed[p]
        falls = None
        lits = None
        for i, f in enumerate(funcs):
            mids = [it.name for it in f.items
                    if isinstance(it, Label) and it.name != f.name
                    and it.name in globals_]
            if not mids:
                continue
            if falls is None:
                falls = [func_falls_through(g.items) for g in funcs]
                lits = collect_literals(p, plines.get(p))
            items, labels, ok = _entry_run(funcs, falls, i)
            # Only the pool words the copied run actually carries travel with
            # it; the rest of the file's literals are none of the absorber's
            # business, and merging them wholesale would let an unrelated label
            # of the same name shadow one the absorber already has.
            body_lits = {k: v for k, v in lits.items() if k in labels}
            for name in mids:
                out[name] = {"path": p, "func": f.name, "items": items,
                             "labels": labels, "lits": body_lits, "ok": ok}
    return out


def absorb_foreign_entries(funcs, path, foreign):
    """
    Give a function a private copy of a body in another file it branches into.

    The cross-file half of merge_multi_entry(); see foreign_entry_map() for why
    this is a copy rather than a call. Returns the literal-pool words that came
    with the copies, for the caller to merge into this file's own.
    """
    lits = {}
    if not foreign:
        return lits
    for f in funcs:
        taken = set()
        # Each round absorbs one body and can expose another (the copy may
        # branch out of the file in turn); bounded by how many there are.
        for _round in range(len(foreign) + 1):
            want = None
            for it in f.items:
                if not (isinstance(it, Insn) and it.mnem in BRANCH_MNEMS):
                    continue
                target = (it.ops or "").strip()
                if target in f.labels:
                    continue
                e = foreign.get(target)
                if e is not None and e["func"] not in taken:
                    want = e
                    break
            if want is None:
                break
            taken.add(want["func"])
            why = None
            if not want["ok"]:
                why = "its body runs off the end of %s" % want["path"]
            else:
                clash = f.labels & want["labels"]
                if clash:
                    why = "label clash: %s" % ", ".join(sorted(clash))
            if why:
                # Never emit a half-absorbed body: leaving the branch alone
                # keeps it a call to an undefined symbol, which fails at the
                # link where it is visible, rather than somewhere at runtime.
                sys.stderr.write("armrec: %s: %s cannot absorb %s (%s)\n"
                                 % (path, f.name, want["func"], why))
                continue
            f.items.extend(copy.copy(it) for it in want["items"])
            f.labels |= want["labels"]
            # So a BL to the absorbed entry point stays a call to the real
            # function next door rather than becoming a goto that never
            # returns. Same rule merge_multi_entry() applies in-file.
            f.entries.add(want["func"])
            lits.update(want["lits"])
    return lits


def call_bx_set(items):
    """
    id() of every `bx rN` that ARMv4T uses as an indirect call.

    `bx rN` is a tail call: whatever lr holds is where the callee returns to.
    The one exception is

        mov lr, pc
        bx  r3
        <the instruction the callee returns to>

    where `mov lr, pc` sets lr to the next-but-one instruction, so control
    comes back and the `bx` is a call. ARMv4T has no `blx rN`, so this is how
    it calls through a register at all, and every site is in arm7/asm.

    The pairing is on the immediately preceding item, label included: a label
    between the two means something can branch to the `bx` without having set
    lr. Both instructions must be unconditional for the same reason.
    """
    out = set()
    prev = None
    for it in items:
        if isinstance(it, Directive):
            continue        # a literal pool between the two is still adjacent
        if (isinstance(it, Insn) and isinstance(prev, Insn)
                and it.mnem == "bx" and it.cond in (None, "al")
                and prev.mnem == "mov" and prev.cond in (None, "al")
                and reg_num(it.ops.strip()) not in (None, 14)):
            toks = split_operands(prev.ops)
            if (len(toks) == 2 and reg_num(toks[0]) == 14
                    and reg_num(toks[1]) == 15):
                out.add(id(it))
        prev = it
    return out


def emit_func(func, symtab, literals, stats, rename=None, asm_funcs=None,
              abi_trap=None):
    """Translate one function. Returns (list_of_c_lines, ctx, ok, failures)."""
    ctx = Ctx(func, symtab, literals, func.name, rename, asm_funcs, abi_trap)
    body = []
    failures = []
    items = func.items
    assign_addresses(func)
    ctx.call_bx = call_bx_set(items)

    # A computed branch can land on an instruction carrying no label of its
    # own, since the ARM branch table is a bare run of B instructions. When a
    # function contains one, label every instruction so the dispatch switch in
    # emit_local_dispatch() can name any of them.
    label_all = any(isinstance(it, Insn) and writes_pc(it) for it in items)

    ctx.local_labels = {}
    for it in items:
        if isinstance(it, Label) and it.addr is not None:
            ctx.local_labels[it.addr] = it.name
    if label_all:
        for it in items:
            if isinstance(it, Insn) and it.addr is not None and it.addr not in ctx.local_labels:
                ctx.local_labels[it.addr] = "a%08X" % it.addr

    i = 0
    n = len(items)
    while i < n:
        it = items[i]

        if isinstance(it, Label):
            body.append("L_%s: ;" % sanitize(it.name))
            i += 1
            continue
        if isinstance(it, Directive):
            i += 1        # literal pool / inline data; handled separately
            continue

        ins = it
        if label_all and ins.addr is not None:
            name = ctx.local_labels.get(ins.addr)
            if name and name.startswith("a"):
                body.append("L_%s: ;" % sanitize(name))

        stmts = []
        ctx.cur_ins = ins
        try:
            emit_insn(ctx, ins, func, stmts)
        except Exception as e:  # noqa: BLE001 - a translator bug must not
            #                     abort the whole run; it becomes a reported
            #                     failure for this one instruction instead.
            if not isinstance(e, Unsupported):
                e = Unsupported("translator error: %s: %s" % (type(e).__name__, e))
            failures.append((ins.lineno, ins.raw.strip(), str(e)))
            stats[str(e)] += 1
            body.append('armrec_trap("%s", "%s");  /* %s */' %
                        (func.name, str(e).replace('"', "'"),
                         ins.raw.strip().replace("*/", "*_/")))
            i += 1
            continue

        c = cond_expr(ins.cond)
        if c:
            body.append("if %s {" % c)
            body.extend(stmts)
            body.append("}")
        else:
            body.extend(stmts)
        i += 1

    return body, ctx, not failures, failures


def render_func(func, body, ctx):
    out = []
    kind = "thumb" if func.thumb else "arm"
    addr = "0x%08X" % func.addr if func.addr is not None else "?"
    cname = ctx.rename.get(func.name, func.name)
    out.append("/* %s func %s @ %s */" % (kind, func.name, addr))
    # Weak, so a hand-decompiled C definition of the same function always wins
    # at link time. Decompiling something is then just adding the .c and
    # deleting the .s, with no build bookkeeping and no duplicate symbols
    # while both exist.
    out.append("__attribute__((weak))")
    out.append("uint64_t %s(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {" % cname)
    out.append("    uint32_t r0 = a0, r1 = a1, r2 = a2, r3 = a3;")
    out.append("    uint32_t r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0;")
    # lr is the return address, not zero: a function that saves it and comes
    # back through another register (`push {..., lr}` ... `pop {r3}; bx r3`)
    # translates to an indirect branch to whatever was pushed. See
    # ARMREC_LR_SENTINEL in armrec_rt.h for the whole argument.
    out.append("    uint32_t r10 = 0, r11 = 0, r12 = 0, lr = ARMREC_LR_SENTINEL;")
    out.append("    uint32_t nf = 0, zf = 0, cf = 0, vf = 0;")
    if ctx.tmp:
        decl = ", ".join("t%d = 0" % k for k in range(1, ctx.tmp + 1))
        out.append("    uint32_t %s;" % decl)
    out.append("    ARMREC_UNUSED();")
    for line in body:
        out.append("    " + line)
    out.append("    ARM_RETURN();")
    out.append("}")
    return out


# --------------------------------------------------------------------------
# Data emission
# --------------------------------------------------------------------------

RELOC_RE = re.compile(r"^\s*([A-Za-z_.$?][\w.$?]*)\s*(?:([-+])\s*(.+))?$")


def split_reloc(tok):
    """Split "sym + 0x14" into ("sym", 0x14). Returns (None, 0) if it is not a
    symbol reference we can relocate."""
    m = RELOC_RE.match(tok)
    if not m:
        return None, 0
    addend = 0
    if m.group(2):
        try:
            addend = eval_asm_expr(m.group(3))
        except Unsupported:
            return None, 0
        if m.group(2) == "-":
            addend = -addend
    return m.group(1), addend


def build_data_blobs(data_items, symtab, ext, thumb_funcs=(), problems=None,
                     asm_names=(), decomp_state=(), src_path=None, incdirs=(),
                     unplaced=None):
    """
    Turn parsed data directives into (addr, bytes, relocs) blobs.
    relocs is a list of (offset, symbol_name) for words we cannot resolve now.

    An expression that will not evaluate is counted, not silently zeroed: a
    16-bit datum has no relocation path, so the alternative is to write 0 and
    say nothing. `problems` makes it loud and test_asm_data requires it empty.

    A word naming a Thumb function carries the interworking bit, because that
    is what the ROM has there and what `bx` needs. Only a bare symbol gets it.

    A target no .s defines is answered from `decomp_state` or counted, never
    guessed: a wrong bit is a permanent disagreement with the ROM.

    `.incbin` is data like any other and is read here. A file that will not
    open is counted and truncates the blob, because skipping it would leave
    every later datum short by the missing length.
    """
    blobs = []
    cur_addr = None
    cur = bytearray()
    relocs = []
    if problems is None:
        problems = Counter()

    def resolve(tok, kind):
        """(value, ok). Counts, rather than hides, an expression that fails."""
        try:
            return eval_asm_expr(tok, symtab), True
        except Unsupported:
            problems["unresolved .%s expression %r" % (kind, tok)] += 1
            return 0, False

    def flush():
        if cur_addr is not None and len(cur):
            blobs.append((cur_addr, bytes(cur), list(relocs)))
        del relocs[:]

    for addr, label, kind, payload in data_items:
        if kind == "label":
            if addr is not None:
                flush()
                cur_addr = addr
                cur = bytearray()
            continue
        # `addr` on a data item is parse_file()'s location counter at that
        # directive. It agrees with cur_addr + len(cur) for contiguous data
        # and disagrees exactly where something occupying bytes sat in
        # between, such as a function body or an explicit `.align`. Starting a
        # new blob there is what puts secure.s's data where the console has it.
        if addr is not None and addr != (cur_addr or 0) + len(cur):
            flush()
            cur_addr = addr
            cur = bytearray()
        if cur_addr is None:
            # A section with no address anywhere in it. Nothing from such a
            # file is placed, and its functions come out `@ ?` too, so this is
            # the same fact rather than a new defect, and it is *counted*
            # rather than reported so that the set stays a checked claim.
            n = directive_size(kind, payload)
            if unplaced is not None:
                unplaced[".%s" % kind] += 0 if n is None else n
            continue
        if kind in ("word", "long", "int"):
            for tok in payload.split(","):
                tok = tok.strip()
                if not tok:
                    continue
                try:
                    v = eval_asm_expr(tok, symtab) & 0xFFFFFFFF
                    if tok in thumb_funcs:
                        v |= 1
                    elif (tok not in asm_names and tok not in decomp_state
                            and BARE_NAME.match(tok)
                            and addr_from_name(tok) is not None):
                        problems["unknown ARM-or-Thumb state for decompiled "
                                 "symbol %r" % tok] += 1
                except Unsupported:
                    base, addend = split_reloc(tok)
                    if base is None:
                        problems["unresolved .word expression %r" % tok] += 1
                        v = 0
                    else:
                        base = ext_name(base)
                        relocs.append((len(cur), base, addend))
                        ext.add(base)
                        v = 0
                cur += v.to_bytes(4, "little")
        elif kind in ("short", "hword"):
            for tok in payload.split(","):
                tok = tok.strip()
                if tok:
                    v, _ = resolve(tok, "short")
                    cur += (v & 0xFFFF).to_bytes(2, "little")
        elif kind == "byte":
            for tok in payload.split(","):
                tok = tok.strip()
                if tok:
                    v, _ = resolve(tok, "byte")
                    cur += bytes([v & 0xFF])
        elif kind in ("space", "skip"):
            tok = payload.split(",")[0].strip()
            v, ok = resolve(tok, "space")
            if ok:
                cur += bytes(v)
        elif kind in ("asciz", "ascii"):
            m = re.match(r'^\s*"(.*)"\s*$', payload, re.S)
            if m:
                s = m.group(1).encode("latin-1", "replace").decode("unicode_escape").encode("latin-1")
                cur += s
                if kind == "asciz":
                    cur += b"\0"
        elif kind == "incbin":
            m = re.match(r'^\s*"([^"]+)"', payload)
            if m is None:
                problems["unresolved .incbin operand %r" % payload.strip()] += 1
                flush()
                cur_addr = None
                cur = bytearray()
                continue
            blob = read_incbin(m.group(1), src_path, incdirs)
            if blob is None:
                problems["missing .incbin file %r" % m.group(1)] += 1
                flush()
                cur_addr = None
                cur = bytearray()
                continue
            cur += blob
    flush()
    return blobs


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

C_HEADER = """/* Generated by tools/armrec/armrec.py. Do not edit.
 * Source: %s
 */
%s#include "armrec_rt.h"

"""

# The maths coprocessor's register block. A file naming an address in it gets
# ARMREC_CP_HOOK, which makes ARM_LD32 and friends consult the model in
# armrec_rt.c instead of dereferencing. Every other file keeps the plain
# dereference that identity mapping is for, because the hook costs 3.2x the
# text.
#
# A whole 32-bit literal is the only way ARM can name one of these, so this
# catches every file that can reach the window on its own.
CP_ADDR_RE = re.compile(r"0x0*40002[89ab][0-9a-f]\b", re.I)


def file_touches_cp(path):
    try:
        with open(path, "r", errors="replace") as fh:
            return CP_ADDR_RE.search(fh.read()) is not None
    except OSError:
        return False


# VRAMCNT, 0x04000240 to 0x04000249. A file naming one gets ARMREC_VRAM_HOOK,
# which makes ARM_ST8 and friends tell armrec_vram_touch(). The store rather
# than the load, because a write is what moves the banks.
#
# 0x04000240 can be reached as the immediate 0x04000000 plus a #0x240 offset,
# so naming it is not the only way in and this regex is not sound on its own.
# test_vram walks every .s for both shapes and fails if the set is not
# exactly what this finds. Today that is one file, arm9/asm/unk_0208AC14.s.
VRAM_ADDR_RE = re.compile(r"0x0*400024[0-9a-f]\b", re.I)


def file_touches_vramcnt(path):
    try:
        with open(path, "r", errors="replace") as fh:
            return VRAM_ADDR_RE.search(fh.read()) is not None
    except OSError:
        return False


# The geometry engine. Two windows: the command ports at 0x04000400 to
# 0x040005CB, where a store is a FIFO push rather than a store, and the status
# and result block at 0x04000600 to 0x040006A3, which is computed rather than
# stored. A file naming an address in either gets ARMREC_GX_HOOK.
#
# 0x04000440 is reachable as the immediate 0x04000000 plus a #0x440 offset, so
# naming it is not the only way in. test_gpu3d walks every .s for both shapes
# and fails if the set is not what this regex finds.
GX_ADDR_RE = re.compile(
    r"0x0*4000(4[0-9a-f]{2}|5[0-9a-b][0-9a-f]|6[0-9a][0-9a-f])\b", re.I)


def file_touches_gx(path):
    try:
        with open(path, "r", errors="replace") as fh:
            return GX_ADDR_RE.search(fh.read()) is not None
    except OSError:
        return False


# The IPC block. Four registers in two places: IPCSYNC (0x04000180),
# IPCFIFOCNT (0x04000184) and IPCFIFOSEND (0x04000188) in the I/O page, and
# IPCFIFORECV a megabyte away at 0x04100000. A file naming one gets
# ARMREC_IPC_HOOK, which routes loads and stores through pc/src/pc_ipc.c. None
# of the four is storage, so identity-mapped memory answers all of them wrongly.

# 0x04100000 is itself an ARM immediate and 0x04000184 is reachable off a base,
# so test_ipc walks every .s for both shapes and fails if the set is not the
# one this regex finds. Today one file, arm7/asm/PXI_fifo.s.
IPC_ADDR_RE = re.compile(r"0x0*4000018[0-9a-b]\b|0x0*4100000\b|\b68157440\b", re.I)


def file_touches_ipc(path):
    try:
        with open(path, "r", errors="replace") as fh:
            return IPC_ADDR_RE.search(fh.read()) is not None
    except OSError:
        return False


# The ARM7's SPI bus. Two registers, SPICNT (0x040001C0) and SPIDATA
# (0x040001C2), and neither is storage: the first releases a chip select as a
# side effect of clearing its enable bit, and the second is a shift register
# whose read gives the selected chip's reply to the previous write. A file
# naming one gets ARMREC_SPI_HOOK and goes through pc/src/pc_spi.c.

# 0x040001C0 is reachable off a base as well as by being named, so test_spi
# walks every .s for both shapes. Today four files, all arm7/asm/, because the
# ARM9 has no SPI bus.
SPI_ADDR_RE = re.compile(r"0x0*40001[cC][0-3]\b|\b6710931[2-5]\b")


def file_touches_spi(path):
    try:
        with open(path, "r", errors="replace") as fh:
            return SPI_ADDR_RE.search(fh.read()) is not None
    except OSError:
        return False


# The GBA slot's backup bus, 0x0A000000 to 0x0A00FFFF: on hardware the
# cartridge's 8-bit SRAM/flash bus, and with a flash chip there a store is a
# command (AA@5555, 55@2AAA, then 90 ID mode, 80/10 or 80/30 erase, A0
# program, B0 bank) and a load in ID mode is the chip's ID, not a cell. A file
# naming the window gets ARMREC_AGB_HOOK, which routes ldrb/strb through
# pc/src/pc_agb_slot.c's chip model (the bus is 8 bits wide and the SDK only
# reaches it with byte accesses).
#
# Named either as a literal (.word 0x0A005555) or as the immediate #0xa000000
# that sector addresses are built on. Today four files, all Diamond/Pearl's
# recompiled SDK: arm9/asm/CTRDG_flash_{common,MX29L010,MX29L512,LE39FW512}.s.
# Platinum's CTRDG is C and goes through the same model by its pc/patches.
AGB_ADDR_RE = re.compile(r"0x0*a00[0-9a-f]{4}\b|#167772160\b", re.I)


def file_touches_agb(path):
    try:
        with open(path, "r", errors="replace") as fh:
            return AGB_ADDR_RE.search(fh.read()) is not None
    except OSError:
        return False


# The game card's ROM bus: ROMCTRL (0x040001A4), which starts a command and
# reports busy/data-ready, the command bytes (0x040001A8) and the data port
# (0x04100010), which is a pop. A file naming one gets ARMREC_CARD_HOOK and
# goes through pc/src/pc_card_rom.c's card model. Diamond/Pearl/Platinum's
# hosts replace the card layer that names them; TWL-SDK's (Black/White)
# reads the ROM ID and small blocks through them with the CPU, named as
# literals (.word 0x040001A4).
CARD_ADDR_RE = re.compile(r"0x0*40001[ab][0-9a-f]\b|0x0*4100010\b", re.I)


def file_touches_card(path):
    try:
        with open(path, "r", errors="replace") as fh:
            return CARD_ADDR_RE.search(fh.read()) is not None
    except OSError:
        return False


# The timers' counters, TM0CNT_L .. TM3CNT_L (0x04000100 to 0x0400010F),
# named as literals (.word 0x04000100: the SDK's OS_GetTick, OS timer code).
# A file naming one gets ARMREC_TIMER_HOOK: its loads there go through
# pc/src/pc_timers.c, which lets time pass for a loop polling a counter.
TIMER_ADDR_RE = re.compile(r"0x0*400010[0-9a-f]\b", re.I)


def file_touches_timer(path):
    try:
        with open(path, "r", errors="replace") as fh:
            return TIMER_ADDR_RE.search(fh.read()) is not None
    except OSError:
        return False


def collect_symbols(paths, defines, incdirs, stems, local_rename=True):
    """
    Pass 1: every function and data symbol, with its guest address.

    Returns (symtab, parsed, local_addr, rename, data_syms, plines).

    `data_syms` is the subset of `symtab` that is a `.global` data label:
    something the assembly places and names but armrec emits no C definition
    for. Decompiled C declares plenty of these extern and nothing defines them,
    so main() turns each into a weak absolute symbol at its guest address.

    `local_addr` and `rename` exist because `local_arm_func_start` marks a
    function mwcc compiled as static, and two files may use the same name for
    one. A flat symbol table cannot hold both, so a local is resolved per file:
    it keeps its own address inside its own file and is emitted under a
    file-qualified C name. That applies whether or not another .s shares the
    name, because .s files are not the whole program and a decompiled .c
    defining the name strongly would beat armrec's weak definition.

    The condition is the marker and not `not is_global`: split_call_targets()
    promotes a plain label to a function and that label claims nothing about
    linkage.

    `local_addr` carries contested file-local data labels too, dropping them
    from the flat table when every definition is file-local, because a
    reference from a third file cannot mean either of them.
    """
    parsed = {}
    plines = {}
    for p in paths:
        # Preprocess once and keep the result: parse_file and collect_literals
        # must see the same text. collect_literals reading the file raw is what
        # put MAP_ROUTE_201, a #define rather than a symbol, into the extern
        # set.
        plines[p] = preprocess(p, defines, list(incdirs))
        parsed[p] = parse_file(p, defines, incdirs, lines=plines[p])

    owners = {}
    for p in paths:
        funcs, data, _, _, _ = parsed[p]
        for f in funcs:
            owners.setdefault(f.name, set()).add(p)
        for addr, label, kind, _ in data:
            if kind == "label" and label and addr is not None:
                owners.setdefault(label, set()).add(p)
    contested = set(n for n, ps in owners.items() if len(ps) > 1)

    # Every name any file emits as a C function, so the data pass below cannot
    # claim one. A weak absolute at a function's guest address would compete
    # with that function's own weak C definition, and whichever the linker
    # picked, half the calls would be a jump into guest memory.
    func_names = set()
    for p in paths:
        for f in parsed[p][0]:
            func_names.add(f.name)

    # One data label declared `.global` in two files is a duplicate symbol the
    # real linker rejects, and the one shape the per-file rule below cannot
    # arbitrate. There are none here, so say so rather than picking one.
    global_data_owners = {}
    for p in paths:
        _, data, globals_, _, _ = parsed[p]
        for addr, label, kind, _ in data:
            if (kind == "label" and label and addr is not None
                    and label in globals_ and label not in func_names):
                global_data_owners.setdefault(label, set()).add(p)
    for label in sorted(n for n, ps in global_data_owners.items() if len(ps) > 1):
        sys.stderr.write("armrec: data label %s is .global in %d files (%s); "
                         "one of them wins the symbol table\n"
                         % (label, len(global_data_owners[label]),
                            ", ".join(sorted(global_data_owners[label]))))

    symtab = {}
    data_syms = {}
    local_addr = {}
    rename = {}
    for p in paths:
        funcs, data, globals_, _, _ = parsed[p]
        for f in funcs:
            if ((f.file_local and local_rename)
                    or (not f.is_global and f.name in contested)):
                if f.addr is not None:
                    local_addr.setdefault(p, {})[f.name] = f.addr
                rename.setdefault(p, {})[f.name] = "%s__%s" % (stems[p],
                                                               sanitize(f.name))
                continue
            if f.addr is not None:
                symtab[f.name] = f.addr
            elif f.name not in symtab:
                symtab[f.name] = None
        for addr, label, kind, _ in data:
            # A function's own label repeats its name; a local one must not
            # leak back into the global table through this path.
            if kind != "label" or not label or addr is None:
                continue
            if label in rename.get(p, {}):
                continue
            if label in globals_ and label not in func_names:
                data_syms.setdefault(label, addr)
            if label in contested and label not in func_names:
                # A data label more than one file defines is resolved the way
                # a contested file-local function is: `static` means the same
                # thing for an object as for a function, and the assembler
                # resolves a reference to whichever definition its own file
                # carries. The flat table could not express that, so the first
                # file processed won and every other file's literal pool
                # pointed at that file's object.
                if label in globals_:
                    # One tree-wide definition, so it owns the flat table
                    # outright and the file-local namesakes below shadow it
                    # inside their own files only. test_local_data re-derives
                    # the set of `.global` data labels defined twice and
                    # requires it empty, so this cannot become a silent
                    # last-wins.
                    symtab[label] = addr
                else:
                    local_addr.setdefault(p, {})[label] = addr
                continue
            symtab.setdefault(label, addr)
    # drop symbols we never resolved to an address
    symtab = {k: v for k, v in symtab.items() if v is not None}

    # The guest's C library must not be the host's. See HOST_LIBC_NAMES: a
    # weak definition in an object file beats libc's, so MSL_C's recompiled
    # strcmp became the whole program's strcmp. The rename is global rather
    # than per file, because every recompiled reference has to move with the
    # definition.
    for name in host_libc_collisions(set(func_names) | set(data_syms)):
        for p in paths:
            rename.setdefault(p, {}).setdefault(name, GUEST_PREFIX + name)

    return symtab, parsed, local_addr, rename, data_syms, plines


def output_stems(paths):
    """
    Map each input path to a unique output name.

    armrec writes one .c per input into one flat directory, so two inputs
    sharing a basename used to overwrite each other silently. Four basenames
    exist in both the arm9 and arm7 trees, and because the file list arrives
    sorted, arm9 always won and the ARM7 translations were discarded.

    A colliding name is disambiguated with as many leading path components as
    it takes to separate the group, so files with a unique basename keep the
    name they have.
    """
    groups = {}
    for p in paths:
        groups.setdefault(os.path.basename(p)[:-2], []).append(p)
    stems = {}
    for base, group in sorted(groups.items()):
        if len(group) == 1:
            stems[group[0]] = sanitize(base)
            continue
        parts = {p: os.path.normpath(p)[:-2].split(os.sep) for p in group}
        depth = max(len(v) for v in parts.values())
        n = 2
        while n < depth:
            cand = {p: "_".join(parts[p][-n:]) for p in group}
            if len(set(cand.values())) == len(group):
                break
            n += 1
        for p in group:
            stems[p] = sanitize("_".join(parts[p][-n:]))
    if len(set(stems.values())) != len(stems):
        raise SystemExit("armrec: cannot make output names unique for %s"
                         % ", ".join(sorted(paths)))
    return stems


# A .s under arm9/overlays/NN/asm/, or an asm-in-C body extracted from
# arm9/overlays/NN/src/ into extracted/overlays/NN/src/ (the D wasm build).
OVERLAY_PATH = re.compile(
    r"(?:^|/)(?:arm9/overlays/(\d+)/asm|extracted/overlays/(\d+)/src)/")


def overlay_of(path):
    """
    Which overlay a source file belongs to, or None for always-resident code.

    The directory is the key, and that is checked rather than assumed:
    test_overlays takes every guest address a file under arm9/overlays/NN/asm/
    places and requires each to fall inside overlay NN's own window as the
    ROM's overlay table describes it.
    """
    m = OVERLAY_PATH.search(os.path.normpath(path).replace(os.sep, "/"))
    return int(m.group(1) or m.group(2)) if m else None


def process(path, stem, funcs, data, symtab, outdir, stats, report, emit=True,
            local_addr=None, rename=None, lines=None, foreign=None,
            asm_funcs=None, abi_trap=None, thumb_funcs=None, overlay=None,
            asm_names=None, decomp_state=None, incdirs=(), host_regs=()):
    """
    Translate one file. Returns (functions, clean functions, extern names,
    boundary call counts, extern names the emitted C actually references).

    `host_regs` is [(addr, name)] for this file's functions that --host-override
    dropped: their guest address is registered to the host's definition (its
    c2u$ adapter under --wasm) so a dispatch through a stored pointer still
    lands somewhere.
    """
    literals_raw = collect_literals(path, lines)
    merge_multi_entry(funcs, path)
    for k, v in absorb_foreign_entries(funcs, path, foreign).items():
        if literals_raw.get(k, v) != v:
            sys.stderr.write("armrec: %s: literal %s is %r here and %r in the "
                             "absorbed body\n" % (path, k, literals_raw[k], v))
            continue
        literals_raw[k] = v
    ext = set()
    # This file's own view of the symbol table: its contested file-local
    # functions shadow anything of the same name elsewhere, which is what
    # `static` means and what the flat table could not express.
    rename = rename or {}
    if local_addr:
        symtab = dict(symtab)
        symtab.update(local_addr)
    # Pre-resolve literal labels to C expressions
    tmpctx = Ctx(None, symtab, {}, path, rename)
    literals = {}
    for k, v in literals_raw.items():
        try:
            literals[k] = literal_c_value(tmpctx, v)
        except Unsupported:
            continue
    ext |= tmpctx.used_ext

    bodies = []
    ok_funcs = 0
    total_funcs = len(funcs)
    ext_calls = Counter()
    for f in funcs:
        body, ctx, ok, failures = emit_func(f, symtab, literals, stats, rename,
                                            asm_funcs, abi_trap)
        ext |= ctx.used_ext
        ext_calls += ctx.ext_calls
        if ok:
            ok_funcs += 1
        else:
            for lineno, raw, why in failures:
                report.append((path, f.name, lineno, raw, why))
        bodies.append((f, body, ctx))

    # Built before the --scan early-out on purpose: a defect in the *data* is
    # invisible to the function counter, so `make coverage` has to see it too.
    data_problems = Counter()
    unplaced = Counter()
    blobs = build_data_blobs(data, symtab, ext, thumb_funcs or (), data_problems,
                             asm_names or (), decomp_state or (),
                             src_path=path, incdirs=incdirs, unplaced=unplaced)
    for k, v in data_problems.items():
        stats["data: " + k.split(" expression ")[0]] += v
        report.append((path, "<data>", 0, k, "data expression does not resolve"))
    # Not a defect, and deliberately not spelled like one. See the note in
    # build_data_blobs(). It still goes in the report, because "armrec places
    # nothing from this file" is worth reading off the build, and
    # test_data_placement holds the set to one entry.
    for k, v in sorted(unplaced.items()):
        stats["data: unplaced (no address in the section)"] += v
        report.append((path, "<data>", 0, "%s, %d bytes" % (k, v),
                       "data in a section with no address is not placed"))

    if not emit:
        return total_funcs, ok_funcs, ext, ext_calls, set()

    rel = stem
    outpath = os.path.join(outdir, rel + ".c")
    called = set()
    ext_refs = set()
    by_sanitized = dict((sanitize(n), n) for n in ext)
    for f, body, ctx in bodies:
        for line in body:
            for m in re.finditer(r"ARM(?:REC_CALL|_TAILCALL)(?:_EXT)?\(([\w$]+)", line):
                called.add(m.group(1))
            for m in re.finditer(r"\barmrec_ext_(\w+)", line):
                ext_refs.add(by_sanitized.get(m.group(1), m.group(1)))
    for _addr, _payload, relocs in blobs:
        for _off, sym, _addend in relocs:
            ext_refs.add(sym)
    host_cname = lambda n: (C2U_PREFIX + n) if TARGET_WASM else n
    for _addr, name in host_regs:
        called.add(host_cname(name))
    defined = set(rename.get(f.name, f.name) for f in funcs)

    with open(outpath, "w") as out:
        out.write(C_HEADER % (path,
                              ("#define ARMREC_CP_HOOK 1\n"
                               if file_touches_cp(path) else "") +
                              ("#define ARMREC_VRAM_HOOK 1\n"
                               if file_touches_vramcnt(path) else "") +
                              ("#define ARMREC_GX_HOOK 1\n"
                               if file_touches_gx(path) else "") +
                              ("#define ARMREC_IPC_HOOK 1\n"
                               if file_touches_ipc(path) else "") +
                              ("#define ARMREC_SPI_HOOK 1\n"
                               if file_touches_spi(path) else "") +
                              ("#define ARMREC_AGB_HOOK 1\n"
                               if file_touches_agb(path) else "") +
                              ("#define ARMREC_CARD_HOOK 1\n"
                               if file_touches_card(path) else "") +
                              ("#define ARMREC_TIMER_HOOK 1\n"
                               if file_touches_timer(path) else "")))
        for name in sorted(called - defined):
            out.write("extern uint64_t %s(uint32_t, uint32_t, uint32_t, uint32_t);\n" % name)
        for name in sorted(ext):
            out.write("extern uint32_t armrec_ext_%s;\n" % sanitize(name))
        out.write("\n")
        for name in sorted(defined):
            out.write("uint64_t %s(uint32_t, uint32_t, uint32_t, uint32_t);\n" % name)
        out.write("\n")

        for i, (addr, payload, relocs) in enumerate(blobs):
            out.write("static const uint8_t blob%d[%d] = {" % (i, len(payload)))
            out.write(",".join(str(b) for b in payload))
            out.write("};\n")
        out.write("\n")

        for f, body, ctx in bodies:
            for line in render_func(f, body, ctx):
                out.write(line + "\n")
            out.write("\n")

        # Always-resident code does both jobs at startup. An overlay does not:
        # Its slots are reserved at startup so a dispatch to a non-resident
        # address can name who owns it, and its data is written into the
        # window only when the overlay loads, because 33 overlays share one
        # window. armrec_load_overlay(id) calls the data half.
        def emit_data(indent="    "):
            for i, (addr, payload, relocs) in enumerate(blobs):
                out.write("%sarmrec_load_data(0x%08Xu, blob%d, %d);\n"
                          % (indent, addr, i, len(payload)))
                for off, sym, addend in relocs:
                    out.write("%sARM_ST32(0x%08Xu, armrec_ext_%s + %d);\n"
                              % (indent, addr + off, sanitize(sym), addend))

        def emit_registrations(indent="    "):
            for f in funcs:
                if f.addr is not None:
                    # The C identifier may be file-qualified; the name
                    # registered for --trace is always the one the assembly
                    # uses.
                    if overlay is None:
                        out.write('%sarmrec_register(0x%08Xu, %s, "%s");\n'
                                  % (indent, f.addr, rename.get(f.name, f.name),
                                     f.name))
                    else:
                        out.write('%sarmrec_register_overlay(0x%08Xu, %s, "%s",'
                                  ' %d);\n'
                                  % (indent, f.addr, rename.get(f.name, f.name),
                                     f.name, overlay))
            for addr, name in host_regs:
                if overlay is None:
                    out.write('%sarmrec_register(0x%08Xu, %s, "%s");\n'
                              % (indent, addr, host_cname(name), name))
                else:
                    out.write('%sarmrec_register_overlay(0x%08Xu, %s, "%s",'
                              ' %d);\n'
                              % (indent, addr, host_cname(name), name, overlay))

        out.write("void armrec_init_%s(void);\n" % sanitize(rel))
        out.write("void armrec_init_%s(void) {\n" % sanitize(rel))
        if overlay is None:
            emit_data()
        emit_registrations()
        out.write("}\n")
        if overlay is not None:
            out.write("\nvoid armrec_data_%s(void);\n" % sanitize(rel))
            out.write("void armrec_data_%s(void) {\n" % sanitize(rel))
            emit_data()
            out.write("}\n")

    return total_funcs, ok_funcs, ext, ext_calls, ext_refs


# A name GAS will accept verbatim in `.set`. Assembly here spells some labels
# with characters C has no use for, such as transFunc$7934, and those are
# unreachable from decompiled C anyway.
ASM_SYM_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def emit_data_syms(outpath, data_syms):
    """
    Define every `.global` data label at its guest address.

    armrec emits a C function for each translated function, so the linker wires
    decompiled C to recompiled code for free. Data had no equivalent: a .s file
    places a table at an address and names it, decompiled C declares it extern,
    and nothing defines the symbol. The bytes are in guest memory but no symbol
    pointed at them.

    Identity mapping is what makes the one-liner work: the guest address is the
    host address, so an absolute symbol is the correct definition rather than
    an approximation.

    Weak, for the same reason recompiled functions are weak, so a hand-written
    C definition of the same object wins without anyone editing a list.

    Within the translation unit carrying the `.set`, GAS folds the address at
    assembly time and emits no relocation, so that unit keeps the guest address
    even when the rest of the program has taken a strong C definition. This
    file therefore contains nothing but these lines, and cannot observe its own
    symbols.
    """
    with open(outpath, "w") as f:
        f.write(C_HEADER % ("every .global data label armrec placed", ""))
        f.write("/*\n"
                " * Weak absolute definitions, one per .global data label.\n"
                " * The bytes are already in guest memory (armrec_load_data);\n"
                " * this is the symbol that points at them. Weak so a\n"
                " * hand-decompiled C definition wins. Nothing else may live\n"
                " * in this file. GAS folds these addresses at assembly\n"
                " * time, so a TU that both defines and uses one would keep\n"
                " * the guest address after C had overridden it.\n"
                " * See emit_data_syms() in tools/armrec/armrec.py.\n"
                " */\n\n")
        for name in sorted(data_syms):
            if not ASM_SYM_RE.match(name):
                continue
            # A data label the host's C library also names gets the same
            # prefix its functions do. `errno` is one: MSL's is a plain word in
            # guest RAM while the host's is thread-local behind
            # __errno_location(), so defining the bare name would hand any host
            # translation unit a guest address.
            cname = (GUEST_PREFIX + name) if name in HOST_LIBC_NAMES else name
            f.write('__asm__(".weak %s\\n\\t.set %s, 0x%08X\\n");\n'
                    % (cname, cname, data_syms[name]))


XMAP_SYM_RE = re.compile(
    r"^\s+([0-9A-Fa-f]{8}) [0-9A-Fa-f]{8} (\.\w+)\s+(\S+)\t\(([^)]+)\)\s*$")
XMAP_LINK_RE = re.compile(
    r"^#>([0-9A-Fa-f]{8})\s+([A-Za-z_]\w*) \(linker command file\)\s*$")


def load_xmap(path):
    """
    ({(name, object): addr}, {name: set(addr)}, {name: value},
    {object: {section: origin}}) from an mwld xMAP.

    The object is part of the key because a static name can be defined by
    more than one object; mapping symbols ($a, $t, $d) are dropped. The third
    map is the symbols the linker command file defines
    (SDK_AUTOLOAD_DTCM_START, SDK_IRQ_STACKSIZE, SDK_OVERLAY_OVERLAY_04_ID):
    no source defines them, and their value is the map's to give. The fourth
    is where each object's sections start; see parse_file().
    """
    by_obj = {}
    by_name = {}
    link = {}
    origins = {}
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = XMAP_LINK_RE.match(line)
            if m:
                link[m.group(2)] = int(m.group(1), 16)
                continue
            m = XMAP_SYM_RE.match(line)
            if not m:
                continue
            sec, name, obj = m.group(2), m.group(3), m.group(4)
            addr = int(m.group(1), 16)
            if name == sec:
                # A section's own line: where this object's piece of it is.
                origins.setdefault(obj, {}).setdefault(sec, set()).add(addr)
                continue
            if name.startswith("$") or name.startswith("."):
                continue
            by_obj[(name, obj)] = addr
            by_name.setdefault(name, set()).add(addr)
    # Only an object with one piece of a section has an origin to give.
    for obj in origins:
        origins[obj] = dict((s, next(iter(a))) for s, a in origins[obj].items()
                            if len(a) == 1)
    return by_obj, by_name, link, origins


def entry_label_mismatches(paths, parsed):
    """
    Functions whose own label sits somewhere other than the function.

    The two must agree: the function's address is what dispatch registers and
    the xMAP checks, the label's anchors every instruction address in the
    body, so every PC-relative value (`add r0, pc` before a jump table, `adr`)
    and every computed-branch case. When they part, the body reads its tables
    from the wrong place and branches nowhere, as Pearl's ov06_0224A0F0 did
    with its labels shifted twice. A build that would emit such a body stops.
    """
    out = []
    for p in paths:
        for f in parsed[p][0]:
            for it in f.items:
                if isinstance(it, Label) and it.name == f.name:
                    if (it.addr is not None and f.addr is not None
                            and it.addr != f.addr):
                        out.append("%s: %s: entry label at 0x%08X, function "
                                   "at 0x%08X" % (p, f.name, it.addr, f.addr))
                    break
    return out


def place_from_xmap(paths, parsed, xmap, symtab, local_addr, rename, stats):
    """
    Give an address to every function the source leaves unplaced, from the
    ROM's own link map, and count those whose placed address it contradicts.

    A handful of SDK files (the MSL buffer I/O, the RVCT float helpers, some
    of SOC) open a function with no `; 0x...` comment and no label naming an
    address, so armrec could not register them for dispatch and nothing
    outside the file could find them. The xMAP is where the ROM put them.
    The object is the source's basename, which is how the ROM build names it.

    The linker-command-file symbols go into the symbol table too, so a
    `.word SDK_AUTOLOAD_DTCM_START` is the value the ROM link gave it.
    """
    by_obj, by_name, link, _origins = xmap
    for name, value in link.items():
        if name not in symtab:
            symtab[name] = value
            stats["xmap: linker symbol"] += 1
    for p in paths:
        obj = os.path.basename(p)[:-2] + ".o"
        prename = rename.get(p, {})
        for f in parsed[p][0]:
            want = by_obj.get((f.name, obj))
            # By name alone only for a function no other file may also name:
            # a file-local one absent under its own object is not the
            # namesake some other object exports.
            if (want is None and not f.file_local
                    and len(by_name.get(f.name, ())) == 1):
                want = next(iter(by_name[f.name]))
            if want is None:
                stats["xmap: function not in the map: " + f.name] += 1
                continue
            if f.addr == want:
                continue
            if f.addr is None:
                stats["xmap: function placed from the map"] += 1
                for it in f.items:
                    if isinstance(it, Label) and it.name == f.name:
                        it.addr = want
            else:
                # The link map is the ROM. The disagreements are extracted
                # asm-in-C bodies, whose file has gaps where the C functions
                # were, so the location counter runs short across them. The
                # whole body ran short by the same amount, so every label in
                # it moves with the function: left at the counter, the walk
                # below re-anchors at the first local label, and a PC-relative
                # `add r0, pc` reads its jump table from somewhere else
                # (ov59_MunchlaxJumpAnimation, 708 bytes short).
                stats["xmap: %s moved from 0x%08X to the map's 0x%08X"
                      % (f.name, f.addr, want)] += 1
                delta = want - f.addr
                floc = local_addr.get(p, {})
                for it in f.items:
                    if not isinstance(it, Label) or it.addr is None:
                        continue
                    old, it.addr = it.addr, it.addr + delta
                    if it.name == f.name:
                        continue
                    if floc.get(it.name) == old:
                        floc[it.name] = it.addr
                    elif symtab.get(it.name) == old:
                        symtab[it.name] = it.addr
            f.addr = want
            assign_addresses(f)
            # The same split collect_symbols() makes: a file-qualified
            # function resolves inside its own file only.
            if (f.name in local_addr.get(p, {})
                    or (f.file_local and f.name in prename)):
                local_addr.setdefault(p, {})[f.name] = want
            else:
                symtab[f.name] = want
        place_data_from_xmap(p, parsed[p][1], obj, by_obj, by_name, symtab,
                             local_addr, stats)


def place_data_from_xmap(p, data, obj, by_obj, by_name, symtab, local_addr,
                         stats):
    """
    Re-anchor a file's data at every label the link map places.

    The `; 0x...` comments and address-named labels are Diamond's, and
    Pearl's `.ifdef`s change sizes, so where armrec's location counter lost
    track (an unsizable directive) Pearl's data was placed at Diamond's
    address: NNS_G3dAnmObjInitFuncArray 8 bytes short, hundreds of labels in
    all. A label the map knows moves to the map's address, and the items
    after it move with it until the next label the map knows.
    """
    delta = 0
    for i, (addr, label, kind, payload) in enumerate(data):
        if kind == "label" and label:
            # Object-qualified only: a file-local label is absent from the map,
            # and a namesake another object exports is somewhere else
            # (calcTexMtx_ is local in three NNS_G3D files, global in one).
            want = by_obj.get((label, obj))
            if want is not None and addr is not None:
                delta = want - addr
        if addr is None or delta == 0:
            continue
        data[i] = (addr + delta, label, kind, payload)
        if kind == "label" and label:
            stats["xmap: data label moved"] += 1
            if label in local_addr.get(p, {}):
                local_addr[p][label] = addr + delta
            elif symtab.get(label) == addr:
                symtab[label] = addr + delta


def main():
    ap = argparse.ArgumentParser(description="ARM/Thumb -> C static recompiler")
    ap.add_argument("files", nargs="+")
    ap.add_argument("--out", help="output directory for generated C")
    ap.add_argument("--scan", action="store_true", help="report coverage only")
    ap.add_argument("--define", action="append", default=["DIAMOND", "ENGLISH"])
    ap.add_argument("--undef", action="append", default=[],
                    help="drop a define, default or given: Pearl is "
                         "--undef DIAMOND --define PEARL, since config.h and "
                         "`.ifdef DIAMOND` take Diamond whenever DIAMOND is set")
    ap.add_argument("--include", action="append", default=["include"],
                    help="preprocessor include directory")
    ap.add_argument("--report", help="write a detailed failure report here")
    ap.add_argument("--boundary",
                    help="write the recompiled -> host call boundary here: one "
                         "line per call target no .s in this run defines, with "
                         "the number of sites that reach it")
    ap.add_argument("--no-local-rename", action="store_true",
                    help="emit a `local_arm_func_start` function under its "
                         "bare name unless another .s file uses the name too. "
                         "This reintroduces a fixed name-collision bug and "
                         "is not a debugging switch")
    ap.add_argument("--abi-trap", action="append", default=[],
                    help="NAME:reason, refuse to call NAME from recompiled "
                         "code, because the word sequence ARMREC_CALL_EXT "
                         "builds cannot express its ABI. Repeatable.")
    ap.add_argument("--decomp-state", default=DECOMP_STATE_FILE,
                    help="table of ARM/Thumb states for decompiled functions "
                         "that assembly still names in a `.word`; see "
                         "tools/armrec/gen_decomp_thumb.py. Pass an empty file "
                         "to translate without it.")
    ap.add_argument("--wasm", action="store_true",
                    help="emit for wasm32: calls that leave recompiled code go "
                         "to the bridge's c2u$NAME adapter (see C2U_PREFIX); "
                         "armrec_externs.c and armrec_data_syms.c are not "
                         "written, the bridge owns both jobs")
    ap.add_argument("--guest-libc", action="append", default=[],
                    help="NAME[,NAME...]: an MSL function compiled from C as "
                         "guest_NAME; recompiled calls to NAME go there")
    ap.add_argument("--host-override",
                    help="file of function names (one per line, # comments) "
                         "the host layer defines: not emitted, every call "
                         "to one crosses the C boundary, and its guest "
                         "address is registered to the host definition")
    ap.add_argument("--classes",
                    help="write the name classification here: `F name 0xaddr` "
                         "(bit 0 = Thumb), `D name 0xaddr`, `B name` (a call "
                         "target defined in C), `X name` (an address taken of "
                         "something no .s defines), sorted")
    ap.add_argument("--xmap",
                    help="the ROM link's arm9.elf.xMAP: gives an address to a "
                         "function whose source carries none, and counts "
                         "every function whose address disagrees with it")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    global TARGET_WASM, GUEST_LIBC_EXT, WASM_ASM_NAMES, XMAP_ORIGINS, FLAG_RESULT_FUNCS
    TARGET_WASM = args.wasm
    GUEST_LIBC_EXT = frozenset(n for spec in args.guest_libc
                               for n in spec.split(",") if n)
    overrides = set()
    if args.host_override:
        with open(args.host_override) as fh:
            for line in fh:
                line = line.split("#", 1)[0].strip()
                if line:
                    overrides.add(line)
    xmap = load_xmap(args.xmap) if args.xmap else None
    if xmap is not None:
        XMAP_ORIGINS = xmap[3]

    abi_trap = {}
    for spec in args.abi_trap:
        name, _, why = spec.partition(":")
        if not name or not why:
            ap.error("--abi-trap wants NAME:reason, got %r" % spec)
        abi_trap[name] = why

    if not args.scan and not args.out:
        ap.error("--out is required unless --scan is given")
    if args.out:
        os.makedirs(args.out, exist_ok=True)

    defines = set(args.define) - set(args.undef)
    stems = output_stems(args.files)
    symtab, parsed, local_addr, rename, data_syms, plines = collect_symbols(
        args.files, defines, args.include, stems,
        local_rename=not args.no_local_rename)
    for name in GUEST_LIBC_EXT:
        for p in args.files:
            rename.setdefault(p, {}).setdefault(name, GUEST_PREFIX + name)
    xmap_stats = Counter()
    if xmap is not None:
        place_from_xmap(args.files, parsed, xmap, symtab, local_addr,
                        rename, xmap_stats)
        for name in data_syms:
            if name in symtab:
                data_syms[name] = symtab[name]
    bad = entry_label_mismatches(args.files, parsed)
    if bad:
        for line in bad:
            print("armrec: " + line, file=sys.stderr)
        sys.exit("armrec: %d function(s) disagree with their own entry label"
                 % len(bad))

    # --host-override: the host layer's definition replaces the recompiled
    # one. The function stays in the symbol table, so a `.word` naming it
    # keeps the ROM's guest address, and that address is registered to the
    # host body so a dispatch through it arrives.
    host_regs = {}
    for p in args.files:
        funcs = parsed[p][0]
        keep = []
        for f in funcs:
            if f.name in overrides and f.name not in rename.get(p, {}):
                if f.addr is not None:
                    host_regs.setdefault(p, []).append((f.addr, f.name))
                continue
            keep.append(f)
        funcs[:] = keep

    stats = Counter(xmap_stats)
    report = []
    total = ok = 0
    files_clean = 0
    all_ext = set()
    all_refs = set()
    per_file = []

    # Built before the loop, so every body it hands out is the file as written:
    # process() grows funcs in place, and an absorber must not be given a body
    # that has already absorbed something of its own.
    foreign_all = foreign_entry_map(args.files, parsed, plines)

    # Every function name this run defines under a flat C symbol. A `bl` to
    # anything else lands in decompiled C or pc/src/, which is the ABI
    # boundary ARMREC_CALL_EXT exists for. Taken from the parse rather than
    # the emitted text, so a merged or absorbed entry point still counts.
    #
    # A renamed name is not one of them: a local_arm_func_start function is
    # emitted file-qualified and an MSL name is emitted guest_-prefixed, so
    # neither is defined here under the name the call site spells.
    asm_funcs = set()
    local_funcs = {}
    for p in args.files:
        for f in parsed[p][0]:
            if f.name not in rename.get(p, {}):
                asm_funcs.add(f.name)
            elif f.file_local:
                local_funcs.setdefault(f.name, set()).add(stems[p])
    ext_calls = Counter()

    # Every function this run places at a guest address in Thumb state, so a
    # `.word` naming one can carry the interworking bit the ROM has there. A
    # plain name is unambiguous: nothing here is declared arm_func_start in
    # one file and thumb_func_start in another.
    thumb_funcs = set()
    for p in args.files:
        for f in parsed[p][0]:
            if f.thumb and f.name in symtab:
                thumb_funcs.add(f.name)

    # The same question for a function that has been decompiled, where the
    # marker went with the .s. See load_decomp_state().
    decomp_state = load_decomp_state(args.decomp_state)
    thumb_funcs |= set(n for n, s in decomp_state.items() if s == "thumb")

    # Every name the assembly in this run defines in any capacity: an entry
    # point, a label inside a function, a data label, a `.global`. Built from
    # the parse rather than symtab, which holds neither local labels nor a
    # name that is only declared.
    asm_names = set(symtab)
    declared = set()
    flag_result = set()
    for p in args.files:
        pfuncs, pdata, pglobals = parsed[p][0], parsed[p][1], parsed[p][2]
        for f in pfuncs:
            asm_names.add(f.name)
            asm_names |= set(f.entries)
            asm_names |= set(f.labels)
            if writes_cpsr_flags(f):
                flag_result |= set(f.entries)
        for _addr, label, kind, _payload in pdata:
            if kind == "label" and label:
                asm_names.add(label)
        declared |= set(pglobals)
    FLAG_RESULT_FUNCS = frozenset(flag_result)
    if args.wasm:
        # Defined, not merely declared: a `.global` alone can name C.
        WASM_ASM_NAMES = frozenset(asm_names)
    asm_names |= declared

    for p in args.files:
        funcs, data, globals_, externs, problems = parsed[p]
        for k, v in problems.items():
            stats["parse: " + k] += v
            # One parse problem is a finding about the tree rather than a
            # construct armrec cannot spell, so it goes in the report beside
            # the data findings: an implicit alignment nothing here can
            # arbitrate. See IMPLICIT_ALIGN.
            if k.startswith(UNARBITRATED_TAG):
                report.append((p, "<data>", 0, k[len(UNARBITRATED_TAG):],
                               UNARBITRATED_TAG.rstrip(": ")))
            # The second: a label whose `; 0x...` comment the assembler's own
            # location counter contradicts. armrec takes the counter and says
            # so here rather than correcting a disassembler in silence.
            elif k.startswith(ADDR_CONTRA_TAG):
                report.append((p, "<label>", 0, k[len(ADDR_CONTRA_TAG):],
                               ADDR_CONTRA_TAG.rstrip(": ")))
            # And the third: a `bl` to a local label that split_call_targets()
            # can decide neither way, because the target has no address. There
            # are none here; one appearing is a call armrec may have compiled
            # to a goto that never returns, so a human should read it.
            elif k.startswith(BL_UNPLACEABLE):
                report.append((p, "<bl>", 0, k[len(BL_UNPLACEABLE):],
                               BL_UNPLACEABLE.rstrip(": ")))
        foreign = {n: e for n, e in foreign_all.items() if e["path"] != p}
        t, o, ext, ec, refs = process(p, stems[p], funcs, data, symtab, args.out,
                                      stats, report, emit=not args.scan,
                                      local_addr=local_addr.get(p),
                                      rename=rename.get(p), lines=plines.get(p),
                                      foreign=foreign, asm_funcs=asm_funcs,
                                      abi_trap=abi_trap, thumb_funcs=thumb_funcs,
                                      overlay=overlay_of(p), asm_names=asm_names,
                                      decomp_state=decomp_state,
                                      incdirs=args.include,
                                      host_regs=host_regs.get(p, ()))
        all_refs |= refs
        all_ext |= ext
        ext_calls += ec
        total += t
        ok += o
        if t and t == o:
            files_clean += 1
        per_file.append((p, t, o))

    if args.out and not args.scan:
        with open(os.path.join(args.out, "armrec_init.c"), "w") as f:
            f.write('#include "armrec_rt.h"\n\n')
            names = [stems[p] for p in args.files]
            # overlay id -> the stems of the files that belong to it, in the
            # order the file list gives them, which is sorted.
            by_ovl = {}
            for p in args.files:
                ovl = overlay_of(p)
                if ovl is not None:
                    by_ovl.setdefault(ovl, []).append(stems[p])
            for n in names:
                f.write("void armrec_init_%s(void);\n" % n)
            for ovl in sorted(by_ovl):
                for n in by_ovl[ovl]:
                    f.write("void armrec_data_%s(void);\n" % n)
            f.write("\n/*\n"
                    " * Every file's slot reservations. An overlay file's data\n"
                " * is deliberately NOT here, see armrec_overlay_data()\n"
                    " * below.\n"
                    " */\n")
            f.write("void armrec_init_all(void);\n")
            f.write("void armrec_init_all(void) {\n")
            for n in names:
                f.write("    armrec_init_%s();\n" % n)
            f.write("}\n")
            f.write("\n/*\n"
                    " * The data half, one function per overlay, called by\n"
                    " * armrec_load_overlay() in armrec_rt.c and by nothing\n"
                    " * else. This is the strong definition of the weak hook\n"
                    " * that file declares, so a link without any generated\n"
                    " * code still works and simply loads no overlay data.\n"
                    " */\n")
            for ovl in sorted(by_ovl):
                f.write("static void armrec_ovl_data_%02d(void) {\n" % ovl)
                for n in by_ovl[ovl]:
                    f.write("    armrec_data_%s();\n" % n)
                f.write("}\n")
            top = (max(by_ovl) + 1) if by_ovl else 0
            f.write("\nstatic void (*const armrec_ovl_data[%d])(void) = {\n"
                    % (top or 1))
            for ovl in sorted(by_ovl):
                f.write("    [%d] = armrec_ovl_data_%02d,\n" % (ovl, ovl))
            if not by_ovl:
                f.write("    0,\n")   # no overlay assembly in this run
            f.write("};\n\n")
            f.write("void armrec_overlay_data(int id);\n")
            f.write("void armrec_overlay_data(int id) {\n")
            f.write("    if (id >= 0 && id < %d && armrec_ovl_data[id])\n" % top)
            f.write("        armrec_ovl_data[id]();\n")
            f.write("}\n")
    if args.out and not args.scan and not args.wasm:
        with open(os.path.join(args.out, "armrec_externs.c"), "w") as f:
            f.write('#include "armrec_rt.h"\n\n')
            for name in sorted(all_ext):
                f.write("extern uint64_t %s(uint32_t, uint32_t, uint32_t, uint32_t);\n" % name)
            for name in sorted(all_ext):
                f.write("uint32_t armrec_ext_%s;\n" % sanitize(name))
            f.write("\nvoid armrec_bind_externs(void);\n")
            f.write("void armrec_bind_externs(void) {\n")
            for name in sorted(all_ext):
                f.write('    armrec_ext_%s = armrec_extern_addr("%s", (void *)%s);\n'
                        % (sanitize(name), name, name))
            f.write("}\n")
        emit_data_syms(os.path.join(args.out, "armrec_data_syms.c"), data_syms)

    # The classification the wasm bridge rewrites decompiled C against
    # (games/diamond/pc/mk/bridge.mk). Everything is the C symbol armrec
    # emits or reaches, so a file-local function, which C cannot name, is
    # left out, and an MSL name appears guest_-prefixed.
    if args.classes:
        recs = {}
        def put(kind, name, addr=None):
            key = (kind, name)
            val = "%s %s" % (kind, name) + ("" if addr is None else " 0x%08X" % addr)
            if recs.get(key, val) != val:
                sys.stderr.write("armrec: classes: %s %s has two addresses\n"
                                 % (kind, name))
            recs[key] = val
        fnames = set()
        for p in args.files:
            prename = rename.get(p, {})
            for f in parsed[p][0]:
                if f.file_local and f.name in prename:
                    continue
                if f.addr is None:
                    stats["classes: function with no address"] += 1
                    continue
                cname = prename.get(f.name, f.name)
                fnames.add(cname)
                put("F", cname, f.addr | (1 if f.thumb else 0))
        # Linker-command-file symbols: decompiled C declares them extern and
        # only the link map defines them, so they are data at a fixed value.
        for name, value in (xmap[2].items() if xmap is not None else ()):
            if name not in fnames:
                put("D", name, value)
        for name, addr in data_syms.items():
            if not ASM_SYM_RE.match(name):
                continue
            cname = (GUEST_PREFIX + name) if name in HOST_LIBC_NAMES else name
            if cname not in fnames:
                put("D", cname, addr)
        for name in ext_calls:
            put("B", name)
        for p in host_regs:
            for _addr, name in host_regs[p]:
                put("B", name)
        for name in all_refs:
            if not ASM_SYM_RE.match(name):
                stats["classes: extern name C cannot spell: " + name] += 1
                continue
            if name in asm_funcs:
                stats["classes: extern is a recompiled function with no "
                      "address: " + name] += 1
            put("X", name)
        with open(args.classes, "w") as f:
            for line in sorted(recs.values()):
                f.write(line + "\n")

    if args.report:
        with open(args.report, "w") as f:
            for path, fn, lineno, raw, why in report:
                f.write("%s:%d: in %s: %s   [%s]\n" % (path, lineno, fn, why, raw))

    # The boundary report is armrec's own answer to "which calls leave
    # recompiled code", taken from the test it makes while emitting rather
    # than re-derived from the output. test_abi_boundary joins it against the
    # prototypes gcc reports for the decompiled C and fails if any callee
    # needs more words than ARMREC_EXT_STACK_WORDS supplies.
    if args.boundary:
        with open(args.boundary, "w") as f:
            f.write("# recompiled -> host call sites, by callee\n")
            f.write("# %d targets, %d sites, %d functions defined here\n"
                    % (len(ext_calls), sum(ext_calls.values()), len(asm_funcs)))
            for name in sorted(ext_calls):
                f.write("%s %d\n" % (name, ext_calls[name]))
            for name in sorted(abi_trap):
                f.write("!trap %s %s\n" % (name, abi_trap[name]))
            # The other side of the same boundary: decompiled C calling into
            # one of these pushes arguments five and up on the host stack,
            # where the recompiled body, which reads armrec_sp, cannot see
            # them.
            for name in sorted(asm_funcs):
                f.write("!def %s\n" % name)
            # And the names this run deliberately does not define flat. A
            # local_arm_func_start function is static, so its C symbol is
            # file-qualified and the bare name belongs to whoever else claims
            # it. test_local_funcs joins this against nm to check that nobody
            # is quietly answering for one.
            for name in sorted(local_funcs):
                f.write("!local %s %s\n" % (name, ",".join(sorted(local_funcs[name]))))

    if not args.quiet:
        pct = (100.0 * ok / total) if total else 0.0
        print("armrec: %d/%d functions translated cleanly (%.2f%%) across %d files"
              % (ok, total, pct, len(args.files)))
        print("armrec: %d files fully clean" % files_clean)
        if stats:
            print("armrec: top unsupported constructs:")
            for k, v in stats.most_common(20):
                print("   %8d  %s" % (v, k))
    return 0


if __name__ == "__main__":
    sys.exit(main())
