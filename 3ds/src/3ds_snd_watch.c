/*
 * 3ds/src/3ds_snd_watch.c: what the game keyed its channels with, judged.
 *
 * See 3ds_snd_watch.h for why this exists. What is here is the walk over
 * pc/hw/pc_spu.c's keyon log, the three-way classification, and the report.
 *
 * The externs are declared here rather than included. Pc/include is not on
 * this chain's include path and pc_spu.h drags in the whole SPU
 * interface for five names. 3ds_frame.c declares pc_video's two hooks the same
 * way. The shapes have to match pc/hw/pc_spu.c and nothing but a link error
 * would say if they stopped; the sizes are deliberately left off the array
 * bounds so only the element type is being claimed.
 *
 * Not weak, on purpose. 3ds_audio.c takes pc_audio's entry points weakly
 * because a build without the game still has to link and run its self-tests.
 * This file is the opposite: a build that has no SPU in it has nothing for
 * this check to say, and a silently absent log would make the check pass by
 * finding no bad addresses. So the host test supplies the log itself
 * (3ds/tests/snd_watch.c) and the console gets pc_spu.c's.
 */

#include "3ds_snd_watch.h"

#include <stdio.h>
#include <string.h>

#include "3ds_guest.h"
#include "3ds_guest_map.h"
#include "3ds_sdcard.h"
#include "3ds_snd_addr.h"
#include "3ds_window.h"

/* pc/hw/pc_spu.c. Every distinct (source address, byte length) a channel has
 * been keyed on with, in the order they were first seen, and the count of the
 * ones that did not fit. */
extern uint32_t pc_spu_src_log[][2];
extern uint32_t pc_spu_src_log_n;
extern uint32_t pc_spu_src_log_dropped;

/* pc/hw/pc_spu.c again: source addresses the SPU could not read at all, split
 * into [0] a pointer into the port's own image and [1] anything else. Those
 * are events at the register write rather than distinct waves, so they are
 * reported next to the tallies below and not added into them. */
extern uint32_t pc_spu_unreadable[2];
extern uint32_t pc_spu_unreadable_first[2];

/* And the distinct ones: raw word, channel, that channel's control word, and
 * how many times the pair arrived. The bound is pc/include/pc_spu.h's; it is
 * spelled again rather than included for the reason the names are. */
#define SND_WATCH_UNREAD_LOG 16
extern uint32_t pc_spu_unread_log[SND_WATCH_UNREAD_LOG][4];
extern uint32_t pc_spu_unread_log_n;

/*
 * 3ds/src/armrec_rt_3ds.c: geometry command ports handed out while the 3D
 * engine is not installed. Reported here because those writes used to land in
 * the sound registers; the two blocks are the same bytes of one I/O page,
 * so a non-zero count beside a clean tally is what says the separation is
 * holding rather than that no geometry ran.
 */
extern unsigned long armrec_gx_discarded(void);

/*
 * And what the geometry engine has done with the commands it now keeps, which
 * is here because this file owns the only text channel off the console and not
 * because it has anything to do with sound. Both are per-frame counts, what
 * the geometry side has built and what the rasterizer drew, so they are
 * sampled every frame and reported as "frames with a polygon" and "the most in
 * one frame". Zero frames with a non-zero discard count means the engine is
 * not installed; zero without one means the game has not asked for a polygon,
 * which is true of the legal screens.
 */
extern uint32_t pc_gpu3d_num_polygons(void);
extern uint32_t pc_gpu3d_soft_polygons_drawn(void);

/*
 * And the three counters that say where a silence began, because "no channel
 * was keyed" has several causes and they are not the same repair:
 *
 *   pc_spu_latched   registers the latch has replayed, ever. Zero means the
 *                    driver never wrote a sound register the latch could see,
 *                    which is a question about the I/O page and not about
 *                    sound at all.
 *   pc_spu_keyons    keyons the latch acted on: [0] a 0->1 edge of bit 31,
 *                    [1] one the driver named separately. Non-zero with an
 *                    empty log means channels were started with no length.
 *   pc_spu_samples() mix steps produced. Zero means the mixer is not being
 *                    run, whatever the registers say.
 */
extern uint64_t pc_spu_latched;
extern uint32_t pc_spu_keyons[2];
extern uint64_t pc_spu_samples(void);

