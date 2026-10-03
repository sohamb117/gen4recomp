/*
 * The port's half of the differential runner: the same trace the oracle writes.
 *
 * What this compares, and why it is hardware state rather than the game's.
 * This port is a native build of the decompilation, so every one of the game's
 * globals is a HOST object the host linker placed. They are not at guest
 * addresses and there is nothing on the console's side to line them up with.
 * What IS in guest memory is what the hardware sees, VRAM, the palettes,
 * OAM, the I/O window, plus whatever the game allocates out of its arenas in
 * main RAM. The first four are pure data and are the comparison worth having;
 * the arenas hold host pointers wherever the game stored the address of a
 * static, so they diverge for a reason that is not a bug. Rather than decide
 * that in advance, this writes a digest for every region the memory map has
 * and lets the runner report which ones agree.
 *
 * The span list is this program's, and the oracle is told it. `--diff-spans`
 * writes the regions out and exits; the runner feeds that file to the oracle.
 * One source of truth, so a region that moves cannot leave the two sides
 * comparing different addresses and calling the result agreement.
 *
 * Looking must not change the run, which is the same property `--watch` is
 * held to. Everything here reads: the digests go through pc_state_digest_span,
 * the SPU through pc_spu_regs, the picture through the surfaces the renderer
 * already wrote. A run with --diff-trace and a run without produce the same
 * --state-digest, and the test requires it.
 */

#include "pc_diff.h"

#include "armrec_rt.h"
#include "pc_spu.h"
#include "pc_state.h"
#include "pc_video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *sTrace;
static unsigned sEvery = 1;

/* ------------------------------------------------------------------ */

/*
 * The picture, as a digest per screen: FNV-1a 64 over the r, g and B bytes of
 * each pixel in raster order. Bytes rather than words so it says nothing about
 * either host's byte order; three rather than four because the fourth byte is
 * not a colour, melonDS keeps its own value there and this port keeps zero,
 * and a digest over it would be comparing two conventions.
 *
 * Upper LCD first. Which engine that is belongs to POWCNT1's DSEL bit and
 * pc_video_upper_engine() reads it, so this follows the game exactly as the
 * oracle's GetFramebuffers() does, melonDS applies the same bit before
 * handing its buffers back.
 */
static uint64_t frame_digest(int engine)
{
    const uint32_t *px = pc_video_surface(engine);
    uint64_t h = PC_STATE_FNV64_OFFSET;
    int i;

    if (px == NULL) return 0;
    for (i = 0; i < PC_VIDEO_PIXELS; i++) {
        unsigned char rgb[3];

        rgb[0] = (unsigned char)(px[i] >> 16);
        rgb[1] = (unsigned char)(px[i] >> 8);
        rgb[2] = (unsigned char)(px[i]);
        h = pc_state_fnv1a(h, rgb, 3);
    }
    return h;
}

static void spu_report(void)
{
    struct pc_spu_regs r;
    int i;

    pc_spu_regs(&r);
    for (i = 0; i < PC_SPU_CHANNELS; i++)
        fprintf(sTrace, "spu ch %d %08X %08X %04X %08X %08X\n", i,
                r.chan[i].cnt, r.chan[i].srcaddr, r.chan[i].timer,
                r.chan[i].looppos, r.chan[i].length);
    for (i = 0; i < 2; i++)
        fprintf(sTrace, "spu cap %d %02X %08X %04X %08X\n", i,
                r.cap[i].cnt, r.cap[i].dstaddr, r.cap[i].timer,
                r.cap[i].length);
    fprintf(sTrace, "spu glob %04X %02X %04X\n",
            r.soundcnt, r.master_volume, r.bias);
}

static void checkpoint(const char *label)
{
    int upper = pc_video_upper_engine();
    int i, n;

    fprintf(sTrace, "cp %s\n", label);
    fprintf(sTrace, "fb %016llX %016llX\n",
            (unsigned long long)frame_digest(upper),
            (unsigned long long)frame_digest(upper == PC_VIDEO_MAIN
                                             ? PC_VIDEO_SUB : PC_VIDEO_MAIN));
    spu_report();

    n = armrec_region_count();
    for (i = 0; i < n; i++) {
        uint32_t base, size;

        if (!armrec_region_at(i, &base, &size, NULL)) continue;
        fprintf(sTrace, "d %08X %u %016llX\n", base, size,
                (unsigned long long)pc_state_digest_span(base, size));
    }
    fflush(sTrace);
}

