#!/usr/bin/env python3
"""Mark proven milestones passing: the status line goes, estimate = the measured frames, [run] frames = 1.5x
(rounded up to 100; a larger existing bound is kept with --keep).

    python3 tests/e2e/tools/ms_pass.py --game platinum 60-hm-cut=2182 61-hm-rock-smash=2180 ...
    python3 tests/e2e/tools/ms_pass.py --game platinum --report build/e2e/ptsys/a/report.md   # every PASS row
"""
import argparse
import math
import os
import re

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def mark(game, name, est, keep):
    p = os.path.join(ROOT, game, name, "milestone.toml")
    t = open(p).read()
    t = re.sub(r'^status = "planned"\n', "", t, flags=re.M)
    t = re.sub(r"^estimate = \d+", "estimate = %d" % est, t, count=1, flags=re.M)
    budget = int(math.ceil(est * 1.5 / 100.0) * 100)
    old = re.search(r"\[run\]\n(?:[^\[]*?\n)?frames = (\d+)", t)
    if keep and old and int(old.group(1)) > budget:
        budget = int(old.group(1))
    t = re.sub(r"(\[run\]\n(?:[^\[]*?\n)?)frames = \d+", lambda m: m.group(1) + "frames = %d" % budget, t, count=1)
    open(p, "w").write(t)
    print(name, est, budget)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--report")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("pairs", nargs="*")
    a = ap.parse_args()
    todo = [(n, int(f)) for n, f in (p.split("=") for p in a.pairs)]
    if a.report:
        for m in re.finditer(r"^\| (\S+) \| PASS \| (\d+) \|", open(a.report).read(), re.M):
            todo.append((m.group(1), int(m.group(2))))
    for name, est in todo:
        mark(a.game, name, est, a.keep)


if __name__ == "__main__":
    main()
