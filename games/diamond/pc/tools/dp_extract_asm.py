#!/usr/bin/env python3
"""
Extract the mwcc `asm` function bodies of one Diamond/Pearl C file into a .s
that armrec (games/platinum/tools/armrec) recompiles like the rest of the
game's assembly.

The parse is games/platinum/tools/armrec/extract_asm.py's, unchanged
(imported, not copied). That tool was written for synthetic test harnesses;
recompiling these functions into the real game needs four things it does not
do, and this wrapper adds them:

  * Macros. The bodies are written against the C preprocessor:
    `bic r1, r1, #HW_PSR_CPU_MODE_MASK`, `.word REG_DIV_NUMER_ADDR`,
    `mov r0, #REGION_BIT(0,1,0,0,0,0,1,0)`, `#HW_CACHE_LINE_SIZE - 1`. mwcc
    expanded them; armrec cannot. So the parse runs on the compiler's own
    preprocessed text (`--cc ... -E -C`, the same flags the object is
    compiled with, comments kept because extract_asm's jump-table repair
    reads commented `.short` lines), and every immediate or pool word that
    is then pure integer arithmetic is folded to one hex constant. An
    immediate that still names something is a C constant the preprocessor
    cannot see (an enum: `mov r0, #OS_PROCMODE_IRQ`); clang evaluates it
    in a probe that includes what the source includes. Pool words that
    still name a symbol (a linker symbol, a C global) are left for armrec
    to resolve.

  * Inactive code. Only lines the preprocessor kept are parsed, so an `asm`
    function under an #if the build does not take is not extracted.

  * ARM or Thumb, per function. mwcc picks the instruction set from
    `#pragma thumb on|off`, which include-mw/code16.h / code32.h hold and
    which a file may switch mid-way (MI_memory.c's MI_Zero36B, the
    FX_mtx*.c rotations are Thumb after ARM functions; game src is Thumb
    through include/global.h). The pragmas survive -E in stream order, so
    the mode in force at each function's header line is the one mwcc used.

  * Addresses. Each function gets its ROM address from the link map
    (`<addr> <size> .text|.itcm <name> (<object>)` in arm9.elf.xMAP,
    matched on name AND object, since static asm functions are file-local),
    in the `name: ; 0x<addr>` form armrec reads, under the section the ROM
    placed it in (OS_IrqHandler and OSi_DoBoot run from ITCM, 0x01FF8000).
    A function missing from the map is an error, not a synthetic address.
    The authors named local labels after ROM addresses (`_020CE1CC:`), and
    armrec reads such a name as the label's address; MI_memory.c reuses
    MIi_CpuClear16's `_020CE1CC` inside MIi_CpuCopy16, which mwcc allowed
    (asm labels are function-local) and which here would be one label
    defined twice at the wrong address. A label whose number lies outside
    its own function's [address, address + size) is renamed
    `_<function>_L<n>`, which states no address.

Functions the host layer replaces (pc/host_overrides.txt, one name per line,
`#` comments) are not emitted: armrec must not define a name the host
defines, or the bridge would route C calls to the recompiled body.

Usage:
    dp_extract_asm.py --cc "<clang + game flags>" --xmap arm9.elf.xMAP \
        --object scrcmd.o [--overrides host_overrides.txt] [--drop NAME]... \
        INPUT.c OUTPUT.s
"""

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ARMREC = os.path.normpath(os.path.join(HERE, "../../../platinum/tools/armrec"))
sys.path.insert(0, ARMREC)
import extract_asm  # noqa: E402

MARKER = re.compile(r'^#\s*(\d+)\s+"((?:[^"\\]|\\.)*)"')
PRAGMA_THUMB = re.compile(r"^\s*#\s*pragma\s+thumb\s+(on|off)\b")
XMAP_CODE = re.compile(
    r"^\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s+\.(text|itcm)\s+(\S+)\s+\((\S+)\)")
