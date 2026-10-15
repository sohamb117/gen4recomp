"""bw_derive.py - derive Black/White's script command layouts from the recompiler's generated assembly.

Used by `bw_script.py derive`. Input: the opcode -> handler table read from the ROM (overlay 10) and the
generated Thumb assembly with addresses (games/ndsrec/build/pc-wasm/<game>/ndsrec/asm/arm9: one `.s` per chunk,
labels `name: ; 0xADDR`, literals `_x: .word V ; 0xADDR`, jump tables as `.byte` pairs after `add pc, rN`).

For each handler, every path is walked (loops cut after two visits, jump tables expanded, tail calls and
`bx` to literal addresses followed, callees summarised recursively) tracking which registers and stack slots
hold the script context (r0 on entry) and its script pointer (ctx+0x14). The reads a path makes, in order,
are its layout: calls with the context in r0 to the read primitives (PRIMS), inline ldrb/ldrh of the script
pointer, and a u32 read added to the script pointer (a code offset). Wait callbacks (sub_020113D0 with a
literal function in r1) count as part of the command: they read their operands when they finish.
"""
import re, glob, struct

REG = re.compile(r'\b(r\d+|sb|sl|fp|ip|lr|sp|pc)\b')
CONDB = {'beq', 'bne', 'bcs', 'bcc', 'bmi', 'bpl', 'bvs', 'bvc', 'bhi', 'bls', 'bge', 'blt', 'bgt', 'ble', 'bhs', 'blo'}


def space_of(f):
    m = re.search(r'/overlays/(\d+)/', f)
    return 'ov%d' % int(m.group(1)) if m else 'arm9'


class Asm:
    def __init__(self, root, spaces=('arm9', 'ov10', 'ov21')):
        self.ins = {}
        self.lab = {}
        self.funcs = {}
        files = []
        for sp in spaces:
            files += sorted(glob.glob(root + ('/asm/*.s' if sp == 'arm9' else '/overlays/%d/asm/*.s' % int(sp[2:]))))
        for f in files:
            sp = space_of(f)
            addr = None
            mode = 'thumb'
            for l in open(f):
                l = l.rstrip('\n')
                if 'arm_func_start' in l:
                    mode = 'arm'
                elif 'thumb_func_start' in l:
                    mode = 'thumb'
                m = re.match(r'^([A-Za-z_]\w*): ; 0x([0-9A-F]+)', l)
                if m:
                    addr = int(m.group(2), 16)
                    self.lab[(sp, m.group(1))] = addr
                    if not m.group(1).startswith('_'):
                        self.funcs.setdefault(addr, []).append(sp)
                    continue
                m = re.match(r'^([A-Za-z_]\w*): \.word (\S+) ; 0x([0-9A-F]+)', l)
                if m:
                    a = int(m.group(3), 16)
                    self.lab[(sp, m.group(1))] = a
                    self.ins[(sp, a)] = ('.word', m.group(2))
                    addr = a + 4
                    continue
                if addr is None:
                    continue
                s = l.strip()
                if not s or s.startswith(';'):
                    continue
                if s.startswith('.byte'):
                    self.ins[(sp, addr)] = ('.byte', s[5:].strip())
                    addr += len(s[5:].split(','))
                    continue
                if s.startswith('.word'):
                    self.ins[(sp, addr)] = ('.word', s[5:].strip())
                    addr += 4
                    continue
                if s.startswith('.'):
                    continue
                parts = s.split(None, 1)
                mn = parts[0]
                ops = parts[1].split(';')[0].strip() if len(parts) > 1 else ''
                self.ins[(sp, addr)] = (mn, ops, mode)
                addr += 4 if (mode == 'arm' or (mn in ('bl', 'blx') and not re.match(r'r\d+$', ops))) else 2

    def func_at(self, addr, prefer=None):
        sps = self.funcs.get(addr, [])
        for s in ([prefer] if prefer else []) + ['ov10', 'ov21', 'arm9'] + sps:
            if s in sps:
                return (s, addr)
        return None

    def resolve(self, sp, name):
        m = re.match(r'(ov(\d+)|sub)_([0-9A-F]{8})$', name)
        if m:
            s = 'arm9' if m.group(1) == 'sub' else 'ov%d' % int(m.group(2))
            return (s, int(m.group(3), 16))
        a = self.lab.get((sp, name))
        return (sp, a) if a is not None else None


