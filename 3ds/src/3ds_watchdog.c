/*
 * 3ds/src/3ds_watchdog.c: see 3ds_watchdog.h.
 */

#include "3ds_watchdog.h"

#include <stdio.h>

#include "3ds_sdcard.h"

#ifdef __3DS__
#include <3ds.h>

#include "3ds_frame.h"
#include "3ds_view.h"
#endif

static const char *sWhere = "start-up";
static unsigned long sSeen;
static unsigned sQuiet;
static unsigned sLimit = WATCHDOG_VBLANKS;
static int sArmed;
static int sPaused;
static int sFired;

void watchdog_where(const char *what)
{
    sWhere = what;
}

const char *watchdog_last_where(void)
{
    return sWhere;
}

unsigned watchdog_quiet(void)
{
    return sQuiet;
}

int watchdog_tick_at(unsigned long frames)
{
    if (!sArmed || sPaused || sFired) {
        return 0;
    }
    if (frames != sSeen) {
        sSeen = frames;
        sQuiet = 0;
        return 0;
    }
    if (++sQuiet < sLimit) {
        return 0;
    }
    /*
     * Once. The callback keeps arriving while the report is being written and
     * drawn (it is the console's VBlank and nothing here stops it) and a
     * second entry would rewrite the file from inside the first.
     */
    sFired = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* The console                                                         */
/* ------------------------------------------------------------------ */

#ifdef __3DS__

/* The guest's own frame clock, weak: the self-test 3dsx links 3ds/src alone
 * and there is no guest in it. Reported for the same reason the breadcrumb is:
 * how far the game got is half of where it stopped. */
extern uint32_t pc_os_vblank_count __attribute__((weak));

/*
 * The save chip's deferred write, weak because the self-test 3dsx has no game.
 * A hang is exactly when an unwritten save is most likely to exist, the port
 * froze partway through doing something, and this is the last code that will
 * run before the process goes.
 */
extern void pc_card_backup_sync(void) __attribute__((weak));

static void watchdog_write(void)
{
    FILE *f = sd_open_write(WATCHDOG_PATH);

    if (f == NULL) {
        return;
    }
    fprintf(f, "verdict HANG\n");
    fprintf(f, "frames %lu\n", frame_count());
    fprintf(f, "quiet-vblanks %u\n", sQuiet);
    fprintf(f, "where %s\n", sWhere);
    fprintf(f, "guest-vblanks %lu\n",
            (unsigned long)(&pc_os_vblank_count != NULL ? pc_os_vblank_count
                                                        : 0u));
    fclose(f);
}

/*
 * The report on both screens, and then hbmenu.
 *
 * NO gspWaitForVBlank() anywhere here. This runs on the GSP event thread,
 * the thread that signals the VBlank event, so waiting for one from inside
 * it waits for itself. svcSleepThread paces the poll instead.
 *
 * It leaves on its own after ten seconds as well as on Start. A console that
 * hung and then went back to its launcher is a console somebody can try again
 * on; one that needs the power button is not, and the report is already on the
 * card by the time this is drawn.
 */
static void watchdog_show(void)
{
    char top[64];
    char bottom[64];
    unsigned i;

    snprintf(top, sizeof top, "HANG: NO FRAME FOR %us AT F:%06lu",
             sQuiet / 60u, frame_count());
    snprintf(bottom, sizeof bottom, "LAST: %s", sWhere);

    for (i = 0; i < 600u; i++) {
        hidScanInput();
        if (hidKeysDown() & (KEY_START | KEY_SELECT)) {
            break;
        }
        view_present(NULL, NULL, top, bottom);
        gfxFlushBuffers();
        gfxSwapBuffers();
        svcSleepThread(16000000ll);
    }

    gfxExit();
    svcExitProcess();
}

static void watchdog_tick(void *arg)
{
    (void)arg;
    if (watchdog_tick_at(frame_count())) {
        if (pc_card_backup_sync != NULL) {
            pc_card_backup_sync();
        }
        watchdog_write();
        watchdog_show();
    }
}

void watchdog_arm(unsigned vblanks)
{
    sLimit = vblanks != 0 ? vblanks : WATCHDOG_VBLANKS;
    sSeen = frame_count();
    sQuiet = 0;
    if (!sArmed) {
        sArmed = 1;
        /* Not one-shot: the callback has to arrive on every VBlank for the
         * whole run, and libctru drops a one-shot after the first. */
        gspSetEventCallback(GSPGPU_EVENT_VBlank0, watchdog_tick, NULL, false);
    }
}

void watchdog_disarm(void)
{
    sArmed = 0;
}

void watchdog_pause(void)
{
    sPaused = 1;
}

void watchdog_resume(void)
{
    sPaused = 0;
    sSeen = frame_count();
    sQuiet = 0;
}

#endif /* __3DS__ */

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#ifdef WATCHDOG_SELFTEST_VERBOSE
#define FAILNOTE() fprintf(stderr, "  3ds_watchdog.c:%d failed\n", __LINE__)
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

static void watchdog_reset(unsigned limit)
{
    sWhere = "start-up";
    sSeen = 0;
    sQuiet = 0;
    sLimit = limit;
    sArmed = 1;
    sPaused = 0;
    sFired = 0;
}

int watchdog_selftest(int *ranOut)
{
    int ran = 0;
    int failed = 0;
    unsigned i;
    int fired = 0;

    /* A console presenting frames never fires, however long it runs. */
    watchdog_reset(10u);
    for (i = 0; i < 1000u; i++) {
        fired += watchdog_tick_at(i);
    }
    CHECK(fired == 0);
    CHECK(watchdog_quiet() == 0);

    /* One that stops fires once, at the limit and not before. */
    watchdog_reset(10u);
    (void)watchdog_tick_at(42u);
    for (i = 0; i < 9u; i++) {
        CHECK(watchdog_tick_at(42u) == 0);
    }
    CHECK(watchdog_tick_at(42u) == 1);
    /* And not again: the callback keeps arriving while the report is being
     * written, and a second one would rewrite the file from inside the
     * first. */
    for (i = 0; i < 100u; i++) {
        CHECK(watchdog_tick_at(42u) == 0);
    }

    /* A frame that arrives late clears the count rather than shortening the
     * next one. A scene that presents every nine ticks of a ten-tick limit
     * runs forever, which is the whole difference between slow and hung. */
    watchdog_reset(10u);
    fired = 0;
    for (i = 0; i < 500u; i++) {
        fired += watchdog_tick_at(i / 9u);
    }
    CHECK(fired == 0);

    /* A suspension is not a hang. */
    watchdog_reset(10u);
    sPaused = 1;
    fired = 0;
    for (i = 0; i < 500u; i++) {
        fired += watchdog_tick_at(7u);
    }
    CHECK(fired == 0);
    CHECK(watchdog_quiet() == 0);

    /* Disarmed is disarmed. */
    watchdog_reset(10u);
    sArmed = 0;
    fired = 0;
    for (i = 0; i < 500u; i++) {
        fired += watchdog_tick_at(7u);
    }
    CHECK(fired == 0);

    /* The breadcrumb is the last one stored, and it is the caller's storage
     * rather than a copy; which is why every caller passes a literal. */
    {
        static const char kWhere[] = "3d rasterizer";

        watchdog_where(kWhere);
        CHECK(watchdog_last_where() == kWhere);
    }

    watchdog_reset(WATCHDOG_VBLANKS);
    sArmed = 0;
    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
