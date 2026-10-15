#!/usr/bin/env python3
"""pc/tools/dp_ovlabel_lint.py: code calling another overlay by the wrong overlay's label.

    dp_ovlabel_lint.py [--allow FILE] SRC...
    dp_ovlabel_lint.py [--allow FILE] --xmap XMAP [--ovmap FILE] SRC...

SRC is the assembly armrec translates and the decompiled C the build
compiles (files, or directories searched for *.s and *.c), patched copies
included: a C file under a `prep/` directory (pc/mk/game.mk's
$(BUILD)/prep/<path under arm9>, patched and its mwcc asm bodies stripped:
those are checked in their extracted .s) stands for arm9/<path>. Exits 1 on a call
not named in the allow file (`<caller> <callee>` per line, the caller an
overlay number or a static file's name, e.g. `11 ov07_02211E60`, `#`
comments: each entry says why the named overlay is the resident one).

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
SDK_OVERLAY_OVERLAY_KK_ID (asm) or FS_OVERLAY_ID(OVERLAY_KK) (C). The call
is reported when one of those, KK, has its own function at X too, unless
the file names NN's own id (it loads NN itself). Such a caller has both
overlays resident at different times, and only the code's context says
which one X means.

In C (one function per top-level definition, bodies indented, as the
decompiled sources are), a top-level line naming ovNN_X( without a closing
`;` defines it; any ovNN_X on an indented line is a use (a call or a
function pointer, both bound by name on wasm): the Poketch digital watch
(ov21) called pokediamond's ov11_02252DB4, a battle overlay label, for the
resident ov20_02252DB4, and ran battle code on a tap on the watch.

With --xmap (HG/SS, built with armrec --overlay-dispatch), the ROM link map
is the source of the overlays and their functions instead of the ovNN_X
names: pokeheartgold names many overlay functions (PokedexApp_MainSeq_19),
and its overlays are main.lsf's, not directories. Every `.text` symbol the
xMAP places between an overlay's SDK_OVERLAY.<name>.START and .END is that
overlay's function at that address; FS_OVERLAY_ID(<name>) (C, and the
assembly's `.word FS_OVERLAY_ID(OVY_18)`) and SDK_OVERLAY_<name>_ID (asm)
are loads of the overlay the map numbers <name>. A file's own overlay is
its path's overlays/NN/ (the staged assembly, the extracted asm bodies) or
its line in --ovmap (`path ovl`, `-` static: the decompiled C,
pc/tools/hg_lsf.py c, matched as a path suffix). What is checked is what
still binds by name:

  * C naming another overlay's function on an indented line (a direct
    call, or a function pointer: wasm-ld binds both by name);
  * assembly `.word X` (a literal pool's `ldr =X`, a table) where X is
    decompiled C of another overlay: armrec stores armrec_ext_X, the C
    function's host pointer, so a call through it reaches X whichever
    overlay is resident;
  * an assembly bl/blx armrec does not dispatch (armrec.py
    overlay_dispatch_addr: the named overlay overlaps the caller's own).
    Every other cross-overlay call into an address another overlay can
    occupy is armrec_dispatch() of the address, resolved by residency.

The rule is the one above, with rivals limited to overlays that can be
resident beside the referencing file's own (their windows do not overlap
it).
"""
import collections, os, re, sys

START = re.compile(r"^\s*(?:arm|thumb|non_word_aligned_thumb)_func_start\s+(ov(\d+)_([0-9A-Fa-f]{8}))\b")
CALL = re.compile(r"^\s*blx?\s+(ov(\d+)_([0-9A-Fa-f]{8}))\s*(?:[;@].*)?$")
OVID = re.compile(r"\bSDK_OVERLAY_OVERLAY_(\d+)_ID\b")
OVDIR = re.compile(r"(?:^|/)overlays/(\d+)/")
C_DEF = re.compile(r"^(?!extern\b)[A-Za-z_][^;]*\b(ov(\d+)_([0-9A-Fa-f]{8}))\s*\([^;]*$")
C_REF = re.compile(r"\b(ov(\d+)_([0-9A-Fa-f]{8}))\b")
C_OVID = re.compile(r"\bFS_OVERLAY_ID\s*\(\s*OVERLAY_(\d+)\s*\)")
C_COMMENT = re.compile(r"//.*|/\*.*?\*/")

