/*
 * 3ds/src/3ds_div0.c: does a division by zero answer the way the DS did?
 *
 * The wrappers are in 3ds_div0_wrap.s and the reasoning is there. This is the
 * check, and it can only run on ARM: the host that builds this port is x86,
 * where the same expressions raise #DE and kill the process before any
 * comparison happens. So unlike every other model in this tree there is no
 * host twin, 3ds/tests/run.sh checks statically that the four wrappers are
 * in the link, and the console runs these.
 *
 * Every operand is volatile and so is the result, and the second half of that
 * is not decoration. Written the obvious way (`expect(ua / ub == 0u)`) GCC
 * compiles the whole expression to `cmp ua, ub; movcc r0, #1`, because `a / b
 * == 0` is `a < b` for unsigned when b is non-zero, and division by zero is
 * undefined behaviour so it may assume that. No call is emitted, the wrapper
 * never runs, and the check fails on a correct port. Measured on the console:
 * `DIV0 FAILED 1 OF 20, FIRST AT 5`, and check 5 was the only one whose
 * comparison GCC could rewrite that way.
 *
 * The same thing is true of the game, and it is a limit of this fix rather
 * than of this test: a wrapper can only answer for divisions the compiler
 * actually calls the runtime for. Where GCC folds one away, a comparison
 * like the above, a constant divisor turned into a shift and a multiply, the
 * cartridge's answer is not available to give. mwcc always called
 * `_u32_div_f`. The PC port has the identical hole for the identical reason,
 * so this is not something the 3DS introduced, and nothing in the game has
 * been found in it: the three sites the PC port had to fix all reached the
 * runtime.
 *
 * Storing through a volatile is what forces the call. GCC has to have the
 * quotient to store it, and the `a < b` identity yields only the boolean.
 */

#include <stddef.h>
#include <stdint.h>

#include "3ds_div0.h"

#define INT_MIN_32 (-2147483647 - 1)

static int sChecks;
static int sFailed;
static int sFirstFail;

static void expect(int held)
{
    sChecks++;
    if (!held) {
        if (sFirstFail == 0) {
            sFirstFail = sChecks;
        }
        sFailed++;
    }
}

int div0_selftest(int *ran)
{
    volatile int a;
    volatile int b;
    volatile unsigned ua;
    volatile unsigned ub;
    volatile int q;
    volatile unsigned uq;

    sChecks = 0;
    sFailed = 0;
    sFirstFail = 0;

    ub = 0u;
    b = 0;

    /* The cartridge's answer, unsigned: a / 0 == a, a % 0 == 0. */
    ua = 1u;
    uq = ua / ub;
    expect(uq == 1u);
    uq = ua % ub;
    expect(uq == 0u);

    ua = 0xFFFFFFFFu;
    uq = ua / ub;
    expect(uq == 0xFFFFFFFFu);
    uq = ua % ub;
    expect(uq == 0u);

    ua = 0u;
    uq = ua / ub;
    expect(uq == 0u);
    uq = ua % ub;
    expect(uq == 0u);

    /* And signed, where libgcc would have said INT_MAX or INT_MIN. */
    a = 1;
    q = a / b;
    expect(q == 1);
    q = a % b;
    expect(q == 0);

    a = -1;
    q = a / b;
    expect(q == -1);
    q = a % b;
    expect(q == 0);

    a = INT_MIN_32;
    q = a / b;
    expect(q == INT_MIN_32);
    q = a % b;
    expect(q == 0);

    a = 0;
    q = a / b;
    expect(q == 0);
    q = a % b;
    expect(q == 0);

    /*
     * A non-zero divisor still goes through libgcc, which is the half a
     * wrapper can break: three instructions in front of a routine are only
     * correct if the fall-through really reaches it.
     */
    a = -7;
    b = 2;
    q = a / b;
    expect(q == -3);
    q = a % b;
    expect(q == -1);

    ua = 7u;
    ub = 2u;
    uq = ua / ub;
    expect(uq == 3u);
    uq = ua % ub;
    expect(uq == 1u);

    /*
     * INT_MIN / -1. The requirement is that it does not abort; there is no
     * trap on this architecture and the DS had none either, so the answer is
     * recorded rather than argued for. ARM's long division returns INT_MIN.
     */
    a = INT_MIN_32;
    b = -1;
    q = a / b;
    expect(q == INT_MIN_32);
    q = a % b;
    expect(q == 0);

    if (ran != NULL) {
        *ran = sChecks;
    }
    return sFailed;
}

int div0_first_failure(void)
{
    return sFirstFail == 0 ? -1 : sFirstFail;
}

/*
 * pc/src/pc_selftest.c's name for the same suite, and the one entry point
 * pc_div0.c had that this rewrite did not carry over. That file is a table of
 * `int (*)(void)` where 1 means the suite passed, so the two conventions do
 * not match and the forwarder is where they meet: div0_selftest() answers with
 * a failure COUNT.
 *
 * It exists here rather than in a stub because pc_selftest.c is a file this
 * port takes and pc_div0.c is one it replaces, so the name is this file's to
 * answer, and without it the game link is short one symbol for a reason that
 * has nothing to do with the ARM7 sound driver.
 */
int pc_div0_selftest(void)
{
    int ran = 0;

    return div0_selftest(&ran) == 0;
}
