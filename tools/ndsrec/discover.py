"""
Function discovery over one module of an NDS ROM (the ARM9 static with its
autoloads, one overlay, or the ARM7), from the bytes alone.

1. Seeds: the module's entry points (the ARM9 entry and autoload-done
   callback, an overlay's static-initialiser table), every call target the
   descent finds, and every word in the module whose value points at
   plausible code (function-pointer tables, callbacks in literal pools). A
   word with bit 0 set is a Thumb pointer, an even one ARM.
2. Recursive descent per function: basic blocks are followed through
   branches, conditional branches and the two switch idioms mwcc emits (ARM
   `addls pc, pc, rN, lsl #2` over a branch table; Thumb `add pc, rN` over a
   halfword offset table); a pc-relative load marks its literal word as data;
   BL/BLX targets become functions in the instruction set the encoding says.
   A branch to a known function start, or to a `push {..., lr}` /
   `stmdb sp!, {..., lr}` (which mwcc emits once, at entry), is a tail call.
3. Gap filling: mwcc lays functions out back to back, each followed by its
   literal pool and alignment padding, so what is left between two
   discovered functions is either padding, a pool, or a function nothing
   reaches by a direct call (dead code, or reached only through a computed
   address). A gap that starts with a prologue, or decodes cleanly to a
   return and fills the gap, becomes a function, and the descent repeats.

The result per module: {addr: Func} plus the literal and data words found
inside the code, which the emitter turns into labelled data.
"""
import struct

import disasm

PROLOGUE_T = lambda hw: (hw & 0xFF00) == 0xB500          # push {..., lr}
PROLOGUE_A = lambda w: (w & 0xFFFF4000) == 0xE92D4000    # stmdb sp!, {..., lr}


class Func(object):
    __slots__ = ("addr", "thumb", "insns", "lits", "end", "calls",
                 "tails", "jt", "source", "data", "longbr")

    def __init__(self, addr, thumb, source):
        self.addr = addr
        self.thumb = thumb
        self.insns = {}         # addr -> Ins
        self.lits = {}          # addr -> size of data the code reads pc-relative
        self.data = set()       # addresses of jump-table / adr data inside
        self.end = addr
        self.calls = set()      # call targets (bit 0 = Thumb)
        self.tails = set()      # tail-call targets
        self.jt = {}            # jump instruction addr -> [targets]
        self.longbr = set()     # Thumb BLs that are long branches
        self.source = source


