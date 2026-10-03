/*
 * 3ds/src/3ds_audio.c: NDSP consumes pc_audio.
 *
 * The two consoles agree on the rate, exactly. pc/hw/pc_spu.c produces one
 * stereo sample every 1024 guest cycles of a 33,513,982 Hz clock, which is
 * 32,728.498046875 Hz. NDSP's native rate is SYSCLOCK_SOC / 512, and
 * 16,756,991 is 33,513,982 halved: the same number. So a sample the mixer
 * produced is a sample the DSP wants, and this file resamples nothing and
 * interpolates nothing. That is measured arithmetic, not a coincidence to lean
 * on quietly: if either clock is ever respelled, the rate below stops matching
 * and the pitch drifts slowly rather than failing.
 *
 * Why the hand-off is on the frame thread and not on NDSP's callback. The
 * obvious design is ndspSetCallback() pulling from the ring whenever the DSP
 * wants more. It was rejected: pc_audio.c's ring is single-producer,
 * single-consumer with a plain `ring_level` that both sides move, and the
 * producer is the guest frame. Reading it from NDSP's own thread is a data
 * race on that counter, and a silently-wrong-sample-count one at that. So the
 * sink runs where the samples are made, at the end of the guest frame, and all
 * this file does there is copy and queue. The DSP still plays asynchronously,
 * which is hardware DMA out of a buffer nobody is writing.
 *
 * Nothing waits for the DSP. If every wave buffer is still in flight the sink
 * leaves the samples in the ring and counts a stall; the next frame drains two
 * frames' worth. If the ring then overflows, that is counted too. The guest
 * never blocks on audio, because a run's timing must not depend on a consumer.
 *
 * And nothing pads with silence. An empty queue is silence already: NDSP plays
 * what it has and stops. Feeding it manufactured silence to keep the queue
 * non-empty would add latency that never comes back, which is the shape of the
 * audio bug that cost the other port a week.
 *
 * The buffer count is headroom, not latency. Only what the guest produced is
 * ever queued, about 547 sample frames per console frame, so the queue sits
 * one or two buffers deep whatever AUDIO_BUFFERS says.
 */

#include "3ds_audio.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

/*
 * The mixer, declared here rather than included: pc/include is not on this
 * chain's search path, and both names are weak because the self-test
 * binary links 3ds/src alone; there is no game in it to make sound. The
 * game link has them. 3ds_init.c declares pc_main.c's five host models the
 * same way for the same reason.
 */
extern unsigned pc_audio_read(short *dst, unsigned frames) __attribute__((weak));
extern void pc_audio_set_sink(void (*sink)(void)) __attribute__((weak));

/* 33,513,982 / 1024, and 16,756,991 / 512. See the header comment. */
#define AUDIO_RATE (SYSCLOCK_SOC / 512.0f)

/*
 * One buffer holds four frames' worth. A frame is 546.9 sample frames at this
 * rate, and the drain takes whatever the ring has, so the size is the backlog
 * a stalled frame can leave rather than a target depth.
 */
#define AUDIO_BUFFERS 4
#define AUDIO_CAP     2200u

#define AUDIO_CHANNEL 0

static ndspWaveBuf sWave[AUDIO_BUFFERS];
static short      *sData;            /* linear memory: the DSP reads it */
static int         sReady;
static int         sSink;

static unsigned long sSamples;
static unsigned long sBuffers;
static unsigned long sStalls;
static int           sProbeFrames;

static short *slot_data(int i)
{
    return sData + (unsigned)i * AUDIO_CAP * 2u;
}

/*
 * A buffer is ours again once the DSP is finished with it. FREE is one that
 * has never been queued; DONE is one that has played out. Anything else is
 * still the DSP's.
 */
static int slot_free(int i)
{
    return sWave[i].status == NDSP_WBUF_FREE || sWave[i].status == NDSP_WBUF_DONE;
}

