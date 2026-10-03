/*
 * 3ds/src/3ds_perf.c: see 3ds_perf.h.
 */

#include "3ds_perf.h"

#include <stdio.h>
#include <string.h>

#include "3ds_sdcard.h"

#ifdef __3DS__
#include <3ds.h>
#endif

/*
 * The picture's digest and the two polygon counts, all three weak: the
 * self-test .3dsx links neither pc/src nor pc/hw, and this file is part of
 * every link. A NULL here is "that model is not in this binary", which is a
 * different thing from a zero it reported.
 */
extern void pc_video_surface_stats(uint64_t *digest, unsigned *colours)
    __attribute__((weak));
extern uint32_t pc_gpu3d_num_polygons(void) __attribute__((weak));
extern uint32_t pc_gpu3d_soft_polygons_drawn(void) __attribute__((weak));

/*
 * The memoised guest-to-host lookup the software 2D engine goes through for
 * every VRAM, palette and OAM byte it reads. On the desktop that macro is a
 * cast and costs nothing; here it is a call, so how often it fires per frame
 * is the size of a tax the DS never paid.
 */
extern unsigned long hostmap_hits(void) __attribute__((weak));

/*
 * The 2D survey's counters. Weak and declared here rather than
 * included, for the same reason as the two above: the host build of this file
 * links neither the renderer nor the guest memory it reads.
 */
extern void gpu2d_report(FILE *f) __attribute__((weak));
extern unsigned long gpu2d_eligible_total(void) __attribute__((weak));
extern unsigned long gpu2d_claimed_total(void) __attribute__((weak));

/* ...and the 3D survey's, on the same terms. */
extern void gpu3d_report(FILE *f) __attribute__((weak));

/* ...and what the 3D layer cost to hand the compositor. */
extern void layer3d_report(FILE *f) __attribute__((weak));

/* The I/O page through the translator. Weak for the same reason as the rest:
 * The self-test binary has no guest memory bound. */
extern void *armrec_host_ptr(uint32_t guest) __attribute__((weak));

static const volatile uint32_t *perf_reg32(uint32_t a)
{
    return armrec_host_ptr != NULL
        ? (const volatile uint32_t *)armrec_host_ptr(a) : NULL;
}

static const volatile uint16_t *perf_reg16(uint32_t a)
{
    return armrec_host_ptr != NULL
        ? (const volatile uint16_t *)armrec_host_ptr(a) : NULL;
}

typedef struct PerfWindow {
    uint32_t first;         /* frame the window opened on                  */
    uint32_t meanTicks;     /* interval, averaged over the window          */
    uint32_t maxTicks;      /* the worst single interval in it             */
    uint32_t polySum;       /* polygons over the window                    */
    uint16_t polyFrames;    /* frames of it that drew at least one         */
    uint16_t polyMax;       /* the most any one of them drew               */
    uint64_t digest;        /* the picture as the window closed            */
    uint32_t hostmap;       /* host-pointer lookups per frame              */
    uint32_t bg;            /* engine-frames 11.4 could draw, in this window */
    uint32_t dispcnt[2];    /* the two 2D engines' control registers       */
    uint16_t powcnt;        /* which engine drives which LCD, and 3D power */
} PerfWindow;

static PerfWindow sWin[PERF_MAX_WINDOWS];
static int sWinN;
static unsigned long sDropped;

static unsigned long sFrames;
static uint64_t sPrevTick;
static uint64_t sExclude;

/* The window being filled. */
static uint32_t sAccFirst;
static uint64_t sAccTicks;
static uint32_t sAccMax;
static uint32_t sAccPolySum;
static uint16_t sAccPolyFrames;
static uint16_t sAccPolyMax;
static unsigned sAccN;

static uint64_t sTotalTicks;
static unsigned long sTotalIntervals;

static unsigned long sHostPrev;
static uint64_t sAccHost;
/* The eligibility total as the last window closed, so a window's own count is
 * a subtraction. */
static unsigned long sAccBgFirst;

/* The frame breakdown, accumulated over the window being filled. */
static uint64_t sPhase[PERF_PHASES];
static uint64_t sAccPhase[PERF_PHASES];
static uint32_t sWinPhase[PERF_MAX_WINDOWS][PERF_PHASES];

