"""
Write discovered modules as armrec-ready assembly.

Layout under OUT (armrec reads overlay membership from the path):

    arm9/asm/ndsrec_arm9_NNN.s            static module, in chunks
    arm9/asm/ndsrec_autoload<K>_NNN.s     ITCM / DTCM autoload segments
    arm9/overlays/<id>/asm/ndsrec_ov<id>_NNN.s

Every byte of a segment is written: discovered functions as instructions,
everything else (literal pools, jump tables, padding, rodata, data) as
labelled `.byte` runs, so armrec's data blobs put the module's initialised
bytes at their guest addresses exactly as the ROM loader would. Every label
carries its `; 0x...` address. Names:

    sub_XXXXXXXX       function in the static module or an autoload
    ovNN_XXXXXXXX      function in overlay NN
    <symbol>           a function or data object named by the symbol file
                       (the host primitives and what the host references)
    _XXXXXXXX          label in the static; _ovNN_XXXXXXXX in overlay NN

A call whose target cannot be bound by name at build time -- into an
overlay other than the caller's own when more than one overlay could be
resident there, or outside every module -- is spelled
`bl armrec_dispatch_XXXXXXXX`, which armrec translates into a run-time
armrec_dispatch() of that guest address (resolved by residency, as the
hardware does).
"""
import bisect
import os
import struct

DYN = "armrec_dispatch_%08X"


def func_name(module, addr, names):
    n = names.get(addr)
    if n:
        return n
    if module == "arm9":
        return "sub_%08X" % addr
    if module == "arm7":
        return "arm7_%08X" % addr
    return "ov%02d_%08X" % (int(module[2:]), addr)


def label_name(module, addr):
    if module == "arm9":
        return "_%08X" % addr
    return "_%s_%08X" % (module, addr)


class Resolver(object):
    """Binds a call target to a symbol name, across modules."""

    def __init__(self, rom, modules, names):
        self.rom = rom
        self.mods = modules          # name -> discover.Module
        self.names = names           # module -> {addr: name}
        self.dyn = 0
        self.bound_cross = 0
        self.xlabels = {}            # module -> branch targets inside another function

    def overlay_range(self, name):
        ov = self.rom.overlay(int(name[2:]))
        return ov.ram, ov.ram + len(ov.data)

    def call(self, module, target):
        t = target & ~1
        m = self.mods[module]
        if t in m.funcs:
            return func_name(module, t, self.names.get(module, {}))
        if module != "arm9" and t in self.mods["arm9"].funcs:
            return func_name("arm9", t, self.names.get("arm9", {}))
        if module == "arm7":
            self.dyn += 1
            return DYN % t
        # another overlay: bind only when exactly one overlay that can be
        # resident beside the caller covers the address, and it has a
        # function there
        cands = []
        if module.startswith("ov"):
            lo, hi = self.overlay_range(module)
        else:
            lo = hi = None
        for ov in self.rom.overlays:
            name = "ov%03d" % ov.id
            if name == module:
                continue
            olo, ohi = ov.ram, ov.ram + len(ov.data)
            if not (olo <= t < ohi):
                continue
            if lo is not None and olo < hi and lo < ohi:
                continue
            cands.append(name)
        if len(cands) == 1 and cands[0] in self.mods and t in self.mods[cands[0]].funcs:
            self.bound_cross += 1
            return func_name(cands[0], t, self.names.get(cands[0], {}))
        self.dyn += 1
        return DYN % t


def chunks(seq, n):
    for i in range(0, len(seq), n):
        yield seq[i:i + n]


