"""
ARMv5TE disassembler (ARM and Thumb) for the NDS ARM9/ARM7, written for
ndsrec from the public ARM Architecture Reference Manual (ARM DDI 0100).

The text is the pre-UAL "divided" syntax the pret disassemblies use and
games/platinum/tools/armrec/armrec.py parses: condition before the size or
mode suffix (`ldrneb`, `ldmeqia`, `addeqs`), Thumb flag-setting forms spelled
without an S (`add r4, r0, #0x0`, `mov r0, #0x1`, `lsl r1, r1, #0x10`),
register lists with ranges, every immediate in hex.

decode(word, addr, thumb, next_half) returns an Ins whose `text` may hold one
`{T}` placeholder: the operand that names an address (a branch or call
target, a literal-pool slot, an ADR target). The emitter substitutes a label
there; everything else about the instruction is final.
"""

REGS = ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10",
        "r11", "r12", "sp", "lr", "pc"]
CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge",
         "lt", "gt", "le", "", "nv"]
DP = ["and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "tst", "teq",
      "cmp", "cmn", "orr", "mov", "bic", "mvn"]
SHIFTS = ["lsl", "lsr", "asr", "ror"]


class Ins(object):
    """One decoded instruction.

    kind:   'op' plain, 'b' branch, 'call' bl/blx immediate, 'ret' return,
            'ijump' indirect jump (bx rN, mov pc, rN, ldr pc...), 'icall'
            indirect call (blx rN), 'jt_arm' (addcc pc, pc, rN, lsl #2),
            'jt_thumb' (add pc, rN), 'swi', 'invalid'
    target: branch/call target address (call: with bit 0 = Thumb target)
    lit:    address of a pc-relative literal word this instruction loads
    adr:    address an `adr` materialises
    cond:   True when the instruction is conditional
    """
    __slots__ = ("addr", "size", "thumb", "text", "kind", "target", "lit",
                 "adr", "cond", "word", "lit_size", "reads_pc_base")

    def __init__(self, addr, size, thumb, text, kind="op", target=None,
                 lit=None, cond=False, word=0):
        self.addr = addr
        self.size = size
        self.thumb = thumb
        self.text = text
        self.kind = kind
        self.target = target
        self.lit = lit
        self.adr = None
        self.cond = cond
        self.word = word
        self.lit_size = 4
        self.reads_pc_base = False

    @property
    def ends_flow(self):
        """No fall-through to the next instruction."""
        if self.cond:
            return False
        return self.kind in ("b", "ret", "ijump", "jt_thumb", "invalid")

    def __repr__(self):
        return "Ins(%08X %s %s)" % (self.addr, "T" if self.thumb else "A",
                                    self.text)


def h(v):
    return "#0x%x" % v


def reglist(mask, spaced=False):
    out = []
    i = 0
    while i < 16:
        if mask & (1 << i):
            j = i
            while j + 1 < 13 and mask & (1 << (j + 1)):
                j += 1
            if j - i >= 1 and i < 13:
                out.append("%s-%s" % (REGS[i], REGS[j]))
            else:
                out.append(REGS[i])
            i = j + 1
        else:
            i += 1
    return "{" + (", " if spaced else ",").join(out) + "}"


