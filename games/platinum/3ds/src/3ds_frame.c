/*
 * 3ds/src/3ds_frame.c: what happens when a frame ends on this console.
 *
 * Two callers, one policy. The self-test loop in 3ds_main.c ends a frame with
 * blit, flush, swap, wait. The game ends its frame inside OS_Halt(), which is
 * pc/src code shared with the PC port and reaches a host only through
 * pc_video_frame_end()'s weak publish hook. Those are two entry points into the
 * same event, and if each kept its own sequence they would drift. So the
 * sequence lives here and both go through it.
 *
 * The order, and why it is this one:
 *
 *   1. hidScanInput(), once per frame, here and nowhere else. libctru latches
 *      down, held and up at the scan, so a second scan in the same frame
 *      silently eats every edge the first one saw.
 *   2. The blit, through 3ds_view.c.
 *   3. gfxFlushBuffers(), gfxSwapBuffers(), gspWaitForVBlank(). The flush is
 *      not optional: the framebuffer is written with the CPU and the LCD reads
 *      it through a different path.
 *   4. aptMainLoop(), which is both "should we still be here" and the pump
 *      that lets Home, sleep and close happen at all.
 *
 * Waiting for vblank is the pacer, and it is the console's own 60 Hz rather
 * than a clock this port reads. A frame that took longer than a refresh misses
 * that VBlank and waits for the next one, and there is no leftover host time
 * to accumulate because no host time is measured.
 *
 * Guest time is not host time. One call is one guest VBlank whatever the host
 * frame cost: OS_Halt has already counted it and run the handler before
 * anything here happens. A slow host frame makes the console slow, not the
 * guest inconsistent.
 *
 * The game path exits and the self-test path returns, because the publish hook
 * is called from OS_Halt on whichever guest thread gave up the CPU and there
 * is nothing to unwind to. So when apt says stop, the guest path ends the
 * process the way the frame limit does, and the self-test loop, which has a
 * real stack to return along, asks frame_running() and falls out of its loop.
 */

#include "3ds_frame.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>

#include "3ds_audio.h"
#include "3ds_cpu.h"
#include "3ds_gpu.h"
#include "3ds_mem.h"
#include "3ds_perf.h"
#include "3ds_snd_watch.h"
#include "3ds_view.h"
#include "3ds_watchdog.h"

static int sRunning = 1;
static unsigned long sFrames;

/*
 * The interval between presents, in ARM11 system ticks, for the first
 * PACE_WINDOW frames. svcGetSystemTick counts at SYSCLOCK_ARM11 whatever the
 * host is doing, so these numbers mean the same thing on hardware and under
 * an emulator running at any speed; which is the only reason a pacing check
 * can be part of an emulator gate at all.
 */
#define PACE_WINDOW 32
static u64 sTick[PACE_WINDOW];
static u64 sPrevTick;

