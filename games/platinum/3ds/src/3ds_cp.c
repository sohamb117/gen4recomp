/*
 * 3ds/src/3ds_cp.c: the ARM9 coprocessor, over the translator.
 *
 * See 3ds_cp.h for what the unit is and why this file is not armrec_rt.c. The
 * semantics below are melonDS's NDS::DivDone/SqrtDone, which the PC port
 * arbitrated against directly; what is different here is four accessors.
 *
 * The operands are read through the translator and so are the results, which
 * is what makes the model work at all on a console that cannot put memory at
 * 0x04000280. Every one of the seven registers is in the I/O row, so an
 * unbound slab makes all four accessors no-ops rather than null dereferences:
 * A division before armrec_mem_init() answers zero, which is what an
 * uninitialised register held anyway.
 *
 * NOT MODELLED, and the same three as on PC: DIVCNT bit 14 (the divide-by-zero
 * flag), the busy bit, and SQRTCNT's busy bit. Nothing in this tree reads any
 * of them and the unit here is never busy, so all three stay plain memory and
 * a CNT read back is the CNT that was written.
 */

#include "3ds_cp.h"

#include <stddef.h>

#include "armrec_rt.h"

#include "3ds_guest.h"

static volatile uint32_t *cp_p32(uint32_t a)
{
    return (volatile uint32_t *)armrec_host_ptr(a);
}

static uint32_t cp_ld32(uint32_t a)
{
    const volatile uint32_t *p = cp_p32(a);

    return p != NULL ? *p : 0u;
}

static uint16_t cp_ld16(uint32_t a)
{
    const volatile uint16_t *p = (const volatile uint16_t *)armrec_host_ptr(a);

    return p != NULL ? *p : 0u;
}

static void cp_st32(uint32_t a, uint32_t v)
{
    volatile uint32_t *p = cp_p32(a);

    if (p != NULL) {
        *p = v;
    }
}

static uint64_t cp_ld64(uint32_t a)
{
    return (uint64_t)cp_ld32(a) | ((uint64_t)cp_ld32(a + 4) << 32);
}

/*
 * melonDS leaves DIVREM_RESULT *unchanged* for 32-bit INT_MIN / -1, where the
 * other two modes zero it. A model that computes from the operands has no
 * "unchanged" to return, and guessing between 0 and the previous remainder is
 * the kind of plausible wrong answer that costs a week later, so this stops.
 * It needs a numerator of exactly -2^31 with a denominator of -1, which no
 * caller in this tree can produce.
 *
 * A COMPLETION does not trap, it declines to write; the register is the
 * storage, so "leave the previous remainder in place" is something this port
 * can express, and it is what melonDS does. Asking for the remainder in that
 * state still stops, because answering with the previous division's is a claim
 * about hardware timing this project has no oracle for.
 */
static void cp_unmodelled(void)
{
    armrec_trap("cp_divrem",
                "DIVREM_RESULT read after INT_MIN / -1 in 32-bit mode. "
                "Hardware leaves the previous division's remainder there and "
                "this unit is stateless, so 0 would be a guess.");
}

