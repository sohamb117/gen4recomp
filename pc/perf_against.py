#!/usr/bin/env python3
"""Diff two perf_bench.py outputs over matching windows.

A costlier build reaches fewer frames in the same wall clock, so two run
TOTALS compare different scenes; these rows already measured fixed windows,
so what is left is to pair them and refuse the pairs that are not the same
scene any more. Rows pair on (scene, hd3d, threads, wide, pace) and only
compare when their station digests agree; a re-pinned station is a changed
scene, and a fps delta over a changed scene is noise wearing a number.

    $ python3 pc/perf_against.py old.tsv new.tsv
    $ python3 pc/perf_against.py --rig linux-3900x bench.tsv   # last two commits

With one TSV, rows split by commit (newest against the one before it),
filtered to --rig if given.
"""

import argparse
import sys


def read(path):
    rows = []
    with open(path) as f:
        header = f.readline().strip().split("\t")
        for line in f:
            cells = line.rstrip("\n").split("\t")
            if len(cells) == len(header):
                rows.append(dict(zip(header, cells)))
    return rows


def key(r):
    return (r["scene"], r["hd3d"], r["threads"], r["wide"], r["pace"])


def latest(rows, k):
    """The last row for a window; a re-run supersedes."""
    out = {}
    for r in rows:
        out[key(r)] = r
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old")
    ap.add_argument("new", nargs="?")
    ap.add_argument("--rig", help="only rows from this rig")
    args = ap.parse_args()

    if args.new:
        old, new = read(args.old), read(args.new)
    else:
        rows = read(args.old)
        if args.rig:
            rows = [r for r in rows if r["rig"] == args.rig]
        commits = []
        for r in rows:
            if r["commit"] not in commits:
                commits.append(r["commit"])
        if len(commits) < 2:
            raise SystemExit("only one commit in %s; nothing to compare"
                             % args.old)
        old = [r for r in rows if r["commit"] == commits[-2]]
        new = [r for r in rows if r["commit"] == commits[-1]]
        print("comparing %s -> %s" % (commits[-2], commits[-1]))

    if args.rig:
        old = [r for r in old if r["rig"] == args.rig]
        new = [r for r in new if r["rig"] == args.rig]

    a, b = latest(old, key), latest(new, key)
    shared = [k for k in a if k in b]
    if not shared:
        raise SystemExit("no matching windows")

    print("%-11s %-4s %-3s %-4s %-5s %9s %9s %8s  %s"
          % ("scene", "hd3d", "thr", "wide", "pace", "old fps", "new fps",
             "delta", "spans old->new (3d/2d ms)"))
    for k in sorted(shared):
        ra, rb = a[k], b[k]
        if ra["digest"] != rb["digest"]:
            print("%-11s %-4s %-3s %-4s %-5s   REFUSED: station re-pinned "
                  "(%s -> %s)" % (k + (ra["digest"], rb["digest"])))
            continue
        try:
            fa, fb = float(ra["fps"]), float(rb["fps"])
        except ValueError:
            continue
        delta = 100.0 * (fb - fa) / fa if fa else 0.0
        print("%-11s %-4s %-3s %-4s %-5s %9.1f %9.1f %+7.1f%%  "
              "%s/%s -> %s/%s"
              % (k[0], k[1], k[2], k[3], k[4], fa, fb, delta,
                 ra["ms3d"], ra["ms2d"], rb["ms3d"], rb["ms2d"]))


if __name__ == "__main__":
    main()
