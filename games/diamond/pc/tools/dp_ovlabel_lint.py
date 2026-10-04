#!/usr/bin/env python3
"""pc/tools/dp_ovlabel_lint.py: assembly calling another overlay by the wrong overlay's label.

    dp_ovlabel_lint.py [--allow FILE] ASM...

ASM is the assembly armrec translates (files, or directories searched for
*.s), patched copies included. Exits 1 on a call not named in the allow file
(`<caller> <callee>` per line, the caller an overlay number or a static
file's name, e.g. `11 ov07_02211E60`, `#` comments: each entry says why the
named overlay is the resident one).

Overlays that load into the same window share addresses, and pokediamond
names a cross-overlay call target after whichever overlay's label it picked
at that address. The ROM does not care: `bl ov05_021EEF68` assembles to a
branch to 0x021EEF68, and whatever is loaded there runs. armrec resolves a
direct call by name, so the port calls the named overlay's function even
when another overlay is resident: the battle's catch sequence called the
field's ov05_021EEF68 instead of the Pokedex-entry screen's ov16_021EEF68
(the same address), its sprite task outlived the battle and drew into the
nickname screen's OAM.

Each direct call to ovNN_X from outside overlay NN is checked against the
overlays its caller also uses: those it calls into (all of an overlay's
files, or the one static file) and those its file loads by
SDK_OVERLAY_OVERLAY_KK_ID. The call is reported when one of those, KK, has
its own function at X too, unless the file names NN's own id (it loads NN
itself). Such a caller has both overlays resident at different times, and
only the code's context says which one X means.
"""
import collections, os, re, sys

START = re.compile(r"^\s*(?:arm|thumb|non_word_aligned_thumb)_func_start\s+(ov(\d+)_([0-9A-Fa-f]{8}))\b")
CALL = re.compile(r"^\s*blx?\s+(ov(\d+)_([0-9A-Fa-f]{8}))\s*(?:[;@].*)?$")
OVID = re.compile(r"\bSDK_OVERLAY_OVERLAY_(\d+)_ID\b")
OVDIR = re.compile(r"(?:^|/)overlays/(\d+)/")


def files(args):
    for a in args:
        if os.path.isdir(a):
            for root, _, names in os.walk(a):
                for n in sorted(names):
                    if n.endswith(".s"):
                        yield os.path.join(root, n)
        elif a.endswith(".s"):
            yield a


def main(argv):
    allow = set()
    if len(argv) > 1 and argv[0] == "--allow":
        for line in open(argv[1]):
            line = line.split("#")[0].split()
            if len(line) == 2:
                allow.add((line[0], line[1]))
        argv = argv[2:]

    # The patched copy of a file replaces the pristine one: key by the
    # path from arm9/ on.
    by_rel = {}
    for p in files(argv):
        i = p.rfind("arm9/")
        by_rel[p[i:] if i >= 0 else p] = p
    funcs_at = collections.defaultdict(set)  # address -> overlays with a function there
    calls = []                               # (caller, rel, callee, ov, addr, line)
    uses = collections.defaultdict(set)      # caller -> overlays it calls into
    loads = collections.defaultdict(set)     # rel -> overlay ids the file names
    for rel, p in sorted(by_rel.items()):
        m = OVDIR.search(rel)
        own = int(m.group(1)) if m else None
        caller = str(own) if m else os.path.basename(rel)
        for n, raw in enumerate(open(p, errors="replace"), 1):
            loads[rel].update(int(x) for x in OVID.findall(raw.split(";")[0]))
            m = START.match(raw)
            if m:
                funcs_at[m.group(3).upper()].add(int(m.group(2)))
                continue
            m = CALL.match(raw)
            if m and int(m.group(2)) != own:
                ov = int(m.group(2))
                uses[caller].add(ov)
                calls.append((caller, rel, m.group(1), ov, m.group(3).upper(), n))

    bad = 0
    for caller, rel, callee, ov, addr, n in calls:
        if ov in loads[rel] or (caller, callee) in allow:
            continue
        rivals = sorted(funcs_at[addr] & (uses[caller] | loads[rel]) - {ov})
        if caller.isdigit():
            rivals = [k for k in rivals if k != int(caller)]
        if not rivals:
            continue
        print("%s:%d: %s calls %s, and also uses overlay%s %s, which %s a "
              "function at 0x%s too" % (
                  rel, n, "overlay " + caller if caller.isdigit() else caller,
                  callee, "s" if len(rivals) > 1 else "",
                  ", ".join(map(str, rivals)),
                  "have" if len(rivals) > 1 else "has", addr))
        bad += 1
    if bad:
        print("dp_ovlabel_lint: %d call(s) name an overlay that may not be the "
              "resident one; patch the label (pc/patches/arm9/...s.patch) or "
              "list the call in the allow file with the reason" % bad)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
