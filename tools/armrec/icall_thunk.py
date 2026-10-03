#!/usr/bin/env python3
"""
Route every indirect call in decompiled guest C through armrec's function table.

The problem: a guest code address is not a host code address. Recompiled code
has always known that, because `bx rN` compiles to armrec_dispatch(), which
resolves the address against the registered function table. Decompiled C does
not, because it calls a function pointer with C's own `p(...)`.
arm9/src/overlay_manager.c is the case that stopped the boot:

    overlayManager->template.initFunc(overlayManager, &overlayManager->overlayStatus)

`initFunc` is a word read out of guest memory, laid in as the guest address it
is, so the port jumped to 0x021D8D11 and segfaulted even though the target is a
perfectly good recompiled host function.

Why this and not a list of files. The obvious fix is to copy each offending .c
into pc/src/ with the call spelled armrec_call_code(). That is 98 indirect call
sites across about 30 decompiled files, and it forks every one of them: two
sources of truth for a file upstream is still editing, forever. Decompilation
convergence is the point of the project, so a mechanism that taxes each new .c
is the wrong shape.

This rewrites the compiler's output instead, so the .c is untouched: the build
compiles guest C to assembly, this pass turns every indirect call into a call
through armrec_icall, and the result is assembled. A file decompiled next year
gets it with no edit anywhere.

The rewrite. i386 cdecl passes every argument on the stack and returns in eax
and edx, so a thunk that only has to replace the target can leave the frame
exactly as it found it:

    call *OP        ->      movl OP, %eax
                            pushl %eax
                            call armrec_icall

armrec_icall pops the target, restores the frame to what `call *OP` would have
left, resolves the address and jumps. The callee returns straight to the
original call site, and the thunk does not appear in a backtrace.

`movl OP, %eax` rather than `pushl OP` on purpose: two call sites are
%esp-relative, and whether `pushl 0x1c(%esp)` computes its address before or
after the decrement is an ISA subtlety this does not need to depend on. %eax is
call-clobbered, so nothing live is in it.

`jmp *OP` is not always a tail call, and that is the one thing that has to be
got right. GCC emits the same instruction for a switch:

    jmp *.L4(,%eax,4)       <- switch table
    jmp *tab(,%eax,4)       <- tail call through a function-pointer array
    jmp *0x4(%esp)          <- tail call through a parameter

Measured at -O2 -fno-pie: the discriminator is the label. GCC's own jump tables
are anchored at a compiler-generated `.L<digits>` label and nothing else is, so
a `.L<digits>` base is left alone and everything else is a tail call. The
shipped build is -O1 and emits no indirect tail calls at all; -O2 does, and
pc/Makefile treats -O2 as a bug detector, so both have to work.

Anything this cannot classify stops the build rather than passing through.

The second rewrite is a direct call, and the other half of the same ABI
boundary:

    call NAME       ->      call armrec_stk_NAME        (NAME recompiled)
    jmp  NAME       ->      jmp  armrec_stk_NAME

ARM passes arguments five and up on the stack, and a recompiled body reads them
off the emulated stack while a C caller pushed them on the host one. So
OS_SetPeriodicAlarm's handler and argument arrived as zeros and the ARM7's
sound alarm was armed with a NULL callback. armrec_stk_NAME is a generated
one-line trampoline over armrec_stkargs_call().

--stkargs takes armrec's own `!def` list, so the set is armrec's answer rather
than a guess; --stkargs-out records what this file actually rewrote, which is
what the trampolines are generated from, so the two cannot drift.

Only `call` and `jmp`, deliberately: taking the address of a recompiled
function is left alone, because that address goes on to be stored in guest
memory and compared, and a trampoline's address is not the function's.

The third rewrite, --demote-def NAME, exists because PE cannot weaken a symbol
away from its own translation unit. On ELF `objcopy --weaken-symbol` delivers
PC_WEAKEN's contract: every call is a relocation, and the linker resolves it to
the fork's strong definition. On COFF, gas resolves a same-section call at
assembly time, so the weakened object called its own unfixed body forever. No
object-level tool can retarget a call with no relocation, so the demotion
happens here, in the assembly: the definition of NAME is renamed to
pc_demoted_NAME, every reference still spells NAME, and the only definition
left for the linker to find is the fork's. The demoted body stays in the object
under its quarantine name, which makes the mechanism visible to nm.
"""