void perf_phase(int phase, uint64_t ticks)
{
    if (phase >= 0 && phase < PERF_PHASES) {
        sPhase[phase] += ticks;
    }
}

unsigned long perf_ticks_to_cms(uint64_t ticks)
{
    /*
     * Hundredths of a millisecond: ticks * 100 * 1000 / (ticks per ms * 1000).
     * The multiply is done in 64 bits before the divide, so a window's summed
     * ticks (which pass 4e9 on any frame slower than 16 ms) keep their
     * precision, and the 1000 that carries the clock's fractional hertz never
     * has to be rounded away.
     */
    return (unsigned long)((ticks * 100000ull) / PERF_TICKS_PER_MS_X1000);
}

void perf_exclude(uint64_t ticks)
{
    sExclude += ticks;
}

static int sClosed;

void perf_mark_closed(void)
{
    sClosed = 1;
}

int perf_windows(void)
{
    return sWinN;
}

unsigned long perf_frames(void)
{
    return sFrames;
}

/*
 * The accumulation, with the tick and the polygon count handed in, so a build
 * machine drives the whole of it. Returns the index of the window it just
 * closed, or -1.
 */
static int perf_sample(uint64_t tick, unsigned long polys)
{
    int closed = -1;

    if (sFrames == 0) {
        sAccFirst = 0;
    } else {
        uint64_t delta = tick - sPrevTick;

        /* The instruments' own cost, taken off the frame it landed in. It can
         * exceed the interval only if the clock went backwards, which it does
         * not, but a wrap would otherwise turn into an enormous worst frame. */
        if (sExclude < delta) {
            delta -= sExclude;
        } else {
            delta = 0;
        }
        sExclude = 0;

        sAccTicks += delta;
        if (delta > sAccMax) {
            sAccMax = (uint32_t)delta;
        }
        sTotalTicks += delta;
        sTotalIntervals++;
    }
    sPrevTick = tick;

    if (polys != 0) {
        sAccPolyFrames++;
        sAccPolySum += (uint32_t)polys;
        if (polys > sAccPolyMax) {
            sAccPolyMax = (uint16_t)polys;
        }
    }

    {
        int i;

        for (i = 0; i < PERF_PHASES; i++) {
            sAccPhase[i] += sPhase[i];
            sPhase[i] = 0;
        }
    }

    if (hostmap_hits != NULL) {
        unsigned long now = hostmap_hits();

        /*
         * The counter is zeroed by hostmap_flush(), which the VRAM model calls
         * on every bank remap, so a frame that remapped a bank sees a
         * counter that went backwards, and the unsigned subtraction turned
         * that into a per-frame mean of 143 million lookups in a 164 ms frame.
         * A restart contributes what the counter reads now, which is what the
         * frame did after the flush; what it did before it is lost, and a
         * silently lost few thousand is a better answer than an impossible
         * hundred million.
         */
        sAccHost += now >= sHostPrev ? (uint64_t)(now - sHostPrev)
                                     : (uint64_t)now;
        sHostPrev = now;
    }

    sFrames++;
    sAccN++;

    if (sAccN >= PERF_WINDOW) {
        if (sWinN < PERF_MAX_WINDOWS) {
            PerfWindow *w = &sWin[sWinN];

            w->first = sAccFirst;
            /* The first frame of the run has no interval before it, so the
             * opening window averages over one sample fewer than it has
             * frames. */
            w->meanTicks = sAccN > 1
                ? (uint32_t)(sAccTicks / (sAccN - (sAccFirst == 0 ? 1 : 0)))
                : 0;
            w->maxTicks = sAccMax;
            w->polySum = sAccPolySum;
            w->polyFrames = sAccPolyFrames;
            w->polyMax = sAccPolyMax;
            w->digest = 0;
            w->hostmap = (uint32_t)(sAccHost / (sAccN ? sAccN : 1));
            /*
             * The background path's survey, as a delta rather than a total: it is what says
             * which windows of two runs are the same scene.
             */
            if (gpu2d_eligible_total != NULL) {
                unsigned long now = gpu2d_eligible_total();

                w->bg = (uint32_t)(now - sAccBgFirst);
                sAccBgFirst = now;
            } else {
                w->bg = 0;
            }
            /*
             * The two engines' control registers as the window closed. The
             * desktop port's frame manifest carries the same three numbers per
             * frame, so a picture that differs can be told apart into "the
             * game programmed it differently" and "the renderer drew the same
             * state differently" without looking at a single pixel.
             */
            {
                const volatile uint32_t *a = perf_reg32(0x04000000u);
                const volatile uint32_t *b = perf_reg32(0x04001000u);
                const volatile uint16_t *p = perf_reg16(0x04000304u);

                w->dispcnt[0] = a != NULL ? *a : 0u;
                w->dispcnt[1] = b != NULL ? *b : 0u;
                w->powcnt = p != NULL ? *p : 0u;
            }
            {
                int i;

                for (i = 0; i < PERF_PHASES; i++) {
                    sWinPhase[sWinN][i] =
                        (uint32_t)(sAccPhase[i] / (sAccN ? sAccN : 1));
                }
            }
            closed = sWinN++;
        } else {
            sDropped++;
        }
        sAccFirst = (uint32_t)sFrames;
        sAccTicks = 0;
        sAccMax = 0;
        sAccPolySum = 0;
        sAccPolyFrames = 0;
        sAccPolyMax = 0;
        sAccHost = 0;
        sAccN = 0;
        {
            int i;

            for (i = 0; i < PERF_PHASES; i++) {
                sAccPhase[i] = 0;
            }
        }
    }
    return closed;
}