void frame_present(const uint32_t *top, const uint32_t *bottom,
                   const char *line_top, const char *line_bottom, int upper)
{
    u64 now;
    u64 mark;

    /*
     * One virtual CPU. The game path into this function is OS_Halt, on
     * whichever guest thread gave up the CPU, and every one of those runs
     * on the host thread the crt bound, because the SDK's scheduler switches
     * contexts rather than threads. A frame that arrives on any other thread
     * means a libctru callback started running guest code, which is the way
     * this model is likely to break and is invisible without a check.
     */
    cpu_assert_main("GUEST FRAME OFF THE MAIN THREAD");

    hidScanInput();

    /*
     * Each step timed where it happens, so "the port is slow" can be answered
     * with which part. The blit and the flush are this port's own cost, the
     * DS's two 256x192 pictures onto two LCDs of a different shape and a
     * different memory layout, and the wait is the opposite, time the
     * console had spare. What none of them cover is the game and the software
     * renderer, and that is deliberate: it is recovered as the remainder, so
     * the four always account for the whole interval.
     */
    mark = svcGetSystemTick();
    watchdog_where("blit");
    /*
     * The GPU present: the PICA draws the picture and the CPU blit is the fallback. The
     * two phases below keep their names and their meaning shifts with the
     * path: on the GPU one, PERF_BLIT is the surface copy into linear memory
     * and its tiling transfer, and PERF_FLUSH is building and submitting the
     * frame. Both are still "what presenting cost this port", which is what
     * makes the two paths' numbers comparable at all.
     */
    if (gpu_ready()) {
        /*
         * Timed inside 3ds_gpu.c, which is where the two halves are: the
         * upload is PERF_BLIT and building and submitting the frame is
         * PERF_FLUSH. No gfxFlushBuffers and no gfxSwapBuffers either, the
         * framebuffer is written by the transfer engine rather than the CPU,
         * so there is no dirty cache line to push, and citro3d swaps the
         * buffer itself when that transfer lands. A swap from here would show
         * the wrong one.
         */
        gpu_present(top, bottom, line_top, line_bottom, upper);
    } else {
        view_present(top, bottom, line_top, line_bottom);
        perf_phase(PERF_BLIT, svcGetSystemTick() - mark);

        mark = svcGetSystemTick();
        watchdog_where("flush");
        gfxFlushBuffers();
        gfxSwapBuffers();
        perf_phase(PERF_FLUSH, svcGetSystemTick() - mark);
    }

    mark = svcGetSystemTick();
    watchdog_where("waiting for vblank");
    gspWaitForVBlank();
    perf_phase(PERF_WAIT, svcGetSystemTick() - mark);

    now = svcGetSystemTick();
    if (sFrames < PACE_WINDOW) {
        sTick[sFrames] = sPrevTick != 0 ? now - sPrevTick : 0;
    }
    sPrevTick = now;

    sFrames++;
    /* The default between frames, so a hang that is not in one of the phases
     * above is named as the game's own code rather than as the last thing
     * this file did. */
    watchdog_where("the game");
    sRunning = aptMainLoop();
}

int frame_running(void)
{
    return sRunning;
}

unsigned long frame_count(void)
{
    return sFrames;
}

/* ------------------------------------------------------------------ */
/* The pacing check                                                  */
/* ------------------------------------------------------------------ */
/*
 * What is being checked, and why it needs a deliberate long frame. The claim
 * is not "frames arrive at 60 Hz", gspWaitForVBlank makes that nearly
 * impossible to get wrong. It is the second half of 7.4: after a frame that
 * overran, the *next* frame must not run short to win the time back. A port
 * that accumulated leftover host milliseconds would look identical at steady
 * state and only differ here, so the only way to check it is to overrun a
 * frame on purpose and watch what the following one does.
 *
 * A catch-up frame is impossible by construction; there is no accumulator
 * and no clock is read to pace with, only a wait for the console's own VBlank,
 * and that is exactly why it is worth pinning: the day somebody adds a
 * sleep-until-deadline to this file, this check is what says no.
 *
 * The burn spins on svcGetSystemTick rather than sleeping. A svcSleepThread
 * would hand the CPU back and might well return in time for the same VBlank,
 * which is not the frame this needs.
 *
 * Fast-forward is the one thing allowed to skip the wait;
 * when it arrives it will have to exempt itself from this check rather than
 * loosen it.
 */

#define PACE_BURN_AT   12ul     /* overrun this frame                     */
#define PACE_EVAL_AT   28ul     /* ... and judge at this one              */
#define PACE_SHORT     (PACE_FRAME_TICKS / 2)        /* a catch-up frame  */
#define PACE_OVERRAN   (PACE_FRAME_TICKS * 7 / 5)    /* the burn landed   */

static int sPaceRan;
static int sPaceBad;
static int sPaceFirst = -1;
static unsigned long sPaceMean;

static void pace_check(int ok)
{
    sPaceRan++;
    if (!ok) {
        if (sPaceFirst < 0) {
            sPaceFirst = sPaceRan;
        }
        sPaceBad++;
    }
}

