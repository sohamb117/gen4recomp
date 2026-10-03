/*
 * 3ds/src/3ds_init.c: the host models, in pc_main.c's order.
 *
 * Every model named here already exists as portable C: pc_pxi.c is the FIFO
 * with no ARM7 behind it, pc_pm.c is the power IC as a register file, and
 * pc_rtc.c, pc_wvr.c and pc_snd.c are the three devices that answer on a FIFO
 * tag. None of them needed rewriting for this console and none was rewritten.
 * What was missing is the thing pc_main.c has and this port did not: the
 * order, in one place, with a failure that names itself.
 *
 * The order is
 *
 *     armrec_mem_init -> rom -> input -> view -> rtc -> wvr -> snd -> gpu
 *
 * and this file is the middle of that line. armrec_mem_init() is the crt's and
 * the ends belong elsewhere: `view` is the PC viewer's shared-memory channel,
 * which this console answers with two LCDs instead. What holds the order is
 * three constraints:
 *
 *   - rom before everything, because pc_rom_init() puts the cartridge header
 *     at HW_ROM_HEADER_BUF, and the 160 bytes above that buffer are the PXI
 *     signal words and the button halfword.
 *   - input after rom for exactly that reason: HW_BUTTON_XY_BUF is inside
 *     those 160 bytes. The overrun that used to eat them was measured.
 *   - rtc, wvr and snd in any order among themselves, since they claim three
 *     different FIFO tags and share nothing, but all three after the slab.
 *     They are kept in pc_main.c's order anyway, and run.sh fails if the two
 *     files disagree.
 *
 * PXI and PM have no step, and that is a finding rather than an omission. PM
 * is eight registers in .data with the reset values a booted console has, so
 * there is nothing to do that the loader has not already done.
 *
 * PXI is the interesting one. The game calls PXI_Init() itself, from inside
 * NitroMain, and PXI_InitFifo() clears every ARM9 receive callback. The steps
 * below run before that, so if PXI_InitFifo() also cleared the responder
 * table, this whole file would be registering into a table the game is about
 * to wipe. It does not: pc_pxi.c keeps the callbacks and the responders as two
 * separate arrays. That separation is not obvious from either file alone, so
 * 3ds/tests/run.sh checks it in pc_pxi.c's source.
 *
 * The steps are weak references because two binaries link this file. The game
 * has all of pc/src and every step is present; the self-test .3dsx links
 * 3ds/src alone, so four of the five names do not exist there. A weak
 * reference makes that a skip with a count rather than a link error.
 * pc_input_init() is not weak: 3ds/src/3ds_input.c defines it.
 *
 * The two return conventions are not normalised. Four of the five return 0 for
 * success and pc_input_init() returns 1. That is pc_main.c's shape, and
 * wrapping either one would put a lie in whichever file did the wrapping. The
 * table carries the convention instead.
 */

#include <stddef.h>
#include <stdio.h>

#include "3ds_init.h"
#include "3ds_input.h"

/*
 * pc/src's four. Weak, so the self-test binary links; see the head comment.
 * Declared here rather than through pc/src headers because those are the DS
 * SDK's include chain and this is a port file compiled against libctru's.
 */
extern int pc_rom_init(void) __attribute__((weak));
extern int pc_rtc_init(void) __attribute__((weak));
extern int pc_wvr_init(void) __attribute__((weak));
extern int pc_snd_init(void) __attribute__((weak));

/*
 * The 2D engines. Not an `_init` and not weak in the same shape as the four
 * above: pc/hw/pc_gpu2d.c returns nothing, because installing a renderer
 * cannot fail; it hands pc_video.c a function pointer and a name. The
 * wrapper below is what puts it in the table with the rest, so the sequence
 * stays one list rather than a list and an afterthought. 8.1.
 */
extern void pc_gpu2d_install(void) __attribute__((weak));
extern void pc_gpu3d_install(void) __attribute__((weak));

static int gpu2d_step(void)
{
    if (pc_gpu2d_install != NULL) {
        pc_gpu2d_install();
    }
    return 0;
}

/*
 * And the geometry engine, in pc_main.c's order: 2D first, because the 3D
 * layer is a background of engine A and the compositor has to exist before
 * anything can be composited into it.
 *
 * Until this ran, every geometry command was discarded. armrec_gx_port()
 * answers an uninstalled engine out of a scratch buffer; it cannot answer
 * out of the I/O page, which at those addresses is the ARM7's sound registers,
 * so the game was issuing matrices and vertices into nothing.
 */
static int gpu3d_step(void)
{
    if (pc_gpu3d_install != NULL) {
        pc_gpu3d_install();
    }
    return 0;
}

/*
 * ok_is_zero says which of the two conventions above a step uses. It is one
 * bit and it exists so neither side has to be wrapped.
 */
struct host_step {
    const char *name;
    int (*fn)(void);
    int ok_is_zero;
};

static const struct host_step kSteps[] = {
    { "rom",   pc_rom_init,   1 },
    { "input", pc_input_init, 0 },
    { "rtc",   pc_rtc_init,   1 },
    { "wvr",   pc_wvr_init,   1 },
    { "snd",   pc_snd_init,   1 },
    { "gpu2d", gpu2d_step,    1 },
    { "gpu3d", gpu3d_step,    1 },
};

#define HOST_STEP_COUNT ((int)(sizeof kSteps / sizeof kSteps[0]))

static const char *sFailed;
static int sRan;
static int sSkipped;

int host_init(void)
{
    int i;

    sFailed = NULL;
    sRan = 0;
    sSkipped = 0;

    for (i = 0; i < HOST_STEP_COUNT; i++) {
        int rc;

        if (kSteps[i].fn == NULL) {
            /* Not in this link. Counted, not silent: a game binary that
             * skipped a step would mean the weak reference outlived the
             * file that was meant to satisfy it, and the count is what
             * says so. */
            sSkipped++;
            continue;
        }
        rc = kSteps[i].fn();
        sRan++;
        if (kSteps[i].ok_is_zero ? (rc != 0) : (rc == 0)) {
            sFailed = kSteps[i].name;
            fprintf(stderr, "3ds_init: %s failed (returned %d)\n",
                    kSteps[i].name, rc);
            return -1;
        }
    }
    return 0;
}

const char *host_init_failed(void)
{
    return sFailed;
}

int host_init_ran(void)
{
    return sRan;
}

int host_init_skipped(void)
{
    return sSkipped;
}
