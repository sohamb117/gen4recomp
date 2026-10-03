/* Integer division by zero, with the cartridge's answer instead of a signal.
 *
 * ARM946E-S has no divide instruction, so every `/` and `%` in the game is a
 * call into the compiler's runtime, and that runtime answers a zero divisor
 * rather than faulting. x86 does not: `div` and `idiv` raise #DE and the
 * process dies. The game divides by zero in at least three places a player
 * reaches, and every one was a fatal signal here and nothing at all on
 * hardware. Guarding them one at a time had already cost two runs when the
 * third turned up, so the fix is the class.
 *
 * What the cartridge answers, read out of the ROM this tree builds rather than
 * assumed. `_u32_div_f` and `_s32_div_f` take (numerator, divisor) in r0 and
 * r1 and return (quotient, remainder) in the same, and both open by testing
 * the divisor:
 *
 *     _u32_div_f:  cmp r1, #0
 *                  bxeq lr              @ r0, the numerator, is the quotient,
 *                                       @ r1, the divisor, is the remainder
 *
 * `_s32_div_f` takes absolute values first and branches to the same tail. So
 * on a DS, signed and unsigned alike:
 *
 *     a / 0 == a          a % 0 == 0
 *
 * Not zero for the quotient, which is the guess the first of those three sites
 * shipped with.
 *
 * How: the fault handler hands the faulting instruction here. If it is a `div`
 * or `idiv` whose divisor really is zero, the results the hardware would have
 * written are written by hand and the program counter is stepped over the
 * instruction. Anything else, including `idiv` overflowing on INT_MIN / -1, is
 * left to die where it is. Every distinct site is named on stderr the first
 * time it fires, because a silent fix-up is indistinguishable from a defect in
 * this port's own code.
 *
 * On ARM the same question has a better answer, and it still has to be
 * answered: glibc's __aeabi_idiv0 raises SIGFPE, so an armhf build dies where
 * the x86 build did. But there the runtime call the DS itself made is still a
 * call, so it can simply be wrapped. pc/src/pc_div0_wrap.s holds that.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pc_div0.h"

#if defined(__i386__)

#define MAX_REPORTED 16

static uint32_t sSeen[MAX_REPORTED];
static int sSeenCount;
static unsigned long sFixups;

static void report(uint32_t eip, const char *what)
{
    int i;

    sFixups++;
    for (i = 0; i < sSeenCount; i++) {
        if (sSeen[i] == eip) {
            return;
        }
    }
    if (sSeenCount == MAX_REPORTED) {
        return;
    }
    sSeen[sSeenCount++] = eip;
    fprintf(stderr, "pc_div0: %s by zero at eip=%#lx, answered the way the "
            "cartridge's runtime does (a/0 = a, a%%0 = 0)\n",
            what, (unsigned long)eip);
    fflush(stderr);
}

unsigned long pc_div0_count(void)
{
    return sFixups;
}

/* The vector suite. Every case here is the ROM's answer, taken off the
 * disassembly of _u32_div_f and _s32_div_f quoted at the top of this file,
 * an oracle rather than this port's own behaviour, which is the whole point
 * of running it.
 *
 * The divisions are inline asm, not C, and that is load-bearing: a division
 * by zero is undefined behaviour in C, so the optimizer may delete the whole
 * expression, and gcc did, volatile operands and all, leaving a suite that
 * "passed" six divisions it never performed. An asm statement is opaque to
 * the compiler, so each case is guaranteed to put a real div or idiv in the
 * text and really fault. It also buys the widths C could not reach: the
 * 16-bit (0x66-prefixed) and 8-bit (F6) forms, which the game's u8/u16
 * arithmetic can emit but integer promotion keeps out of any C expression
 * here. */
static uint32_t sMemZero;

static void udiv32_reg(uint32_t num, uint32_t den, uint32_t *q, uint32_t *r)
{
    uint32_t qq = num, rr = 0;
    __asm__ volatile("divl %2" : "+a"(qq), "+d"(rr) : "r"(den) : "cc", "memory");
    *q = qq;
    *r = rr;
}

static void idiv32_reg(int32_t num, int32_t den, int32_t *q, int32_t *r)
{
    int32_t qq = num, rr = num >> 31; /* what cdq would have put in edx */
    __asm__ volatile("idivl %2" : "+a"(qq), "+d"(rr) : "r"(den) : "cc", "memory");
    *q = qq;
    *r = rr;
}

