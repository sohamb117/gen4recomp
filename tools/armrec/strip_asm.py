#!/usr/bin/env python3
"""
Strip CodeWarrior `asm` function definitions from a C source file.

Files under arm9/ and the NitroSDK mix ordinary C with inline-ARM `asm`
functions. GCC cannot parse the `asm` bodies, so the whole translation unit
fails and every function in it goes missing, including large pure-C ones that
have nothing wrong with them.

Stripping the `asm` definitions lets the C in those files compile normally and
narrows the hand-porting job to the assembly functions themselves, which
pc/src/ supplies.

Forms handled, all from real SDK sources:

  asm OSIntrMode OS_EnableInterrupts (void)          plain definition
  static asm void OSi_CpuClear32 (...)               storage class first
  asm static void OSi_DebuggerExceptionHook (void)   storage class second
  SDK_WEAK_SYMBOL asm void OS_Halt (void)            macro prefix
      asm void OS_SpinWait (u32 cycle)               indented inside #ifdef
  SDK_WEAK_SYMBOL asm void __call_via_r0 (void){     brace on the header line
  static asm void OSi_ExceptionHandler(void);        prototype: the keyword is
                                                     removed, the declaration
                                                     is kept

A C function that merely contains a statement-level `asm { ... }` block is a
special case. Removing just the block would be silently wrong: the function
would still compile, still link, and quietly do something different. So the
whole containing function is stripped, its symbol goes undefined, and the link
fails loudly until pc/src/ supplies it. Each one is reported on stderr.

Why the parse is safe: `asm` bodies contain braces, so brace matching inside
them would be wrong. But mwcc formatting always closes a function with `}` at
the same indentation as the line that opened it, and nothing inside the body
sits at that indentation except labels. Anything unexpected fails the compile
loudly rather than silently dropping code.

Usage:
    strip_asm.py INPUT.c OUTPUT.c
    strip_asm.py --list INPUT.c      # names of the functions removed
"""

import argparse
import re
import sys

# An `asm` function definition or prototype: optional indentation, optional
# prefix tokens (static, SDK_WEAK_SYMBOL, ...), the `asm` keyword, then
# return-type/name tokens up to the parameter list's `(`.
ASM_FUNC = re.compile(
    r"^(?P<indent>[ \t]*)"
    r"(?:[A-Za-z_]\w*[ \t]+)*"          # prefixes before `asm`
    r"asm"
    r"(?P<rest>(?:[ \t]+[A-Za-z_]\w*|[ \t]*\*+)+)"  # type tokens + name
    r"[ \t]*\(")

# A statement-level `asm` block opener inside a C function: `asm` alone on a
# line, or `asm {`. The `{ ... }` body follows.
ASM_STMT = re.compile(r"^[ \t]*asm[ \t]*(\{.*)?$")

FUNC_NAME = re.compile(r"([A-Za-z_]\w*)\s*\(")


def _code_text(line, state):
    """Return `line` with comments and string/char literals blanked, updating
    the cross-line block-comment flag in `state`. Used only for brace
    counting, so fidelity beyond that does not matter."""
    out = []
    i = 0
    n = len(line)
    while i < n:
        if state["comment"]:
            j = line.find("*/", i)
            if j < 0:
                return "".join(out)
            state["comment"] = False
            i = j + 2
            continue
        c = line[i]
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            break
        if c == "/" and i + 1 < n and line[i + 1] == "*":
            state["comment"] = True
            i += 2
            continue
        if c in "\"'":
            quote = c
            i += 1
            while i < n:
                if line[i] == "\\":
                    i += 2
                    continue
                if line[i] == quote:
                    break
                i += 1
            i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _find_close(lines, start, indent, what, path):
    """Index of the line closing a function opened at indentation `indent`:
    the first subsequent line that is exactly `indent` + '}'."""
    close = re.compile(re.escape(indent) + r"\}[ \t]*$")
    j = start
    n = len(lines)
    while j < n and not close.match(lines[j].rstrip("\n")):
        j += 1
    if j >= n:
        raise SystemExit(
            "%s: unterminated %s (no closing '}' at the opening indentation)"
            % (path, what))
    return j


def _declaration_for(header):
    """A file-scope declaration derived from a stripped definition's header
    line: the `asm` keyword removed, any trailing `{` dropped, `;` appended.
    Declaring without defining is honest; the symbol still fails loudly at
    link, but it keeps C code that references the function compiling when
    the stripped definition was the only declaration in scope
    (OSi_DebuggerExceptionHook in os_exception.c is the canonical case)."""
    decl = re.sub(r"(?<![\w])asm[ \t]+", "", header, count=1)
    decl = decl.rstrip()
    if decl.endswith("{"):
        decl = decl[:-1].rstrip()
    return decl + ";\n"