import argparse
import os
import re
import shlex
import subprocess
import sys

# `call *OP` / `jmp *OP`, with the operand ending at a comment or end of line.
CALL_RE = re.compile(r"^(?P<pre>\s*)call\s+\*(?P<op>[^#;]*?)\s*(?P<post>(?:[#;].*)?)$")
JMP_RE = re.compile(r"^(?P<pre>\s*)jmp\s+\*(?P<op>[^#;]*?)\s*(?P<post>(?:[#;].*)?)$")

# A GCC switch jump table: base is a compiler-generated local label, `.L4`
# on ELF, `L4` on PE, where gas's local-label prefix has no dot. Missing the
# PE spelling rewrote a *switch* into an icall tail call, and the switch arm
# then ran with %eax holding its own address instead of the object: EIP
# writing twelve bytes ahead of itself in Bg_SetPosText, at frame 59, on the
# first Windows boot that got that far.
SWITCH_RE = re.compile(r"^\.?L\d+\(,%e(?:ax|bx|cx|dx|si|di|bp),[1248]\)$")
LOCAL_LABEL_RE = re.compile(r"^\.?L\d+")

# `call NAME` / `jmp NAME` to a plain symbol; no `*`, so this is a direct
# call or a tail call, never a switch. A `jmp .L3` inside a function matches the
# shape too and is filtered by the name not being a recompiled function.
DIRECT_RE = re.compile(
    r"^(?P<pre>\s*)(?P<op>call|jmp)\s+(?P<sym>[A-Za-z_$][A-Za-z0-9_$.]*)"
    r"\s*(?P<post>(?:[#;].*)?)$")

# The prefix the trampolines are generated under. Shared with gen_stkargs.py.
STK_PREFIX = "armrec_stk_"

# The tells that the input is not the ABI this rewrite is written for.
PIC_TELLS = ("@GOTOFF", "@GOT", "_GLOBAL_OFFSET_TABLE_", "get_pc_thunk")
X64_TELLS = ("%rax", "%rbx", "%rsp", "%rbp", "%rdi", "%rsi", "%r8", "%rip")


class Refused(Exception):
    """The input holds something this pass will not guess at."""


def rewrite(text, where="<asm>", stkargs=frozenset(), pe=False):
    """
    Rewrite one i386 assembly file.

    Returns (text, calls, tailcalls, stk) where stk is the set of recompiled
    functions whose direct calls were routed through a stack-argument
    trampoline.

    `pe` says the assembly is i386 PE (mingw), where every C symbol carries a
    leading underscore: the boundary list and the sidecar speak C names, so
    the lookup strips one underscore and every symbol this pass *inserts*
    gains one. The rewrite itself is identical; the ABI is cdecl either way.
    """
    for tell in PIC_TELLS:
        if tell in text:
            raise Refused(
                "%s: position-independent code (%s). The thunk calls "
                "armrec_resolve_code directly, which -fno-pie makes a plain "
                "call; the build is -m32 -fno-pie." % (where, tell))
    for tell in X64_TELLS:
        if tell in text:
            raise Refused(
                "%s: this is not 32-bit x86 (%s). The port is -m32 and "
                "the thunk's stack shuffle is cdecl."
                % (where, tell))

    out = []
    calls = tails = 0
    stk = set()
    deco = "_" if pe else ""
    for line in text.split("\n"):
        m = DIRECT_RE.match(line)
        if m:
            sym = m.group("sym")
            if pe and sym.startswith("_"):
                sym = sym[1:]
            if sym in stkargs:
                stk.add(sym)
                out.append("%s%s\t%s%s%s%s" % (m.group("pre"), m.group("op"),
                                               deco, STK_PREFIX, sym,
                                               ("\t" + m.group("post"))
                                               if m.group("post") else ""))
                continue
        m = CALL_RE.match(line)
        if m:
            pre, op, post = m.group("pre"), m.group("op"), m.group("post")
            out.append("%smovl\t%s, %%eax%s" % (pre, op, ("\t" + post) if post else ""))
            out.append("%spushl\t%%eax" % pre)
            out.append("%scall\t%sarmrec_icall" % (pre, deco))
            calls += 1
            continue
        m = JMP_RE.match(line)
        if m:
            pre, op, post = m.group("pre"), m.group("op"), m.group("post")
            if SWITCH_RE.match(op):
                out.append(line)          # a switch, not a call
                continue
            if LOCAL_LABEL_RE.match(op):
                raise Refused(
                    "%s: `jmp *%s` is anchored at a compiler-generated label "
                    "but is not the switch-table form this pass knows. "
                    "Refusing to guess whether it is a switch or a tail call."
                    % (where, op))
            out.append("%smovl\t%s, %%eax%s" % (pre, op, ("\t" + post) if post else ""))
            out.append("%spushl\t%%eax" % pre)
            out.append("%sjmp\t%sarmrec_icall_tail" % (pre, deco))
            tails += 1
            continue
        out.append(line)
    return "\n".join(out), calls, tails, stk