def emit_module(rom, module, mod, resolver, names, data_names, outdir,
                prims=None, per_file=300, stats=None):
    """Write one module. names: {addr: name} for functions of this module;
    data_names: {addr: (name, size, section)}; prims: {value: name} of words
    to spell symbolically in data (host primitive addresses, Thumb bit
    included). Returns the list of files written."""
    prims = prims or {}
    stats = stats if stats is not None else {}
    files = []
    if module == "arm9":
        segs = [("arm9", rom.static)] + [("autoload%d" % i, s)
                                         for i, s in enumerate(rom.autoloads)
                                         if s.data]
        subdir = os.path.join(outdir, "arm9", "asm")
    elif module == "arm7":
        from nds import Segment
        segs = [("arm7", Segment("arm7", rom.arm7_ram, rom.arm7))]
        subdir = os.path.join(outdir, "arm7", "asm")
    else:
        ov = rom.overlay(int(module[2:]))
        from nds import Segment
        segs = [(module, Segment(module, ov.ram, ov.data, ov.bss))]
        subdir = os.path.join(outdir, "arm9", "overlays", "%d" % ov.id, "asm")
    os.makedirs(subdir, exist_ok=True)

    lits = {}
    forced = set()
    for f in mod.funcs.values():
        for la, n in f.lits.items():
            lits[la] = max(lits.get(la, 0), n)
        forced |= f.data
    forced |= set(lits)

    # Branches into the middle of another function (mwcc and the runtime
    # library share code between entry points: _ll_udiv jumps into
    # _ull_mod's body). The target becomes a label in its owner and the
    # branch names that label; armrec then gives the branching function a
    # private copy of the code it enters (armrec.merge_multi_entry), which
    # keeps every register live where a dispatch would not. Both functions
    # must be in one file, so the chunking below never cuts between them.
    starts_all = sorted(mod.funcs)
    xlabels = set()
    xpairs = []
    for i, a0 in enumerate(starts_all):
        f = mod.funcs[a0]
        end = starts_all[i + 1] if i + 1 < len(starts_all) else a0 + 0x100000
        for ia, ins in f.insns.items():
            if ins.kind != "b" or ins.target is None or not (a0 <= ia < end):
                continue
            t = ins.target
            if a0 <= t < end or t in mod.funcs:
                continue
            j = bisect.bisect_right(starts_all, t) - 1
            if j < 0:
                continue
            g = mod.funcs[starts_all[j]]
            gend = starts_all[j + 1] if j + 1 < len(starts_all) else g.addr + 0x100000
            if t < gend and t in g.insns and g.thumb == f.thumb:
                xlabels.add(t)
                xpairs.append((min(ia, t), max(ia, t)))
    resolver.xlabels.setdefault(module, set()).update(xlabels)
    forced |= xlabels

    first_file = True
    for segname, seg in segs:
        starts = sorted(a for a in mod.funcs if seg.ram <= a < seg.end)
        # pieces: (start, end, func or None)
        pieces = []
        cur = seg.ram
        for i, a in enumerate(starts):
            if a > cur:
                pieces.append((cur, a, None))
            end = starts[i + 1] if i + 1 < len(starts) else seg.end
            pieces.append((a, end, mod.funcs[a]))
            cur = end
        if cur < seg.end:
            pieces.append((cur, seg.end, None))
        if not pieces:
            continue
        # group into files by function count
        groups = []
        g = []
        nf = 0
        for k, p in enumerate(pieces):
            g.append(p)
            if p[2] is not None:
                nf += 1
            if nf >= per_file:
                cut = pieces[k + 1][0] if k + 1 < len(pieces) else None
                if cut is not None and any(lo < cut <= hi for lo, hi in xpairs):
                    continue
                groups.append(g)
                g = []
                nf = 0
        if g:
            groups.append(g)
        stem = "ndsrec_%s" % (segname if module == "arm9" else module)
        for gi, grp in enumerate(groups):
            path = os.path.join(subdir, "%s_%03d.s" % (stem, gi))
            out = ["@ generated by tools/ndsrec from the ROM; do not commit",
                   "\t.section .text", ""]
            for (a, end, f) in grp:
                if f is None:
                    emit_data(out, module, seg, a, end, lits, data_names, prims, forced)
                else:
                    emit_func(out, module, seg, f, end, lits, resolver, names,
                              data_names, prims, stats, forced)
            if first_file:
                # objects in BSS, or anywhere no segment's bytes cover (the
                # DTCM's BSS after its 0x60 initialised bytes)
                bss = [(addr, v) for addr, v in data_names.items()
                       if v[2] == "bss" or not any(s.ram <= addr < s.end for _n, s in segs)]
                if bss:
                    # one section each: they are not contiguous, and armrec's
                    # location counter runs per section
                    for addr, (nm, size, _sec) in sorted(bss):
                        out.append("\t.section .bss.%s" % nm)
                        out.append("\t.global %s" % nm)
                        out.append("%s: ; 0x%08X" % (nm, addr))
                        out.append("\t.space 0x%x" % max(size, 4))
                first_file = False
            with open(path, "w") as fh:
                fh.write("\n".join(out) + "\n")
            files.append(path)
    return files


def resolve_operand(module, f, ins, end, resolver, names, local_targets):
    t = ins.target if ins.target is not None else (
        ins.lit if ins.lit is not None else ins.adr)
    if ins.lit is not None or ins.adr is not None:
        return label_name(module, t)
    if ins.kind == "call":
        ta = t & ~1
        if ins.addr in f.longbr and f.addr < ta < end and ta in f.insns:
            return label_name(module, ta)          # long branch
        return resolver.call(module, t)
    # branch
    if f.addr < t < end and t in f.insns:
        return label_name(module, t)
    if t in resolver.xlabels.get(module, ()):
        return label_name(module, t)
    if t == f.addr:
        return func_name(module, t, names)
    return resolver.call(module, t | (1 if f.thumb else 0))