static void queue(int i, unsigned frames)
{
    sWave[i].data_pcm16 = slot_data(i);
    sWave[i].nsamples = frames;
    sWave[i].looping = false;
    sWave[i].offset = 0;

    /* The DSP reads this memory itself, so what the ARM11 wrote has to be out
     * of the cache before it is queued and not merely written. */
    DSP_FlushDataCache(slot_data(i), frames * 2u * sizeof(short));
    ndspChnWaveBufAdd(AUDIO_CHANNEL, &sWave[i]);

    sSamples += frames;
    sBuffers++;
}

/*
 * What pc_audio_advance() calls at the end of a guest frame. Drains the ring
 * into every buffer the DSP has given back, so a frame that could not queue
 * catches up here rather than leaving the backlog to the ring's overrun
 * counter.
 */
static void audio_sink(void)
{
    int i;
    int queued = 0;

    if (!sReady || pc_audio_read == NULL) {
        return;
    }

    for (i = 0; i < AUDIO_BUFFERS; i++) {
        unsigned n;

        if (!slot_free(i)) {
            continue;
        }
        n = pc_audio_read(slot_data(i), AUDIO_CAP);
        if (n == 0) {
            return;             /* ring empty: nothing was left behind */
        }
        queue(i, n);
        queued++;
    }

    /*
     * Every buffer was in flight and the ring still had samples. They stay
     * there and go out next frame. Counted rather than dropped here, because
     * dropping is the ring's decision and it already has a counter for it.
     */
    if (queued == AUDIO_BUFFERS) {
        sStalls++;
    }
}

/*
 * How many console frames a buffer of a known length takes to play out.
 *
 * The one thing libctru's own documentation leaves ambiguous is what
 * `nsamples` counts for a stereo PCM16 buffer, sample frames or halfwords,
 * and the two differ by a factor of two, which is an octave. So this queues
 * silence of a known length and counts VBlanks until the DSP says DONE:
 * AUDIO_CAP frames at 32,728 Hz is 2200/546.9 = 4.02 console frames. A result
 * near 2 would mean the field counts halfwords and every sound this port makes
 * would play an octave high.
 *
 * It runs only in the binary with no game in it. A probe that made the first
 * four frames of a real boot silent to check the units would be paying at run
 * time for something a self-test binary can answer once.
 */
static void probe(void)
{
    int frames = 0;

    memset(slot_data(0), 0, AUDIO_CAP * 2u * sizeof(short));
    queue(0, AUDIO_CAP);

    while (frames < 60) {
        gspWaitForVBlank();
        frames++;
        if (sWave[0].status == NDSP_WBUF_DONE) {
            break;
        }
    }
    sProbeFrames = frames;
}

int audio_init(void)
{
    float mix[12];
    Result rc;

    if (sReady) {
        return 0;
    }

    rc = ndspInit();
    if (R_FAILED(rc)) {
        /*
         * libctru loads the sound firmware itself, from sdmc:/3ds/dspfirm.cdc:
         * The path is a string in its own ndsp.o, so this is what a
         * console whose owner has never dumped it says, and it is what this
         * emulator says. Not fatal: everything else works and the game runs
         * silent. The frame line says NODSP so it is not discovered by
         * listening.
         */
        fprintf(stderr, "3ds-audio: no DSP (%08lX). Dump dspfirm.cdc to "
                        "sdmc:/3ds/ for sound; the game runs silent without "
                        "it.\n", (unsigned long)rc);
        return -1;
    }

    sData = (short *)linearAlloc(AUDIO_BUFFERS * AUDIO_CAP * 2u * sizeof(short));
    if (sData == NULL) {
        ndspExit();
        fprintf(stderr, "3ds-audio: %u bytes of linear memory refused\n",
                (unsigned)(AUDIO_BUFFERS * AUDIO_CAP * 2u * sizeof(short)));
        return -1;
    }
    memset(sData, 0, AUDIO_BUFFERS * AUDIO_CAP * 2u * sizeof(short));
    memset(sWave, 0, sizeof sWave);

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(AUDIO_CHANNEL);
    ndspChnSetInterp(AUDIO_CHANNEL, NDSP_INTERP_NONE);
    ndspChnSetRate(AUDIO_CHANNEL, AUDIO_RATE);
    ndspChnSetFormat(AUDIO_CHANNEL, NDSP_FORMAT_STEREO_PCM16);

    /* Front left and front right at unity. The mixer has already applied the
     * DS's master volume, SOUNDCNT's output selector and SOUNDBIAS. */
    memset(mix, 0, sizeof mix);
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(AUDIO_CHANNEL, mix);

    sReady = 1;

    if (pc_audio_set_sink != NULL) {
        pc_audio_set_sink(audio_sink);
        sSink = 1;
    } else {
        probe();
    }
    return 0;
}

