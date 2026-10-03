/*
 * The port's in-binary vector suites, run together and reported per suite.
 *
 * Why an umbrella and not three env vars. Each of these is a set of
 * known-answer vectors checked against code compiled into this binary, and each
 * was written for a different reason at a different time. What they have in
 * common is the shape of the question, "does this routine still produce the
 * answer the standard says", so one input runs them all and prints a line
 * each, and the test harness needs one invocation instead of three.
 *
 * PC_MI_SELFTEST survives as a narrower instrument and is not a second way to
 * do this. It runs the decompressor vectors *inside the boot*, at the moment
 * MI's first call initializes it, which is what makes it usable while debugging
 * a decompression that only misbehaves after a particular boot state. This
 * umbrella runs the same vectors from a defined point instead, before the game
 * starts. Same vectors, two vantage points, and the comment in pc_mi.c says so.
 *
 * Why it exits rather than continuing. A run that has just proved its own
 * crypto has nothing further to say about the game, and a suite that had to
 * drive a whole boot to reach its verdict would cost the harness a boot per
 * suite. Exit status is the verdict: 0 all passed, 1 something did not.
 *
 * What is not here. The SPU and 2D/3D engines have known-answer tests too, but
 * theirs are driven from the other side; an emulator's own implementation fed
 * the same inputs, and that harness does not exist in this tree yet. They are
 * named in the plan for the phase that builds it; putting an empty placeholder
 * here would suggest coverage that does not exist.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_selftest.h"

/* Each suite lives with the code it checks, so a change to that code and a
 * change to its vectors land in the same file. */
extern int pc_dgt_selftest(void);   /* pc/src/pc_dgt.c : MD5, SHA-1, HMAC  */
extern int pc_rc4_selftest(void);   /* pc/src/pc_crypto_rc4.c: RC4, both variants */
extern void pc_mi_selftest(void);   /* pc/src/pc_mi.c  : LZ8/LZ16/copy  */
/* pc/hw/pc_gpu3d_soft.c: the 3D layer's horizontal scroll on the wide row,
 * which no replayable scene reaches: measured, every frame in both replays
 * that sets BG0HOFS has BG0 as an ordinary background rather than the 3D
 * layer, so the arithmetic is checked here or nowhere. */
extern int pc_gpu3d_soft_scroll_selftest(void);
extern int pc_gpu2d_hd3d_selftest(void);  /* pc/hw/pc_gpu2d.c: the HD 3D
                                           * layer's coverage and compose */
/* pc/src/pc_div0.c: integer division by zero, against the answer the
 * cartridge's own runtime gives. */
extern int pc_div0_selftest(void);
/* pc/hw/pc_gpu3d_soft.c: the producer diff's claim classes and thresholds
 * pinned as vectors before any GL renderer exists so a
 * renderer cannot move them by being built. */
extern int pc_gpu3d_diff_selftest(void);
/* pc/hw/pc_gpu3d_soft.c: the gl producer's texture converter against the
 * rasterizer's own texture unit, which is why it lives with the oracle and
 * not the converter. Last in the table: it loads VRAM banks the way the
 * SDK does and puts VRAMCNT back, and nothing should run on its heels. */
extern int pc_gpu3d_gltex_selftest(void);

/*
 * MI's suite ABORTS on a mismatch instead of returning a verdict, and that is
 * not a wart to route around: an abort is louder than a FAIL line and it stops
 * the run at the vector that failed, with the message naming it. So the only
 * verdict this wrapper can report is "reached the end", and a real MI failure
 * never gets here; the process is already gone, saying more than this line
 * would have.
 */
static int mi_suite(void)
{
    pc_mi_selftest();
    return 1;
}

/*
 * Dgt's suite returns a status code, not a boolean, 0 for success and 1, 2 or
 * 3 to name which vector failed. The first version of this file read it as a
 * boolean and reported the passing suite as FAIL, which is worth recording
 * because the failure looked exactly like a real defect: a suite that had been
 * written and never run, failing the moment it was wired up. It cost a diagnosis
 * to find out the bug was in the adapter.
 *
 * So the translation is explicit and the code is turned back into the name of
 * the vector it belongs to, which is more than a bare FAIL would have said.
 */
static int dgt_suite(void)
{
    static const char *which[] = { "", "MD5(\"abc\")", "SHA-1(\"abc\")",
                                   "HMAC-SHA-1 RFC 2202 case 2" };
    int rc = pc_dgt_selftest();

    if (rc == 0) return 1;
    fprintf(stderr, "pc-selftest dgt: %s mismatched\n",
            (rc >= 1 && rc <= 3) ? which[rc] : "an unnamed vector");
    return 0;
}

struct suite {
    const char *name;
    int (*run)(void);
    const char *what;
};

static const struct suite SUITES[] = {
    { "rc4", pc_rc4_selftest,
      "RC4 and RC4Fast against the classic vector, including a split call" },
    { "dgt", dgt_suite,
      "MD5(\"abc\"), SHA-1(\"abc\") and HMAC-SHA-1 RFC 2202 case 2" },
    { "mi",  mi_suite,
      "LZ8, LZ16 and the copy path against hand-assembled streams (aborts on"
      " mismatch, so PASS here means it reached the end)" },
    { "div0", pc_div0_selftest,
      "a / 0 == a and a % 0 == 0, signed and unsigned, register and memory"
      " divisor, read off _u32_div_f and _s32_div_f in the ROM" },
    { "3d",  pc_gpu3d_soft_scroll_selftest,
      "the 3D layer's scroll over the whole nine-bit range: wherever the"
      " native row shows a pixel, the wide row's column shows the same one" },
    { "hd3d", pc_gpu2d_hd3d_selftest,
      "which native pixels the 3D layer shows at under --hd3d, over all five"
      " ways a phantom survives or is covered, and the two sub-pixel"
      " composes, the covered one reproducing the native pixel exactly" },
    { "p3d", pc_gpu3d_diff_selftest,
      "the producer diff's claim classes off the rasterizer's own attribute"
      " bits and drawn extents, and its thresholds: exact byte-equal,"
      " blended one six-bit step, rasterized reported" },
    { "gltex", pc_gpu3d_gltex_selftest,
      "the gl producer's texture converter against the rasterizer's own"
      " texture unit: 114,752 texels, all eight formats, both texture slots,"
      " two palette sources, colour-zero both ways, banks loaded through"
      " LCDC the way the SDK loads them" },
};

int pc_selftest_run(FILE *out)
{
    int i, failed = 0;
    const int n = (int)(sizeof SUITES / sizeof SUITES[0]);

    if (out == NULL) out = stderr;
    for (i = 0; i < n; i++) {
        int ok = SUITES[i].run();

        fprintf(out, "pc-selftest %-4s %-4s %s\n", SUITES[i].name,
                ok ? "PASS" : "FAIL", SUITES[i].what);
        if (!ok) failed++;
    }
    fprintf(out, "pc-selftest %-4s %-4s %d of %d suite(s) passed\n",
            "all", failed ? "FAIL" : "PASS", n - failed, n);
    fflush(out);
    return failed == 0;
}