int perf_write(const char *path)
{
    FILE *f = sd_open_write(path);
    int i;

    if (f == NULL) {
        return -1;
    }

    fprintf(f, "frames %lu\n", sFrames);
    fprintf(f, "windows %d\n", sWinN);
    fprintf(f, "window-frames %u\n", (unsigned)PERF_WINDOW);
    fprintf(f, "dropped-windows %lu\n", sDropped);
    fprintf(f, "mean-cms %lu\n",
            sTotalIntervals != 0
                ? perf_ticks_to_cms(sTotalTicks / sTotalIntervals)
                : 0ul);
    fprintf(f, "digests %d\n", pc_video_surface_stats != NULL);
    fprintf(f, "closed %d\n", sClosed);

    /*
     * The background path's survey, which belongs in this file's report rather than in one of
     * its own: it is a per-frame count of what the 2D engines were asked for,
     * and every reader of it is already reading the frame times beside it.
     */
    if (gpu2d_report != NULL) {
        gpu2d_report(f);
    }

    /* The 3D rasterizer's, for the same reason and in the same place: a per-frame count of
     * what the 3D engine was asked for, beside the span it costs. */
    if (gpu3d_report != NULL) {
        gpu3d_report(f);
    }

    /* The 3D layer's: how often the layer was packed for the compositor, and how often
     * the blend then kept the frame off the GPU path anyway. */
    if (layer3d_report != NULL) {
        layer3d_report(f);
    }

    /* One window per line, the first frame first, so a range of frames is a
     * grep and an awk rather than a parser. Milliseconds are printed in
     * hundredths and spelled with the point, because every reader of this
     * file is a person or an awk and both want the same thing. */
    fprintf(f, "# w first mean-ms max-ms poly-frames poly-max poly-mean "
               "digest blit-ms flush-ms wait-ms gpu2d-ms gpu3d-ms game-ms "
               "hostmap powcnt dispcnt-main dispcnt-sub bg\n");
    for (i = 0; i < sWinN; i++) {
        const PerfWindow *w = &sWin[i];
        unsigned long mean = perf_ticks_to_cms(w->meanTicks);
        unsigned long max = perf_ticks_to_cms(w->maxTicks);
        unsigned long blit = perf_ticks_to_cms(sWinPhase[i][PERF_BLIT]);
        unsigned long flush = perf_ticks_to_cms(sWinPhase[i][PERF_FLUSH]);
        unsigned long wait = perf_ticks_to_cms(sWinPhase[i][PERF_WAIT]);
        unsigned long g2d = perf_ticks_to_cms(sWinPhase[i][PERF_GPU2D]);
        unsigned long g3d = perf_ticks_to_cms(sWinPhase[i][PERF_GPU3D]);
        /* What is left is the game's own code, which is the quantity the
         * other four exist to isolate. Floored at zero: the measured phases
         * are sampled inside the interval they belong to, but the window's
         * means are rounded independently. */
        unsigned long acct = blit + flush + wait + g2d + g3d;
        unsigned long game = mean > acct ? mean - acct : 0;

        fprintf(f, "w %lu %lu.%02lu %lu.%02lu %u %u %lu %016llX"
                   " %lu.%02lu %lu.%02lu %lu.%02lu %lu.%02lu %lu.%02lu"
                   " %lu.%02lu %lu"
                   " %04lX %08lX %08lX %lu\n",
                (unsigned long)w->first,
                mean / 100, mean % 100,
                max / 100, max % 100,
                (unsigned)w->polyFrames, (unsigned)w->polyMax,
                w->polyFrames != 0
                    ? (unsigned long)(w->polySum / w->polyFrames) : 0ul,
                (unsigned long long)w->digest,
                blit / 100, blit % 100,
                flush / 100, flush % 100,
                wait / 100, wait % 100,
                g2d / 100, g2d % 100,
                g3d / 100, g3d % 100,
                game / 100, game % 100,
                (unsigned long)w->hostmap,
                (unsigned long)w->powcnt,
                (unsigned long)w->dispcnt[0],
                (unsigned long)w->dispcnt[1],
                (unsigned long)w->bg);
    }

    fclose(f);
    return 0;
}