/* The 64-bit quotient and remainder for the operands currently in memory. */
static void cp_divide(uint64_t *quot, uint64_t *rem, int want_rem)
{
    uint32_t mode = cp_ld16(CP_DIVCNT_ADDR) & 3u;

    if (mode == 0) {
        int32_t num = (int32_t)cp_ld32(CP_NUMER_ADDR);
        int32_t den = (int32_t)cp_ld32(CP_DENOM_ADDR);

        if (den == 0) {
            /* Not a sign-extended +/-1: the low word is the 32-bit answer and
             * the high word carries the opposite sign. melonDS's own words. */
            *quot = (num < 0) ? 0xFFFFFFFF00000001ULL : 0x00000000FFFFFFFFULL;
            *rem = (uint64_t)(int64_t)num;
        } else if (num == (-0x7FFFFFFF - 1) && den == -1) {
            *quot = 0x0000000080000000ULL;
            if (want_rem) {
                cp_unmodelled();
            }
            *rem = 0;
        } else {
            *quot = (uint64_t)(int64_t)(num / den);
            *rem = (uint64_t)(int64_t)(num % den);
        }
        return;
    }

    {
        int64_t num = (int64_t)cp_ld64(CP_NUMER_ADDR);
        int64_t den = (mode == 2) ? (int64_t)cp_ld64(CP_DENOM_ADDR)
                                  : (int64_t)(int32_t)cp_ld32(CP_DENOM_ADDR);

        if (den == 0) {
            *quot = (uint64_t)(int64_t)((num < 0) ? 1 : -1);
            *rem = (uint64_t)num;
        } else if (num == (-0x7FFFFFFFFFFFFFFFLL - 1) && den == -1) {
            *quot = 0x8000000000000000ULL;
            *rem = 0;
        } else {
            *quot = (uint64_t)(num / den);
            *rem = (uint64_t)(num % den);
        }
    }
}

/* floor(sqrt(v)) over the unsigned operand, 32 or 64 bits by SQRTCNT bit 0. */
static uint32_t cp_sqrt(void)
{
    uint64_t v = (cp_ld16(CP_SQRTCNT_ADDR) & 1u)
                     ? cp_ld64(CP_SQRTPARAM_ADDR)
                     : (uint64_t)cp_ld32(CP_SQRTPARAM_ADDR);
    uint64_t r = 0, bit;
    int shift = 62;

    while (shift > 0 && (v >> shift) == 0) {
        shift -= 2;
    }
    for (bit = 1ULL << shift; bit; bit >>= 2) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
    }
    return (uint32_t)r;
}

/*
 * 32-bit INT_MIN / -1 is the one state whose remainder has no answer here. A
 * completion has to recognise it *without* computing, because the point is
 * that it leaves DIVREM alone rather than stopping.
 */
static int cp_rem_unmodelled(void)
{
    return (cp_ld16(CP_DIVCNT_ADDR) & 3u) == 0
           && (int32_t)cp_ld32(CP_NUMER_ADDR) == (-0x7FFFFFFF - 1)
           && (int32_t)cp_ld32(CP_DENOM_ADDR) == -1;
}

/*
 * The divider finishing: both result registers written from the operands as
 * they stand. `want_rem` is whether the caller is asking for the remainder;
 * a plain completion is not, and must not stop on a state it can simply leave
 * as it found it.
 */
static void cp_div_complete(int want_rem)
{
    uint64_t q, r;

    if (!want_rem && cp_rem_unmodelled()) {
        cp_divide(&q, &r, 0);
        cp_st32(CP_RESULT_ADDR, (uint32_t)q);
        cp_st32(CP_RESULT_ADDR + 4, (uint32_t)(q >> 32));
        return; /* DIVREM keeps what it had */
    }
    cp_divide(&q, &r, want_rem);
    cp_st32(CP_RESULT_ADDR, (uint32_t)q);
    cp_st32(CP_RESULT_ADDR + 4, (uint32_t)(q >> 32));
    cp_st32(CP_REM_ADDR, (uint32_t)r);
    cp_st32(CP_REM_ADDR + 4, (uint32_t)(r >> 32));
}

static void cp_sqrt_complete(void)
{
    cp_st32(CP_SQRTRES_ADDR, cp_sqrt());
}

