/*
 * The port's audio output surface, pokediamond (carried for 4.1).
 *
 * This file is the port's own code, not melonDS's: pc/hw/pc_spu.c is the device
 * and this is where its samples go. PC_DUMP_AUDIO=path writes the WAV. The split is the one the renderer insisted on
 * for the renderer and the argument is the same one; "the model must produce
 * an in-memory buffer, with the file dump and any host audio device as two
 * independent consumers of it", because retrofitting headless behind an
 * already-written SDL path is a refactor of the whole output chain.
 *
 * What drives it (pokeplatinum): OS_Halt's frame boundary calls
 * pc_audio_advance() with one frame of guest cycles right after the sound
 * driver's pump (pc_arm7snd.c), and that is the only clock involved: the
 * port has no host clock call anywhere, so the sample rate here is exactly
 * guest cycles / 1024 and a run's audio is as reproducible as its guest
 * memory. A slow host produces the same samples, later. The file itself is
 * pokediamond's pc/src/pc_audio.c, carried whole.
 *
 * Why the ring never blocks. Same contract as pc/src/pc_view.c: the port must
 * not wait for a consumer, or a listener would change the timing of a run and
 * --state-digest would stop being a function of the command line. A reader that
 * falls behind loses the oldest frames and pc_audio_overruns() counts them.
 */

#include "pc_bench.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pc_spu.h"

/* One second at the SPU's own rate, which is the largest amount a run can
 * plausibly get ahead of a consumer between two frames. */
#define RING_FRAMES 32768u

static int16_t  ring[RING_FRAMES * 2];
static unsigned ring_write;
static unsigned ring_read;
static unsigned ring_level;

static uint64_t audio_frames;
static uint64_t audio_overruns;

/* Leftover guest cycles that were not a whole sample last time. */
static uint64_t cycle_debt;

static FILE    *wav;
static uint32_t wav_frames;

/* ------------------------------------------------------------------ */
/* The ring                                                            */
/* ------------------------------------------------------------------ */

static void ring_push(int16_t l, int16_t r)
{
    ring[ring_write * 2 + 0] = l;
    ring[ring_write * 2 + 1] = r;
    ring_write = (ring_write + 1) % RING_FRAMES;

    if (ring_level == RING_FRAMES) {
        ring_read = (ring_read + 1) % RING_FRAMES;
        audio_overruns++;
    } else {
        ring_level++;
    }
}

unsigned pc_audio_read(int16_t *dst, unsigned frames)
{
    unsigned n = 0;

    while (n < frames && ring_level > 0) {
        dst[n * 2 + 0] = ring[ring_read * 2 + 0];
        dst[n * 2 + 1] = ring[ring_read * 2 + 1];
        ring_read = (ring_read + 1) % RING_FRAMES;
        ring_level--;
        n++;
    }
    return n;
}

uint64_t pc_audio_frames(void)   { return audio_frames; }
uint64_t pc_audio_overruns(void) { return audio_overruns; }

/* ------------------------------------------------------------------ */
/* The WAV file                                                        */
/* ------------------------------------------------------------------ */

/*
 * RIFF/WAVE, 16-bit stereo little-endian. The two length fields are written
 * again at close, so a run killed by a signal still leaves a header claiming
 * zero frames rather than a plausible wrong number, and every media player
 * and Python's own `wave` module will read what is there anyway.
 *
 * The rate is the guest's: 33,513,982 / 1024 = 32,728 Hz, not the 32,768 Hz the
 * DS is usually said to run at. That figure is what the port's own VBlank
 * spacing gives (pc/src/pc_irq.c), and writing 32,768 here would make a dump
 * play 0.1% fast for the sake of a rounder number.
 */
#define WAV_RATE 32728u