/* The divisor as a memory operand rather than a register: the other half of
 * the ModRM decode. */
static void udiv32_mem(uint32_t num, const uint32_t *den,
                       uint32_t *q, uint32_t *r)
{
    uint32_t qq = num, rr = 0;
    __asm__ volatile("divl %2" : "+a"(qq), "+d"(rr) : "m"(*den) : "cc", "memory");
    *q = qq;
    *r = rr;
}

static void udiv16_reg(uint16_t num, uint16_t den, uint16_t *q, uint16_t *r)
{
    uint16_t qq = num, rr = 0;
    __asm__ volatile("divw %2" : "+a"(qq), "+d"(rr) : "r"(den) : "cc", "memory");
    *q = qq;
    *r = rr;
}

static void udiv8_reg(uint8_t num, uint8_t den, uint8_t *q, uint8_t *r)
{
    uint16_t ax = num; /* the 8-bit form's dividend is AX, AH already 0 */
    __asm__ volatile("divb %1" : "+a"(ax) : "q"(den) : "cc", "memory");
    *q = (uint8_t)ax;
    *r = (uint8_t)(ax >> 8);
}

int pc_div0_selftest(void)
{
    unsigned long before = sFixups;
    int ok = 1;
    uint32_t uq, ur;
    int32_t sq, sr;
    uint16_t wq, wr;
    uint8_t bq, br;

    sMemZero = 0;

    udiv32_reg(0xFFFFu, 0, &uq, &ur);
    if (uq != 0xFFFFu || ur != 0) ok = 0;
    idiv32_reg(-12345, 0, &sq, &sr);
    if (sq != -12345 || sr != 0) ok = 0;
    idiv32_reg(12345, 0, &sq, &sr);
    if (sq != 12345 || sr != 0) ok = 0;
    udiv32_mem(777, &sMemZero, &uq, &ur);
    if (uq != 777 || ur != 0) ok = 0;
    udiv16_reg(0x1234, 0, &wq, &wr);
    if (wq != 0x1234 || wr != 0) ok = 0;
    udiv8_reg(77, 0, &bq, &br);
    if (bq != 77 || br != 0) ok = 0;

    if (sFixups - before != 6) {
        fprintf(stderr, "pc-selftest div0: %lu of 6 divisions faulted\n",
                sFixups - before);
        ok = 0;
    }
    return ok;
}

/* The 8-bit and 16-bit forms exist because the DS game is full of u8 and u16
 * arithmetic; gcc promotes most of it to 32 bits, but not all. */
static uint32_t read_operand(const void *at, int width)
{
    uint32_t v = 0;
    memcpy(&v, at, (size_t)width);
    return v;
}

