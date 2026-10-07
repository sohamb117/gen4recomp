#!/usr/bin/env python3
"""
srcfix: make a decomp TU's preprocessed C (after the decomp's own preproc)
lay data out the way agbcc does, and compile for wasm32.

    srcfix.py IN.i OUT.i

1. agbcc (old GCC, APCS) aligns every struct and union to at least 4 bytes
   and pads its size to a multiple of 4: `struct UCoords8 {u8 x, y;}` is 4
   bytes, so a table of four is 16 (ELF: sMauvilleGymSwitchCoords). Every
   `struct`/`union` definition (one with a body) that is not packed gets
   __attribute__((aligned(4))), which does the same on clang; one made of
   bit-fields alone is also packed (see only_bitfields). The bridge then
   checks each dropped global's size against the ELF.
2. Inline ARM asm cannot compile for wasm. `asm("")` (a cross-jump barrier,
   with or without a clobber list such as pokeruby's `asm("":::"r9")`)
   becomes nothing; a register variable's binding (`register u32 x
   asm("r4")`) is dropped; any other asm statement becomes __builtin_trap()
   (only unreachable paths still have one: the script engine's halt on a null
   script, an unused SWI wrapper, pokeruby's asm-only functions, which also
   lose their naked attribute so the trap compiles).
"""
import re
import sys

ATTR_ALIGN = " __attribute__((aligned(4)))"
ATTR_PACK_ALIGN = " __attribute__((packed, aligned(4)))"


def only_bitfields(body):
    """True for a struct body made of bit-fields alone. agbcc lets a bit-field
    straddle its type's units (gListMenuOverride: u8 fields of 4,4,4,6,6,7,1
    bits take 4 bytes, not clang's 5+3), which clang does only when packed;
    with every member a bit-field, packed changes nothing else."""
    if "{" in body:
        return False
    decls = [d.strip() for d in body.split(";") if d.strip()]
    return bool(decls) and all(re.search(r":\s*\w+\s*$", d) for d in decls)
WORD = re.compile(r"[A-Za-z_]\w*")
ASM = re.compile(r'\b(?:asm|__asm__)\s*(?:volatile\s*|__volatile__\s*)?\(')
NAKED = re.compile(r'\b__attribute(?:__)?\s*\(\(\s*(?:__)?naked(?:__)?\s*\)\)')
REG_NAME = re.compile(r'(?:r\d+|sp|lr|pc|ip|fp|sb|sl)$')


def skip_string(s, i):
    q = s[i]
    i += 1
    while i < len(s) and s[i] != q:
        i += 2 if s[i] == "\\" else 1
    return i + 1


def skip_balanced(s, i, open_ch, close_ch):
    """i at open_ch; returns the index just past the matching close_ch."""
    depth = 0
    while i < len(s):
        c = s[i]
        if c in "\"'":
            i = skip_string(s, i)
            continue
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return i


def skip_ws(s, i):
    while i < len(s) and s[i] in " \t\r\n":
        i += 1
    return i


def align_structs(s):
    out, last, i, n = [], 0, 0, len(s)
    while i < n:
        c = s[i]
        if c in "\"'":
            i = skip_string(s, i)
            continue
        if c == "#" and (i == 0 or s[i - 1] == "\n"):  # line markers
            j = s.find("\n", i)
            i = n if j < 0 else j
            continue
        m = WORD.match(s, i)
        if not m:
            i += 1
            continue
        word = m.group(0)
        if i > 0 and (s[i - 1].isalnum() or s[i - 1] == "_"):
            i = m.end()
            continue
        if word not in ("struct", "union"):
            i = m.end()
            continue
        # struct [attrs] [tag] [attrs] {
        j = skip_ws(s, m.end())
        packed = False
        while True:
            if s.startswith("__attribute__", j):
                k = skip_ws(s, j + len("__attribute__"))
                end = skip_balanced(s, k, "(", ")")
                if "packed" in s[k:end]:
                    packed = True
                j = skip_ws(s, end)
                continue
            m2 = WORD.match(s, j)
            if m2 and m2.group(0) not in ("struct", "union"):
                j = skip_ws(s, m2.end())
                continue
            break
        if j < n and s[j] == "{":
            end = skip_balanced(s, j, "{", "}")
            k = skip_ws(s, end)
            if s.startswith("__attribute__", k):
                k2 = skip_ws(s, k + len("__attribute__"))
                if "packed" in s[k2:skip_balanced(s, k2, "(", ")")]:
                    packed = True
            if not packed:
                out.append(s[last:m.end()])
                out.append(ATTR_PACK_ALIGN if only_bitfields(s[j + 1:end - 1]) else ATTR_ALIGN)
                last = m.end()
            i = j + 1  # nested definitions are visited too
            continue
        i = j
    out.append(s[last:])
    return "".join(out)


def scrub_asm(s):
    out, last = [], 0
    depth, pos = 0, 0  # brace depth at pos
    for m in ASM.finditer(s):
        if m.start() < last:
            continue
        while pos < m.start():
            c = s[pos]
            if c in "\"'":
                pos = skip_string(s, pos)
                continue
            depth += (c == "{") - (c == "}")
            pos += 1
        end = skip_balanced(s, m.end() - 1, "(", ")")
        pos = end
        inner = s[m.end():end - 1]
        tmpl = re.match(r'\s*((?:"(?:[^"\\]|\\.)*"\s*)*)', inner)
        body = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', tmpl.group(1)))
        rest = inner[tmpl.end():].strip()
        before = s[max(0, m.start() - 64):m.start()].rstrip()
        if depth == 0:
            # file-scope asm (pokeruby: .space padding, .set aliases, .include):
            # the bridge places every global at its ELF address anyway
            repl = ""
            end = skip_ws(s, end)
            if s[end:end + 1] == ";":
                end += 1
            pos = end
        elif not body.strip():
            repl = "((void)0)"
        elif not rest and REG_NAME.match(body.strip()) and before[-1:].isalnum():
            repl = ""  # register variable binding
        else:
            repl = "__builtin_trap()"
        out.append(s[last:m.start()])
        out.append(repl)
        last = end
    out.append(s[last:])
    return NAKED.sub("", "".join(out))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    s = open(sys.argv[1], encoding="latin-1").read()
    s = align_structs(scrub_asm(s))
    open(sys.argv[2], "w", encoding="latin-1").write(s)


if __name__ == "__main__":
    main()