void *cp_ptr(uint32_t guest)
{
    switch (guest & ~3u) {
    case CP_DIVCNT_ADDR: /* the `while (CNT & 0x8000)` wait */
    case CP_RESULT_ADDR:
    case CP_RESULT_ADDR + 4:
        cp_div_complete(0);
        break;
    case CP_REM_ADDR:
    case CP_REM_ADDR + 4:
        cp_div_complete(1);
        break;
    case CP_SQRTCNT_ADDR:
    case CP_SQRTRES_ADDR:
        cp_sqrt_complete();
        break;
    default: /* operands and the gaps: storage */
        break;
    }
    return armrec_host_ptr(guest);
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#ifdef CP_SELFTEST_VERBOSE
#include <stdio.h>
#define FAILNOTE() fprintf(stderr, "  3ds_cp.c:%d failed\n", __LINE__)
#else
#define FAILNOTE() ((void)0)
#endif

#define CHECK(cond)                                                           \
    do {                                                                      \
        ran++;                                                                \
        if (!(cond)) {                                                        \
            failed++;                                                         \
            FAILNOTE();                                                       \
        }                                                                     \
    } while (0)

/* Set the operands the way the SDK's inline functions do, a plain store to a
 * register the unit does not own, and then read a result through cp_ptr(),
 * which is the only thing that makes the unit run. */
static void cp_set_div(uint32_t mode, uint64_t num, uint64_t den)
{
    cp_st32(CP_DIVCNT_ADDR, mode);
    cp_st32(CP_NUMER_ADDR, (uint32_t)num);
    cp_st32(CP_NUMER_ADDR + 4, (uint32_t)(num >> 32));
    cp_st32(CP_DENOM_ADDR, (uint32_t)den);
    cp_st32(CP_DENOM_ADDR + 4, (uint32_t)(den >> 32));
}

static uint64_t cp_read64(uint32_t addr)
{
    uint32_t lo = *(const volatile uint32_t *)cp_ptr(addr);
    uint32_t hi = *(const volatile uint32_t *)cp_ptr(addr + 4);

    return (uint64_t)lo | ((uint64_t)hi << 32);
}

static uint32_t cp_read_sqrt(uint32_t mode, uint64_t param)
{
    cp_st32(CP_SQRTCNT_ADDR, mode);
    cp_st32(CP_SQRTPARAM_ADDR, (uint32_t)param);
    cp_st32(CP_SQRTPARAM_ADDR + 4, (uint32_t)(param >> 32));
    return *(const volatile uint32_t *)cp_ptr(CP_SQRTRES_ADDR);
}

int cp_selftest(int *ranOut)
{
    int ran = 0;
    int failed = 0;

    /* Nothing works without the row, and it must say so rather than fault. */
    if (armrec_host_ptr(CP_DIVCNT_ADDR) == NULL) {
        ran++;
        if (cp_ptr(CP_DIVCNT_ADDR) != NULL) {
            failed++;
            FAILNOTE();
        }
        if (ranOut != NULL) {
            *ranOut = ran;
        }
        return failed + 1;
    }

    /* Mode 0, 32 / 32. C truncates towards zero and so does the unit, which is
     * the half a rounding implementation gets wrong for negative numerators. */
    cp_set_div(0, 100u, 7u);
    CHECK(cp_read64(CP_RESULT_ADDR) == 14u);
    CHECK(cp_read64(CP_REM_ADDR) == 2u);

    cp_set_div(0, (uint32_t)-100, 7u);
    CHECK((int64_t)cp_read64(CP_RESULT_ADDR) == -14);
    CHECK((int64_t)cp_read64(CP_REM_ADDR) == -2);

    /* A zero divisor. The quotient is not a sign-extended +/-1: the low word
     * is the 32-bit answer and the high word carries the opposite sign. */
    cp_set_div(0, 5u, 0u);
    CHECK(cp_read64(CP_RESULT_ADDR) == 0x00000000FFFFFFFFULL);
    CHECK((int64_t)cp_read64(CP_REM_ADDR) == 5);

    cp_set_div(0, (uint32_t)-5, 0u);
    CHECK(cp_read64(CP_RESULT_ADDR) == 0xFFFFFFFF00000001ULL);
    CHECK((int64_t)cp_read64(CP_REM_ADDR) == -5);

    /* Mode 1, 64 / 32: the denominator is sign-extended from its low word. */
    cp_set_div(1, 0x0000000200000000ULL, (uint32_t)-2);
    CHECK((int64_t)cp_read64(CP_RESULT_ADDR) == -0x100000000LL);
    CHECK(cp_read64(CP_REM_ADDR) == 0u);

    /* Mode 2, 64 / 64, and its zero divisor, which answers +/-1 whole. */
    cp_set_div(2, 0x0000000700000000ULL, 0x0000000200000000ULL);
    CHECK(cp_read64(CP_RESULT_ADDR) == 3u);
    CHECK(cp_read64(CP_REM_ADDR) == 0x0000000100000000ULL);

    cp_set_div(2, 9u, 0u);
    CHECK((int64_t)cp_read64(CP_RESULT_ADDR) == -1);
    CHECK(cp_read64(CP_REM_ADDR) == 9u);

    /*
     * 32-bit INT_MIN / -1. The quotient is 0x80000000 and DIVREM is left
     * exactly as it was, which is why the marker below is written first: a
     * model that computed a remainder here would overwrite it, and one that
     * zeroed it would look right for the wrong reason. Only a completion is
     * asked for, reading DIVREM in this state stops on purpose.
     */
    cp_st32(CP_REM_ADDR, 0xA5A5A5A5u);
    cp_st32(CP_REM_ADDR + 4, 0x5A5A5A5Au);
    cp_set_div(0, 0x80000000u, (uint32_t)-1);
    CHECK(cp_read64(CP_RESULT_ADDR) == 0x0000000080000000ULL);
    CHECK(cp_ld32(CP_REM_ADDR) == 0xA5A5A5A5u);
    CHECK(cp_ld32(CP_REM_ADDR + 4) == 0x5A5A5A5Au);

    /* 64-bit INT_MIN / -1 has an answer and zeroes the remainder. */
    cp_set_div(2, 0x8000000000000000ULL, (uint64_t)-1);
    CHECK(cp_read64(CP_RESULT_ADDR) == 0x8000000000000000ULL);
    CHECK(cp_read64(CP_REM_ADDR) == 0u);

    /* The square root, in both widths. 0xFFFFFFFF is the one an implementation
     * that computes in doubles gets wrong. */
    CHECK(cp_read_sqrt(0, 0x00010000u) == 0x100u);
    CHECK(cp_read_sqrt(0, 0u) == 0u);
    CHECK(cp_read_sqrt(0, 0xFFFFFFFFu) == 0xFFFFu);
    CHECK(cp_read_sqrt(1, 0xFFFFFFFFFFFFFFFFULL) == 0xFFFFFFFFu);
    CHECK(cp_read_sqrt(1, 0x0000000100000000ULL) == 0x10000u);
    /* Mode 0 reads the low word only, so the high word must not reach it. */
    CHECK(cp_read_sqrt(0, 0xFFFFFFFF00000009ULL) == 3u);

    /*
     * The operands are storage and the CNTs read back what was written: the
     * unit owns the CNT addresses for the completion, not for the value.
     */
    cp_st32(CP_NUMER_ADDR, 0xDEADBEEFu);
    CHECK(*(const volatile uint32_t *)cp_ptr(CP_NUMER_ADDR) == 0xDEADBEEFu);
    CHECK(cp_ptr(CP_NUMER_ADDR) == armrec_host_ptr(CP_NUMER_ADDR));
    cp_st32(CP_DIVCNT_ADDR, 2u);
    CHECK((*(const volatile uint16_t *)cp_ptr(CP_DIVCNT_ADDR) & 3u) == 2u);

    /* An address outside the window is plain memory and comes back
     * untouched, SQRT_PARAM is the neighbour that must stay storage. */
    cp_st32(CP_SQRTPARAM_ADDR, 0x12345678u);
    CHECK(cp_ptr(CP_SQRTPARAM_ADDR) == armrec_host_ptr(CP_SQRTPARAM_ADDR));
    CHECK(*(const volatile uint32_t *)cp_ptr(CP_SQRTPARAM_ADDR) == 0x12345678u);

    /* Leave the window the way a reset console has it. */
    {
        uint32_t a;

        for (a = CP_DIVCNT_ADDR; a <= CP_SQRTPARAM_ADDR + 4u; a += 4u) {
            cp_st32(a, 0u);
        }
    }

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
