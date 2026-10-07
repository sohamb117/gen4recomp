#!/usr/bin/env python3
"""
SDK primitive identification by signature.

    sigs.py learn ROM.nds XMAP PRIMITIVES --out SIGDB.json
    sigs.py match ROM.nds SIGDB.json --out SYMBOLS.txt [--verify XMAP]

`learn` takes the functions and objects tools/ndsrec/primitives.txt names
from a ROM whose link map is known (Diamond) and records, for each
primitive and for a set of helper functions around it:

  bytes   a masked byte pattern: every BL/BLX, every branch leaving the
          function and every literal word holding a RAM address is a
          wildcard (layout, not code); the rest must match;
  seq     the normalised instruction sequence: branch, call and ADR targets
          and RAM literals abstracted, every conditional ARM instruction
          spelled `cond:<unconditional text>`, and the three-instruction
          form some toolchains assemble a conditional instruction into
          (`b<cond> +8; b +8; X`) folded back into `cond:X`, so the same
          source assembled two ways compares equal;
  calls   the ordered list of call targets (by name) and literal values.

Helpers are the functions that call a primitive (with which of their calls
it is), hold its address in their literal pool (a veneer), or load an
object's address (with which literal, where in the function it is loaded
and the displacement), and the primitive's own callees.

`match` places them in another ROM's ARM9 static module and autoloads, in
rounds until nothing changes:

  1. a byte pattern at a unique position;
  2. a normalised sequence equal to one discovered function's
     (tools/ndsrec/discover.py), several narrowed by address order between
     already-placed functions (SDK objects link in the same order);
  3. a placed caller's k-th call (functions), the literal a placed veneer
     branches through (functions), or a placed function's literal plus the
     displacement (objects); the literal is the k-th when the function's
     code is the learned one, else the one the aligned instruction streams
     put at the learned load (the same source rebuilt by another SDK);
  4. call structure: the one function between the placed neighbours whose
     ordered callees agree with every placed callee;
  5. a similar instruction stream between placed neighbours;
  6. once the rounds stall: the one function anywhere in the module whose
     instruction stream is a primitive's (TWL-SDK's rewritten OS, PXI, TP,
     PM functions, whose neighbours moved too), then the rounds again.

NitroMain, which is game code, is the literal crt0's `_start` branches
through. `--verify` compares with a link map and prints correct/wrong.

The database holds bytes of the learning ROM's code: a build artifact
(build/), never committed. The symbol file is `ndsrec.py emit --symbols`.
"""
import argparse
import bisect
import collections
import difflib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import discover  # noqa: E402
import disasm  # noqa: E402
import nds  # noqa: E402


def is_ram(v):
    """ITCM, main RAM and its mirrors up to the TWL-SDK's DTCM (0x02FE0000)
    and system area (0x02FFFxxx): layout, not code."""
    return 0x01FF8000 <= v < 0x03000000


def sdk_obj(obj):
    """An SDK library object of the learning ROM's link map, not game code."""
    return not obj.startswith(("unk_", "ov", "overlay", "main")) and "_" in obj


def load_primitives(path):
    out = []
    for line in open(path):
        line = line.split("#", 1)[0].split()
        if len(line) == 3:
            out.append(tuple(line))       # kind, role, name
    return out


class Image(object):
    """The ARM9 static module and autoload segments of a ROM."""

    def __init__(self, rom):
        self.rom = rom
        self.view = rom.module_segment("arm9")

    def mem(self, a, n):
        d, base = self.view.lookup(a)
        if d is None or a + n > base + len(d):
            return None
        return d[a - base:a - base + n]

    def u32(self, a):
        b = self.mem(a, 4)
        return None if b is None else struct.unpack("<I", b)[0]

    def decode(self, a, thumb):
        b = self.mem(a, 4) or self.mem(a, 2)
        if b is None:
            return None
        return disasm.decode(b, 0, a, thumb)


# ------------------------------------------------------- function shapes