class Deriver:
    """Path-wise read-effect analysis of script command handlers.

    Effects: h=u16 (ScriptReadU16), w=u32, o=u32 code offset (added to the script pointer),
    p=u16 var id (var pointer), v=u16 var-or-value, b=u8 inline read."""

    def __init__(self, asm, prims, waits=()):
        self.asm = asm
        self.waits = set(waits)
        self.prims = prims    # (space, addr) -> effect letter (consumes with ctx in r0)
        self.summ = {}

    def analyze(self, fn, depth=0, maxpaths=4000):
        if fn in self.summ:
            return self.summ[fn]
        self.summ[fn] = None
        results = set()
        npaths = 0
        stack = [(fn[1], {'r0': 'ctx'}, (), {})]
        sp = fn[0]
        while stack:
            pc, tags, seq, visits = stack.pop()
            tags = dict(tags)
            visits = dict(visits)
            while True:
                ins = self.asm.ins.get((sp, pc))
                if ins is None:
                    results.add(seq + ('?END',))
                    break
                v = visits.get(pc, 0)
                if v >= 2:
                    break
                visits[pc] = v + 1
                if ins[0] in ('.word', '.byte'):
                    results.add(seq + ('?DATA',))
                    break
                mn, ops, mode = ins
                if mode == 'arm':
                    results.add(seq)  # ARM-mode code (callbacks, DTCM helpers) never reads the script
                    break
                regs = REG.findall(ops)
                dst = regs[0] if regs else None
                nxt = pc + (4 if mn in ('bl', 'blx') and not re.match(r'r\d+$', ops) else 2)
                if mn in ('bx', 'blx') and isinstance(tags.get(ops), tuple) and tags[ops][0] == 'lit':
                    tgt = self.func_at(tags[ops][1] & ~1, sp)
                    eff = self.call_effect(tgt, tags.get('r0'), depth) if tgt else ()
                    if eff is None:
                        results.add(seq + ('?REC',))
                        break
                    if mn == 'bx':
                        results.add(seq + eff)
                        break
                    for r in ('r0', 'r1', 'r2', 'r3'):
                        tags.pop(r, None)
                    seq = seq + eff
                    pc = nxt
                    continue
                if (mn == 'pop' and 'pc' in ops) or mn == 'bx' or (mn == 'mov' and dst == 'pc'):
                    results.add(seq)
                    break
                if mn == 'ldr' and re.match(r'r\d+, _\w+$', ops):
                    a = self.asm.lab.get((sp, ops.split(', ')[1]))
                    w = self.asm.ins.get((sp, a)) if a is not None else None
                    if w and w[0] == '.word' and w[1].startswith('0x'):
                        tags[dst] = ('lit', int(w[1], 16))
                    else:
                        tags.pop(dst, None)
                    pc = nxt
                    continue
                if mn == 'add' and dst == 'pc':
                    t = pc + 4      # Thumb: pc reads as this instruction + 4; the table starts right after it
                    a = nxt
                    ents = []
                    while self.asm.ins.get((sp, a), ('',))[0] == '.byte':
                        b = [int(x, 16) for x in self.asm.ins[(sp, a)][1].split(',')]
                        if len(b) != 2:
                            break
                        val = b[0] | b[1] << 8
                        if val >= 0x8000:
                            val -= 0x10000
                        ents.append(t + val)
                        a += 2
                        if a in ents:
                            break
                    for e in set(ents):
                        stack.append((e, tags, seq, visits))
                    break
                if mn in CONDB:
                    tgt = self.asm.resolve(sp, ops)
                    stack.append((tgt[1], tags, seq, visits))
                    pc = nxt
                    continue
                if mn == 'b':
                    tgt = self.asm.resolve(sp, ops)
                    if not ops.startswith('_'):
                        eff = self.call_effect(tgt, tags.get('r0'), depth)
                        results.add(seq + (eff if eff is not None else ('?REC',)))
                        break
                    pc = tgt[1]
                    continue
                if mn in ('bl', 'blx'):
                    eff = ()
                    if re.match(r'(ov\d+|sub)_[0-9A-F]{8}$', ops):
                        tgt = self.asm.resolve(sp, ops)
                        eff = self.call_effect(tgt, tags.get('r0'), depth)
                        cb = tags.get('r1')
                        if (eff is not None and tgt in self.waits and tags.get('r0') == 'ctx'
                                and isinstance(cb, tuple) and cb[0] == 'lit'):
                            # a wait callback runs every frame until it returns TRUE; the frames that only
                            # poll read nothing, the finishing one reads its operands
                            cr = self.analyze(self.func_at(cb[1] & ~1, sp), depth + 1)
                            cr = {x for x in (cr or ()) if x and x[-1] != '?DATA'}
                            eff = eff + (next(iter(cr)) if len(cr) == 1 else ('?MULTI',) if cr else ())
                        if eff is None:
                            results.add(seq + ('?REC',))
                            break
                    for r in ('r0', 'r1', 'r2', 'r3'):
                        tags.pop(r, None)
                    if eff:
                        if eff == ('w',):
                            tags['r0'] = ('w', len(seq))
                        seq = seq + eff
                    pc = nxt
                    continue
                ms = re.match(r'(r\d+), \[sp, #(0x[0-9a-f]+)\]$', ops)
                if ms and mn == 'str':
                    if ms.group(1) in tags:
                        tags['sp' + ms.group(2)] = tags[ms.group(1)]
                    else:
                        tags.pop('sp' + ms.group(2), None)
                    pc = nxt
                    continue
                if ms and mn == 'ldr':
                    if 'sp' + ms.group(2) in tags:
                        tags[dst] = tags['sp' + ms.group(2)]
                    else:
                        tags.pop(dst, None)
                    pc = nxt
                    continue
                m = re.match(r'(r\d+), \[(r\d+), #(0x[0-9a-f]+)\]', ops)
                if mn == 'ldr' and m and m.group(3) == '0x14' and tags.get(m.group(2)) == 'ctx':
                    tags[dst] = 'ptr'
                    pc = nxt
                    continue
                if mn in ('ldrb', 'ldrh') and m and tags.get(m.group(2)) == 'ptr':
                    seq = seq + ('b' if mn == 'ldrb' else 'H',)
                    tags.pop(dst, None)
                    pc = nxt
                    continue
                if mn in ('mov', 'add') and re.match(r'(r\d+), (r\d+)(, #0x0)?$', ops):
                    if regs[1] in tags:
                        tags[dst] = tags[regs[1]]
                    else:
                        tags.pop(dst, None)
                    pc = nxt
                    continue
                if mn == 'add' and len(regs) == 3:
                    x, y = tags.get(regs[1]), tags.get(regs[2])
                    for p_, q_ in ((x, y), (y, x)):
                        if p_ == 'ptr' and isinstance(q_, tuple) and q_[0] == 'w':
                            seq = seq[:q_[1]] + ('o',) + seq[q_[1] + 1:]
                    tags.pop(dst, None)
                    pc = nxt
                    continue
                if mn in ('str', 'strh', 'strb', 'cmp', 'tst', 'push', 'cmn') or dst is None:
                    pc = nxt
                    continue
                tags.pop(dst, None)
                pc = nxt
            npaths += 1
            if npaths > maxpaths:
                self.summ[fn] = {('?MANY',)}
                return self.summ[fn]
        self.summ[fn] = results
        return results

    def func_at(self, addr, prefer=None):
        return self.asm.func_at(addr, prefer)

    def call_effect(self, tgt, r0tag, depth):
        if tgt in self.prims:
            return (self.prims[tgt],) if r0tag == 'ctx' else ()
        if r0tag != 'ctx' or depth > 6 or tgt is None:
            return ()
        r = self.analyze(tgt, depth + 1)
        if r is None:
            return None
        r = {x for x in r if not (x and x[-1] in ('?DATA',))} or r
        if len(r) == 1:
            return next(iter(r))
        return ('?MULTI',)