DEMOTE_PREFIX = "pc_demoted_"

def demote_defs(text, names, where="<asm>"):
    """
    Rename the *definition* of each symbol in `names` to its quarantine name,
    leaving every reference spelling the original, the third rewrite in the
    module docstring. `names` are assembly-level (a PE caller passes them with
    the underscore already on).

    Only definition directives move: the label, .globl, .def (COFF) and
    .type/.size (ELF). A reference, call, jmp, or an address stored in data
   , keeps the original name on purpose; undefined is the point.

    A name with no definition here stops the build: PC_WEAKEN said this file
    defines it, so a miss is a renamed function upstream or a typo in the
    list, and either should fail by name rather than link the unfixed body.
    """
    out = []
    found = {n: 0 for n in names}
    globl_re = {n: re.compile(r"^(\s*\.globl\s+)" + re.escape(n) + r"\s*$")
                for n in names}
    coffdef_re = {n: re.compile(r"^(\s*\.def\s+)" + re.escape(n) + r"\s*(;.*)$")
                  for n in names}
    elfmeta_re = {n: re.compile(r"^(\s*\.(?:type|size)\s+)" + re.escape(n)
                                + r"\s*(,.*)$")
                  for n in names}
    label_re = {n: re.compile(r"^" + re.escape(n) + r":\s*$") for n in names}
    for line in text.split("\n"):
        for n in names:
            q = DEMOTE_PREFIX + n
            m = label_re[n].match(line)
            if m:
                found[n] += 1
                line = q + ":"
                break
            m = globl_re[n].match(line)
            if m:
                line = m.group(1) + q
                break
            m = coffdef_re[n].match(line)
            if m:
                line = m.group(1) + q + "\t" + m.group(2)
                break
            m = elfmeta_re[n].match(line)
            if m:
                line = m.group(1) + q + m.group(2)
                break
        out.append(line)
    for n, count in found.items():
        if count == 0:
            raise Refused(
                "%s: --demote-def %s, but nothing here defines it. PC_WEAKEN "
                "names a function of this file, so either upstream renamed it "
                "or the list has a typo; both should fail here by name."
                % (where, n))
    return "\n".join(out)


def read_defined(path):
    """
    The names armrec says it defines, out of its own --boundary file.

    `!def NAME` is armrec's answer to "is this function assembly", which is the
    only question that matters here; a name defined by a decompiled .c as well
    is still safe to route, because the trampoline forwards the words as well as
    storing them (armrec_rt.h). Absent file means the rewrite is off, which is
    what every consumer outside pc/Makefile wants.
    """
    names = set()
    if not path:
        return names
    with open(path) as fh:
        for line in fh:
            p = line.split()
            if len(p) >= 2 and p[0] == "!def":
                names.add(p[1])
    return names


def split_args(argv):
    """Find -o, the input .c, and the flags the assembler pass needs."""
    out = src = None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "-o" and i + 1 < len(argv):
            out = argv[i + 1]
            i += 2
            continue
        if a.endswith(".c") and not a.startswith("-"):
            src = a
        i += 1
    return out, src


