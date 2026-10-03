/*
 * 3ds/src/3ds_apt.c: see 3ds_apt.h.
 */

#include "3ds_apt.h"

#include <stdio.h>
#include <stdlib.h>

#include "3ds_perf.h"

#ifdef __3DS__
#include <3ds.h>

#include "3ds_audio.h"
#include "3ds_gpu.h"
#include "3ds_rom.h"
#include "3ds_watchdog.h"
#endif

static int      sSuspends;
static int      sOpen;              /* a suspension is in progress          */
static uint64_t sSuspendTick;
static uint32_t sSuspendVBlanks;
static uint64_t sTotalTicks;
static uint32_t sLost;              /* guest VBlanks that passed while away */

void apt_suspend_at(uint64_t tick, uint32_t vblanks)
{
    /*
     * A second suspension without an intervening resume keeps the first
     * one's marks. libctru pairs its hooks, but a sleep that arrives while
     * the Home Menu is coming up would otherwise re-base the interval and
     * hand perf_exclude() a number smaller than the gap it is covering,
     * which is the direction that quietly inflates a frame time.
     */
    if (sOpen) {
        return;
    }
    sOpen = 1;
    sSuspends++;
    sSuspendTick = tick;
    sSuspendVBlanks = vblanks;
}

uint64_t apt_resume_at(uint64_t tick, uint32_t vblanks)
{
    uint64_t elapsed;

    if (!sOpen) {
        return 0;
    }
    sOpen = 0;

    /* The tick counter does not go backwards, but a resume reported before
     * its own suspend would underflow into an exclusion larger than the run. */
    elapsed = tick > sSuspendTick ? tick - sSuspendTick : 0;
    sTotalTicks += elapsed;

    /*
     * The check the whole file exists for. Guest time is pc_os_vblank_count
     * and nothing but the guest moves it, so this difference is zero unless
     * somebody has taught the port to catch up on wall-clock time; which is
     * exactly the change that would make the day/night cycle and the RNG
     * depend on how long the player left the lid shut.
     */
    if (vblanks != sSuspendVBlanks) {
        sLost += vblanks - sSuspendVBlanks;
    }
    return elapsed;
}

int apt_suspends(void) { return sSuspends; }
uint32_t apt_guest_frames_lost(void) { return sLost; }
uint64_t apt_suspended_ticks(void) { return sTotalTicks; }

/* ------------------------------------------------------------------ */
/* The console                                                         */
/* ------------------------------------------------------------------ */

#ifdef __3DS__

/*
 * The guest's own frame clock, weak: the self-test 3dsx links 3ds/src alone
 * and there is no guest in it to have a clock. A NULL here reads as a clock
 * that never moves, which is the truth for that binary.
 */
extern uint32_t pc_os_vblank_count __attribute__((weak));

/* The save chip's deferred write. Weak for the same reason: the self-test
 * 3dsx has no game and no save. */
extern void pc_card_backup_sync(void) __attribute__((weak));

static void save_sync(void)
{
    if (pc_card_backup_sync != NULL) {
        pc_card_backup_sync();
    }
}

static uint32_t guest_vblanks(void)
{
    return &pc_os_vblank_count != NULL ? pc_os_vblank_count : 0u;
}

static aptHookCookie sCookie;
static int sClosing;
static int sDown;                   /* apt_shutdown() has run */

int apt_closing(void) { return sClosing; }

void apt_shutdown(void)
{
    if (sDown) {
        return;
    }
    sDown = 1;

    /* First of all: frames stop here on purpose, and a teardown that takes
     * longer than the limit is not a hang. */
    watchdog_disarm();

    /* Then the save, before anything that can fail: a run that ends between a
     * save's last card command and the settled write would otherwise lose it,
     * and losing a save is the worst thing this port can do to a player. */
    save_sync();

    /*
     * Then the report, because it is the only thing this console says about
     * what a frame cost and it is rewritten every tenth window, an ordinary
     * quit would otherwise lose up to ten of them, and quitting is how a
     * measured run normally ends.
     */
    perf_mark_closed();
    (void)perf_write(PERF_PATH);

    /* Then the services, innermost first: the DSP is the one with a hardware
     * component still running, the cartridge is a mount, and the screens are
     * what libctru's own exit path would otherwise trip over. */
    audio_exit();
    rom_fs_exit();
    /* citro3d before the screens it draws on, and it writes PRESENT_VERIFY's
     * report on the way out for the same reason the perf report is written
     * above: an ordinary quit is how a measured run ends. */
    gpu_exit();
    gfxExit();
}