/* pc/src/pc_arm7snd.c: how many times the HLE sound driver's loop body has
 * run, and how many command words the ARM9 has sent it over PXI tag 7. The
 * pair separates a driver that is never pumped from one that is pumped and
 * never asked for a sound, from the SPU's end those look the same. */
extern unsigned long pc_arm7snd_steps;
extern unsigned long pc_arm7snd_commands;
extern uint32_t pc_arm7snd_cmdlog[8];
extern uint32_t pc_arm7snd_arglog[6][5];
extern unsigned long pc_arm7snd_arglog_n;

/*
 * The sound driver's shared work area, which the ARM9 hands it as a pointer
 * and which is where the driver publishes what it is doing: the acknowledged
 * command tag, the mask of active players, the mask of active channels, and
 * player 0's tick counter. Between them these say whether a silence is a
 * sequencer that never started, one that started and never ticked, or one
 * that is ticking and cannot get a channel. Declared by hand for the reason
 * the SPU's names are: the driver's headers are not on this include path, and
 * only the first four fields are being claimed.
 */
struct snd_shared_head {
    uint32_t finished_command_tag;
    uint32_t player_status;
    uint16_t channel_status;
    uint16_t capture_status;
    uint8_t  pad[0x14];
    struct {
        int16_t  local_vars[16];
        uint32_t tick_counter;
    } players[1];
};
extern struct snd_shared_head *arm7_SNDi_SharedWork;

/* The ARM7's sound registers, where the driver stores them: sixteen channels
 * of four words at 0x04000400. Read through the translator, because on this
 * console that address is a number. */
#define SND_IO_BASE 0x04000400u

/* One report every this many frames at most, so a boot that keeps finding new
 * waves does not spend its frame budget on the SD card. */
#define SND_WATCH_EVERY 60u

/*
 * ...and one every this many whatever happens, because "how far did the boot
 * get" is half of what a NONE verdict means and the frame count is the only
 * thing that says it. Without this the file keeps frame 1's number for ever
 * and a run that never reached a note looks the same as one that never
 * started. Two seconds of a console running at speed, which on this one is
 * about forty.
 */
#define SND_WATCH_HEARTBEAT 120u

/* What one logged address turned out to be. Internal: what leaves this file
 * is the tallies and the verdict. */
enum {
    SND_WATCH_WINDOW_KIND,
    SND_WATCH_STRAY_KIND,
    SND_WATCH_LOST_KIND
};

static uint32_t sSeen;          /* entries of the log already classified */
static unsigned sWindow;
static unsigned sStray;
static unsigned sLost;
static uint32_t sFirstWindow;
static uint32_t sFirstBad;
static uint32_t sFirstLen;      /* the length that came with sFirstWindow */
static unsigned long sFrames;
static unsigned long sUnreadAt;  /* the frame the first bad lane arrived on */
static unsigned long sPolyFrames; /* frames that drew at least one polygon   */
static unsigned long sPolyMax;    /* the most drawn in any one of them       */
static unsigned long sWrittenAt;
static int sDirty;
static const char *sPath = SND_WATCH_PATH;

void snd_watch_set_path(const char *path)
{
    sPath = (path != NULL) ? path : SND_WATCH_PATH;
}

/*
 * The one judgement in this file. `sad` is what the register holds, already
 * masked to bits 26..2 by pc_spu.c, so this is asking what a DMA engine
 * would find there.
 *
 * The window test is a range test on the guest address and not a question
 * about the host pointer, because that is the claim: the wave is in the block
 * pc_guest_window_alloc() handed SoundSystem_Get(). armrec_host_ptr() is still
 * asked first, because an address inside the window that does not translate
 * would mean the window itself is not mapped, and that is a different failure
 * from a wave being in the wrong place.
 */
static int classify(uint32_t sad)
{
    if (armrec_host_ptr(sad) == NULL) {
        return SND_WATCH_LOST_KIND;
    }
    if (sad - (uint32_t)GUEST_BASE_WINDOW < (uint32_t)GUEST_SIZE_WINDOW) {
        return SND_WATCH_WINDOW_KIND;
    }
    return SND_WATCH_STRAY_KIND;
}