class Module(object):
    def __init__(self, rom, name):
        self.rom = rom
        self.name = name
        self.view = rom.module_segment(name)
        self.funcs = {}
        self.external_calls = []    # (from_func, insn addr, target)
        self.claimed = {}           # byte-granular owner map: addr -> func addr

    # -------------------------------------------------------------- bytes
    def mem(self, addr, n):
        data, base = self.view.lookup(addr)
        if data is None or addr + n > base + len(data):
            return None
        return data[addr - base: addr - base + n]

    def u16(self, addr):
        b = self.mem(addr, 2)
        return None if b is None else b[0] | (b[1] << 8)

    def u32(self, addr):
        b = self.mem(addr, 4)
        return None if b is None else struct.unpack("<I", b)[0]

    def decode(self, addr, thumb):
        data, base = self.view.lookup(addr)
        if data is None:
            return None
        need = 2 if thumb else 4
        if addr + need > base + len(data):
            return None
        return disasm.decode(data, addr - base, addr, thumb)

    def is_prologue(self, addr, thumb):
        if thumb:
            hw = self.u16(addr)
            return hw is not None and PROLOGUE_T(hw)
        w = self.u32(addr)
        return w is not None and PROLOGUE_A(w)

    # ------------------------------------------------------------ descent
    def explore(self, f, known):
        """Fill f.insns/lits from f.addr. `known` maps function starts."""
        thumb = f.thumb
        work = [f.addr]
        seen = f.insns
        while work:
            a = work.pop()
            while True:
                if a in seen or a & (1 if thumb else 3):
                    break
                if a != f.addr and a in known and known[a] != f.addr:
                    break
                ins = self.decode(a, thumb)
                if ins is None or ins.kind == "invalid":
                    if ins is not None:
                        seen[a] = ins
                    break
                if a in f.lits:
                    break
                seen[a] = ins
                if ins.lit is not None:
                    f.lits[ins.lit] = max(f.lits.get(ins.lit, 0), ins.lit_size)
                if ins.adr is not None:
                    f.data.add(ins.adr)
                k = ins.kind
                if k == "call" and thumb and self.is_long_branch(f, ins, known):
                    # a branch, not a call: no fall-through (a pool or
                    # another block follows)
                    work.append(ins.target & ~1)
                    f.longbr.add(a)
                    break
                elif k == "call":
                    f.calls.add(ins.target)
                elif k == "b":
                    t = ins.target
                    if self.is_tail(f, t, known):
                        f.tails.add(t | (1 if thumb else 0))
                    else:
                        work.append(t)
                elif k == "jt_arm":
                    n = self.arm_jt_count(f, a)
                    tg = [a + 8 + 4 * i for i in range(n)] if n else []
                    f.jt[a] = tg
                    work.extend(tg)
                elif k == "jt_thumb":
                    tg, tab, nbytes = self.thumb_jt(f, a)
                    f.jt[a] = tg
                    if tab is not None:
                        for o in range(0, nbytes, 2):
                            f.lits[tab + o] = 2
                    work.extend(tg)
                if ins.ends_flow:
                    break
                a += ins.size
        if seen:
            last = max(seen)
            end = last + seen[last].size
            for la, n in f.lits.items():
                if la >= f.addr and la + n > end and la - end < 0x1000:
                    end = la + n
            f.end = end

    def is_long_branch(self, f, ins, known):
        """mwcc spells a Thumb branch beyond B's +-2 KB as BL (the prologue
        saved LR). The target is then inside the caller: past its start,
        not a prologue, with no function start in between, and not right
        after a return or padding (where a function nothing else reaches
        would begin)."""
        t = ins.target
        if not (t & 1):
            return False
        t &= ~1
        if t <= f.addr or t - f.addr > 0x4000 or t in known:
            return False
        if self.has_prologue(t, True):
            return False
        for k in known:
            if f.addr < k < t:
                return False
        prev = self.u16(t - 2)
        if (prev is None or prev in (0, 0x4770, 0x46C0)
                or (prev & 0xFF00) == 0xBD00 or (prev & 0xFF87) == 0x4700):
            return False
        # a literal pool right before the target: a function starts there
        w = self.u32(t - 4)
        if (t - 4) in f.lits or (w is not None and (
                0x01FF8000 <= w < 0x02800000 or 0x04000000 <= w < 0x04800000)):
            return False
        if (prev & 0xF800) == 0xE000:           # unconditional b: could be either
            pass
        return True

    def is_tail(self, f, t, known):
        if t == f.addr:
            return False
        if t in known:
            return True
        if t < f.addr:
            return True
        if self.is_prologue(t, f.thumb):
            return True
        if not self.view.contains(t):
            return True
        return False

    def arm_jt_count(self, f, a):
        # cmp rN, #K a few instructions before: K + 1 cases.
        for back in range(4, 24, 4):
            ins = self.decode(a - back, False)
            if ins is None:
                break
            w = ins.word
            if (w & 0x0FF00000) == 0x03500000:        # cmp rn, #imm
                v = disasm.ror32(w & 0xFF, ((w >> 8) & 0xF) * 2)
                return v + 1
        return 0

    def thumb_jt(self, f, a):
        """Targets of the mwcc Thumb switch ending in `add pc, rN` at a."""
        # add rX, pc at a-8 (add rX,rX,rX; add rX,pc; ldrh rX,[rX,#k];
        # lsl; asr; add pc, rX)
        base = None
        k = None
        for back in range(2, 14, 2):
            hw = self.u16(a - back)
            if hw is None:
                break
            if (hw & 0xFF00) == 0x4400 and ((hw >> 3) & 0xF) == 15:   # add rX, pc
                base = ((a - back) + 4)
                break
            if (hw & 0xF800) == 0x8800:                               # ldrh imm
                k = ((hw >> 6) & 0x1F) * 2
        if base is None or k is None:
            return [], None, 0
        tab = base + k
        n = 0
        for back in range(2, 40, 2):
            hw = self.u16(a - back)
            if hw is None:
                break
            if (hw & 0xF800) == 0x2800:                               # cmp rX, #imm
                n = (hw & 0xFF) + 1
                break
        if n == 0:
            return [], None, 0
        tg = []
        for i in range(n):
            v = self.u16(tab + 2 * i)
            if v is None:
                break
            v = disasm.sext(v, 16)
            tg.append((a + 4 + v) & 0xFFFFFFFF)
        return tg, tab, 2 * n

    # --------------------------------------------------------------- run
    def add(self, addr, thumb, source):
        if addr in self.funcs or addr & (1 if thumb else 3):
            return False
        # the ARM9 secure area (the static's first 2 KB) is the SDK's
        # syscall blob: data with a few SWI thunks reached by call only
        if (self.name == "arm9" and source in ("gap", "pointer")
                and self.rom.arm9_ram <= addr < self.rom.arm9_ram + 0x800):
            return False
        if not self.view.contains(addr):
            return False
        self.funcs[addr] = Func(addr, thumb, source)
        return True

    def code_ranges(self):
        return [(s.ram, s.end) for s in self.view.segs]

    def run(self, seeds, hints=(), pointer_scan=True, gap_fill=True):
        """seeds: [(addr, thumb, source)] taken as given; hints: pointer-like
        values from other modules, validated like the module's own words."""
        for a, t, s in seeds:
            self.add(a, t, s)
        self.descend()
        for _ in range(16):
            changed = False
            if hints:
                changed |= self.try_hints(hints)
            if pointer_scan:
                changed |= self.try_hints(self.pointer_words(self.view.segs))
            self.descend()
            if gap_fill and self.funcs:
                changed |= self.fill_gaps()
                self.descend()
            if not changed:
                break
        self.resolve_overlaps()
        self.prune_calls()

    def descend(self):
        done = set(a for a, f in self.funcs.items() if f.insns)
        while True:
            pending = [a for a in self.funcs if a not in done]
            if not pending:
                break
            for a in sorted(pending):
                done.add(a)
                f = self.funcs[a]
                known = self.funcs
                self.explore(f, known)
                for t in f.calls | f.tails:
                    th = bool(t & 1)
                    ta = t & ~1
                    if self.view.contains(ta):
                        self.add(ta, th, "call")
                    else:
                        self.external_calls.append((a, ta, th))
        # a tail target that became a function start after its caller was
        # explored: re-explore callers whose blocks swallowed a later start
        changed = True
        rounds = 0
        while changed and rounds < 4:
            changed = False
            rounds += 1
            for a, f in list(self.funcs.items()):
                swallowed = [b for b in f.insns if b != a and b in self.funcs]
                if swallowed:
                    f.insns = {}
                    f.lits = {}
                    f.data = set()
                    f.calls = set()
                    f.tails = set()
                    f.jt = {}
                    f.longbr = set()
                    self.explore(f, self.funcs)
                    changed = True

    def literal_words(self):
        s = set()
        for f in self.funcs.values():
            for la in f.lits:
                s.add(la & ~3)
        return s

    def scan_pointers(self):
        """Words anywhere in the module that point at a function start."""
        self.try_hints(self.pointer_words(self.view.segs))

    def pointer_words(self, segs):
        lo = min(s.ram for s in self.view.segs)
        hi = max(s.end for s in self.view.segs)
        insn = self.insn_bytes(lits=False)
        out = []
        for seg in segs:
            data = seg.data
            for off in range(0, len(data) - 3, 4):
                if seg.ram + off in insn:
                    continue
                v = struct.unpack_from("<I", data, off)[0]
                if lo <= (v & ~1) < hi:
                    out.append(v)
        return out

    def try_hints(self, values):
        """Accept pointer-like values that land on a function start: a
        prologue, or the first instruction after a known function's end
        (past its padding and pool) that decodes to a return."""
        bounds = self.boundaries()
        insn = self.insn_bytes()
        added = False
        for v in values:
            th = bool(v & 1)
            t = v & ~1
            if t in self.funcs or t in insn or not self.view.contains(t):
                continue
            if not th and t & 3:
                continue
            if self.has_prologue(t, th) or (t in bounds and self.plausible(t, th, 200)):
                added |= self.add(t, th, "pointer")
        return added

    def has_prologue(self, addr, thumb, weak=False):
        if self.is_prologue(addr, thumb):
            return True
        if weak:
            # a leaf saving callee-saved registers only: push {r4-r7} /
            # stmdb sp!, {r4-r11}
            if thumb:
                hw = self.u16(addr)
                if hw is not None and (hw & 0xFF00) == 0xB400 and hw & 0xF0:
                    return True
            else:
                w = self.u32(addr)
                if w is not None and (w & 0xFFFF0000) == 0xE92D0000 and w & 0x0FF0:
                    return True
        # varargs: push {r0-r3} / stmdb sp!, {r0-r3} first
        if thumb:
            hw = self.u16(addr)
            return (hw is not None and (hw & 0xFFF0) == 0xB400 and hw & 0xF
                    and self.is_prologue(addr + 2, True))
        w = self.u32(addr)
        return (w is not None and (w & 0xFFFFFFF0) == 0xE92D0000 and w & 0xF
                and self.is_prologue(addr + 4, False))

    def skip_filler(self, a, limit, covered, thumb=True):
        """Past padding and pools. Thumb pads with a zero halfword (or a
        `nop` before a 4-aligned pool); ARM only with zero words."""
        while a < limit:
            if a in covered:
                a += 2
                continue
            if thumb or a & 2:
                hw = self.u16(a)
                if hw is None:
                    break
                if hw == 0 or (hw == 0x46C0 and a & 2):
                    a += 2
                    continue
                break
            w = self.u32(a)
            if w == 0:
                a += 4
                continue
            break
        return a

    def boundaries(self):
        """Addresses where the next function would start after each known
        function's end."""
        covered = self.insn_bytes()
        out = set()
        hi = max(s.end for s in self.view.segs)
        for f in self.funcs.values():
            out.add(self.skip_filler(f.end, hi, covered, f.thumb))
        return out

    def insn_bytes(self, lits=True):
        s = set()
        for f in self.funcs.values():
            for a, ins in f.insns.items():
                for i in range(0, ins.size, 2):
                    s.add(a + i)
            if not lits:
                continue
            for la, n in f.lits.items():
                for i in range(0, max(n, 2), 2):
                    s.add((la & ~1) + i)
        return s

    def plausible(self, addr, thumb, limit=400):
        """Linear decode from addr reaches a return before anything invalid,
        an odd branch, or a branch outside the module."""
        if self.is_prologue(addr, thumb):
            return True
        a = addr
        for _ in range(limit):
            ins = self.decode(a, thumb)
            if ins is None or ins.kind == "invalid":
                return False
            if ins.kind in ("b", "call") and not thumb and ins.target is not None:
                pass
            if ins.kind == "b" and not self.view.contains(ins.target):
                return False
            if not thumb and ins.cond is False and (ins.word >> 28) not in (14, 15):
                pass
            if ins.kind in ("ret", "ijump", "jt_thumb", "jt_arm") and not ins.cond:
                return True
            if ins.kind == "b" and not ins.cond:
                return True
            a += ins.size
        return False

    def fill_gaps(self):
        """Promote functions in the holes between discovered ones, walking
        each hole function by function."""
        if not self.funcs:
            return False
        covered = self.insn_bytes()
        code_hi = self.code_end()
        mod_hi = max(s.end for s in self.view.segs)
        added = False
        starts = sorted(self.funcs)
        for i, s0 in enumerate(starts):
            f = self.funcs[s0]
            last = i + 1 >= len(starts)
            nxt = mod_hi if last else starts[i + 1]
            a = f.end
            while True:
                a = self.skip_filler(a, nxt, covered, f.thumb)
                if a >= nxt or a in self.funcs or a & 1:
                    break
                th = None
                if (a & 3) == 0 and self.has_prologue(a, False, weak=True):
                    th = False
                elif self.has_prologue(a, True, weak=True):
                    th = True
                elif (a < code_hi and (f.thumb or (a & 3) == 0)
                      and self.plausible(a, f.thumb, 64)):
                    th = f.thumb
                if th is None and not last:
                    # data inside the hole (a table nothing names): the next
                    # strong prologue before the following function
                    b = a + 2
                    while b < nxt:
                        if b not in covered:
                            if (b & 3) == 0 and self.has_prologue(b, False):
                                th = False
                                break
                            if self.has_prologue(b, True):
                                th = True
                                break
                        b += 2
                    if th is not None:
                        a = b
                if th is None:
                    break
                if not self.add(a, th, "gap"):
                    break
                g = self.funcs[a]
                self.explore(g, self.funcs)
                if not g.insns:
                    break
                added = True
                for ia, ins in g.insns.items():
                    for k in range(0, ins.size, 2):
                        covered.add(ia + k)
                for la, n in g.lits.items():
                    for k in range(0, max(n, 2), 2):
                        covered.add((la & ~1) + k)
                for t in g.calls | g.tails:
                    if self.view.contains(t & ~1):
                        self.add(t & ~1, bool(t & 1), "call")
                f = g
                a = g.end
        return added

    def code_end(self):
        """Where code stops: the end of the last function the descent
        reached from a call or an entry point (rodata follows)."""
        return max(f.end for f in self.funcs.values())

    def prune_calls(self):
        """Drop call-sourced functions nothing calls any more (a target
        recorded before its caller was re-explored, or before a Thumb BL was
        recognised as a long branch)."""
        while True:
            refs = set()
            for f in self.funcs.values():
                for t in f.calls | f.tails:
                    refs.add(t & ~1)
                for tg in f.jt.values():
                    refs.update(tg)
            dead = [a for a, f in self.funcs.items()
                    if f.source == "call" and a not in refs
                    and not self.has_prologue(a, f.thumb)]
            if not dead:
                return
            for a in dead:
                del self.funcs[a]

    def resolve_overlaps(self):
        """Drop functions that start inside another function's instructions."""
        owner = {}
        for a in sorted(self.funcs):
            f = self.funcs[a]
            for ia, ins in f.insns.items():
                owner.setdefault(ia, a)
        for a in list(self.funcs):
            o = owner.get(a)
            if o is not None and o != a and self.funcs[a].source in ("pointer", "gap"):
                del self.funcs[a]