static void apt_callback(APT_HookType hook, void *param)
{
    (void)param;

    switch (hook) {
    case APTHOOK_ONSUSPEND:
    case APTHOOK_ONSLEEP:
        /* The sound before the bookkeeping: the DSP is playing into the Home
         * Menu until this call and every tick of it is audible. */
        audio_suspend();
        /* No frames are presented while the Home Menu has the console and it
         * can hold it for minutes, which is exactly what the hang detector
         * is looking for. */
        watchdog_pause();
        apt_suspend_at(svcGetSystemTick(), guest_vblanks());
        break;

    case APTHOOK_ONRESTORE:
    case APTHOOK_ONWAKEUP:
        perf_exclude(apt_resume_at(svcGetSystemTick(), guest_vblanks()));
        watchdog_resume();
        audio_resume();
        break;

    case APTHOOK_ONEXIT:
        /*
         * Nothing is torn down here. This hook fires from inside aptExit(),
         * which libctru runs as part of exit(), so apt_shutdown() has
         * already gone through atexit() by the time it arrives, and doing the
         * work here instead would put gfxExit() after the APT session it
         * needs. What it is good for is the flag: a close that the system
         * decided is visible one call earlier than aptMainLoop() answering
         * false.
         */
        sClosing = 1;
        break;

    default:
        break;
    }
}

void apt_install(void)
{
    /*
     * atexit before the hook. The guest path out of a frame is exit(0) from
     * pc_view_publish(), main() returns down its own stack, and the system's
     * close goes through both, one registration covers all three, and the
     * handler runs before libctru's __appExit takes apt and srv away.
     */
    (void)atexit(apt_shutdown);
    aptHook(&sCookie, apt_callback, NULL);
}

#endif /* __3DS__ */

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#ifdef APT_SELFTEST_VERBOSE
#define FAILNOTE() fprintf(stderr, "  3ds_apt.c:%d failed\n", __LINE__)
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

static void apt_reset(void)
{
    sSuspends = 0;
    sOpen = 0;
    sSuspendTick = 0;
    sSuspendVBlanks = 0;
    sTotalTicks = 0;
    sLost = 0;
}

int apt_selftest(int *ranOut)
{
    int ran = 0;
    int failed = 0;

    /* A Home visit: the guest is frozen at frame 1,200 and comes back at the
     * same frame, ninety seconds of host time later. */
    apt_reset();
    apt_suspend_at(1000ull, 1200u);
    CHECK(apt_suspends() == 1);
    CHECK(apt_resume_at(1000ull + 24000000ull, 1200u) == 24000000ull);
    CHECK(apt_guest_frames_lost() == 0);
    CHECK(apt_suspended_ticks() == 24000000ull);

    /* Two of them accumulate, and the exclusion is per suspension. */
    apt_suspend_at(30000000ull, 1200u);
    CHECK(apt_resume_at(30000500ull, 1200u) == 500ull);
    CHECK(apt_suspends() == 2);
    CHECK(apt_suspended_ticks() == 24000500ull);

    /* Guest time that moved while we were away is the failure this is for. */
    apt_reset();
    apt_suspend_at(1000ull, 1200u);
    (void)apt_resume_at(2000ull, 1260u);
    CHECK(apt_guest_frames_lost() == 60u);

    /* A resume with no suspension open excludes nothing. Handing
     * perf_exclude() a whole run's worth of ticks would floor the interval it
     * landed in at zero and read as a frame that cost nothing. */
    apt_reset();
    CHECK(apt_resume_at(9999ull, 0u) == 0ull);
    CHECK(apt_suspends() == 0);

    /* A sleep arriving inside a Home suspension keeps the first one's mark,
     * so the exclusion still covers the whole gap. */
    apt_reset();
    apt_suspend_at(1000ull, 5u);
    apt_suspend_at(5000ull, 5u);
    CHECK(apt_suspends() == 1);
    CHECK(apt_resume_at(9000ull, 5u) == 8000ull);

    /* And a resume reported before its own suspend excludes nothing rather
     * than underflowing into an exclusion the size of the address space. */
    apt_reset();
    apt_suspend_at(5000ull, 0u);
    CHECK(apt_resume_at(1000ull, 0u) == 0ull);

    apt_reset();
    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