# the read primitives (called with the script context in r0), per game code: the ARM9 ones are shared,
# overlay 10's sit 0x20 higher in White
def prims(code):
    d = 0x20 if code == "IRAO" else 0
    return {("arm9", 0x02011330): "h",          # ScriptReadU16
            ("arm9", 0x0201134C): "w",          # ScriptReadU32
            ("arm9", 0x02011290): "E",          # end the script
            ("ov10", 0x02159AE8 + d): "p",      # var pointer from a u16
            ("ov10", 0x02159B10 + d): "v"}      # var-or-value from a u16


WAITS = [("arm9", 0x020113D0)]                  # set a wait callback (r1)


def _merge(paths):
    """One layout from a handler's path summaries: ignore paths that run into data, accept a u32 that some
    paths add to the script pointer (o) and others only read (w), and report whether paths end the script."""
    r = {x for x in paths if not (x and x[-1] == "?DATA")} or set(paths)
    ends = {("E" in x) for x in r}
    r = {tuple(c for c in x if c != "E") for x in r}
    if len(r) > 1:
        r2 = {tuple("o" if c == "w" else c for c in x) for x in r}
        if len(r2) == 1 and any("o" in x for x in r):
            r = r2
    flag = "E" if ends == {True} else "e" if True in ends else ""
    return r, flag