XMAP_OVL = re.compile(r"^#>([0-9A-Fa-f]{8})\s+SDK_OVERLAY\.(\w+)\.(START|END|ID) \(linker command file\)")
XMAP_SYM = re.compile(r"^\s+([0-9A-Fa-f]{8}) [0-9A-Fa-f]{8} (\.\w+)\s+(\S+)\t\(([^)]+)\)\s*$")
ANY_START = re.compile(r"^\s*(?:arm|thumb|non_word_aligned_thumb)_func_start\s+(\w+)")
ANY_CALL = re.compile(r"^\s*blx?\s+([A-Za-z_]\w*)\s*(?:[;@].*)?$")
WORD = re.compile(r"\.word\s+([A-Za-z_]\w*)")
IDENT = re.compile(r"\b[A-Za-z_]\w*\b")
C_OVNAME = re.compile(r"\bFS_OVERLAY_ID\s*\(\s*(\w+)\s*\)")
S_OVNAME = re.compile(r"\bSDK_OVERLAY_(\w+)_ID\b")


def files(args):
    for a in args:
        if os.path.isdir(a):
            for root, _, names in os.walk(a):
                for n in sorted(names):
                    if n.endswith((".s", ".c")):
                        yield os.path.join(root, n)
        elif a.endswith((".s", ".c")):
            yield a


def rel_of(p):
    """The path from arm9/ on, which a patched copy shares with its original."""
    j = p.rfind("/prep/")
    if p.endswith(".c") and j >= 0:
        return "arm9/" + p[j + len("/prep/"):]
    i = p.rfind("arm9/")
    return p[i:] if i >= 0 else p


def load_xmap(path):
    """({name: [(overlay, addr, object)]} for the overlays' .text symbols,
    {overlay: (start, end)}, {overlay name: id})."""
    syms, ranges, ids = collections.defaultdict(list), {}, {}
    cur = None
    for line in open(path, errors="replace"):
        m = XMAP_OVL.match(line)
        if m:
            name, val = m.group(2), int(m.group(1), 16)
            if m.group(3) == "ID":
                ids[name] = val
            elif m.group(3) == "START":
                cur = name
                ranges[name] = [val, val]
            else:
                ranges[name][1] = val
                cur = None
            continue
        if cur is None:
            continue
        m = XMAP_SYM.match(line)
        if m and m.group(2) == ".text" and m.group(3) != ".text" and not m.group(3).startswith(("$", ".")):
            syms[m.group(3)].append((cur, int(m.group(1), 16) & ~1, m.group(4)))
    syms = {n: [(ids[o], a, obj) for o, a, obj in v] for n, v in syms.items()}
    return syms, {ids[o]: tuple(r) for o, r in ranges.items()}, ids