def shape(img, insns, thumb):
    """(seq, calls, lits, litpos) of a function given its instructions in
    order; litpos[k] is the index in seq of the instruction loading lits[k]."""
    seq, calls, lits, litpos = [], [], [], []
    i = 0
    n = len(insns)
    while i < n:
        ins = insns[i]
        cond = None
        if (not thumb and ins.kind == "b" and ins.cond and i + 2 < n
                and ins.target == ins.addr + 8 and insns[i + 1].kind == "b"
                and not insns[i + 1].cond and insns[i + 1].target == ins.addr + 12
                and insns[i + 2].addr == ins.addr + 8):
            cond = disasm.CONDS[ins.word >> 28]
            i += 2
            ins = insns[i]
        if not thumb and ins.cond and ins.kind not in ("b", "call"):
            cond = disasm.CONDS[ins.word >> 28]
            ins = disasm.decode_arm((ins.word & 0x0FFFFFFF) | 0xE0000000, ins.addr)
        text = ins.text
        if ins.kind == "call":
            calls.append(ins.target & ~1)
            text = "bl T"
        elif "{T}" in text:
            if ins.lit is not None:
                v = img.u32(ins.lit) if ins.lit_size == 4 else None
                lits.append(v)
                litpos.append(len(seq))
                text = text.replace("{T}", "=R" if v is None or is_ram(v & ~1)
                                    else "=0x%x" % v)
            else:
                text = text.replace("{T}", "T")
        elif ins.lit is not None:
            lits.append(None)
            litpos.append(len(seq))
        seq.append(("%s:" % cond if cond else "") + text)
        i += 1
    folded, where = fold_epilogues(seq)
    return folded, calls, lits, [where[p] for p in litpos]


def fold_epilogues(seq):
    """`ldmia sp!, {..., lr}; bx lr` (older mwcc) and `ldmia sp!, {..., pc}`
    (newer) are one return. Also returns each input line's index in the
    output."""
    out, where = [], []
    for t in seq:
        if out and t.endswith("bx lr"):
            prev = out[-1]
            c1 = t[:-len("bx lr")]
            if prev.startswith(c1 + "ldmia sp!, {") and prev.endswith(",lr}"):
                out[-1] = prev[:-len("lr}")] + "pc}"
                where.append(len(out) - 1)
                continue
            if prev.startswith(c1 + "ldmia sp!, {lr}"):
                out[-1] = c1 + "ldmia sp!, {pc}"
                where.append(len(out) - 1)
                continue
        where.append(len(out))
        out.append(t)
    return out, where


def make_pattern(img, start, end, thumb):
    raw = img.mem(start, end - start)
    if raw is None:
        return None
    pat = list(raw)
    litset = set()
    a = start
    while a < end:
        ins = disasm.decode(raw, a - start, a, thumb)
        o = a - start
        if ins.kind == "invalid":
            a += 2 if thumb else 4
            continue
        if ins.kind == "call":
            for k in range(o, min(o + (4 if thumb else 3), len(pat))):
                pat[k] = None
        elif ins.kind == "b" and ins.target is not None and not (start <= ins.target < end):
            if thumb:
                pat[o] = None
                if not ins.cond:
                    pat[o + 1] = None
            else:
                for k in range(o, o + 3):
                    pat[k] = None
        if ins.lit is not None and start <= ins.lit < end:
            litset.add(ins.lit)
        a += ins.size
    for la in litset:
        o = la - start
        if o + 4 <= len(pat):
            v = struct.unpack_from("<I", raw, o)[0]
            if is_ram(v & ~1):
                for k in range(o, o + 4):
                    pat[k] = None
    while pat and pat[-1] == 0:
        pat.pop()
    return pat


def pat_to_str(p):
    return "".join(".." if b is None else "%02x" % b for b in p)


def str_to_pat(s):
    return [None if s[i:i + 2] == ".." else int(s[i:i + 2], 16) for i in range(0, len(s), 2)]


def explore_insns(mod, addr, thumb, end):
    """Instructions of the function at addr, in address order, clipped."""
    f = discover.Func(addr, thumb, "entry")
    mod.explore(f, {addr: addr})
    return [f.insns[a] for a in sorted(f.insns) if addr <= a < end
            and f.insns[a].kind != "invalid"]


# ------------------------------------------------------------------ learn

