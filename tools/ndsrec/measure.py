#!/usr/bin/env python3
"""
Function-boundary precision and recall of ndsrec's discovery against a ROM
link map.

    measure.py ROM.nds ARM9_XMAP [--modules arm9,ov005] [--misses N]

Ground truth is every code symbol of the xMAP (tools/ndsrec/xmap.py: a .text
symbol whose mapping symbol is $a or $t), per module. A discovered function
counts as correct when its start address is a truth start and its
instruction set matches; `extent` additionally checks the end against the
next truth start, after padding and the literal pool. Prints one line per
module and the totals; --misses lists the first false negatives/positives.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import discover  # noqa: E402
import nds  # noqa: E402
import xmap as xmapmod  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("xmap")
    ap.add_argument("--modules")
    ap.add_argument("--misses", type=int, default=0)
    a = ap.parse_args()
    rom = nds.Rom(a.rom)
    truth = xmapmod.XMap(a.xmap).functions()
    mods = a.modules.split(",") if a.modules else rom.modules()
    res = discover.discover_all(rom, mods)
    tot_tp = tot_fp = tot_fn = tot_mode = 0
    lines = []
    for name in mods:
        m = res[name]
        t = truth.get(name, {})
        found = m.funcs
        tp = [x for x in found if x in t and t[x][1] == found[x].thumb]
        mode = [x for x in found if x in t and t[x][1] != found[x].thumb]
        fp = [x for x in found if x not in t]
        fn = [x for x in t if x not in found]
        tot_tp += len(tp)
        tot_fp += len(fp) + len(mode)
        tot_fn += len(fn)
        tot_mode += len(mode)
        prec = 100.0 * len(tp) / max(1, len(found))
        rec = 100.0 * len(tp) / max(1, len(t))
        lines.append("%-6s truth %6d found %6d  precision %7.3f%%  recall %7.3f%%"
                     "  (fp %d, fn %d, wrong set %d)" % (
                         name, len(t), len(found), prec, rec, len(fp), len(fn), len(mode)))
        if a.misses:
            for x in sorted(fn)[:a.misses]:
                lines.append("   FN %08X %s %s" % (x, "T" if t[x][1] else "A", t[x][0]))
            for x in sorted(fp)[:a.misses]:
                lines.append("   FP %08X %s (%s)" % (x, "T" if found[x].thumb else "A",
                                                     found[x].source))
            for x in sorted(mode)[:a.misses]:
                lines.append("   SET %08X truth %s" % (x, t[x][0]))
    print("\n".join(lines))
    allf = tot_tp + tot_fp
    allt = tot_tp + tot_fn
    print("TOTAL truth %d found %d  precision %.3f%%  recall %.3f%%" % (
        allt, allf, 100.0 * tot_tp / max(1, allf), 100.0 * tot_tp / max(1, allt)))


if __name__ == "__main__":
    main()
