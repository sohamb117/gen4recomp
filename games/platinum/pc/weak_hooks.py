#!/usr/bin/env python3
"""The port's weak host hooks, and whether each one is bound.

tools/armrec/armrec_rt.c defines a set of host hooks weak, so a build that
supplies no platform layer still links: the weak body is a no-op or plain
memory. That is the arrangement that makes the runtime shared between this
port and pokediamond, and it has a hazard with it, a hook nobody wrote
Links silently. Next door two sat missing across three rounds of work while
the renderer covered for the gap, and nothing said so.

  $ python3 pc/weak_hooks.py                       # the ELF build
  $ python3 pc/weak_hooks.py --binary build/pc-arm/pokeplatinum \\
        --nm arm-linux-gnueabihf-nm --objects build/pc-arm/obj

What it fails on, and it is not "resolved to the stub". Twelve of the
twenty-four hooks are unbound in this tree and every one of them is correct:
eight are recompilation hooks (armrec_swi, the coprocessor four, the overlay
and decompiled-symbol registries) and Platinum is a port with no recompiled
ARM at all, and four are pokediamond's IPC and SPI models, whose strong
definitions live in ITS pc/src and whose callers are arm7/asm files this tree
does not have. Failing on those would be a red light that means "correct",
which is the kind of gate people learn to ignore.

So a hook is a FINDING when it is unbound and **the platform layer calls
it**; that is the shape of a hook someone forgot to write. Three cases are
told apart, and by RELOCATIONS rather than undefined symbols, because a call
from inside armrec_rt.c to its own weak default leaves no undefined symbol
anywhere and an nm-only check reads it as "nobody calls this":

  * called from an object other than armrec_rt.o, the platform layer or the
    game reaches it. Unbound here is the failure.
  * called only from armrec_rt.o; the runtime's own fallback path. Unbound
    is fine exactly when that path is unreachable, which is why these are
    reported rather than passed over in silence. The four IPC/SPI hooks are
    this: armrec_ipc_store and armrec_ipc_load call them, and nothing in this
    tree calls those.
  * never referenced at all, cannot hide anything.

Exit status is 0 when no unbound hook is called from outside the runtime.
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RT = os.path.join(ROOT, "tools", "armrec", "armrec_rt.c")

# `__attribute__((weak))` then, on that line or the next, the declarator. The
# name is the last identifier before the ( of a function, or before the = or ;
# of a variable.
WEAK = re.compile(r"__attribute__\(\(weak\)\)(.*?)(?:\(|=|;)", re.S)
NAME = re.compile(r"(\w+)\s*$")


def hooks():
    text = open(RT).read()
    out = []
    for m in WEAK.finditer(text):
        n = NAME.search(m.group(1).strip())
        if n:
            out.append(n.group(1))
    return sorted(set(out))


def nm_lines(nm, *paths):
    r = subprocess.run([nm] + list(paths), capture_output=True, text=True)
    return r.stdout.splitlines()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--binary", default=os.path.join(ROOT, "build", "pc", "pokeplatinum"))
    ap.add_argument("--objects", default=os.path.join(ROOT, "build", "pc", "obj"))
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--objdump", default="objdump")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    want = hooks()
    if not want:
        sys.exit("weak_hooks: found no weak hooks in %s; the parse is wrong"
                 % os.path.relpath(RT, ROOT))

    kind = {}
    for line in nm_lines(args.nm, args.binary):
        f = line.split()
        if len(f) >= 3 and f[2] in want:
            kind.setdefault(f[2], f[1])

    objs = []
    for dirpath, _dirs, files in os.walk(args.objects):
        objs += [os.path.join(dirpath, f) for f in files if f.endswith(".o")]
    outside, inside = set(), set()
    for o in objs:
        r = subprocess.run([args.objdump, "-r", o], capture_output=True, text=True)
        hit = {l.split()[-1] for l in r.stdout.splitlines()
               if l.split() and l.split()[-1] in want}
        if os.path.basename(o) == "armrec_rt.o":
            inside |= hit
        else:
            outside |= hit
    inside -= outside

    bad, runtime_only = [], []
    for h in want:
        k = kind.get(h, "?")
        stub = k in ("W", "w", "V", "v", "?")
        where = ("called by the platform layer" if h in outside else
                 "called only by armrec_rt" if h in inside else "unreferenced")
        if not args.quiet:
            print("  %-30s %-12s %s" % (h, "STUB" if stub else "bound(%s)" % k, where))
        if stub and h in outside:
            bad.append(h)
        elif stub and h in inside:
            runtime_only.append(h)

    nbound = sum(1 for h in want
                 if kind.get(h, "?") not in ("W", "w", "V", "v", "?"))
    print("weak_hooks: %d hook(s), %d bound, %d unbound (%d reached only by "
          "armrec_rt's own fallback, %d never referenced), %d UNBOUND AND "
          "CALLED BY THE PLATFORM LAYER"
          % (len(want), nbound, len(want) - nbound, len(runtime_only),
             len(want) - nbound - len(runtime_only) - len(bad), len(bad)))
    for h in bad:
        print("  %s is called outside armrec_rt.c and resolved to the weak "
              "default" % h)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