def learn(a):
    import xmap as xmapmod
    rom = nds.Rom(a.rom)
    img = Image(rom)
    mod = discover.Module(rom, "arm9")
    xm = xmapmod.XMap(a.xmap)
    syms = [s for s in xm.symbols() if s[0] == "arm9"]
    code = sorted((s[1], s[4], s[6] == "thumb", s[5]) for s in syms if s[6] != "data")
    starts = [c[0] for c in code]
    name_at = {}
    for c in code:
        name_at.setdefault(c[0], c[1])
    byname = {}
    for s in syms:
        byname.setdefault(s[4], s)
    bss = {}
    for r in xm.rows:
        if r[1] == ".arm9.bss":
            bss.setdefault(r[5], r[2])

    def extent(addr):
        i = bisect.bisect_right(starts, addr)
        end = starts[i] if i < len(starts) else addr + 4
        return min(end, addr + 0x2000)

    info = {}            # addr -> dict
    callers = collections.defaultdict(list)     # callee -> [(caller, k)]
    litrefs = collections.defaultdict(list)     # value -> [(func, k)]
    for addr, name, thumb, obj in code:
        if name_at.get(addr) != name:
            continue
        end = extent(addr)
        insns = explore_insns(mod, addr, thumb, end)
        if not insns:
            continue
        seq, calls, lits, litpos = shape(img, insns, thumb)
        info[addr] = {"name": name, "thumb": thumb, "obj": obj, "end": end,
                      "seq": seq, "calls": calls, "lits": lits, "litpos": litpos}
        for k, t in enumerate(calls):
            callers[t].append((addr, k))
        for k, v in enumerate(lits):
            if v is not None:
                litrefs[v].append((addr, k))

    db = {"rom": rom.sha1, "funcs": {}, "data": {}, "shapes": {}}

    def keep(addr):
        f = info[addr]
        if f["name"] not in db["shapes"]:
            db["shapes"][f["name"]] = {
                "addr": addr, "thumb": f["thumb"], "obj": f["obj"],
                "bytes": pat_to_str(make_pattern(img, addr, f["end"], f["thumb"]) or []),
                "seq": f["seq"],
                "calls": [info[t]["name"] if t in info else None for t in f["calls"]],
            }
        return f["name"]

    def rank(h):
        f = info[h]
        return (not sdk_obj(f["obj"]), -len(f["seq"]))

    for kind, role, name in load_primitives(a.primitives):
        if kind == "func":
            s = byname.get(name)
            if s is None or s[1] not in info:
                print("learn: %s not in the link map" % name, file=sys.stderr)
                continue
            addr = s[1]
            keep(addr)
            hs = sorted(callers.get(addr, []), key=lambda h: rank(h[0]))
            helpers = []
            seen = set()
            for (h, k) in hs:
                if h in seen:
                    continue
                seen.add(h)
                helpers.append({"func": keep(h), "call": k})
                if len(helpers) >= 8:
                    break
            for t in info[addr]["calls"]:
                if t in info:
                    keep(t)
            # Functions holding its address in their literal pool: an SDK
            # veneer (`ldr r1, =target; bx r1`, OS_UnLockCartridge ->
            # OS_UnlockCartridge) or a table names its target that way.
            lithelpers = []
            for (h, k) in sorted(litrefs.get(addr, []) + litrefs.get(addr | 1, []),
                                 key=lambda h: rank(h[0])):
                if all(x["func"] != info[h]["name"] for x in lithelpers):
                    lithelpers.append({"func": keep(h), "lit": k,
                                       "pos": info[h]["litpos"][k]})
                if len(lithelpers) >= 4:
                    break
            db["funcs"][name] = {"role": role, "thumb": info[addr]["thumb"],
                                 "helpers": helpers, "lithelpers": lithelpers}
        else:
            addr = byname[name][1] if name in byname else bss.get(name)
            if addr is None:
                print("learn: %s not in the link map" % name, file=sys.stderr)
                continue
            refs = []
            for delta in range(0, 0x400, 4):
                for (h, k) in litrefs.get(addr - delta, []):
                    refs.append((h, k, delta))
            # Functions already learned (primitives, their callers and
            # callees) first: those are the ones another ROM places.
            refs.sort(key=lambda r: (r[2], info[r[0]]["name"] not in db["shapes"]) + rank(r[0]))
            helpers = []
            seen = set()
            for (h, k, delta) in refs:
                if h in seen:
                    continue
                seen.add(h)
                helpers.append({"func": keep(h), "lit": k, "delta": delta,
                                "pos": info[h]["litpos"][k]})
                if len(helpers) >= 12:
                    break
            db["data"][name] = {"role": role, "helpers": helpers,
                                "section": "bss" if name in bss else "data"}
    with open(a.out, "w") as fh:
        json.dump(db, fh)
    print("learn: %d functions, %d objects, %d shapes -> %s" % (
        len(db["funcs"]), len(db["data"]), len(db["shapes"]), a.out))