MEMBER_OFFSET = re.compile(
    r"\b([A-Za-z_]\w*)\.([A-Za-z_]\w*(?:\[[^\]]*\])*(?:\.[A-Za-z_]\w*(?:\[[^\]]*\])*)*)")
ADDR_LABEL = re.compile(r"\b_([0-9A-Fa-f]{8})\b")

# Casts and integer suffixes the SDK's register/constant macros carry.
CAST = re.compile(
    r"\(\s*(?:const\s+|volatile\s+)*"
    r"(?:u8|u16|u32|u64|s8|s16|s32|s64|vu8|vu16|vu32|int|long|short|char|"
    r"unsigned(?:\s+(?:int|long|short|char))?|signed(?:\s+(?:int|long|short|char))?)"
    r"(?:\s*\*)*\s*\)")
SUFFIX = re.compile(r"\b(0[xX][0-9A-Fa-f]+|\d+)[uUlL]+\b")
ARITH = re.compile(r"^[\s0-9A-Fa-fxX()+\-*/%<>|&^~]+$")
NUMBER = re.compile(r"^\s*(?:0[xX][0-9A-Fa-f]+|\d+)\s*$")
WORD = re.compile(r"^(\s*(?:[A-Za-z_.$][\w.$]*:\s*)?\.word\s+)(.+?)(\s*(?://.*|;.*)?)$")


def preprocess(cc, src):
    cmd = shlex.split(cc) + ["-E", "-C", src]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       universal_newlines=True)
    if r.returncode:
        sys.stderr.write(r.stderr)
        raise SystemExit("%s: preprocessing failed" % src)
    return r.stdout


def main_file_lines(text, src):
    """Expanded lines of `src` itself, by original line number, plus the
    instruction set in force at each of them."""
    lines, modes = {}, {}
    cur_file, cur_line = None, 0
    thumb = False
    want = os.path.realpath(src)
    for raw in text.split("\n"):
        m = MARKER.match(raw)
        if m:
            cur_line = int(m.group(1))
            cur_file = m.group(2)
            continue
        pm = PRAGMA_THUMB.match(raw)
        if pm:
            thumb = pm.group(1) == "on"
        if cur_file is not None and cur_file not in ("<built-in>", "<command line>") \
                and os.path.realpath(cur_file) == want:
            lines[cur_line] = raw
            modes[cur_line] = thumb
        cur_line += 1
    n = max(lines) if lines else 0
    return [lines.get(i, "") + "\n" for i in range(1, n + 1)], modes


def fold(expr):
    """Integer arithmetic -> '0x%X', or None when it names anything."""
    if NUMBER.match(expr):
        return None
    e = expr
    while True:
        e2 = CAST.sub("", e)
        if e2 == e:
            break
        e = e2
    e = SUFFIX.sub(r"\1", e)
    if not ARITH.match(e):
        return None
    e = re.sub(r"(?<![/])/(?![/])", "//", e)
    try:
        v = eval(e, {"__builtins__": {}}, {})
    except Exception:
        return None
    if not isinstance(v, int):
        return None
    return "0x%X" % (v & 0xFFFFFFFF)


def split_operand(s, start):
    """End index of the operand that starts at s[start]."""
    depth = 0
    # Brackets opened inside the operand (an mwcc member offset such as
    # `#OSiExContext.debug[1]`) close inside it; a `]` at bracket depth 0
    # ends a memory operand.
    bdepth = 0
    i = start
    while i < len(s):
        c = s[i]
        if c == "(":
            depth += 1
        elif c == ")":
            if depth == 0:
                break
            depth -= 1
        elif c == "[":
            bdepth += 1
        elif c == "]" and bdepth:
            bdepth -= 1
        elif depth == 0 and (c in ",]}!;" or s.startswith("//", i)):
            break
        i += 1
    return i