void audio_exit(void)
{
    if (!sReady) {
        return;
    }
    if (sSink && pc_audio_set_sink != NULL) {
        pc_audio_set_sink(NULL);
        sSink = 0;
    }
    ndspChnWaveBufClear(AUDIO_CHANNEL);
    ndspExit();
    if (sData != NULL) {
        linearFree(sData);
        sData = NULL;
    }
    sReady = 0;
}

/*
 * 9.6. The queue is cleared as well as paused: a paused channel keeps its
 * buffers, so coming back from a five-minute Home visit would resume the
 * music from where it was interrupted and then jump to the present, two
 * discontinuities where the player expects one. Clearing sets every buffer to
 * DONE, which is what audio_sink() reads as its own again, so the next guest
 * frame refills them with no special case.
 */
void audio_suspend(void)
{
    if (!sReady) {
        return;
    }
    ndspChnSetPaused(AUDIO_CHANNEL, true);
    ndspChnWaveBufClear(AUDIO_CHANNEL);
}

void audio_resume(void)
{
    if (!sReady) {
        return;
    }
    ndspChnSetPaused(AUDIO_CHANNEL, false);
}

int audio_ready(void) { return sReady; }
unsigned long audio_samples(void) { return sSamples; }
unsigned long audio_buffers(void) { return sBuffers; }
unsigned long audio_stalls(void) { return sStalls; }
int audio_probe_frames(void) { return sProbeFrames; }

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#ifdef AUDIO_SELFTEST_VERBOSE
#define FAILNOTE() fprintf(stderr, "  3ds_audio.c:%d failed\n", __LINE__)
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

/*
 * AUDIO_CAP sample frames at 32,728.5 Hz is 67.2 ms, and a console frame is
 * 16.7 ms: 4.02 of them. The window is 3 to 6, the queue happens partway
 * through a frame and the DONE is noticed at the next VBlank, and it
 * excludes 2, which is what a halfword-counting nsamples would produce.
 */
#define PROBE_MIN 3
#define PROBE_MAX 6

/*
 * A console with no DSP firmware is not a failing port. Libctru reads the
 * sound firmware off the SD card, `/3ds/dspfirm.cdc`, which is a string in
 * its own ndsp.o and has to be dumped from a console you own, so ndspInit
 * fails on a console that has never had it dumped, and on an emulator with
 * no such file. What this port owes in that case is to degrade cleanly and
 * say so, and that is what is checked: no sink, no buffers, and the frame
 * line carries NODSP. The checks that judge the DSP itself only run where
 * there is one.
 */
int audio_selftest(int *ranOut)
{
    int ran = 0;
    int failed = 0;

    if (!sReady) {
        CHECK(!sSink);
        CHECK(sBuffers == 0);
        CHECK(sSamples == 0);
    } else if (!sSink) {
        CHECK(sProbeFrames >= PROBE_MIN && sProbeFrames <= PROBE_MAX);
        CHECK(sSamples == AUDIO_CAP);
        CHECK(sBuffers == 1);
    } else {
        /* The game link. There is no probe to judge; what this binary can
         * say at start-up is that the mixer will be asked for its samples. */
        CHECK(pc_audio_read != NULL);
    }

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