void snd_watch_reset(void)
{
    sSeen = 0;
    sWindow = 0;
    sStray = 0;
    sLost = 0;
    sFirstWindow = 0;
    sFirstBad = 0;
    sFirstLen = 0;
    sFrames = 0;
    sUnreadAt = 0;
    sPolyFrames = 0;
    sPolyMax = 0;
    sWrittenAt = 0;
    sDirty = 0;
}

/* Classify whatever the SPU has added to the log since the last call. Split
 * out so the self-test can drive it without a frame. */
static void take(void)
{
    while (sSeen < pc_spu_src_log_n) {
        uint32_t sad = pc_spu_src_log[sSeen][0];
        uint32_t len = pc_spu_src_log[sSeen][1];

        switch (classify(sad)) {
        case SND_WATCH_WINDOW_KIND:
            if (sWindow++ == 0) {
                sFirstWindow = sad;
                sFirstLen = len;
            }
            break;
        case SND_WATCH_STRAY_KIND:
            if (sStray++ == 0 && sFirstBad == 0) {
                sFirstBad = sad;
            }
            break;
        default:
            if (sLost++ == 0 && sFirstBad == 0) {
                sFirstBad = sad;
            }
            break;
        }
        sSeen++;
        sDirty = 1;
    }
}

unsigned snd_watch_keyons(void) { return sWindow + sStray + sLost; }
unsigned snd_watch_window(void) { return sWindow; }
unsigned snd_watch_stray(void)  { return sStray; }
unsigned snd_watch_lost(void)   { return sLost; }

uint32_t snd_watch_first_window(void) { return sFirstWindow; }
uint32_t snd_watch_first_bad(void)    { return sFirstBad; }

int snd_watch_verdict(void)
{
    if (snd_watch_keyons() == 0) {
        return SND_WATCH_NONE;
    }
    if (sStray != 0 || sLost != 0
        || pc_spu_unreadable[0] != 0 || pc_spu_unreadable[1] != 0) {
        return SND_WATCH_FAIL;
    }
    return SND_WATCH_PASS;
}