/* ------------------------------------------------------------------ */

/*
 * A region's name as ONE token, because every reader of these files takes the
 * name with %s. armrec's names have spaces in them ("main RAM", "VRAM main
 * BG"), so an unmangled name truncates at the first one, and since the span
 * list and the trace header are written by different functions, the two would
 * truncate to the same wrong thing in one place and not the other. One
 * function, used by both.
 */
static int region_token(int i, uint32_t *base, uint32_t *size,
                        char *out, size_t cap)
{
    const char *name = NULL;
    size_t k;

    if (!armrec_region_at(i, base, size, &name)) return 0;
    snprintf(out, cap, "%s", name ? name : "region");
    for (k = 0; out[k]; k++)
        if (out[k] == ' ' || out[k] == '\t') out[k] = '-';
    return 1;
}

/* ------------------------------------------------------------------ */

/*
 * --diff-spans: the memory map, for the oracle to read the same addresses.
 *
 * Written and then the process exits, because this is a question about the
 * build rather than about a run; the regions are the map armrec_mem_init()
 * laid down and no frame changes them. Answering it without booting the game
 * keeps the runner's first step to a few milliseconds.
 */
int pc_diff_write_spans(const char *path)
{
    FILE *f = fopen(path, "w");
    int i, n;

    if (f == NULL) {
        fprintf(stderr, "pc-diff: cannot write %s\n", path);
        return 0;
    }
    fprintf(f, "# addr len name, written by the port, read by the oracle\n");
    n = armrec_region_count();
    for (i = 0; i < n; i++) {
        uint32_t base, size;
        char tok[64];

        if (!region_token(i, &base, &size, tok, sizeof tok)) continue;
        fprintf(f, "%08X %u %s\n", base, size, tok);
    }
    fclose(f);
    return 1;
}

void pc_diff_open(const char *path, unsigned every)
{
    if (path == NULL || *path == '\0') return;
    sTrace = fopen(path, "w");
    if (sTrace == NULL) {
        fprintf(stderr, "pc-diff: cannot write %s\n", path);
        return;
    }
    sEvery = every ? every : 1;
    fprintf(sTrace, "pcdiff 1\n");
    fprintf(sTrace, "side port\n");
    fprintf(sTrace, "every %u\n", sEvery);

    {
        int i, n = armrec_region_count();

        for (i = 0; i < n; i++) {
            uint32_t base, size;
            char tok[64];

            if (!region_token(i, &base, &size, tok, sizeof tok)) continue;
            fprintf(sTrace, "span %08X %u %s\n", base, size, tok);
        }
    }

    /*
     * The port's `boot` is not the console's. This one is taken at the game's
     * own entry point, with the cartridge boot already done by the host; the
     * oracle's is taken before the ARM9's first instruction. The runner knows
     * and prints the offset rather than pretending the two line up.
     */
    checkpoint("boot");
}

void pc_diff_frame(unsigned long long frame)
{
    char label[64];

    if (sTrace == NULL) return;
    if (frame % sEvery != 0) return;
    snprintf(label, sizeof label, "frame:%llu", frame);
    checkpoint(label);
}

/*
 * However the run ends, the frame limit, a return from the game's main loop,
 * or a fatal signal. Registered through the same hook the state digest uses,
 * because there can only be one set of signal handlers, and it takes a last
 * checkpoint before closing: a trace that stopped because the port crashed is
 * still evidence, and the frame it stopped at is the answer.
 */
void pc_diff_ending(FILE *out, const char *label)
{
    (void)out;
    if (sTrace == NULL) return;
    checkpoint(label != NULL ? label : "end");
    fprintf(sTrace, "end\n");
    fclose(sTrace);
    sTrace = NULL;
}

void pc_diff_close(void)
{
    pc_diff_ending(NULL, "end");
}

int pc_diff_active(void) { return sTrace != NULL; }