#ifdef __3DS__
/* The claimed total as the last window closed, for the digest below. Declared
 * inside this guard because the host build of this file has no frame loop and
 * would carry it unused. */
static unsigned long sAccClaimedFirst;

void perf_frame(void)
{
    unsigned long polys = 0;
    int closed;

    /*
     * The diagnostic screen ends frames through the same frame_present(), so
     * its blits and its waits land in sPhase before the game has drawn
     * anything. Dropping them at the first game frame keeps the opening
     * window a measurement of the game rather than of the menu in front of
     * it, without this it reported phases that summed to more than the
     * interval they were supposed to divide.
     */
    if (sFrames == 0) {
        memset(sPhase, 0, sizeof sPhase);
    }

    if (pc_gpu3d_soft_polygons_drawn != NULL && pc_gpu3d_num_polygons != NULL) {
        /*
         * The same two counts the sound check samples, and for the same
         * reason: the live count is reset by the next SWAP_BUFFERS and the
         * drawn count by the next render, so both have to be read every frame
         * or neither means anything. The larger is the frame's work, a
         * frame that submitted geometry the rasterizer then culled still did
         * the submitting.
         */
        unsigned long drawn = pc_gpu3d_soft_polygons_drawn();
        unsigned long live = pc_gpu3d_num_polygons();

        polys = drawn > live ? drawn : live;
    }

    closed = perf_sample(svcGetSystemTick(), polys);
    if (closed < 0) {
        return;
    }

    /* Everything below is the instrument's, so it is timed and handed back
     * to the next interval's exclusion. */
    {
        uint64_t started = svcGetSystemTick();

        /*
         * The digest is of pc_video's surfaces, so it means nothing for a
         * window in which the PICA composed an engine and the software
         * renderer never wrote one. Zero, which no picture hashes to,
         * rather than the stale frame that would otherwise be reported.
         */
        if (pc_video_surface_stats != NULL) {
            unsigned long claimed = gpu2d_claimed_total != NULL
                                        ? gpu2d_claimed_total() : 0ul;

            if (claimed != sAccClaimedFirst) {
                sWin[closed].digest = 0ull;
                sAccClaimedFirst = claimed;
            } else {
                pc_video_surface_stats(&sWin[closed].digest, NULL);
            }
        }
        if ((closed % PERF_WRITE_EVERY) == 0) {
            (void)perf_write(PERF_PATH);
        }
        perf_exclude(svcGetSystemTick() - started);
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#ifdef PERF_SELFTEST_VERBOSE
#define FAILNOTE() fprintf(stderr, "  3ds_perf.c:%d failed\n", __LINE__)
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

static void perf_reset(void)
{
    memset(sWin, 0, sizeof sWin);
    sWinN = 0;
    sDropped = 0;
    sFrames = 0;
    sPrevTick = 0;
    sExclude = 0;
    sAccFirst = 0;
    sAccTicks = 0;
    sAccMax = 0;
    sAccPolySum = 0;
    sAccPolyFrames = 0;
    sAccPolyMax = 0;
    sAccN = 0;
    sTotalTicks = 0;
    sTotalIntervals = 0;
    sClosed = 0;
    memset(sPhase, 0, sizeof sPhase);
    memset(sAccPhase, 0, sizeof sAccPhase);
    memset(sWinPhase, 0, sizeof sWinPhase);
}

int perf_selftest(int *ranOut)
{
    /* One display refresh, from 3ds_frame.h, repeated rather than included
     * so this file has no opinion about the frame boundary. */
    const uint64_t refresh = 4481519ull;
    uint64_t t = 1000000ull;
    unsigned i;
    int ran = 0;
    int failed = 0;

    /* The conversion, against the clock's own rate. */
    CHECK(perf_ticks_to_cms(0) == 0);
    CHECK(perf_ticks_to_cms(268112ull) == 100);          /* 1.00 ms */
    CHECK(perf_ticks_to_cms(refresh) == 1671);           /* 16.71 ms */
    /* And in the range a whole window sums to, where a 32-bit intermediate
     * would have wrapped: sixty frames of a slow console. */
    CHECK(perf_ticks_to_cms(refresh * 60) == 100290);

    /* A console holding exactly one refresh a frame, for two windows. */
    perf_reset();
    for (i = 0; i < PERF_WINDOW * 2; i++) {
        (void)perf_sample(t, 0);
        t += refresh;
    }
    CHECK(perf_frames() == PERF_WINDOW * 2);
    CHECK(perf_windows() == 2);
    CHECK(sWin[0].first == 0);
    CHECK(sWin[1].first == PERF_WINDOW);
    /* The opening window has one fewer interval than it has frames, and both
     * windows must still come out at the refresh rather than one of them
     * reading 59/60ths of it. */
    CHECK(perf_ticks_to_cms(sWin[0].meanTicks) == 1671);
    CHECK(perf_ticks_to_cms(sWin[1].meanTicks) == 1671);
    CHECK(sWin[0].polyFrames == 0 && sWin[0].polyMax == 0);

    /* One long frame, and it must land in max and not be averaged away. */
    perf_reset();
    for (i = 0; i < PERF_WINDOW; i++) {
        (void)perf_sample(t, 0);
        t += (i == 30) ? refresh * 10 : refresh;
    }
    CHECK(perf_windows() == 1);
    CHECK(perf_ticks_to_cms(sWin[0].maxTicks) == 16715);   /* ten refreshes */
    CHECK(sWin[0].meanTicks > refresh);

    /* The exclusion: the same long frame, declared as the instrument's own
     * cost, leaves the window reading as if it had never happened. */
    perf_reset();
    for (i = 0; i < PERF_WINDOW; i++) {
        (void)perf_sample(t, 0);
        if (i == 30) {
            t += refresh * 10;
            perf_exclude(refresh * 9);
        } else {
            t += refresh;
        }
    }
    CHECK(perf_ticks_to_cms(sWin[0].maxTicks) == 1671);
    CHECK(perf_ticks_to_cms(sWin[0].meanTicks) == 1671);

    /* An exclusion larger than the interval it lands in floors at zero
     * instead of wrapping into an enormous worst frame. */
    perf_reset();
    (void)perf_sample(t, 0);
    perf_exclude(refresh * 4);
    t += refresh;
    (void)perf_sample(t, 0);
    CHECK(sAccMax == 0);

    /* Polygons: counted per frame, and the mean is over the frames that drew
     * rather than over the window, because a scene that draws every third
     * frame is not a scene drawing a third as much. */
    perf_reset();
    for (i = 0; i < PERF_WINDOW; i++) {
        (void)perf_sample(t, (i % 3 == 0) ? 30 + i : 0);
        t += refresh;
    }
    CHECK(sWin[0].polyFrames == 20);
    CHECK(sWin[0].polyMax == 30 + 57);
    CHECK(sWin[0].polySum / sWin[0].polyFrames == (30 + (0 + 57) / 2));

    /* The window that closes returns its own index, and nothing else does. */
    perf_reset();
    for (i = 0; i < PERF_WINDOW - 1; i++) {
        CHECK(perf_sample(t, 0) == -1);
        t += refresh;
    }
    CHECK(perf_sample(t, 0) == 0);

    perf_reset();
    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