# Flags that still mean something when the input is assembly. Everything else
# (-I, -D, -include, -std, -W) is a preprocessor or C-frontend flag and would
# be noise or an error on a .s.
ASM_FLAG_RE = re.compile(r"^(-m|-f(no-)?(pie|PIC|pic)$|-g|-no-pie$|-O)")


def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--cc", required=True,
                    help="the compiler, as one shell word list")
    ap.add_argument("--keep", action="store_true",
                    help="keep the intermediate .s files")
    ap.add_argument("--stkargs",
                    help="armrec's --boundary file. Every `!def` name in it is "
                         "a function defined in assembly, and a direct call to "
                         "one from this C is routed through its stack-argument "
                         "trampoline")
    ap.add_argument("--stkargs-out",
                    help="write the names actually rewritten here, one per "
                         "line, for gen_stkargs.py")
    ap.add_argument("--pe", action="store_true",
                    help="the target is i386 PE (mingw), whose C symbols "
                         "carry a leading underscore")
    ap.add_argument("--demote-def", action="append", default=[],
                    metavar="SYM", dest="demote",
                    help="rename SYM's *definition* in this file to "
                         "pc_demoted_SYM, leaving references undefined so "
                         "the linker binds them to a fork's strong def, "
                         "PC_WEAKEN's delivery on PE, where a same-section "
                         "call to a defined symbol is baked with no "
                         "relocation. SYM is assembly-level: pass the "
                         "underscore on PE")
    ap.add_argument("rest", nargs=argparse.REMAINDER)
    ns = ap.parse_args()

    argv = ns.rest
    if argv and argv[0] == "--":
        argv = argv[1:]
    cc = shlex.split(ns.cc)

    stkargs = read_defined(ns.stkargs)

    obj, src = split_args(argv)
    if obj is None or src is None or "-c" not in argv:
        # Not a compile of one .c to one .o; nothing here to rewrite, and
        # guessing would be worse than passing it straight through.
        return subprocess.call(cc + argv)

    raw = obj + ".raw.s"
    fixed = obj + ".icall.s"

    # Pass one: C -> assembly, with the original flags (so -MMD still writes
    # the dependency file, and every -I/-D still applies).
    cc1 = [("-S" if a == "-c" else a) for a in argv]
    cc1 = [(raw if a == obj else a) for a in cc1]
    rc = subprocess.call(cc + cc1)
    if rc != 0:
        return rc

    try:
        with open(raw, "r") as fh:
            text = fh.read()
        text, calls, tails, stk = rewrite(text, where=src, stkargs=stkargs,
                                          pe=ns.pe)
        if ns.demote:
            text = demote_defs(text, ns.demote, where=src)
    except Refused as exc:
        # This must be visible: pc/Makefile compiles guest C with stderr
        # silenced so that a file which genuinely does not build can print one
        # SKIP line, and a refusal swallowed there would be a silent hole.
        sys.stdout.write("  ICALL   refused: %s\n" % exc)
        sys.stdout.flush()
        return 1
    with open(fixed, "w") as fh:
        fh.write(text)

    # Written before the assemble, and unconditionally: an empty sidecar is the
    # true answer for a file that calls no recompiled function, and make needs
    # the file to exist either way.
    if ns.stkargs_out:
        with open(ns.stkargs_out, "w") as fh:
            for name in sorted(stk):
                fh.write(name + "\n")

    # Pass two: assembly -> object. Only the flags that survive the frontend.
    cc2 = [a for a in argv if ASM_FLAG_RE.match(a)]
    rc = subprocess.call(cc + cc2 + ["-c", "-o", obj, fixed])
    if rc == 0 and not ns.keep:
        for path in (raw, fixed):
            try:
                os.unlink(path)
            except OSError:
                pass
    if os.environ.get("ARMREC_ICALL_VERBOSE") and (calls or tails):
        sys.stdout.write("  ICALL   %s: %d call, %d tail\n" % (src, calls, tails))
    return rc


if __name__ == "__main__":
    sys.exit(main())
