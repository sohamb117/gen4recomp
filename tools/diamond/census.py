#!/usr/bin/env python3
"""Count functions still in assembly vs. decompiled C in games/diamond.

    tools/diamond/census.py

Assembly: arm_func_start / thumb_func_start / non_word_aligned_thumb_func_start
markers in .s files. C: function definitions in .c files (a definition is a
top-level declarator followed by `{`); `asm` function bodies inside .c files
are reported separately because they are hand-written assembly too.
Units: arm9 static (src, lib, asm, data), each arm9 overlay, arm7.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "games", "diamond")
ASM_MARK = re.compile(r"^\s*(arm_func_start|thumb_func_start|non_word_aligned_thumb_func_start)\b", re.M)
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
STRING = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'')
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do", "else"}


def asm_count(path):
    with open(path, errors="replace") as f:
        return len(ASM_MARK.findall(f.read()))


def c_count(path):
    """(C function definitions, asm-bodied definitions) at file scope."""
    with open(path, errors="replace") as f:
        src = f.read()
    src = STRING.sub('""', COMMENT.sub(" ", src))
    src = re.sub(r"^\s*#.*?(?<!\\)$", "", src, flags=re.M | re.S)
    c = a = depth = 0
    head = []
    i = 0
    while i < len(src):
        ch = src[i]
        if ch == "{":
            if depth == 0:
                text = "".join(head).strip()
                m = re.search(r"([A-Za-z_]\w*)\s*\([^;{}]*\)\s*$", text)
                if m and m.group(1) not in KEYWORDS and not re.search(r"=\s*$|\b(struct|union|enum)\s+\w*\s*$", text):
                    if re.search(r"(^|[\s;}])asm\b", text.rsplit(";", 1)[-1]):
                        a += 1
                    else:
                        c += 1
                head = []
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                head = []
        elif depth == 0:
            head.append(ch)
            if ch == ";":
                head = []
        i += 1
    return c, a


def walk(paths, ext):
    for base in paths:
        if os.path.isfile(base):
            if base.endswith(ext):
                yield base
            continue
        for d, _, files in os.walk(base):
            for f in sorted(files):
                if f.endswith(ext):
                    yield os.path.join(d, f)


def unit(name, dirs):
    s = sum(asm_count(p) for p in walk(dirs, ".s"))
    c = a = 0
    for p in walk(dirs, ".c"):
        cc, aa = c_count(p)
        c += cc
        a += aa
    return name, s, c, a


def main():
    arm9 = os.path.join(ROOT, "arm9")
    rows = [unit("arm9 static", [os.path.join(arm9, d) for d in ("src", "lib", "asm", "data")])]
    ovl = os.path.join(arm9, "overlays")
    for d in sorted(os.listdir(ovl), key=lambda x: int(x) if x.isdigit() else 999):
        rows.append(unit(f"overlay {d}", [os.path.join(ovl, d)]))
    rows.append(unit("arm7", [os.path.join(ROOT, "arm7")]))

    out = sys.stdout
    out.write("| unit | asm funcs (.s) | C funcs | asm-in-C funcs | C share |\n")
    out.write("|---|---:|---:|---:|---:|\n")
    tot = [0, 0, 0]
    ovt = [0, 0, 0]
    for name, s, c, a in rows:
        share = f"{100 * c / (s + c + a):.1f}%" if s + c + a else "-"
        out.write(f"| {name} | {s} | {c} | {a} | {share} |\n")
        for i, v in enumerate((s, c, a)):
            tot[i] += v
            if name.startswith("overlay"):
                ovt[i] += v
    for label, t in (("overlays total", ovt), ("total", tot)):
        out.write(f"| **{label}** | {t[0]} | {t[1]} | {t[2]} | {100 * t[1] / sum(t):.1f}% |\n")


if __name__ == "__main__":
    main()