def module_seeds(rom, name):
    seeds = []
    if name == "arm9":
        seeds.append((rom.arm9_entry, False, "entry"))
        if rom.arm9_autoload_cb:
            seeds.append((rom.arm9_autoload_cb & ~1, bool(rom.arm9_autoload_cb & 1),
                          "entry"))
    elif name == "arm7":
        seeds.append((rom.arm7_entry, False, "entry"))
        if rom.arm7_autoload_cb:
            seeds.append((rom.arm7_autoload_cb & ~1, bool(rom.arm7_autoload_cb & 1),
                          "entry"))
    else:
        ov = rom.overlay(int(name[2:]))
        view = rom.module_segment(name)
        a = ov.sinit
        while a < ov.sinit_end:
            data, base = view.lookup(a)
            if data is None:
                break
            v = struct.unpack_from("<I", data, a - base)[0]
            if v:
                seeds.append((v & ~1, bool(v & 1), "sinit"))
            a += 4
        # The overlay's text starts at its load address.
        m = Module(rom, name)
        if ov.data:
            if m.has_prologue(ov.ram, True, weak=True):
                seeds.append((ov.ram, True, "entry"))
            elif m.has_prologue(ov.ram, False, weak=True):
                seeds.append((ov.ram, False, "entry"))
            elif m.plausible(ov.ram, True, 64):
                seeds.append((ov.ram, True, "entry"))
            elif m.plausible(ov.ram, False, 64):
                seeds.append((ov.ram, False, "entry"))
    return seeds


