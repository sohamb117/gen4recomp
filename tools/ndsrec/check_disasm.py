#!/usr/bin/env python3
"""
Check ndsrec's disassembler against Diamond's own assembly.

    check_disasm.py ROM.nds DECOMP_ROOT XMAP [--show N]

pokediamond's arm9/asm and arm9/overlays/*/asm hold ~22,000 functions in the
divided syntax armrec reads. For every instruction they list, this decodes
the same bytes out of the ROM with tools/ndsrec/disasm.py and compares the
two after normalising what is spelling rather than meaning: register
aliases, hex against decimal, register-list ranges, a zero offset, the
position of a condition in an LDM/STM or sized load mnemonic, and symbolic
operands (each side's label or symbol resolved to its address through the
ROM link map). Prints per-module counts and the first mismatches.
"""
import argparse
import collections
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import disasm  # noqa: E402
import nds  # noqa: E402
import xmap as xmapmod  # noqa: E402

ALIAS = {"sb": "r9", "sl": "r10", "fp": "r11", "ip": "r12", "r13": "sp",
         "r14": "lr", "r15": "pc"}
REGN = {n: i for i, n in enumerate(disasm.REGS)}
REGN.update({"sb": 9, "sl": 10, "fp": 11, "ip": 12, "r13": 13, "r14": 14,
             "r15": 15})
COND = set(c for c in disasm.CONDS if c) | {"hs", "lo"}
CANON_COND = {"hs": "cs", "lo": "cc"}
LDM_ALIAS = {"ldm": {"fd": "ia", "ed": "ib", "fa": "da", "ea": "db"},
             "stm": {"ea": "ia", "fa": "ib", "ed": "da", "fd": "db"}}


def canon_mnem(m):
    m = m.lower()
    mm = re.match(r"^(ldm|stm)(..)?(..)?$", m)
    if mm and m not in ("ldm", "stm"):
        a, b = mm.group(2) or "", mm.group(3) or ""
        if a in COND or a in CANON_COND:
            cond, mode = a, b
        else:
            mode, cond = a, b
        mode = LDM_ALIAS[mm.group(1)].get(mode, mode) or "ia"
        return mm.group(1) + CANON_COND.get(cond, cond) + mode
    mm = re.match(r"^(ldr|str)(.*)$", m)
    if mm:
        rest = mm.group(2)
        cond = ""
        for c in sorted(COND, key=len, reverse=True):
            if rest.startswith(c) and rest[len(c):] in ("", "b", "h", "sb", "sh", "d"):
                cond, rest = c, rest[len(c):]
                break
            if rest.endswith(c) and rest[:-len(c)] in ("b", "h", "sb", "sh", "d"):
                cond, rest = c, rest[:-len(c)]
                break
        return mm.group(1) + CANON_COND.get(cond, cond) + rest
    for c in ("hs", "lo"):
        if m.endswith(c) and len(m) > 2 and m[:-2] in ("b", "bl", "bx", "mov", "add", "sub",
                                                       "and", "orr", "ldr", "str", "cmp"):
            return m[:-2] + CANON_COND[c]
    return m


def canon_ops(ops, resolve):
    ops = ops.strip().lower()
    # register lists
    def rl(mo):
        regs = set()
        for part in mo.group(1).split(","):
            part = part.strip()
            if not part:
                continue
            if "-" in part:
                a, b = [p.strip() for p in part.split("-")]
                for i in range(REGN[a], REGN[b] + 1):
                    regs.add(i)
            else:
                regs.add(REGN[part])
        return "{" + ",".join(str(r) for r in sorted(regs)) + "}"
    ops = re.sub(r"\{([^}]*)\}", rl, ops)
    toks = re.split(r"(\W)", ops)
    out = []
    for t in toks:
        if t in ALIAS:
            t = ALIAS[t]
        out.append(t)
    ops = "".join(out)
    # symbolic operands
    def sym(mo):
        name = mo.group(0)
        if name in REGN or re.match(r"^(r\d+|p\d+|c\d+|cpsr\w*|spsr\w*|lsl|lsr|asr|ror|rrx|sp|lr|pc|0x[0-9a-f]+)$", name):
            return name
        a = resolve(name)
        return "@%x" % a if a is not None else "?" + name
    ops = re.sub(r"(?<![#\w])[A-Za-z_.$][\w.$]*", sym, ops)
    # numbers
    ops = re.sub(r"#\s*(-?)(0x[0-9a-f]+|\d+)",
                 lambda mo: "#%d" % (int(mo.group(2), 0) * (-1 if mo.group(1) else 1)), ops)
    ops = re.sub(r"(?<![\w#@])(0x[0-9a-f]+)", lambda mo: str(int(mo.group(1), 16)), ops)
    ops = re.sub(r",\s*#0\]", "]", ops)
    ops = re.sub(r"#-0\]", "]", ops)
    ops = re.sub(r"\s+", "", ops)
    return ops


def norm(text, resolve):
    text = text.split(";")[0].strip()
    parts = text.split(None, 1)
    m = canon_mnem(parts[0])
    ops = canon_ops(parts[1], resolve) if len(parts) > 1 else ""
    if m == "nop":
        return "nop"
    if m == "blx" and ops.startswith("@"):
        m = "bl"
    if m == "swi":
        ops = ops.lstrip("#")
    # thumb "mov r8, r8" is nop; ARM "mov r0, r0" also
    return m + " " + ops


LABEL_RE = re.compile(r"^([A-Za-z_.$][\w.$]*):\s*(.*)$")
ADDR_C = re.compile(r";\s*0x([0-9A-Fa-f]+)")