class Folder(object):
    """Folds operands; asks clang for immediates that name a C constant."""

    INCLUDE = re.compile(r"^\s*#\s*include\b.*$", re.M)
    INT32 = re.compile(r"^__dp_imm:\s*\n\s*\.int32\s+(-?\d+)\b", re.M)

    def __init__(self, cc, src, workdir):
        self.cc = cc
        with open(src, errors="replace") as fh:
            text = fh.read()
        self.includes = "\n".join(self.INCLUDE.findall(text))
        # Retried with the TU's own top-level typedefs when the includes
        # alone do not resolve it: an immediate may name a type the .c
        # defines itself (HG/SS's os_exception.c: `#OSiExContext.debug[1]`).
        self.local = "\n".join(self.typedefs(text))
        self.workdir = workdir
        self.cache = {}

    @staticmethod
    def typedefs(text):
        out = []
        for m in re.finditer(r"^typedef\b", text, re.M):
            depth = 0
            for i in range(m.start(), len(text)):
                c = text[i]
                if c == "{":
                    depth += 1
                elif c == "}":
                    depth -= 1
                elif c == ";" and depth == 0:
                    out.append(text[m.start():i + 1])
                    break
        return out

    def probe(self, expr):
        if expr in self.cache:
            return self.cache[expr]
        # mwcc's inline assembler spells a member offset `#Type.member`
        # (HG/SS's os_irqHandler.c: `#OSThread.link.next`,
        # os_exception.c: `#OSiExContext.context.r[4]`): offsetof in C.
        c_expr = MEMBER_OFFSET.sub(
            lambda m: "__builtin_offsetof(%s, %s)" % (m.group(1), m.group(2)), expr)
        v = self.evaluate(self.includes, c_expr)
        if v is None and self.local:
            v = self.evaluate(self.includes + "\n" + self.local, c_expr)
        self.cache[expr] = v
        return v

    def evaluate(self, prelude, c_expr):
        fd, path = tempfile.mkstemp(suffix=".c", prefix=".dp_imm_", dir=self.workdir)
        with os.fdopen(fd, "w") as fh:
            fh.write("%s\nconst unsigned long __dp_imm = (unsigned long)(%s);\n"
                     % (prelude, c_expr))
        r = subprocess.run(shlex.split(self.cc) + ["-S", "-o", "-", path],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           universal_newlines=True)
        os.unlink(path)
        m = self.INT32.search(r.stdout) if r.returncode == 0 else None
        return "0x%X" % (int(m.group(1)) & 0xFFFFFFFF) if m else None

    def line(self, line):
        m = WORD.match(line)
        if m:
            v = fold(m.group(2))
            return m.group(1) + v + m.group(3) if v else line
        out = []
        i = 0
        while True:
            j = line.find("#", i)
            if j < 0:
                out.append(line[i:])
                break
            # A `//` or `;` comment ends the instruction.
            cpos = min([p for p in (line.find("//", i), line.find(";", i)) if p >= 0]
                       or [len(line)])
            if cpos < j:
                out.append(line[i:])
                break
            end = split_operand(line, j + 1)
            expr = line[j + 1:end]
            v = fold(expr)
            if v is None and re.search(r"[A-Za-z_]", re.sub(r"0[xX][0-9A-Fa-f]+", "", expr)):
                v = self.probe(expr)
                if v is None:
                    raise SystemExit("cannot evaluate immediate #%s" % expr.strip())
            out.append(line[i:j + 1])
            out.append(v if v else expr)
            i = end
        return "".join(out)