int snd_watch_write(const char *path)
{
    static const char *const names[3] = { "NONE", "PASS", "FAIL" };
    FILE *f = sd_open_write(path);
    int verdict = snd_watch_verdict();

    if (f == NULL) {
        return -1;
    }

    /*
     * One key per line, the value last, so a grep can read any of it without
     * a parser. The SAD word worth recording is `first-window`: the
     * first address the game actually keyed a channel with, the length that
     * came with it, and the host pointer the translator answers for it, the
     * three numbers together are what says the sound heap is where the patch
     * put it.
     */
    fprintf(f, "frames %lu\n", sFrames);
    fprintf(f, "keyons %u\n", snd_watch_keyons());
    fprintf(f, "dropped %lu\n", (unsigned long)pc_spu_src_log_dropped);
    fprintf(f, "window %u\n", sWindow);
    fprintf(f, "stray %u\n", sStray);
    fprintf(f, "lost %u\n", sLost);
    fprintf(f, "unreadable-host %lu %08lX\n",
            (unsigned long)pc_spu_unreadable[0],
            (unsigned long)pc_spu_unreadable_first[0]);
    fprintf(f, "unreadable-lost %lu %08lX\n",
            (unsigned long)pc_spu_unreadable[1],
            (unsigned long)pc_spu_unreadable_first[1]);
    fprintf(f, "unreadable-at %lu\n", sUnreadAt);
    fprintf(f, "gx-discarded %lu\n", armrec_gx_discarded());
    fprintf(f, "gx-polygons %lu %lu\n", sPolyFrames, sPolyMax);
    {
        uint32_t i;

        for (i = 0; i < pc_spu_unread_log_n && i < SND_WATCH_UNREAD_LOG; i++) {
            fprintf(f, "unread %08lX %lu %08lX %lu\n",
                    (unsigned long)pc_spu_unread_log[i][0],
                    (unsigned long)pc_spu_unread_log[i][1],
                    (unsigned long)pc_spu_unread_log[i][2],
                    (unsigned long)pc_spu_unread_log[i][3]);
        }
    }
    fprintf(f, "window-region %08lX %08lX\n",
            (unsigned long)(uint32_t)GUEST_BASE_WINDOW,
            (unsigned long)(uint32_t)GUEST_SIZE_WINDOW);
    /*
     * What the window is actually holding, which is the other half of the
     * claim and the only half a boot that never reaches a note can make:
     * SoundSystem_Get() takes its ~750 KB block from here, so a used count
     * that never grows says the sound system did not come up at all, and a
     * NONE verdict means something different in each case.
     */
    fprintf(f, "window-used %lu %d\n",
            (unsigned long)pc_guest_window_used(), pc_guest_window_blocks());
    fprintf(f, "first-window %08lX %lu\n",
            (unsigned long)sFirstWindow, (unsigned long)sFirstLen);
    fprintf(f, "first-window-host %p\n",
            sFirstWindow != 0 ? armrec_host_ptr(sFirstWindow) : NULL);
    fprintf(f, "first-bad %08lX\n", (unsigned long)sFirstBad);
    fprintf(f, "latched %lu\n", (unsigned long)pc_spu_latched);
    fprintf(f, "keyons-seen %lu %lu\n",
            (unsigned long)pc_spu_keyons[0], (unsigned long)pc_spu_keyons[1]);
    fprintf(f, "samples %lu\n", (unsigned long)pc_spu_samples());
    fprintf(f, "driver %lu %lu\n", pc_arm7snd_steps, pc_arm7snd_commands);
    {
        unsigned i;
        unsigned n = pc_arm7snd_commands < 8u ? (unsigned)pc_arm7snd_commands : 8u;

        fprintf(f, "commands");
        for (i = 0; i < n; i++) {
            fprintf(f, " %08lX", (unsigned long)pc_arm7snd_cmdlog[i]);
        }
        fprintf(f, "\n");

        for (i = 0; i < pc_arm7snd_arglog_n && i < 6u; i++) {
            const uint32_t *row = pc_arm7snd_arglog[i];

            fprintf(f, "cmd %lu %08lX %08lX %08lX %08lX\n",
                    (unsigned long)row[0], (unsigned long)row[1],
                    (unsigned long)row[2], (unsigned long)row[3],
                    (unsigned long)row[4]);
            /*
             * The two arguments that are pointers, the sequence and the
             * bank, with the first bytes they point at. A sequencer handed
             * a valid pointer to an empty buffer produces exactly what a
             * broken one does, which is nothing, and only the bytes tell them
             * apart.
             *
             * Bounded to the port window's own host span, and nothing wider:
             * These are words out of a list the game built, so following one
             * that is not a pointer would be a fault, and the application
             * heap is not a safe bound because most of it is not mapped. Both
             * of these live in the sound heap, which is the window.
             */
            if (row[0] == 0 || row[0] == 2) {
                const unsigned char *lo =
                    (const unsigned char *)armrec_host_ptr(
                        (uint32_t)GUEST_BASE_WINDOW);
                int k;

                for (k = 1; lo != NULL && k <= 3; k += 2) {
                    const unsigned char *p =
                        (const unsigned char *)(uintptr_t)row[k + 1];
                    int b;

                    if (p < lo || p >= lo + (uint32_t)GUEST_SIZE_WINDOW) {
                        continue;
                    }
                    fprintf(f, "cmdmem %08lX", (unsigned long)row[k + 1]);
                    for (b = 0; b < 8; b++) {
                        fprintf(f, " %02X", p[b]);
                    }
                    fprintf(f, "\n");
                }
            }
        }
    }

    /*
     * And the driver's own storage for any channel it has touched. This is
     * upstream of everything above, what the sound driver wrote, before the
     * latch reads it, so a report with waves in these words and nothing in
     * the tallies points at the latch, and one with the words still zero
     * points at the driver.
     */
    {
        const uint32_t *io = (const uint32_t *)armrec_host_ptr(SND_IO_BASE);
        unsigned i;

        for (i = 0; io != NULL && i < 16u; i++) {
            const uint32_t *c = io + i * 4u;

            if ((c[0] | c[1] | c[2] | c[3]) == 0) {
                continue;
            }
            fprintf(f, "chan %u %08lX %08lX %08lX %08lX\n", i,
                    (unsigned long)c[0], (unsigned long)c[1],
                    (unsigned long)c[2], (unsigned long)c[3]);
        }
    }

    /*
     * And the conversion at the other end of the same path: what the driver
     * handed snd_sad_from_host() that it could not turn into a source address
     * at all. A channel set up with one of these is pointed at 0 and is
     * silent, so a non-zero `host` here names an object that still has to
     * move into the window; the class the guest window was built to make findable.
     */
    fprintf(f, "sndaddr %lu %lu %lu %lu\n",
            (unsigned long)snd_addr_bad[SND_ADDR_HOST],
            (unsigned long)snd_addr_bad[SND_ADDR_WIDE],
            (unsigned long)snd_addr_bad[SND_ADDR_LOST],
            (unsigned long)snd_addr_bad[SND_ADDR_STRAY]);
    fprintf(f, "sndaddr-first %08lX %08lX\n",
            (unsigned long)snd_addr_first[SND_ADDR_HOST],
            (unsigned long)snd_addr_first[SND_ADDR_WIDE]);

    if (arm7_SNDi_SharedWork != NULL) {
        const struct snd_shared_head *w = arm7_SNDi_SharedWork;

        fprintf(f, "shared %lu %08lX %04X %lu\n",
                (unsigned long)w->finished_command_tag,
                (unsigned long)w->player_status,
                (unsigned)w->channel_status,
                (unsigned long)w->players[0].tick_counter);
    } else {
        fprintf(f, "shared none\n");
    }

    fprintf(f, "verdict %s\n", names[verdict]);

    if (fclose(f) != 0) {
        return -1;
    }
    sWrittenAt = sFrames;
    sDirty = 0;
    return 0;
}