static void pace_burn(void)
{
    u64 until = svcGetSystemTick() + (PACE_FRAME_TICKS * 3 / 2);

    while (svcGetSystemTick() < until) {
        /* spin: see above */
    }
}

static void pace_evaluate(void)
{
    u64 sum = 0;
    unsigned long i;
    unsigned long n = 0;

    /* The burn's own frame missed a refresh and waited for the next one. */
    pace_check(sTick[PACE_BURN_AT] >= PACE_OVERRAN);

    /* And nothing after it ran short to make the time back. This is the
     * check the whole task is about; the rest is context for it. */
    for (i = PACE_BURN_AT + 1; i < PACE_EVAL_AT; i++) {
        pace_check(sTick[i] >= PACE_SHORT);
    }

    /* Steady state either side of the burn, against the console's refresh.
     * Generous on purpose: this is a sanity bound on the pacer, not a
     * measurement of it, and frame time is measured elsewhere. */
    for (i = 4; i < PACE_BURN_AT; i++) {
        sum += sTick[i];
        n++;
    }
    for (i = PACE_BURN_AT + 4; i < PACE_EVAL_AT; i++) {
        sum += sTick[i];
        n++;
    }
    sPaceMean = (unsigned long)(sum / n);
    pace_check(sPaceMean >= PACE_FRAME_TICKS / 2
               && sPaceMean <= PACE_FRAME_TICKS * 2);
}

void pace_step(void)
{
    if (sFrames == PACE_BURN_AT) {
        pace_burn();
    } else if (sFrames == PACE_EVAL_AT && sPaceRan == 0) {
        pace_evaluate();
    }
}

int pace_ran(void)
{
    return sPaceRan;
}

int pace_bad(void)
{
    return sPaceBad;
}

int pace_first_failure(void)
{
    return sPaceFirst;
}

unsigned long pace_mean_ticks(void)
{
    return sPaceMean;
}

/*
 * The game's frame boundary.
 *
 * Which surface goes on which screen is the game's decision, not a
 * convention here: POWCNT1's DSEL bit selects the 2D engine that drives the
 * upper LCD and pc_video_upper_engine() reads it, so this follows the
 * register the same way the PC port's PNG dump does.
 *
 * The two externs are declared at the call site rather than included:
 * pc/include is not on this chain's include path, and pc_video.c
 * declares its own frame hooks exactly this way.
 *
 * No text in the letterbox. The bands carry the self-test verdict in the
 * other loop because that loop has nothing else to say; a running game does,
 * and what goes there is a question for the phase that has something to put
 * in it.
 */
void pc_view_publish(uint64_t frame)
{
    extern uint32_t *pc_video_surface(int engine);
    extern int pc_video_upper_engine(void);

    int upper = pc_video_upper_engine();

    /*
     * Before the present, because the report is the only thing this console
     * says about its sound and a frame that ends the run must not lose it
     *. It reads a counter and returns on almost every frame.
     */
    snd_watch_step();

    /*
     * And the frame-time heartbeat, beside it and for the same reason. It
     * measures present to present, so it has to be called at the same point
     * in every frame; here is that point, and it is before the present so a
     * frame that ends the run has already been counted.
     */
    perf_frame();

    /*
     * And the memory budget, which has to be sampled while the game is
     * running rather than before NitroMain: the guest's own heaps come out of
     * the slab as it boots, so what start-up cost is not the number that
     * answers "does this fit in 64 MB". Every six hundred frames, because
     * mallinfo() walks a free list.
     */
    mem_step((unsigned long)frame);

    frame_present(pc_video_surface(upper), pc_video_surface(upper ^ 1),
                  "", "", upper);

    if (!frame_running()) {
        fprintf(stderr, "3ds-frame: apt ended the run at frame %u\n",
                (unsigned)frame);
        /* The teardown is 3ds_apt.c's and it is registered with atexit(), so
         * this is a plain exit: the report, the DSP, the cartridge and the
         * screens all go through the same sequence the self-test loop takes
         * when it falls out of its own loop. */
        exit(0);
    }
}