def load_xmap(path, obj):
    """{name: [(addr, size, section)]} for the code of one object."""
    addrs = {}
    with open(path, errors="replace") as fh:
        for line in fh:
            m = XMAP_CODE.match(line)
            if m and m.group(5) == obj:
                addrs.setdefault(m.group(4), []).append(
                    (int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
    return addrs


def rename_foreign_labels(name, addr, size, body):
    """Rename address-named labels that do not lie in this function."""
    defined = set(m.group(1) for m in
                  (re.match(r"^\s*_([0-9A-Fa-f]{8}):", l) for l in body) if m)
    foreign = sorted(d for d in defined if not addr <= int(d, 16) < addr + size)
    if not foreign:
        return body
    new = dict((d, "_%s_L%d" % (name, i)) for i, d in enumerate(foreign))
    return [ADDR_LABEL.sub(lambda m: new.get(m.group(1), m.group(0)), l) for l in body]


AT_LABEL = re.compile(r"@(\w+)")


def rename_at_labels(name, body):
    """
    mwcc's `@name` local labels (HG/SS's os_cache.c `@innerLoop`) become
    `_<function>_name`: `@` opens a comment for armrec, as for GNU as on ARM.
    """
    defined = set(m.group(1) for m in
                  (re.match(r"^\s*@(\w+):", l) for l in body) if m)
    if not defined:
        return body
    return [AT_LABEL.sub(lambda m: ("_%s_%s" % (name, m.group(1))
                                    if m.group(1) in defined else m.group(0)), l)
            for l in body]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cc", required=True)
    ap.add_argument("--xmap", required=True)
    ap.add_argument("--object", required=True, help="object name in the xMAP")
    ap.add_argument("--overrides")
    ap.add_argument("--drop", action="append", default=[],
                    help="a function that never runs on the host (not emitted)")
    ap.add_argument("--label", help="source path to name in the output header")
    ap.add_argument("input")
    ap.add_argument("output")
    args = ap.parse_args()

    skip = set()
    if args.overrides and os.path.exists(args.overrides):
        with open(args.overrides) as fh:
            for line in fh:
                line = line.split("#", 1)[0].strip()
                if line:
                    skip.add(line)

    lines, modes = main_file_lines(preprocess(args.cc, args.input), args.input)
    funcs = extract_asm.extract(lines, args.label or args.input)
    xmap = load_xmap(args.xmap, args.object)
    folder = Folder(args.cc, args.input, os.path.dirname(os.path.abspath(args.output)))

    header_line = {}
    for idx, l in enumerate(lines):
        if extract_asm.ASM_START.match(l):
            m = extract_asm.NAME.match(l)
            if m:
                header_line.setdefault(m.group(1), idx + 1)

    out = ["/* Generated by pc/tools/dp_extract_asm.py from %s: the mwcc asm\n"
           " * function bodies, preprocessed, at their ROM addresses. */\n"
           % (args.label or args.input)]
    section = None
    dropped, dead = [], []
    for name, body, subst in funcs:
        if name in skip:
            dropped.append(name)
            continue
        if name in args.drop:
            dead.append(name)
            continue
        a = xmap.get(name)
        if not a or len(a) != 1:
            raise SystemExit("%s: %s: %s in %s's xMAP code entries"
                             % (args.input, name,
                                "missing" if not a else "ambiguous", args.object))
        addr, size, sect = a[0]
        if sect != section:
            out.append("\t.section .%s\n\n" % sect)
            section = sect
        thumb = modes.get(header_line[name], False)
        body = rename_at_labels(name, rename_foreign_labels(name, addr, size, body))
        body, pool = extract_asm.pool_literals(name, extract_asm.jump_tables(body))
        body = [folder.line(l) for l in body]
        pool = [folder.line(l) for l in pool]
        kind = "thumb" if thumb else "arm"
        if subst:
            out.append("\t/* %s names its arguments: %s */\n"
                       % (name, ", ".join("%s=%s" % (k, v) for k, v in subst)))
        out.append("\t%s_func_start %s\n" % (kind, name))
        out.append("%s: ; 0x%08X\n" % (name, addr))
        out.extend(l + "\n" for l in body + pool)
        out.append("\t%s_func_end %s\n\n" % (kind, name))
    if dropped:
        out.append("/* Replaced by the host layer (pc/host_overrides.txt): %s */\n"
                   % " ".join(dropped))
    if dead:
        out.append("/* Never run on the host (pc/mk/game.mk GAME_ASM_DEAD): %s */\n"
                   % " ".join(dead))

    tmp = args.output + ".tmp"
    with open(tmp, "w") as fh:
        fh.writelines(out)
    os.replace(tmp, args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
