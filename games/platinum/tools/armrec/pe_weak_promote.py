#!/usr/bin/env python3
"""
Make PE weak twins linkable, the Windows build's counterpart to ELF weak.

The defect this exists for, measured rather than read: GNU ld's PE backend
does not resolve a strong undefined reference against a *weak external* from
another object. On ELF a weak definition is a definition, so armrec's whole
override scheme, every recompiled function emitted weak, hand-decompiled C
winning by being strong, costs nothing. On PE the weak twin is an
IMAGE_SYM_CLASS_WEAK_EXTERNAL whose code hides behind a companion symbol gas
names `.weak.<sym>.<module>`, and a reference from another object simply does
not see it: `i686-w64-mingw32-gcc wref.c PXI_init.o` fails with `undefined
reference to PXI_Init` while the object visibly holds the code.

What this does. For every weak pair whose name has no strong definition
anywhere in the link (i.e. the recompiled twin that *would have won*) the
companion is renamed to the real name, which makes it an ordinary strong
definition. Where a strong definition exists (a decompiled override), the
pair is left alone: cross-object references bind to the strong symbol, and
the weak external's only remaining reader is its own object, which PE
resolves correctly. Where several objects define the same weak name, the
first one listed wins, which is the same tie ELF's linker breaks by order.

Idempotent: a promoted companion no longer matches `.weak.`, and a name with
a strong definition is skipped, so a second run over the same objects does
nothing.
"""

import argparse
import subprocess
import sys
import tempfile
import os

# Defined, global, not weak: the types a strong definition shows as. `A` is
# an absolute, armrec_data_syms.c's `.weak X; .set X, 0xADDR` labels and
# pc_arm7.c's linker values produce absolute companions.
STRONG = set("TDBRGSCA")

# What a promotable companion may look like. Diamond's companions are strong
# (nothing there weakens these objects); platinum's SDK pass runs
# `objcopy --weaken` over every SDK object, which weakens the companions too:
# A W-defined symbol still resolves cross-object once it carries the real
# name (measured: GNU ld binds strong-less references to W definitions), so
# a weak companion is promotable, it just stays weak.
COMPANION = STRONG | set("W")


def parse_nm(nm, objs):
    """{obj: [(type, name), ...]} over one batched nm invocation."""
    out = subprocess.run([nm] + objs, capture_output=True, text=True)
    per = {}
    cur = None
    for line in out.stdout.splitlines():
        if line.endswith(":") and not line.startswith(" "):
            cur = line[:-1]
            per.setdefault(cur, [])
            continue
        f = line.split()
        if len(f) == 3:
            per.setdefault(cur, []).append((f[1], f[2]))
        elif len(f) == 2:
            per.setdefault(cur, []).append((f[0], f[1]))
    return per


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--objcopy", default="objcopy")
    ap.add_argument("--ns-prefix", default=None,
                    help="these objects' symbols are about to be renamed "
                         "NAME -> <prefix>NAME (the ARM7 pass): skip a twin "
                         "whose *renamed* spelling is strongly defined in an "
                         "--also-scan object, or the rename would collide")
    ap.add_argument("--also-scan", action="append", default=[],
                    help="objects whose strong definitions count but which "
                         "are not rewritten")
    ap.add_argument("objs", nargs="+")
    args = ap.parse_args()

    per = parse_nm(args.nm, args.objs + args.also_scan)

    strong = set()
    for obj, syms in per.items():
        scan_only = obj in args.also_scan
        for t, name in syms:
            if t not in STRONG:
                continue
            # A scan-only object lives on the *other* side of the namespace
            # rename: only its already-prefixed names can collide with a twin
            # here. Its bare names are the other processor's and must not
            # suppress anything; the ARM9's PXI_SetFifoRecvCallback is not
            # the ARM7's.
            if scan_only and args.ns_prefix is not None and \
                    not name.startswith("_" + args.ns_prefix):
                continue
            strong.add(name)

    promoted = set()
    changed = 0
    for obj in args.objs:
        syms = per.get(obj, [])
        weaks = {name for t, name in syms if t in "wW"}
        renames = []
        for t, name in syms:
            if not name.startswith(".weak.") or t not in COMPANION:
                continue
            parts = name.split(".")
            if len(parts) < 4:
                continue
            base = parts[2]
            if t == "A":
                # An absolute companion is gas's `.weak X; .set x, v`, an
                # asm-level spelling of a C name, always bare however many
                # underscores the C name itself starts with, and sometimes
                # with no weak external beside it at all. The C references
                # that want it are `_X` on PE, unconditionally.
                final = "_" + base
            else:
                # A section companion is the compiler's own weak definition;
                # its weak external carries the already-decorated name (or
                # the arm7-namespaced spelling after the rename pass).
                target = None
                if base in weaks:
                    target = base
                else:
                    ns = "_arm7_" + base.lstrip("_")
                    if ns in weaks:
                        target = ns
                if target is None:
                    continue
                final = target if target.startswith("_") else "_" + target
            if final in strong or final in promoted:
                continue
            if args.ns_prefix is not None:
                ns = "_" + args.ns_prefix + final.lstrip("_")
                if ns in strong or ns in promoted:
                    continue
            promoted.add(final)
            renames.append((name, final))
        if not renames:
            continue
        with tempfile.NamedTemporaryFile("w", suffix=".syms",
                                         delete=False) as fh:
            for old, new in renames:
                fh.write("%s %s\n" % (old, new))
            tmp = fh.name
        rc = subprocess.call([args.objcopy, "--redefine-syms=" + tmp, obj])
        os.unlink(tmp)
        if rc != 0:
            print("pe_weak_promote: objcopy failed on %s" % obj,
                  file=sys.stderr)
            return 1
        changed += len(renames)

    print(changed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
