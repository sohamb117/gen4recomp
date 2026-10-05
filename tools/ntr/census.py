#!/usr/bin/env python3
"""Count functions still in assembly vs. decompiled C in a pret DS decomp.

    tools/ntr/census.py diamond|heartgold

Assembly: arm_func_start / thumb_func_start / non_word_aligned_thumb_func_start
markers in .s files. C: function definitions in .c files (a definition is a
top-level declarator followed by `{`); `asm` function bodies inside .c files
are reported separately because they are hand-written assembly too.
Units: arm9 static, each arm9 overlay, arm7.
  diamond:   by directory (arm9/{src,lib,asm,data}, arm9/overlays/<n>, arm7).
  heartgold: by link unit -- the objects main.lsf places in Static main and
             its autoloads, in each Overlay, and sub/ichneumon_sub.lsf's
             objects, each mapped back to its .c or .s source.
"""
import os
import re
import sys

GAMES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "games")
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


def count(name, sources):
    s = c = a = 0
    for p in sources:
        if p.endswith(".s"):
            s += asm_count(p)
        else:
            cc, aa = c_count(p)
            c += cc
            a += aa
    return name, s, c, a


def unit(name, dirs):
    return count(name, list(walk(dirs, ".s")) + list(walk(dirs, ".c")))


def diamond():
    root = os.path.join(GAMES, "diamond")
    arm9 = os.path.join(root, "arm9")
    rows = [unit("arm9 static", [os.path.join(arm9, d) for d in ("src", "lib", "asm", "data")])]
    ovl = os.path.join(arm9, "overlays")
    for d in sorted(os.listdir(ovl), key=lambda x: int(x) if x.isdigit() else 999):
        rows.append(unit(f"overlay {d}", [os.path.join(ovl, d)]))
    rows.append(unit("arm7", [os.path.join(root, "arm7")]))
    return rows, []


def lsf_units(path):
    """[(kind, name, [object paths])] in file order; autoloads fold into Static."""
    units, cur = [], None
    for line in open(path):
        words = line.split()
        if len(words) >= 2 and words[0] in ("Static", "Autoload", "Overlay"):
            if words[0] == "Autoload":
                cur = units[0]
            else:
                cur = (words[0], words[1], [])
                units.append(cur)
        elif len(words) >= 2 and words[0] == "Object" and cur is not None:
            if words[1] not in cur[2]:
                cur[2].append(words[1])
    return units


def heartgold():
    root = os.path.join(GAMES, "heartgold")
    unresolved = []
    # lib/dsprot links encrypted/encoded wrappers built from lib/dsprot/src/<stem>.c.
    dsprot = os.path.join(root, "lib", "dsprot", "src")
    dsprot_stems = sorted((f[:-2] for f in os.listdir(dsprot) if f.endswith(".c")), key=len, reverse=True)

    def sources(base, objs):
        out = []
        for o in objs:
            stem = os.path.join(base, o[:-2])
            src = next((stem + e for e in (".c", ".s") if os.path.isfile(stem + e)), None)
            if not src and o.startswith("lib/dsprot/"):
                name = os.path.basename(o[:-2])
                hit = next((s for s in dsprot_stems if name == s or name.startswith(s + "_")), None)
                src = os.path.join(dsprot, hit + ".c") if hit else None
            if src:
                if src not in out:
                    out.append(src)
            else:
                unresolved.append(os.path.relpath(os.path.join(base, o), root))
        return out

    rows = []
    for i, (kind, name, objs) in enumerate(lsf_units(os.path.join(root, "main.lsf"))):
        label = "arm9 static" if kind == "Static" else f"overlay {i - 1} {name}"
        rows.append(count(label, sources(root, objs)))
    sub = os.path.join(root, "sub")
    objs = [o for _, _, objs in lsf_units(os.path.join(sub, "ichneumon_sub.lsf")) for o in objs]
    rows.append(count("arm7", sources(sub, objs)))
    return rows, unresolved


def main():
    game = sys.argv[1] if len(sys.argv) == 2 else ""
    if game not in ("diamond", "heartgold"):
        sys.exit(__doc__)
    rows, unresolved = globals()[game]()

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
    if unresolved:
        out.write(f"\nobjects without a .c/.s source: {' '.join(unresolved)}\n")


if __name__ == "__main__":
    main()