def strip(lines, path="<input>"):
    """Return (kept_lines, removed_function_names)."""
    out = []
    out_code = []           # sanitized twin of `out`, for reference checks
    removed = []
    stripped_info = []      # (index into out of the marker, header line, name)
    decls = set()           # names declared at file scope in kept lines
    depth = 0               # brace depth of the surrounding C code
    func_header = None      # index into `out` of the current function header
    func_indent = ""        # its indentation
    comment_state = {"comment": False}
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]

        if not comment_state["comment"]:
            m = ASM_FUNC.match(line)
            if m:
                if line.rstrip().endswith(";"):
                    # Prototype. Dropping the `asm` keyword changes nothing
                    # about C semantics; the declaration must survive so
                    # callers still compile.
                    kept = re.sub(r"(?<![\w])asm[ \t]+", "", line, count=1)
                    out.append(kept)
                    out_code.append(_code_text(kept, comment_state))
                    if depth == 0:
                        decls.update(FUNC_NAME.findall(out_code[-1]))
                    i += 1
                    continue
                if depth > 0:
                    raise SystemExit(
                        "%s:%d: apparent asm function definition inside "
                        "another function, refusing to guess" % (path, i + 1))
                names = re.findall(r"[A-Za-z_]\w*", m.group("rest"))
                name = names[-1] if names else "<unnamed>"
                removed.append(name)
                j = _find_close(lines, i + 1, m.group("indent"),
                                "asm function %s (line %d)" % (name, i + 1),
                                path)
                out.append("%s/* armrec: asm function %s stripped; "
                           "see pc/src/ for the host implementation */\n"
                           % (m.group("indent"), name))
                out_code.append("")
                stripped_info.append((len(out) - 1, line, name))
                i = j + 1
                continue

            if ASM_STMT.match(line):
                # Statement-level asm block inside a C function. Stripping
                # just the block would silently change the function's
                # behaviour, so strip the whole containing function instead;
                # its symbol fails loudly at link like any asm function.
                if depth <= 0 or func_header is None:
                    raise SystemExit(
                        "%s:%d: statement-level 'asm' outside any tracked "
                        "function, refusing to guess" % (path, i + 1))
                header = out[func_header]
                fm = FUNC_NAME.search(header)
                name = fm.group(1) if fm else "<unnamed>"
                removed.append(name)
                sys.stderr.write(
                    "strip_asm: %s: function %s contains a statement-level "
                    "asm block; whole function stripped\n" % (path, name))
                del out[func_header:]
                del out_code[func_header:]
                stripped_info = [s for s in stripped_info
                                 if s[0] < func_header]
                out.append("%s/* armrec: C function %s stripped; it "
                           "contained a statement-level asm block; see "
                           "pc/src/ for the host implementation */\n"
                           % (func_indent, name))
                out_code.append("")
                stripped_info.append((len(out) - 1, header, name))
                j = _find_close(lines, i + 1, func_indent,
                                "function %s (asm block at line %d)"
                                % (name, i + 1), path)
                i = j + 1
                depth = 0
                func_header = None
                continue

        # Ordinary line: emit it and track function boundaries by brace
        # depth (comments and string literals blanked first).
        code = _code_text(line, comment_state)
        opens = code.count("{")
        closes = code.count("}")
        out.append(line)
        out_code.append(code)
        if depth == 0 and code.rstrip().endswith(";"):
            decls.update(FUNC_NAME.findall(code))
        if depth == 0 and opens > closes:
            before = code[:code.index("{")]
            if before.strip():
                func_header = len(out) - 1
            else:
                # `{` on its own line: the header is the nearest previous
                # non-blank line.
                k = len(out) - 2
                while k >= 0 and not out[k].strip():
                    k -= 1
                func_header = k if k >= 0 else len(out) - 1
            hdr = out[func_header]
            func_indent = hdr[:len(hdr) - len(hdr.lstrip())]
        depth += opens - closes
        if depth <= 0:
            depth = 0
            func_header = None
        i += 1

    # If a stripped function is still referenced by the surviving C and the
    # file holds no other file-scope declaration of it, put a declaration
    # where the definition stood so those references keep compiling. The
    # symbol stays undefined, so nothing gets quieter at link.
    body = "\n".join(out_code)
    for idx, header, name in reversed(stripped_info):
        if name in decls:
            continue
        if re.search(r"(?<!\w)%s(?!\w)" % re.escape(name), body):
            out.insert(idx + 1, _declaration_for(header))
    return out, removed


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--list", action="store_true",
                    help="print the names of stripped functions and exit")
    args = ap.parse_args()

    with open(args.input, "r", errors="replace") as fh:
        lines = fh.readlines()

    kept, removed = strip(lines, args.input)

    if args.list:
        for name in removed:
            print(name)
        return 0

    if not args.output:
        ap.error("an output path is required unless --list is given")
    with open(args.output, "w") as fh:
        fh.writelines(kept)
    return 0


if __name__ == "__main__":
    sys.exit(main())