void snd_watch_step(void)
{
    sFrames++;
    take();

    /*
     * When the first unreadable lane arrived, which is the number that says how
     * long a check has to run to see one. The tallies alone cannot: the report
     * is rewritten in place, so a boot that goes bad at frame 900 and one that
     * goes bad at frame 90 leave the same file.
     */
    if (sUnreadAt == 0
        && (pc_spu_unreadable[0] != 0 || pc_spu_unreadable[1] != 0)) {
        sUnreadAt = sFrames;
        sDirty = 1;
    }

    /*
     * The geometry engine, sampled here because this is the one thing that
     * runs every frame. Both of its counts are per-frame; the live polygon
     * count is reset by the next SWAP_BUFFERS and the drawn count by the next
     * render, so reading them when the report happens to be written would
     * answer about one arbitrary frame. A frame count and a maximum answer
     * about the boot.
     */
    {
        unsigned long drawn = pc_gpu3d_soft_polygons_drawn();
        unsigned long live = pc_gpu3d_num_polygons();
        unsigned long n = drawn > live ? drawn : live;

        if (n != 0) {
            sPolyFrames++;
            if (n > sPolyMax) {
                sPolyMax = n;
            }
            sDirty = 1;
        }
    }

    /*
     * The first frame writes unconditionally, so the file exists, and says
     * NONE, even on a run that never reaches a note. After that only a
     * changed tally is worth the SD card, and not more than once a second.
     */
    if (sFrames == 1
        || (sDirty && sFrames - sWrittenAt >= SND_WATCH_EVERY)
        || sFrames - sWrittenAt >= SND_WATCH_HEARTBEAT) {
        (void)snd_watch_write(sPath);
    }
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#ifdef SND_WATCH_SELFTEST_VERBOSE
#define FAILNOTE() fprintf(stderr, "  3ds_snd_watch.c:%d failed\n", __LINE__)
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

/* SOUNDxSAD's own field, the mask pc_spu.c applies before anything reaches
 * the log. Spelled here too because the whole point of the last check below
 * is to put a host pointer through it. */
#define SAD_MASK 0x07FFFFFCu

int snd_watch_selftest(int *ranOut)
{
    int ran = 0;
    int failed = 0;
    uint32_t savedN = pc_spu_src_log_n;
    uint32_t savedUnread[2];
    uint32_t savedLog[2][2];
    uint32_t truncated;

    savedUnread[0] = pc_spu_unreadable[0];
    savedUnread[1] = pc_spu_unreadable[1];
    memcpy(savedLog, pc_spu_src_log, sizeof savedLog);

    /*
     * The log is the game's on the console, so it is put back exactly as it
     * was: this runs before the game starts today, and a check that ate the
     * first four waves of a boot would be worse than no check.
     */
    pc_spu_src_log_n = 0;
    pc_spu_unreadable[0] = 0;
    pc_spu_unreadable[1] = 0;
    snd_watch_reset();

    /* Nothing keyed yet is not a pass. */
    CHECK(snd_watch_verdict() == SND_WATCH_NONE);
    CHECK(snd_watch_keyons() == 0);

    /* A wave in the window, which is where every wave in this port lives. */
    pc_spu_src_log[0][0] = (uint32_t)GUEST_BASE_WINDOW;
    pc_spu_src_log[0][1] = 0x1000u;
    pc_spu_src_log_n = 1;
    take();
    CHECK(snd_watch_window() == 1);
    CHECK(snd_watch_stray() == 0);
    CHECK(snd_watch_lost() == 0);
    CHECK(snd_watch_first_window() == (uint32_t)GUEST_BASE_WINDOW);
    CHECK(snd_watch_verdict() == SND_WATCH_PASS);

    /*
     * Main RAM: readable, and not the window. A console could legitimately
     * play from there and this port never does, so it is the fingerprint of
     * something having gone wrong rather than an unreadable address, and it
     * has to fail the check even though the SPU would happily read it. This
     * is the whole difference from the PC port's version of this test.
     */
    pc_spu_src_log[1][0] = (uint32_t)GUEST_BASE_MAIN + 0x1000u;
    pc_spu_src_log[1][1] = 0x400u;
    pc_spu_src_log_n = 2;
    take();
    CHECK(armrec_host_ptr(pc_spu_src_log[1][0]) != NULL);
    CHECK(snd_watch_stray() == 1);
    CHECK(snd_watch_first_bad() == pc_spu_src_log[1][0]);
    CHECK(snd_watch_verdict() == SND_WATCH_FAIL);

    /* An address with nothing behind it. 0x02800000 is the hole between main
     * RAM and the port window. */
    snd_watch_reset();
    pc_spu_src_log[0][0] = 0x02800000u;
    pc_spu_src_log[0][1] = 0x400u;
    pc_spu_src_log_n = 1;
    take();
    CHECK(armrec_host_ptr(0x02800000u) == NULL);
    CHECK(snd_watch_lost() == 1);
    CHECK(snd_watch_verdict() == SND_WATCH_FAIL);

    /*
     * And the failure this whole file exists for: a real host pointer that
     * reached SOUNDxSAD. The pointer is this function's own frame, so it is
     * host memory by construction; the mask is hardware's. Which of the two
     * bad classes it lands in is not fixed, which was measured on this
     * console a good deal of the process's memory masks onto readable guest
     * regions, so it can arrive as a plausible address rather than as
     * nothing. What is fixed is that it is never the window and never a pass.
     */
    snd_watch_reset();
    truncated = (uint32_t)(uintptr_t)&ran & SAD_MASK;
    pc_spu_src_log[0][0] = truncated;
    pc_spu_src_log[0][1] = 0x400u;
    pc_spu_src_log_n = 1;
    take();
    CHECK(snd_watch_window() == 0);
    CHECK(snd_watch_stray() + snd_watch_lost() == 1);
    CHECK(snd_watch_first_bad() == truncated);
    CHECK(snd_watch_verdict() == SND_WATCH_FAIL);

    /*
     * And what pc_spu.c caught on its own: a source address it could not read
     * is a failure here even when the log is clean, because the log only
     * holds the addresses a channel was actually started with.
     */
    snd_watch_reset();
    pc_spu_src_log[0][0] = (uint32_t)GUEST_BASE_WINDOW;
    pc_spu_src_log[0][1] = 0x1000u;
    pc_spu_src_log_n = 1;
    take();
    CHECK(snd_watch_verdict() == SND_WATCH_PASS);
    pc_spu_unreadable[0] = 1;
    CHECK(snd_watch_verdict() == SND_WATCH_FAIL);
    pc_spu_unreadable[0] = 0;
    pc_spu_unreadable[1] = 1;
    CHECK(snd_watch_verdict() == SND_WATCH_FAIL);

    snd_watch_reset();
    memcpy(pc_spu_src_log, savedLog, sizeof savedLog);
    pc_spu_src_log_n = savedN;
    pc_spu_unreadable[0] = savedUnread[0];
    pc_spu_unreadable[1] = savedUnread[1];

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