def main_xmap(allow, xmap, ovmap_path, args):
    """The --xmap mode (see the module docstring)."""
    syms, ranges, ids = load_xmap(xmap)
    funcs_at = collections.defaultdict(set)
    for n, hits in syms.items():
        for o, a, _obj in hits:
            funcs_at[a].add(o)
    ovmap = {}
    if ovmap_path:
        for line in open(ovmap_path):
            f = line.split()
            if len(f) == 2:
                ovmap[f[0]] = None if f[1] == "-" else int(f[1])

    def overlaps(a, b):
        if a is None or b is None or a not in ranges or b not in ranges:
            return False
        (s1, e1), (s2, e2) = ranges[a], ranges[b]
        return s1 < e2 and s2 < e1

    def own_of(p):
        m = OVDIR.search(p)
        if m:
            return int(m.group(1))
        parts = p.split("/")
        for i in range(len(parts)):
            k = "/".join(parts[i:])
            if k in ovmap:
                return ovmap[k]
        return None

    def resolve(name, own):
        """(overlay, addr) of another overlay's function `name`, or None."""
        hits = syms.get(name)
        if not hits or any(o == own for o, _a, _obj in hits):
            return None
        if len(set(a for _o, a, _obj in hits)) != 1:
            return None
        return hits[0][0], hits[0][1]

    def dispatched(ov, addr, own):
        """Whether armrec --overlay-dispatch turns a call into armrec_dispatch()
        (armrec.py overlay_dispatch_addr)."""
        if overlaps(ov, own):
            return False
        return any(k not in (ov, own) and s <= addr < e and not overlaps(k, own)
                   for k, (s, e) in ranges.items())

    paths = sorted(set(files(args)))
    asm_defined = set()
    c_objects = set()
    for p in paths:
        if p.endswith(".s"):
            for raw in open(p, errors="replace"):
                m = ANY_START.match(raw)
                if m:
                    asm_defined.add(m.group(1))
        else:
            c_objects.add(os.path.basename(p)[:-2] + ".o")

    def is_c(name):
        return name not in asm_defined and any(obj in c_objects for _o, _a, obj in syms[name])

    refs = []                                # (caller, path, line, name, ov, addr, kind)
    uses = collections.defaultdict(set)
    loads = collections.defaultdict(set)
    for p in paths:
        own = own_of(p)
        caller = str(own) if own is not None else os.path.basename(p)
        is_c_file = p.endswith(".c")
        for n, raw in enumerate(open(p, errors="replace"), 1):
            if is_c_file:
                line = C_COMMENT.sub("", raw)
                loads[p].update(ids[x] for x in C_OVNAME.findall(line) if x in ids)
                if not line[:1].isspace():
                    continue
                found = [(x, "C") for x in IDENT.findall(line)]
            else:
                code = raw.split(";")[0].split("@")[0]
                loads[p].update(ids[x] for x in S_OVNAME.findall(code) + C_OVNAME.findall(code)
                                if x in ids)
                m = ANY_CALL.match(code)
                found = [(m.group(1), "call")] if m else []
                found += [(x, "word") for x in WORD.findall(code)]
            for name, kind in found:
                r = resolve(name, own)
                if r is None:
                    continue
                ov, addr = r
                uses[caller].add(ov)
                if kind == "call" and dispatched(ov, addr, own):
                    continue
                if kind == "word" and not is_c(name):
                    continue
                refs.append((caller, p, n, name, ov, addr, kind))

    bad = 0
    seen = set()
    for caller, p, n, name, ov, addr, kind in refs:
        if ov in loads[p] or (caller, name) in allow or (p, n, name) in seen:
            continue
        seen.add((p, n, name))
        own = int(caller) if caller.isdigit() else None
        rivals = sorted(k for k in funcs_at[addr] & (uses[caller] | loads[p])
                        if k not in (ov, own) and not overlaps(k, own))
        if not rivals:
            continue
        print("%s:%d: %s %s %s (overlay %d), and also uses overlay%s %s, which %s a "
              "function at 0x%08X too" % (
                  p, n, "overlay " + caller if own is not None else caller,
                  {"C": "names", "word": "stores", "call": "calls"}[kind], name, ov,
                  "s" if len(rivals) > 1 else "", ", ".join(map(str, rivals)),
                  "have" if len(rivals) > 1 else "has", addr))
        bad += 1
    print("  OVLABEL %d by-name references to another overlay that may not be the resident one "
          "(%d cross-overlay references checked)" % (bad, len(refs)))
    return 1 if bad else 0



def main(argv):
    allow = set()
    if len(argv) > 1 and argv[0] == "--allow":
        for line in open(argv[1]):
            line = line.split("#")[0].split()
            if len(line) == 2:
                allow.add((line[0], line[1]))
        argv = argv[2:]
    if len(argv) > 1 and argv[0] == "--xmap":
        ovmap = None
        if len(argv) > 3 and argv[2] == "--ovmap":
            ovmap, argv = argv[3], argv[:2] + argv[4:]
        return main_xmap(allow, argv[1], ovmap, argv[2:])

    # The patched copy of a file replaces the pristine one: key by the
    # path from arm9/ on.
    by_rel = {}
    for p in files(argv):
        by_rel[rel_of(p)] = p
    funcs_at = collections.defaultdict(set)  # address -> overlays with a function there
    calls = []                               # (caller, rel, callee, ov, addr, line)
    uses = collections.defaultdict(set)      # caller -> overlays it calls into
    loads = collections.defaultdict(set)     # rel -> overlay ids the file names
    for rel, p in sorted(by_rel.items()):
        m = OVDIR.search(rel)
        own = int(m.group(1)) if m else None
        caller = str(own) if m else os.path.basename(rel)
        is_c = rel.endswith(".c")
        for n, raw in enumerate(open(p, errors="replace"), 1):
            if is_c:
                line = C_COMMENT.sub("", raw)
                loads[rel].update(int(x) for x in C_OVID.findall(line))
                if not line[:1].isspace():
                    m = C_DEF.match(line)
                    if m:
                        funcs_at[m.group(3).upper()].add(int(m.group(2)))
                    continue
                refs = C_REF.findall(line)
            else:
                loads[rel].update(int(x) for x in OVID.findall(raw.split(";")[0]))
                m = START.match(raw)
                if m:
                    funcs_at[m.group(3).upper()].add(int(m.group(2)))
                    continue
                m = CALL.match(raw)
                refs = [m.groups()] if m else []
            for name, ov, addr in refs:
                if int(ov) != own:
                    uses[caller].add(int(ov))
                    calls.append((caller, rel, name, int(ov), addr.upper(), n))

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