def emit_func(out, module, seg, f, end, lits, resolver, names, data_names,
              prims, stats, forced):
    name = func_name(module, f.addr, names)
    # local branch targets
    targets = set()
    for ia, ins in f.insns.items():
        if not (f.addr <= ia < end):
            continue
        if ins.kind == "b" and ins.target is not None:
            targets.add(ins.target)
        if ia in f.longbr:
            targets.add(ins.target & ~1)
        for tg in f.jt.get(ia, []):
            targets.add(tg)
    long_br = set(t for t in targets)
    # thumb_func_start opens with `.balign 4` (armrec's location counter
    # follows it), so a Thumb entry at a halfword address takes the
    # non-word-aligned form or every label after it would shift by two
    if not f.thumb:
        start = "arm_func_start"
    elif f.addr & 2:
        start = "non_word_aligned_thumb_func_start"
    else:
        start = "thumb_func_start"
    out.append("\t%s %s" % (start, name))
    if name in data_names_by_name(data_names):
        pass
    out.append("%s: ; 0x%08X" % (name, f.addr))
    a = f.addr
    while a < end:
        ins = f.insns.get(a)
        if ins is not None and a + ins.size <= end:
            if a != f.addr and (a in targets or a in forced):
                out.append("%s: ; 0x%08X" % (label_name(module, a), a))
            text = ins.text
            if "{T}" in text:
                op = resolve_operand(module, f, ins, end, resolver, names, long_br)
                text = text.replace("{T}", op)
                if ins.kind == "call":
                    # BLX immediate and BL are one call to armrec. A long
                    # branch (a BL to a block of its own function, lr already
                    # saved) is spelled `b`: armrec would read a BL to a
                    # local label by B's exact reach, and mwcc used BL for
                    # targets just inside it (Black's 0x0201558C, -2048)
                    lb = (a in f.longbr
                          and op == label_name(module, ins.target & ~1))
                    text = ("b " if lb else "bl ") + op
            out.append("\t" + text)
            stats["insns"] = stats.get("insns", 0) + 1
            a += ins.size
            continue
        # data inside the function's range: up to the next instruction
        b = a + 1
        while b < end and b not in f.insns:
            b += 1
        emit_data(out, module, seg, a, b, lits, data_names, prims, targets | forced)
        a = b
    out.append("")


_dn_cache = {}


def data_names_by_name(data_names):
    k = id(data_names)
    if k not in _dn_cache:
        _dn_cache[k] = set(v[0] for v in data_names.values())
    return _dn_cache[k]


def emit_data(out, module, seg, a, end, lits, data_names, prims, targets):
    """Labelled bytes for [a, end). Each literal word gets its own label and
    `.word` (armrec's literal-pool form); named objects get their symbol."""
    data = seg.data
    base = seg.ram
    first = True
    while a < end:
        named = data_names.get(a)
        if named is not None and named[2] != "bss":
            out.append("\t.global %s" % named[0])
            out.append("%s: ; 0x%08X" % (named[0], a))
            first = False
        if a in lits and lits[a] == 4 and a + 4 <= end and not a & 3:
            v = struct.unpack_from("<I", data, a - base)[0]
            out.append("%s: .word %s ; 0x%08X" % (
                label_name(module, a), prims.get(v, "0x%08X" % v), a))
            a += 4
            first = False
            continue
        if not (a & 3) and a + 4 <= end and struct.unpack_from("<I", data, a - base)[0] in prims:
            v = struct.unpack_from("<I", data, a - base)[0]
            out.append("%s: .word %s ; 0x%08X" % (label_name(module, a), prims[v], a))
            a += 4
            first = False
            continue
        # a run of plain bytes up to the next literal / named object / 16
        b = a
        while b < end and b - a < 16:
            if b != a and (b in lits or b in data_names or b in targets):
                break
            if b != a and not (b & 3) and b + 4 <= end and \
                    struct.unpack_from("<I", data, b - base)[0] in prims:
                break
            b += 1
        if first or a in lits or a in targets or not (a & 0xFF):
            out.append("%s: ; 0x%08X" % (label_name(module, a), a))
            first = False
        out.append("\t.byte " + ", ".join("0x%02X" % x for x in data[a - base:b - base]))
        a = b