int pc_div0_fixup(struct pc_x86_regs *g)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)g->eip;
    const uint8_t *start = p;
    int opsize16 = 0;
    uint8_t modrm, mod, reg, rm;
    int width, is_signed;
    uint32_t divisor;

    /* Prefixes. A segment override would move the operand somewhere this
     * cannot follow, and an address-size override changes the whole ModRM
     * encoding, so both bail out rather than guess. */
    for (;;) {
        if (*p == 0x66) {
            opsize16 = 1;
            p++;
        } else if (*p == 0xF0 || *p == 0xF2 || *p == 0xF3) {
            p++;
        } else {
            break;
        }
    }

    if (*p != 0xF6 && *p != 0xF7) {
        return 0;
    }
    width = (*p == 0xF6) ? 1 : (opsize16 ? 2 : 4);
    p++;

    modrm = *p++;
    mod = (uint8_t)(modrm >> 6);
    reg = (uint8_t)((modrm >> 3) & 7);
    rm = (uint8_t)(modrm & 7);

    if (reg != 6 && reg != 7) {
        return 0; /* not div/idiv: some other F6/F7 group member */
    }
    is_signed = (reg == 7);

    if (mod == 3) {
        divisor = g->r[rm] & (width == 4 ? 0xFFFFFFFFu
                              : width == 2 ? 0xFFFFu : 0xFFu);
    } else {
        uint32_t addr = 0;
        int have_base = 1;

        if (rm == 4) {
            uint8_t sib = *p++;
            uint8_t base = (uint8_t)(sib & 7);
            uint8_t index = (uint8_t)((sib >> 3) & 7);
            uint8_t scale = (uint8_t)(sib >> 6);

            if (base == 5 && mod == 0) {
                have_base = 0;
            } else {
                addr = g->r[base];
            }
            if (index != 4) {
                addr += g->r[index] << scale;
            }
        } else if (rm == 5 && mod == 0) {
            have_base = 0;
        } else {
            addr = g->r[rm];
        }

        if (!have_base || mod == 2) {
            uint32_t disp;
            memcpy(&disp, p, 4);
            p += 4;
            addr += disp;
        } else if (mod == 1) {
            addr += (uint32_t)(int32_t)(int8_t)*p;
            p++;
        }
        divisor = read_operand((const void *)(uintptr_t)addr, width);
    }

    if (divisor != 0) {
        return 0; /* an idiv overflow, not a zero divisor */
    }

    /* The quotient register keeps the numerator and the remainder register is
     * cleared, in each of the three widths. On x86 the dividend is the pair
     * (DX:AX and friends); the numerator the ARM runtime saw is the low half,
     * which is exactly what is left in place. */
    switch (width) {
    case 1:
        g->r[0] &= 0xFFFF00FFu; /* AL is the quotient and stays; AH = 0 */
        break;
    case 2:
        g->r[2] &= 0xFFFF0000u; /* DX = 0 */
        break;
    default:
        g->r[2] = 0; /* EDX = 0 */
        break;
    }

    report((uint32_t)(uintptr_t)start, is_signed ? "idiv" : "div");
    g->eip = (uint32_t)(uintptr_t)p;
    return 1;
}

#elif defined(__arm__)

/*
 * The ARM half. pc/src/pc_div0_wrap.s answers the division before anything
 * faults, so there is no fixup to perform and nothing to count; what is
 * left is proving the wrappers are in the link and doing the right thing.
 *
 * Every operand is volatile and so is the result, and the second half is not
 * decoration. Written the obvious way (expect(ua / ub == 0u)) GCC compiles
 * the whole expression to a compare, because `a / b == 0` is `a < b` for
 * unsigned when b is non-zero and division by zero is undefined behaviour, so
 * it may assume that. No call is emitted, the wrapper never runs, and the
 * check fails on a correct port. Storing through a volatile forces the call:
 * GCC has to have the quotient to store it, and the identity yields only the
 * boolean.
 *
 * The same is true of the game, and it is a limit of the fix rather than of
 * the test: a wrapper can only answer for divisions the compiler really calls
 * the runtime for. Where GCC folds one away, a comparison like the above, a
 * constant divisor turned into a shift and a multiply, the cartridge's
 * answer is not available to give. mwcc always called _u32_div_f. The x86
 * half has the identical hole for the identical reason, so ARM introduces
 * nothing, and none of the three sites this port had to fix is in it.
 */

unsigned long pc_div0_count(void)
{
    return 0;
}

/*
 * Nothing reaches this on ARM: the wrappers answer before a signal exists, so
 * a SIGFPE here is a real one. pc_main.c does not call it (its handler is
 * x86-only and says so); the prototype is in the shared header and the two
 * Windows callers are i386, so this exists to keep the header honest.
 */
int pc_div0_fixup(struct pc_x86_regs *g)
{
    (void)g;
    return 0;
}

int pc_div0_selftest(void)
{
    volatile int a, b, q;
    volatile unsigned ua, ub, uq;
    int ok = 1;

    ub = 0u;
    b = 0;

    /* Unsigned: a / 0 == a, a % 0 == 0. */
    ua = 1u;          uq = ua / ub; ok &= (uq == 1u);
                      uq = ua % ub; ok &= (uq == 0u);
    ua = 0xFFFFFFFFu; uq = ua / ub; ok &= (uq == 0xFFFFFFFFu);
                      uq = ua % ub; ok &= (uq == 0u);
    ua = 0u;          uq = ua / ub; ok &= (uq == 0u);
                      uq = ua % ub; ok &= (uq == 0u);

    /* Signed, where libgcc alone would have said INT_MAX or INT_MIN. */
    a = 1;            q = a / b;    ok &= (q == 1);
                      q = a % b;    ok &= (q == 0);
    a = -1;           q = a / b;    ok &= (q == -1);
                      q = a % b;    ok &= (q == 0);
    a = -2147483647 - 1;
                      q = a / b;    ok &= (q == -2147483647 - 1);
                      q = a % b;    ok &= (q == 0);

    /* A non-zero divisor still goes to libgcc, which is the half a wrapper
     * can break: two instructions in front of a routine are only correct if
     * the fall-through really reaches it. */
    a = -7; b = 2;    q = a / b;    ok &= (q == -3);
                      q = a % b;    ok &= (q == -1);
    ua = 7u; ub = 2u; uq = ua / ub; ok &= (uq == 3u);
                      uq = ua % ub; ok &= (uq == 1u);

    if (!ok) {
        fprintf(stderr, "pc-selftest div0: the AEABI wrappers are not "
                        "answering, is --wrap in LDFLAGS?\n");
    }
    return ok;
}