static void wr32(FILE *f, uint32_t v)
{
    unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                           (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    (void)fwrite(b, 1, 4, f);
}

static void wr16(FILE *f, uint32_t v)
{
    unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
    (void)fwrite(b, 1, 2, f);
}

int pc_audio_open_dump(const char *path)
{
    extern int atexit(void (*)(void));
    static int armed;

    if (wav != NULL) {
        return -1;
    }
    /* The run this port has ends in exit() from the frame limit;
     * pokediamond closed the WAV from its CLI teardown, this port from
     * atexit, so the header's length fields are real either way. */
    if (!armed) {
        armed = 1;
        (void)atexit(pc_audio_close);
    }
    wav = fopen(path, "wb");
    if (wav == NULL) {
        fprintf(stderr, "pc-audio: cannot write %s\n", path);
        return -1;
    }
    wav_frames = 0;

    (void)fwrite("RIFF", 1, 4, wav);
    wr32(wav, 36);                      /* fixed up at close */
    (void)fwrite("WAVEfmt ", 1, 8, wav);
    wr32(wav, 16);                      /* PCM fmt chunk size */
    wr16(wav, 1);                       /* PCM */
    wr16(wav, 2);                       /* stereo */
    wr32(wav, WAV_RATE);
    wr32(wav, WAV_RATE * 4);            /* byte rate */
    wr16(wav, 4);                       /* block align */
    wr16(wav, 16);                      /* bits */
    (void)fwrite("data", 1, 4, wav);
    wr32(wav, 0);                       /* fixed up at close */
    /*
     * Flushed here and again at every frame boundary, which is pc_video.c's
     * manifest contract for the same reason: the run this port has today ends in
     * a fatal signal, atexit() does not run, and a header left in a FILE buffer
     * is a zero-byte file rather than a file saying "no samples". A test that
     * asked whether --dump-audio wrote anything got 0 bytes before this line
     * existed.
     */
    (void)fflush(wav);
    return ferror(wav) ? -1 : 0;
}

void pc_audio_close(void)
{
    if (wav == NULL) {
        return;
    }
    if (fseek(wav, 4, SEEK_SET) == 0) {
        wr32(wav, 36 + wav_frames * 4);
        if (fseek(wav, 40, SEEK_SET) == 0) {
            wr32(wav, wav_frames * 4);
        }
    }
    (void)fclose(wav);
    wav = NULL;
}

/* ------------------------------------------------------------------ */
/* The frame boundary                                                  */
/* ------------------------------------------------------------------ */

/*
 * Where samples go the moment they exist. pc/src/pc_view.c registers its
 * shared-ring publisher here; without a viewer the sink stays NULL and the
 * internal ring is drained at the frame boundary as before. A function
 * pointer rather than a weak symbol, because the Windows link resolves weak
 * externals differently enough to have cost a night already.
 */
static void (*audio_sink)(void);

void pc_audio_set_sink(void (*sink)(void))
{
    audio_sink = sink;
}

void pc_audio_advance(uint64_t cycles)
{
    uint64_t steps;
    uint32_t was = wav_frames;
    uint64_t produced;

    /*
     * The registers first, then the samples they describe. A guest that set a
     * channel up during the frame just ended should be heard in this frame's
     * samples rather than the next one's, and the latch is what carries the
     * writes across; see pc/hw/pc_spu.c for why this is a latch and not a
     * store hook.
     */
    (void)pc_spu_latch();

    cycle_debt += cycles;
    steps = cycle_debt / PC_SPU_MIX_CYCLES;
    cycle_debt -= steps * PC_SPU_MIX_CYCLES;
    produced = steps;

    while (steps-- > 0) {
        int16_t s[2];

        {
            PC_BENCH_BEGIN(bench_t);
            pc_spu_mix(s);
            PC_BENCH_END(PC_BENCH_AUDIO, bench_t);
        }
        ring_push(s[0], s[1]);
        audio_frames++;

        if (wav != NULL) {
            wr16(wav, (uint32_t)(uint16_t)s[0]);
            wr16(wav, (uint32_t)(uint16_t)s[1]);
            wav_frames++;
        }
    }

    if (produced > 0 && audio_sink != NULL) {
        audio_sink();
    }

    if (wav != NULL && wav_frames != was) {
        (void)fflush(wav);
    }
}