def overlay_hints(rom, name, words):
    ov = rom.overlay(int(name[2:]))
    lo, hi = ov.ram, ov.ram + len(ov.data)
    hints = [w for w in words if lo <= (w & ~1) < hi]
    for other in rom.overlays:
        if other.id == ov.id:
            continue
        if other.ram < hi and lo < other.ram + len(other.data):
            continue        # never resident together
        d = other.data
        for off in range(0, len(d) - 3, 4):
            w = struct.unpack_from("<I", d, off)[0]
            if lo <= (w & ~1) < hi:
                hints.append(w)
    return hints


def discover_all(rom, modules=None):
    """Run every module. The static goes first; then each overlay, with the
    pointer-like words of the static and of every overlay that can be
    resident beside it as hints (validated in the overlay: a prologue, or a
    function boundary that decodes to a return). A second round adds the
    call targets the first round found going into each overlay from the
    static and from the overlays that can be resident beside it."""
    modules = list(modules or rom.modules())
    want_ov = [n for n in modules if n.startswith("ov")]
    out = {}
    static = Module(rom, "arm9")
    static.run(module_seeds(rom, "arm9"))
    out["arm9"] = static
    words = []
    insn = static.insn_bytes(lits=False)
    for seg in static.view.segs:
        for off in range(0, len(seg.data) - 3, 4):
            if seg.ram + off not in insn:
                words.append(struct.unpack_from("<I", seg.data, off)[0])
    calls = [ta | (1 if th else 0) for (_f, ta, th) in static.external_calls]
    # every overlay takes part in the call census, so the second round sees
    # calls from overlays the caller did not ask about
    first = {}
    for ov in rom.overlays:
        name = "ov%03d" % ov.id
        m = Module(rom, name)
        m.run(module_seeds(rom, name), hints=overlay_hints(rom, name, words) + calls)
        first[name] = m
    xcalls = {}
    for name, m in first.items():
        ov = rom.overlay(int(name[2:]))
        for (_f, ta, th) in m.external_calls:
            for other in rom.overlays:
                if other.id == ov.id:
                    continue
                if not (other.ram <= ta < other.ram + len(other.data)):
                    continue
                if other.ram < ov.ram + len(ov.data) and ov.ram < other.ram + len(other.data):
                    continue
                xcalls.setdefault("ov%03d" % other.id, []).append(ta | (1 if th else 0))
    for name in want_ov:
        m = first[name]
        extra = [c for c in xcalls.get(name, []) if (c & ~1) not in m.funcs]
        if extra:
            m.run([], hints=extra)
        out[name] = m
    for name in modules:
        if name not in out:
            m = Module(rom, name)
            m.run(module_seeds(rom, name))
            out[name] = m
    if "arm9" not in modules:
        del out["arm9"]
    return out