#elif defined(__wasm__)

/*
 * The wasm half. Here `/` and `%` are instructions again, and wasm's
 * div/rem trap on a zero divisor (and div_s on INT_MIN / -1). Nothing in
 * this file can answer that: there are no signals, and no call to wrap.
 * The answer is given after the fact instead, by
 * tools/wasm2c_postprocess.py, which rewrites the macros wasm2c turns every
 * div/rem instruction into. It gives the ROM's answers, measured by running
 * _s32_div_f, _u32_div_f and the four 64-bit routines out of main.sbin:
 * the 32-bit ones above, INT_MIN / -1 == INT_MIN with remainder 0, and for
 * 64-bit a / 0 == a AND a % 0 == a (mwcc's _ll_* return the numerator
 * pair untouched for both). Its docstring has the disassembly.
 *
 * So there is nothing to fix up and nothing to count. The selftest still
 * divides, and is a real check of the post-processing: run unpatched (raw
 * wasm2c, or any wasm engine) it traps instead of returning. Same volatile
 * discipline as the ARM half, for the same reason.
 */

unsigned long pc_div0_count(void)
{
    return 0;
}

int pc_div0_fixup(struct pc_x86_regs *g)
{
    (void)g;
    return 0;
}

int pc_div0_selftest(void)
{
    volatile int32_t a, b, q;
    volatile uint32_t ua, ub, uq;
    volatile int64_t la, lb, lq;
    volatile uint64_t lua, lub, luq;
    int ok = 1;

    b = 0;
    ub = 0u;
    lb = 0;
    lub = 0u;

    a = 7;            q = a / b;    ok &= (q == 7);
                      q = a % b;    ok &= (q == 0);
    a = -7;           q = a / b;    ok &= (q == -7);
                      q = a % b;    ok &= (q == 0);
    a = INT32_MIN;    q = a / b;    ok &= (q == INT32_MIN);
                      q = a % b;    ok &= (q == 0);
    ua = 0xFFFFFFFFu; uq = ua / ub; ok &= (uq == 0xFFFFFFFFu);
                      uq = ua % ub; ok &= (uq == 0u);

    /* The 64-bit runtime keeps the numerator for the remainder too. */
    la = -7;          lq = la / lb; ok &= (lq == -7);
                      lq = la % lb; ok &= (lq == -7);
    lua = 1ull << 40; luq = lua / lub; ok &= (luq == 1ull << 40);
                      luq = lua % lub; ok &= (luq == 1ull << 40);

    /* Overflow: wasm traps, the ROM wraps. */
    a = INT32_MIN; b = -1;  q = a / b;   ok &= (q == INT32_MIN);
                            q = a % b;   ok &= (q == 0);
    la = INT64_MIN; lb = -1; lq = la / lb; ok &= (lq == INT64_MIN);
                             lq = la % lb; ok &= (lq == 0);

    /* Non-zero divisors untouched. */
    a = -7; b = 2;    q = a / b;    ok &= (q == -3);
                      q = a % b;    ok &= (q == -1);
    ua = 7u; ub = 2u; uq = ua / ub; ok &= (uq == 3u);
                      uq = ua % ub; ok &= (uq == 1u);

    if (!ok) {
        fprintf(stderr, "pc-selftest div0: wasm division answers are not the "
                        "cartridge's; was the wasm2c output run through "
                        "tools/wasm2c_postprocess.py?\n");
    }
    return ok;
}

#else
#error "pc_div0: this architecture has no answer for a division by zero yet"
#endif