def walk_asm(path, defines=("DIAMOND", "ENGLISH")):
    """Yield (addr, thumb, text) for every instruction a .s lists."""
    loc = None
    thumb = False
    section = ".text"
    skip = []
    labels = {}
    insns = []
    for raw in open(path, errors="replace"):
        line = raw.rstrip("\n")
        cmt = ""
        if ";" in line:
            line, cmt = line.split(";", 1)
            cmt = ";" + cmt
        code = line.strip()
        low = code.lower()
        if low.startswith(".ifdef") or low.startswith(".ifndef"):
            sym = code.split(None, 1)[1].strip()
            live = (sym in defines) if low.startswith(".ifdef") else (sym not in defines)
            skip.append(not live)
            continue
        if low.startswith(".else"):
            skip[-1] = not skip[-1]
            continue
        if low.startswith(".endif"):
            skip.pop()
            continue
        if any(skip) or not code:
            continue
        while True:
            m = LABEL_RE.match(code)
            if not m:
                break
            name, code = m.group(1), m.group(2).strip()
            am = ADDR_C.search(cmt)
            if am and section == ".text":
                loc = int(am.group(1), 16)
            if loc is not None and section == ".text":
                labels[name] = loc
        if not code:
            continue
        head = code.split(None, 1)[0].lower()
        rest = code.split(None, 1)[1] if len(code.split(None, 1)) > 1 else ""
        if head in ("thumb_func_start", "non_word_aligned_thumb_func_start"):
            thumb = True
            section = ".text"
            continue
        if head in ("arm_func_start", "local_arm_func_start"):
            thumb = False
            section = ".text"
            continue
        if head in ("arm_func_end", "thumb_func_end", "exception"):
            continue
        if head.startswith("."):
            d = head[1:]
            if d == "text":
                section = ".text"
            elif d in ("data", "rodata", "bss"):
                section = "." + d
            elif d == "section":
                section = rest.split(",")[0].strip()
            elif d == "thumb":
                thumb = True
            elif d == "arm":
                thumb = False
            elif d in ("balign", "align") and loc is not None and section == ".text":
                n = int(rest.split(",")[0].strip(), 0)
                if d == "align":
                    n = 1 << n
                loc = (loc + n - 1) & ~(n - 1)
            elif loc is not None and section == ".text":
                if d in ("word", "long", "int"):
                    loc += 4 * (rest.count(",") + 1)
                elif d in ("short", "hword"):
                    loc += 2 * (rest.count(",") + 1)
                elif d == "byte":
                    loc += rest.count(",") + 1
                elif d in ("space", "skip"):
                    loc += int(rest.split(",")[0], 0)
                elif d in ("include", "global", "extern", "public", "type", "size"):
                    pass
                else:
                    loc = None
            continue
        if section != ".text" or loc is None:
            continue
        insns.append((loc, thumb, code))
        if thumb:
            loc += 4 if head in ("bl", "blx") and rest.strip() not in REGN else 2
        else:
            loc += 4
    return insns, labels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("root")
    ap.add_argument("xmap")
    ap.add_argument("--show", type=int, default=25)
    a = ap.parse_args()
    rom = nds.Rom(a.rom)
    xm = xmapmod.XMap(a.xmap)
    names = {k.lower(): v for k, v in xm.name_addrs().items()}
    files = sorted(glob.glob(os.path.join(a.root, "arm9/asm/*.s")) +
                   glob.glob(os.path.join(a.root, "arm9/overlays/*/asm/*.s")))
    stats = collections.Counter()
    bad = collections.Counter()
    shown = 0
    for f in files:
        ovm = re.search(r"overlays/(\d+)/", f)
        module = "ov%03d" % int(ovm.group(1)) if ovm else "arm9"
        seg = rom.module_segment(module)
        insns, labels = walk_asm(f)
        labels = {k.lower(): v for k, v in labels.items()}

        def resolve(name, labels=labels):
            if name in labels:
                return labels[name]
            m = re.match(r"^(?:_|sub_|ov\d+_|FUN_)([0-9a-f]{8})$", name)
            if m:
                return int(m.group(1), 16)
            return names.get(name)
        for addr, thumb, text in insns:
            data, base = seg.lookup(addr)
            if data is None:
                stats["unmapped"] += 1
                continue
            ins = disasm.decode(data, addr - base, addr, thumb)
            mine = ins.text
            if "{T}" in mine:
                t = ins.target if ins.target is not None else (
                    ins.lit if ins.lit is not None else ins.adr)
                if ins.kind == "call":
                    t &= ~1
                mine = mine.replace("{T}", "_%08x" % t)
            n1 = norm(text, resolve)
            n2 = norm(mine, lambda n: int(n[1:], 16) if re.match(r"^_[0-9a-f]{8}$", n) else None)
            stats[module] += 1
            if n1 != n2:
                key = n1.split()[0]
                bad[key] += 1
                stats["mismatch"] += 1
                if shown < a.show:
                    shown += 1
                    print("%s %08X %s\n   asm:  %-40s %s\n   mine: %-40s %s" % (
                        module, addr, "T" if thumb else "A", text, n1, mine, n2))
    total = sum(v for k, v in stats.items() if k.startswith(("arm9", "ov")))
    print("instructions compared %d, mismatches %d (%.4f%%), unmapped %d" % (
        total, stats["mismatch"], 100.0 * stats["mismatch"] / max(1, total),
        stats["unmapped"]))
    print("mismatch by asm mnemonic:", bad.most_common(30))


if __name__ == "__main__":
    main()
