#!/usr/bin/env python3
"""Print a milestone's steps as the JSON list tools/pt_explore.py --steps takes, to replay part of a milestone from a
save made part-way through it.

    python3 tests/e2e/tools/ms_steps.py tests/e2e/platinum/49-route223-victory-road [FROM [TO]] [--list]

FROM/TO: 0-based step indexes, TO exclusive (default: to the end). --list prints each step's index, `do` and target.
"""
import json
import os
import sys
import tomllib


def main():
    args = [a for a in sys.argv[1:] if a != "--list"]
    if not args:
        sys.exit(__doc__)
    path = args[0]
    if os.path.isdir(path):
        path = os.path.join(path, "milestone.toml")
    with open(path, "rb") as f:
        steps = tomllib.load(f).get("step", [])
    lo = int(args[1]) if len(args) > 1 else 0
    hi = int(args[2]) if len(args) > 2 else len(steps)
    if "--list" in sys.argv:
        for i, st in enumerate(steps):
            if lo <= i < hi:
                tgt = st.get("map") or (st.get("x"), st.get("z")) if "x" in st or "map" in st else st.get("dirs", "")
                print("%3d %-10s %s" % (i, st["do"], tgt))
        return
    print(json.dumps(steps[lo:hi]))


if __name__ == "__main__":
    main()