# ------------------------------------------------------------------ match

def find_bytes(segs, pat, thumb):
    step = 2 if thumb else 4
    anchor = None
    for o in range(0, len(pat) - 3, step):
        if all(pat[o + k] is not None for k in range(4)):
            anchor = o
            break
    out = []
    if anchor is None:
        return out
    key = bytes(pat[anchor:anchor + 4])
    for seg in segs:
        d = seg.data
        i = d.find(key)
        while i >= 0:
            st = i - anchor
            if st >= 0 and (seg.ram + st) % step == 0 and st + len(pat) <= len(d):
                if all(b is None or d[st + k] == b for k, b in enumerate(pat)):
                    out.append(seg.ram + st)
            i = d.find(key, i + 1)
    return out


def match(a):
    rom = nds.Rom(a.rom)
    img = Image(rom)
    db = json.load(open(a.db))
    shapes = db["shapes"]
    mod = discover.Module(rom, "arm9")
    mod.run(discover.module_seeds(rom, "arm9"))
    starts = sorted(mod.funcs)
    tshape = {}
    for addr in starts:
        f = mod.funcs[addr]
        ins = [f.insns[x] for x in sorted(f.insns) if f.insns[x].kind != "invalid"]
        tshape[addr] = shape(img, ins, f.thumb)
    by_seq = collections.defaultdict(list)
    for addr, (seq, _c, _l, _p) in tshape.items():
        by_seq[(mod.funcs[addr].thumb, tuple(seq))].append(addr)
    tcallers = collections.defaultdict(set)
    for addr, (_s, calls, _l, _p) in tshape.items():
        for c in calls:
            tcallers[c].add(addr)

    placed = {}
    how = {}
    cands = {}
    for n, s in shapes.items():
        hits = find_bytes(img.view.segs, str_to_pat(s["bytes"]), s["thumb"]) if s["bytes"] else []
        if len(hits) == 1:
            placed[n] = hits[0]
            how[n] = "bytes"
        elif hits:
            cands[n] = hits

    def order_pick(n, hits, same_obj=False):
        """The one hit between the placed neighbours of n (by the learning
        ROM's address order); with same_obj, neighbours from n's own object
        only, which bound it tightly."""
        obj = shapes[n]["obj"]
        anchors = sorted((shapes[m]["addr"], placed[m]) for m in placed
                         if m in shapes and (not same_obj or shapes[m]["obj"] == obj))
        src = [x[0] for x in anchors]
        la = shapes[n]["addr"]
        i = bisect.bisect_left(src, la)
        lo = anchors[i - 1][1] if i > 0 else -1
        hi = anchors[i][1] if i < len(anchors) else 1 << 32
        inside = [x for x in hits if lo < x < hi]
        return inside[0] if len(inside) == 1 else None, (lo, hi)

    def decoded_call(addr, thumb, k):
        if addr in tshape:
            calls = tshape[addr][1]
            return calls[k] if k < len(calls) else None
        return None

    def ensure_shape(addr, thumb):
        if addr not in tshape:
            f = discover.Func(addr, thumb, "entry")
            mod.explore(f, mod.funcs)
            ins = [f.insns[x] for x in sorted(f.insns) if f.insns[x].kind != "invalid"]
            tshape[addr] = shape(img, ins, thumb)

    def mnem(seq):
        return [t.split(" ")[0] for t in seq]

    def aligned_literal(h, ha):
        """The literal of the placed ha that is the learned helper h's k-th:
        by index when the code is the same (the same normalised sequence or
        a byte match), else through the alignment of the two instruction
        streams (at least 80% alike) at the instruction loading it: the same
        source rebuilt, e.g. TWL-SDK's OSi_RescheduleThread (0.85 alike, its
        thread statics regrouped, one literal fewer)."""
        hn, k = h["func"], h["lit"]
        s = shapes[hn]
        ensure_shape(ha, s["thumb"])
        seq, _c, lits, litpos = tshape[ha]
        if seq == s["seq"] or how.get(hn) == "bytes":
            return lits[k] if k < len(lits) else None
        pos = h.get("pos")
        sm = difflib.SequenceMatcher(None, s["seq"], seq, autojunk=False)
        if pos is None or sm.ratio() < 0.8:
            return None
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if i1 <= pos < i2 and (tag == "equal" or (tag == "replace" and i2 - i1 == j2 - j1)):
                j = j1 + pos - i1
                return lits[litpos.index(j)] if j in litpos else None
        return None

    def similar_anywhere():
        """6. The static module's one function whose instruction stream is
        a primitive's (mnemonics, >= 85%, >= 90% under 20 instructions, and
        0.1 ahead of the next best): an SDK function TWL-SDK rebuilt where
        no placed neighbour bounds it. Under 12 instructions too many
        functions look alike (Black's game code has two 0.86-0.88 matches
        for 9- and 10-instruction CTRDG functions it does not contain). A
        candidate that does not call every placed callee the primitive
        calls is not it. Then the same for the SDK callers (helpers) of
        the primitives still unplaced, which rule 3 then follows to the
        primitive (TWL-SDK's FSi_ReadRomCallback -> CARDi_ReadRom, its
        GX_LoadBG*Char -> MI_DmaCopy32): a helper is never output itself,
        and its call count must still equal the learned one for rule 3."""
        taken = set(placed.values())
        helpers = sorted(set(
            h["func"] for n, info in db["funcs"].items() if n not in placed
            for h in info["helpers"]
            if h["func"] in shapes and h["func"] not in db["funcs"]
            and sdk_obj(shapes[h["func"]]["obj"])))
        for n in list(db["funcs"]) + helpers:
            if n in placed or n not in shapes or len(shapes[n]["seq"]) < 12:
                continue
            s = shapes[n]
            mine = mnem(s["seq"])
            least = 0.85 if len(mine) >= 20 else 0.9
            need = set(placed[c] for c in s["calls"] if c and c in placed)
            sm = difflib.SequenceMatcher(None, autojunk=False)
            sm.set_seq2(mine)
            scores = []
            for t in starts:
                theirs = tshape[t][0]
                if (mod.funcs[t].thumb != s["thumb"] or t in taken
                        or abs(len(theirs) - len(mine)) > max(4, len(mine) // 4)
                        or not need <= set(tshape[t][1])):
                    continue
                sm.set_seq1(mnem(theirs))
                if sm.real_quick_ratio() >= least and sm.quick_ratio() >= least:
                    scores.append((sm.ratio(), t))
            scores.sort(reverse=True)
            if scores and scores[0][0] >= least and (
                    len(scores) == 1 or scores[0][0] - scores[1][0] >= 0.1):
                placed[n], how[n] = scores[0][1], "similar-anywhere %.2f" % scores[0][0]
                taken.add(scores[0][1])

    # Rounds until nothing changes; once they stall, rule 6 runs (once) and
    # the rounds resume from what it placed.
    anywhere_done = False
    for rnd in range(12):
        before = len(placed)
        # 2. normalised sequence
        for n, s in shapes.items():
            if n in placed:
                continue
            hits = by_seq.get((s["thumb"], tuple(s["seq"])), [])
            hits = [h for h in hits if h not in placed.values()]
            short = len(s["seq"]) < 6 and not any(
                t.startswith("swi") or "=0x" in t for t in s["seq"])
            if len(hits) == 1 and not short:
                placed[n], how[n] = hits[0], "sequence"
            elif hits:
                p, _ = order_pick(n, hits, same_obj=short)
                if p is not None:
                    placed[n], how[n] = p, "sequence+order"
        for n, hits in cands.items():
            if n not in placed:
                p, _ = order_pick(n, hits, same_obj=True)
                if p is not None:
                    placed[n], how[n] = p, "bytes+order"
        # 3. callers
        for n, info in db["funcs"].items():
            if n in placed:
                continue
            for h in info["helpers"]:
                ha = placed.get(h["func"])
                if ha is None:
                    continue
                ensure_shape(ha, shapes[h["func"]]["thumb"])
                if len(tshape[ha][1]) != len(shapes[h["func"]]["calls"]):
                    continue
                t = decoded_call(ha, shapes[h["func"]]["thumb"], h["call"])
                if t is not None:
                    placed[n], how[n] = t, "call %d of %s" % (h["call"], h["func"])
                    break
        # 3b. callees named by a placed function's shape
        for n, s in list(shapes.items()):
            ha = placed.get(n)
            if ha is None:
                continue
            ensure_shape(ha, s["thumb"])
            tcalls = tshape[ha][1]
            if len(tcalls) != len(s["calls"]):
                continue
            for k, cn in enumerate(s["calls"]):
                if cn and cn not in placed and cn in shapes:
                    placed[cn], how[cn] = tcalls[k], "call %d of %s" % (k, n)
        # 3c. a placed function whose literal pool names the primitive (a
        # veneer: `ldr r1, =target; bx r1`)
        for n, info in db["funcs"].items():
            if n in placed:
                continue
            for h in info.get("lithelpers", []):
                ha = placed.get(h["func"])
                if ha is None:
                    continue
                v = aligned_literal(h, ha)
                if v is not None and is_ram(v & ~1) and (v & 1) == int(info["thumb"]):
                    placed[n], how[n] = v & ~1, "literal %d of %s" % (h["lit"], h["func"])
                    break
        # 4. call structure between placed neighbours
        for n, s in shapes.items():
            if n in placed or not s["calls"]:
                continue
            want = [placed.get(c) if c else None for c in s["calls"]]
            if not any(w is not None for w in want):
                continue
            _, (lo, hi) = order_pick(n, [], same_obj=True)
            if lo < 0 or hi >= 1 << 32:
                continue
            i = bisect.bisect_right(starts, lo)
            ok = []
            while i < len(starts) and starts[i] < hi:
                t = starts[i]
                i += 1
                if mod.funcs[t].thumb != s["thumb"] or t in placed.values():
                    continue
                tc = tshape[t][1]
                if len(tc) != len(want):
                    continue
                if all(w is None or w == c for w, c in zip(want, tc)):
                    ok.append(t)
            agree = sum(1 for w in want if w is not None)
            if len(ok) == 1 and (agree >= 2 or abs(len(tshape[ok[0]][0]) - len(s["seq"]))
                                 <= max(2, len(s["seq"]) // 8)):
                placed[n], how[n] = ok[0], "call structure"
        # 4b. the one function calling every placed callee a primitive
        # calls whose instruction stream is clearly the closest to it
        # (TWL-SDK's FS_StartOverlay: MIi_UncompressBackward's caller
        # besides crt0)
        for n in db["funcs"]:
            if n in placed or n not in shapes:
                continue
            s = shapes[n]
            need = set(placed[c] for c in s["calls"] if c and c in placed)
            if not need:
                continue
            cand = set.intersection(*(tcallers.get(c, set()) for c in need))
            cand = [t for t in cand if mod.funcs[t].thumb == s["thumb"]
                    and t not in placed.values()]
            mine = mnem(s["seq"])
            scores = sorted(((difflib.SequenceMatcher(None, mine, mnem(tshape[t][0]),
                                                      autojunk=False).ratio(), t)
                             for t in cand), reverse=True)
            if scores and scores[0][0] >= 0.6 and (
                    len(scores) == 1 or scores[0][0] - scores[1][0] >= 0.15):
                placed[n], how[n] = scores[0][1], "caller %.2f" % scores[0][0]
        # 5. similar instruction stream between the placed neighbours of
        # the same object (the same source through another compiler)
        for n, s in shapes.items():
            if n in placed or len(s["seq"]) < 6:
                continue
            _, (lo, hi) = order_pick(n, [], same_obj=True)
            if lo < 0 or hi >= 1 << 32:
                _, (lo, hi) = order_pick(n, [])
            if lo < 0 or hi >= 1 << 32 or hi - lo > 0x2000:
                continue
            mine = [t.split(" ")[0] for t in s["seq"]]
            scores = []
            i = bisect.bisect_right(starts, lo)
            while i < len(starts) and starts[i] < hi:
                t = starts[i]
                i += 1
                if mod.funcs[t].thumb != s["thumb"] or t in placed.values():
                    continue
                theirs = [x.split(" ")[0] for x in tshape[t][0]]
                r = difflib.SequenceMatcher(None, mine, theirs, autojunk=False).ratio()
                scores.append((r, t))
            scores.sort(reverse=True)
            if scores and scores[0][0] >= 0.75 and (len(scores) == 1 or
                                                    scores[0][0] - scores[1][0] >= 0.15):
                placed[n], how[n] = scores[0][1], "similar %.2f" % scores[0][0]
        if len(placed) == before:
            if anywhere_done:
                break
            anywhere_done = True
            similar_anywhere()
            if len(placed) == before:
                break

    out = []
    found = {}
    for name, info in db["funcs"].items():
        addr = placed.get(name)
        thumb = info["thumb"]
        if addr is None and name == "NitroMain":
            addr, thumb = nitromain(img, rom)
            if addr is not None:
                how[name] = "crt0 _start literal"
        if addr is None:
            continue
        found[name] = addr
        out.append("func %s 0x%08X %s arm9 %s" % (name, addr, "thumb" if thumb else "arm",
                                                   info["role"]))
    for name, info in db["data"].items():
        addr = None
        for h in info["helpers"]:
            ha = placed.get(h["func"])
            if ha is None:
                continue
            v = aligned_literal(h, ha)
            if v is not None and is_ram(v):
                addr = v + h["delta"]
                how[name] = "literal %d of %s" % (h["lit"], h["func"])
                break
        if addr is None:
            continue
        found[name] = addr
        out.append("data %s 0x%08X 4 %s arm9" % (name, addr, info["section"]))
    with open(a.out, "w") as fh:
        fh.write("# generated by tools/ndsrec/sigs.py match from %s (sha1 %s)\n" % (
            os.path.basename(a.rom), rom.sha1))
        fh.write("\n".join(sorted(out)) + "\n")
    names = list(db["funcs"]) + list(db["data"])
    kinds = collections.Counter(how.get(n, "").split(" ")[0] for n in found)
    print("match: %d of %d primitives found (%s)" % (
        len(found), len(names), ", ".join("%s %d" % kv for kv in sorted(kinds.items()))))
    missing = [n for n in names if n not in found]
    if missing:
        print("match: not found: " + " ".join(missing))
    if a.debug:
        for n in sorted(found):
            print("  placed %s 0x%08X: %s" % (n, found[n], how.get(n, "")))
        for n in missing:
            info = db["funcs"].get(n) or db["data"].get(n)
            print("  %s: helpers %s" % (n, ", ".join(
                "%s%s" % (h["func"], "@" + how[h["func"]] if h["func"] in placed else "")
                for h in info["helpers"])))
    if a.verify:
        verify(a.verify, found, how)


def nitromain(img, rom):
    """crt0 ends `ldr r1, =NitroMain; ldr lr, =HW_RESET_VECTOR; bx r1`."""
    ea = rom.arm9_entry
    while ea < rom.arm9_entry + 0x400:
        ins = img.decode(ea, False)
        if ins is not None and ins.kind == "ijump" and ins.text == "bx r1":
            for back in range(4, 40, 4):
                p = img.decode(ea - back, False)
                if p.lit is not None and p.text.startswith("ldr r1"):
                    v = img.u32(p.lit)
                    return v & ~1, bool(v & 1)
            return None, False
        ea += 4
    return None, False


def verify(xmap_path, found, how):
    import xmap as xmapmod
    xm = xmapmod.XMap(xmap_path)
    truth = {}
    for r in xm.rows:
        if r[5] not in truth and r[5] not in xmapmod.MAPPING:
            truth[r[5]] = r[2]
    ok = bad = unknown = 0
    for n, addr in sorted(found.items()):
        t = truth.get(n)
        if t is None:
            unknown += 1
            print("verify: %-36s 0x%08X  (%s)  not in the link map" % (n, addr, how.get(n)))
        elif t == addr:
            ok += 1
        else:
            bad += 1
            print("verify: %-36s 0x%08X  WRONG, link map 0x%08X  (%s)" % (n, addr, t, how.get(n)))
    print("verify: %d correct, %d wrong, %d not named in the link map" % (ok, bad, unknown))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("learn")
    p.add_argument("rom")
    p.add_argument("xmap")
    p.add_argument("primitives")
    p.add_argument("--out", required=True)
    p = sub.add_parser("match")
    p.add_argument("rom")
    p.add_argument("db")
    p.add_argument("--out", required=True)
    p.add_argument("--verify")
    p.add_argument("--debug", action="store_true")
    a = ap.parse_args()
    {"learn": learn, "match": match}[a.cmd](a)


if __name__ == "__main__":
    main()