def ror32(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF if n else v


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


# --------------------------------------------------------------------- ARM

def _shift_imm(w):
    rm = REGS[w & 0xF]
    typ = (w >> 5) & 3
    amt = (w >> 7) & 0x1F
    if typ == 0 and amt == 0:
        return rm
    if typ == 3 and amt == 0:
        return "%s, rrx" % rm
    if typ in (1, 2) and amt == 0:
        amt = 32
    return "%s, %s %s" % (rm, SHIFTS[typ], h(amt))


def _op2(w):
    if w & (1 << 25):
        rot = (w >> 8) & 0xF
        return h(ror32(w & 0xFF, rot * 2))
    if w & 0x10:
        return "%s, %s %s" % (REGS[w & 0xF], SHIFTS[(w >> 5) & 3],
                              REGS[(w >> 8) & 0xF])
    return _shift_imm(w)


def decode_arm(w, addr):
    c = w >> 28
    cs = CONDS[c]
    cond = c != 14
    I = Ins(addr, 4, False, None, cond=cond, word=w)

    def bad():
        I.text = ".word 0x%08x" % w
        I.kind = "invalid"
        I.cond = False
        return I

    if c == 15:
        if (w & 0x0E000000) == 0x0A000000:      # BLX immediate
            off = sext(w & 0xFFFFFF, 24) << 2
            t = (addr + 8 + off + ((w >> 23) & 2)) & 0xFFFFFFFF
            I.text = "blx {T}"
            I.kind = "call"
            I.target = t | 1
            I.cond = False
            return I
        return bad()
    op = (w >> 25) & 7
    if op in (0, 1):
        if op == 0 and (w & 0x90) == 0x90:
            # multiplies, swap, extra loads/stores
            if (w & 0x0F0000F0) == 0x00000090 and not (w & 0x01000000):
                sub = (w >> 21) & 7
                s = "s" if w & (1 << 20) else ""
                rd, rn, rs, rm = ((w >> 16) & 0xF, (w >> 12) & 0xF,
                                  (w >> 8) & 0xF, w & 0xF)
                if sub == 0:
                    I.text = "mul%s%s %s, %s, %s" % (cs, s, REGS[rd], REGS[rm], REGS[rs])
                elif sub == 1:
                    I.text = "mla%s%s %s, %s, %s, %s" % (cs, s, REGS[rd], REGS[rm],
                                                        REGS[rs], REGS[rn])
                elif sub >= 4:
                    nm = ["umull", "umlal", "smull", "smlal"][sub - 4]
                    I.text = "%s%s%s %s, %s, %s, %s" % (nm, cs, s, REGS[rn], REGS[rd],
                                                       REGS[rm], REGS[rs])
                else:
                    return bad()
                return I
            if (w & 0x0FB00FF0) == 0x01000090:
                b = "b" if w & (1 << 22) else ""
                I.text = "swp%s%s %s, %s, [%s]" % (cs, b, REGS[(w >> 12) & 0xF],
                                                  REGS[w & 0xF], REGS[(w >> 16) & 0xF])
                return I
            sh = (w >> 5) & 3
            if sh == 0:
                return bad()
            P, U, Ib, W, L = ((w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1,
                              (w >> 21) & 1, (w >> 20) & 1)
            rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
            if sh == 1:
                nm = "ldr%sh" if L else "str%sh"
            elif sh == 2:
                nm = "ldr%ssb" if L else "ldr%sd"
            else:
                nm = "ldr%ssh" if L else "str%sd"
            nm = nm % cs
            if Ib:
                off = ((w >> 4) & 0xF0) | (w & 0xF)
                offs = "#%s0x%x" % ("" if U else "-", off)
            else:
                offs = "%s%s" % ("" if U else "-", REGS[w & 0xF])
            if rn == 15 and Ib and P and not W:
                tgt = (addr + 8 + (off if U else -off)) & 0xFFFFFFFF
                I.text = "%s %s, [pc, %s]" % (nm, REGS[rd], offs)
                I.reads_pc_base = True
                I.lit = tgt
                I.lit_size = 2 if sh == 1 or sh == 3 else (1 if sh == 2 else 4)
                if not L or sh == 2 and not L:
                    I.lit = None
                return I
            if P:
                I.text = "%s %s, [%s, %s]%s" % (nm, REGS[rd], REGS[rn], offs,
                                                "!" if W else "")
            else:
                I.text = "%s %s, [%s], %s" % (nm, REGS[rd], REGS[rn], offs)
            if L and rd == 15:
                I.kind = "ijump"
            return I
        opc = (w >> 21) & 0xF
        S = (w >> 20) & 1
        if 8 <= opc <= 11 and not S:
            # miscellaneous instructions
            if op == 1:
                if opc in (9, 11):          # MSR immediate
                    psr = "spsr" if opc == 11 else "cpsr"
                    f = "".join(ch for i, ch in enumerate("cxsf")
                                if w & (1 << (16 + i)))
                    I.text = "msr%s %s_%s, %s" % (cs, psr, f,
                                                  h(ror32(w & 0xFF, ((w >> 8) & 0xF) * 2)))
                    return I
                return bad()
            if (w & 0x0FBF0FFF) == 0x010F0000:
                I.text = "mrs%s %s, %s" % (cs, REGS[(w >> 12) & 0xF],
                                           "spsr" if w & (1 << 22) else "cpsr")
                return I
            if (w & 0x0FB0FFF0) == 0x0120F000:
                f = "".join(ch for i, ch in enumerate("cxsf") if w & (1 << (16 + i)))
                I.text = "msr%s %s_%s, %s" % (cs, "spsr" if w & (1 << 22) else "cpsr",
                                              f, REGS[w & 0xF])
                return I
            if (w & 0x0FFFFFF0) == 0x012FFF10:
                rm = w & 0xF
                I.text = "bx%s %s" % (cs, REGS[rm])
                I.kind = "ret" if rm == 14 else "ijump"
                return I
            if (w & 0x0FFFFFF0) == 0x012FFF30:
                I.text = "blx%s %s" % (cs, REGS[w & 0xF])
                I.kind = "icall"
                return I
            if (w & 0x0FFF0FF0) == 0x016F0F10:
                I.text = "clz%s %s, %s" % (cs, REGS[(w >> 12) & 0xF], REGS[w & 0xF])
                return I
            if (w & 0x0F900FF0) == 0x01000050:
                nm = ["qadd", "qsub", "qdadd", "qdsub"][(w >> 21) & 3]
                I.text = "%s%s %s, %s, %s" % (nm, cs, REGS[(w >> 12) & 0xF],
                                              REGS[w & 0xF], REGS[(w >> 16) & 0xF])
                return I
            if (w & 0x0F900090) == 0x01000080:
                o = (w >> 21) & 3
                x = "t" if w & 0x20 else "b"
                y = "t" if w & 0x40 else "b"
                rd, rn, rs, rm = ((w >> 16) & 0xF, (w >> 12) & 0xF,
                                  (w >> 8) & 0xF, w & 0xF)
                if o == 0:
                    I.text = "smla%s%s%s %s, %s, %s, %s" % (x, y, cs, REGS[rd], REGS[rm],
                                                           REGS[rs], REGS[rn])
                elif o == 1:
                    if w & 0x20:
                        I.text = "smulw%s%s %s, %s, %s" % (y, cs, REGS[rd], REGS[rm], REGS[rs])
                    else:
                        I.text = "smlaw%s%s %s, %s, %s, %s" % (y, cs, REGS[rd], REGS[rm],
                                                              REGS[rs], REGS[rn])
                elif o == 2:
                    I.text = "smlal%s%s%s %s, %s, %s, %s" % (x, y, cs, REGS[rn], REGS[rd],
                                                            REGS[rm], REGS[rs])
                else:
                    I.text = "smul%s%s%s %s, %s, %s" % (x, y, cs, REGS[rd], REGS[rm],
                                                       REGS[rs])
                return I
            if (w & 0xFFF000F0) == 0xE1200070:
                I.text = "bkpt %s" % h(((w >> 4) & 0xFFF0) | (w & 0xF))
                return I
            return bad()
        if op == 0 and (w & 0x10) and (w & 0x80):
            return bad()
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        nm = DP[opc]
        o2 = _op2(w)
        s = "s" if S else ""
        if opc in (8, 9, 10, 11):
            I.text = "%s%s %s, %s" % (nm, cs, REGS[rn], o2)
        elif opc in (13, 15):
            I.text = "%s%s%s %s, %s" % (nm, cs, s, REGS[rd], o2)
            if opc == 13 and w == 0xE1A00000:
                I.text = "mov r0, r0"
        else:
            I.text = "%s%s%s %s, %s, %s" % (nm, cs, s, REGS[rd], REGS[rn], o2)
            if (opc == 4 and rd == 15 and rn == 15 and not (w & (1 << 25))
                    and not (w & 0x10) and ((w >> 5) & 3) == 0
                    and ((w >> 7) & 0x1F) == 2):
                I.kind = "jt_arm"
                return I
            if opc in (2, 4) and rn == 15 and (w & (1 << 25)) and rd != 15:
                # adr: add/sub rd, pc, #imm
                v = ror32(w & 0xFF, ((w >> 8) & 0xF) * 2)
                I.adr = (addr + 8 + (v if opc == 4 else -v)) & 0xFFFFFFFF
                if not S and not cond:
                    I.text = "adr %s, {T}" % REGS[rd]
                return I
        if rd == 15 and opc in (0, 1, 3, 5, 6, 7, 12, 14, 15):
            # and/eor/rsb/adc/sbc/rsc/orr/bic/mvn into pc: never compiler
            # output; data decoded as code
            return bad()
        if rd == 15 and opc not in (8, 9, 10, 11):
            if opc == 13 and (w & 0xFFF) == 0x00E and not (w & (1 << 25)):
                I.kind = "ret"
            else:
                I.kind = "ijump"
        if rn == 15 and opc not in (13, 15):
            I.reads_pc_base = True
        if (w & 0xF) == 15 and not (w & (1 << 25)):
            I.reads_pc_base = True
        return I
    if op in (2, 3):
        if op == 3 and (w & 0x10):
            return bad()
        P, U, B, W, L = ((w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1,
                         (w >> 21) & 1, (w >> 20) & 1)
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        if not P and W:
            nm = ("ldr" if L else "str") + cs + ("bt" if B else "t")
            I.text = nm + " ?"
            return bad()
        nm = ("ldr" if L else "str") + cs + ("b" if B else "")
        if op == 2:
            off = w & 0xFFF
            if rn == 15 and P and not W:
                tgt = (addr + 8 + (off if U else -off)) & 0xFFFFFFFF
                I.reads_pc_base = True
                if L and not B:
                    I.text = "%s %s, {T}" % (nm, REGS[rd])
                    I.lit = tgt
                    if rd == 15:
                        I.kind = "ijump"
                    return I
                I.text = "%s %s, [pc, #%s0x%x]" % (nm, REGS[rd], "" if U else "-", off)
                if L:
                    I.lit = tgt
                    I.lit_size = 1
                return I
            offs = "#%s0x%x" % ("" if U else "-", off)
        else:
            offs = ("" if U else "-") + _shift_imm(w)
        if P:
            I.text = "%s %s, [%s, %s]%s" % (nm, REGS[rd], REGS[rn], offs, "!" if W else "")
        else:
            I.text = "%s %s, [%s], %s" % (nm, REGS[rd], REGS[rn], offs)
        if L and rd == 15:
            if rn == 13 and not P and U and (w & 0xFFF) == 4 and op == 2:
                I.kind = "ret"
            else:
                I.kind = "ijump"
        return I
    if op == 4:
        P, U, S, W, L = ((w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1,
                         (w >> 21) & 1, (w >> 20) & 1)
        rn = (w >> 16) & 0xF
        mask = w & 0xFFFF
        if mask == 0:
            return bad()
        mode = ["da", "ia", "db", "ib"][(P << 1) | U]
        I.text = "%s%s%s %s%s, %s%s" % ("ldm" if L else "stm", cs, mode, REGS[rn],
                                        "!" if W else "", reglist(mask),
                                        "^" if S else "")
        if L and mask & 0x8000:
            I.kind = "ret" if rn == 13 else "ijump"
        return I
    if op == 5:
        off = sext(w & 0xFFFFFF, 24) << 2
        t = (addr + 8 + off) & 0xFFFFFFFF
        if w & (1 << 24):
            I.text = "bl%s {T}" % cs
            I.kind = "call"
            I.target = t
        else:
            I.text = "b%s {T}" % cs
            I.kind = "b"
            I.target = t
        return I
    if op == 7:
        if w & (1 << 24):
            I.text = "swi%s 0x%x" % (cs, w & 0xFFFFFF)
            I.kind = "swi"
            return I
        if w & 0x10:
            L = (w >> 20) & 1
            cp = (w >> 8) & 0xF
            I.text = "%s%s p%d, %d, %s, c%d, c%d, %d" % (
                "mrc" if L else "mcr", cs, cp, (w >> 21) & 7, REGS[(w >> 12) & 0xF],
                (w >> 16) & 0xF, w & 0xF, (w >> 5) & 7)
            if cp != 15:
                return bad()
            return I
        return bad()
    return bad()


# ------------------------------------------------------------------- Thumb

def decode_thumb(hw, addr, nxt=None):
    I = Ins(addr, 2, True, None, word=hw)

    def bad():
        I.text = ".short 0x%04x" % hw
        I.kind = "invalid"
        return I

    top = hw >> 11
    rd = hw & 7
    rs = (hw >> 3) & 7
    if top in (0, 1, 2):                                   # shift imm
        amt = (hw >> 6) & 0x1F
        if top != 0 and amt == 0:
            amt = 32
        I.text = "%s %s, %s, %s" % (SHIFTS[top], REGS[rd], REGS[rs], h(amt))
        return I
    if top == 3:                                           # add/sub
        sub = hw & (1 << 9)
        nm = "sub" if sub else "add"
        v = (hw >> 6) & 7
        if hw & (1 << 10):
            I.text = "%s %s, %s, %s" % (nm, REGS[rd], REGS[rs], h(v))
        else:
            I.text = "%s %s, %s, %s" % (nm, REGS[rd], REGS[rs], REGS[v])
        return I
    if top in (4, 5, 6, 7):                                # imm8
        r = (hw >> 8) & 7
        I.text = "%s %s, %s" % (["mov", "cmp", "add", "sub"][top - 4], REGS[r],
                                h(hw & 0xFF))
        return I
    if (hw >> 10) == 0x10:                                 # ALU
        o = (hw >> 6) & 0xF
        nm = ["and", "eor", "lsl", "lsr", "asr", "adc", "sbc", "ror", "tst",
              "neg", "cmp", "cmn", "orr", "mul", "bic", "mvn"][o]
        I.text = "%s %s, %s" % (nm, REGS[rd], REGS[rs])
        return I
    if (hw >> 10) == 0x11:                                 # hi regs / bx
        o = (hw >> 8) & 3
        d = rd | ((hw >> 4) & 8)
        m = (hw >> 3) & 0xF
        if o == 3:
            if hw & 0x80:
                I.text = "blx %s" % REGS[m]
                I.kind = "icall"
                if hw & 7:
                    return bad()
            else:
                I.text = "bx %s" % REGS[m]
                I.kind = "ret" if m == 14 else "ijump"
            return I
        if not (hw & 0xC0) and o != 3:
            # low-low hi-op: unpredictable before v6 except the nop
            pass
        if o == 2 and d == 8 and m == 8:
            I.text = "nop"
            return I
        nm = ["add", "cmp", "mov"][o]
        I.text = "%s %s, %s" % (nm, REGS[d], REGS[m])
        if d == 15 and o == 0:
            I.kind = "jt_thumb"
        elif d == 15 and o == 2:
            I.kind = "ret" if m == 14 else "ijump"
        if m == 15:
            I.reads_pc_base = True
        return I
    if top == 9:                                           # ldr pc-rel
        r = (hw >> 8) & 7
        I.lit = ((addr + 4) & ~3) + (hw & 0xFF) * 4
        I.text = "ldr %s, {T}" % REGS[r]
        return I
    if (hw >> 12) == 5:                                    # ld/st reg off
        o = (hw >> 9) & 7
        nm = ["str", "strh", "strb", "ldrsb", "ldr", "ldrh", "ldrb", "ldrsh"][o]
        I.text = "%s %s, [%s, %s]" % (nm, REGS[rd], REGS[rs], REGS[(hw >> 6) & 7])
        return I
    if (hw >> 13) == 3:                                    # ld/st imm5
        B = (hw >> 12) & 1
        L = (hw >> 11) & 1
        off = ((hw >> 6) & 0x1F) * (1 if B else 4)
        nm = ("ldr" if L else "str") + ("b" if B else "")
        I.text = "%s %s, [%s, %s]" % (nm, REGS[rd], REGS[rs], h(off))
        return I
    if (hw >> 12) == 8:                                    # ldrh/strh imm5
        L = (hw >> 11) & 1
        off = ((hw >> 6) & 0x1F) * 2
        I.text = "%s %s, [%s, %s]" % ("ldrh" if L else "strh", REGS[rd], REGS[rs], h(off))
        return I
    if (hw >> 12) == 9:                                    # sp-rel
        L = (hw >> 11) & 1
        I.text = "%s %s, [sp, %s]" % ("ldr" if L else "str", REGS[(hw >> 8) & 7],
                                      h((hw & 0xFF) * 4))
        return I
    if (hw >> 12) == 10:                                   # add pc/sp
        r = REGS[(hw >> 8) & 7]
        v = (hw & 0xFF) * 4
        if hw & (1 << 11):
            I.text = "add %s, sp, %s" % (r, h(v))
        else:
            I.adr = ((addr + 4) & ~3) + v
            I.text = "adr %s, {T}" % r
        return I
    if (hw >> 8) == 0xB0:                                  # sp adjust
        v = (hw & 0x7F) * 4
        I.text = "%s sp, %s" % ("sub" if hw & 0x80 else "add", h(v))
        return I
    if (hw & 0xF600) == 0xB400:                            # push/pop
        L = (hw >> 11) & 1
        R = (hw >> 8) & 1
        mask = hw & 0xFF
        if R:
            mask |= 1 << (15 if L else 14)
        if not mask:
            return bad()
        I.text = "%s %s" % ("pop" if L else "push", reglist(mask, spaced=True))
        if L and R:
            I.kind = "ret"
        return I
    if (hw >> 8) == 0xBE:
        I.text = "bkpt %s" % h(hw & 0xFF)
        return I
    if (hw >> 12) == 0xC:                                  # ldmia/stmia
        L = (hw >> 11) & 1
        rn = (hw >> 8) & 7
        mask = hw & 0xFF
        if not mask:
            return bad()
        wb = "" if (L and mask & (1 << rn)) else "!"
        I.text = "%s %s%s, %s" % ("ldmia" if L else "stmia", REGS[rn], wb,
                                  reglist(mask, spaced=True))
        return I
    if (hw >> 12) == 0xD:                                  # cond branch / swi
        c = (hw >> 8) & 0xF
        if c == 15:
            I.text = "swi 0x%x" % (hw & 0xFF)
            I.kind = "swi"
            return I
        if c == 14:
            return bad()
        I.target = (addr + 4 + (sext(hw & 0xFF, 8) << 1)) & 0xFFFFFFFF
        I.text = "b%s {T}" % CONDS[c]
        I.kind = "b"
        I.cond = True
        return I
    if top == 0x1C:                                        # b
        I.target = (addr + 4 + (sext(hw & 0x7FF, 11) << 1)) & 0xFFFFFFFF
        I.text = "b {T}"
        I.kind = "b"
        return I
    if top == 0x1E:                                        # bl/blx prefix
        if nxt is None:
            return bad()
        n2 = nxt >> 11
        if n2 not in (0x1F, 0x1D):
            return bad()
        off = (sext(hw & 0x7FF, 11) << 12) | ((nxt & 0x7FF) << 1)
        t = (addr + 4 + off) & 0xFFFFFFFF
        I.size = 4
        I.word = hw | (nxt << 16)
        I.kind = "call"
        if n2 == 0x1F:
            I.text = "bl {T}"
            I.target = t | 1
        else:
            if nxt & 1:
                return bad()
            I.text = "blx {T}"
            I.target = t & ~3
        return I
    return bad()


def decode(mem, off, addr, thumb):
    """Decode at byte offset `off` of buffer `mem` (guest address `addr`)."""
    if thumb:
        hw = mem[off] | (mem[off + 1] << 8)
        nxt = None
        if off + 3 < len(mem):
            nxt = mem[off + 2] | (mem[off + 3] << 8)
        return decode_thumb(hw, addr, nxt)
    w = mem[off] | (mem[off + 1] << 8) | (mem[off + 2] << 16) | (mem[off + 3] << 24)
    return decode_arm(w, addr)