def _overlay_spaces(root, addrs):
    """The overlays whose asm defines a function at one of addrs (beyond arm9/ov10/ov21)."""
    want = {"_%08X:" % a for a in addrs}
    found = set()
    for f in sorted(glob.glob(root + "/overlays/*/asm/*.s")):
        ov = "ov%d" % int(f.split("/overlays/")[1].split("/")[0])
        if ov in ("ov10", "ov21") or ov in found:
            continue
        text = open(f).read()
        if any(w in text for w in want):
            found.add(ov)
    return sorted(found)


def derive(root, handlers, code):
    """Rows `NNN layout module_ADDR [E|e]` for every handler (`-` for null entries)."""
    asm = Asm(root, spaces=("arm9", "ov10", "ov21"))
    missing = [h & ~1 for h in handlers if h and asm.func_at(h & ~1) is None]
    if missing:
        asm = Asm(root, spaces=("arm9", "ov10", "ov21") + tuple(_overlay_spaces(root, missing)))
    der = Deriver(asm, prims(code), waits=WAITS)
    rows = []
    for op, h in enumerate(handlers):
        if not h:
            rows.append("%03X -" % op)
            continue
        fn = asm.func_at(h & ~1)
        if fn is None:
            rows.append("%03X ? %08X" % (op, h & ~1))
            continue
        r, flag = _merge(der.analyze(fn))
        lay = "".join(next(iter(r))) if len(r) == 1 else "?" + "|".join("".join(x) for x in sorted(r))
        if op == 0x64:
            lay = lay.replace("o", "m")       # ApplyMovement: the offset is a movement list
        if op == 0x1CB:
            lay = lay.replace("o", "d")       # the offset is handed to the field as data
        rows.append(("%03X %s %s_%08X %s" % (op, lay or ".", fn[0], h & ~1, flag)).rstrip())
    return rows
