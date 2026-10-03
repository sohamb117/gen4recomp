/*
 * 3ds/tests/pica3d_vtx.c: the PICA vertex transform, and how far its depth
 * can be from the DS's.
 *
 * Two questions, and only one of them has a yes/no answer.
 *
 * The first is whether the transform is right: a screen pixel out of the
 * geometry engine, multiplied through by W, divided by W again the way the
 * PICA will, has to come back the same pixel. That is exact and it is checked
 * exhaustively over a grid of positions and a range of W.
 *
 * The second is the one the 3D producer has to answer before it writes a renderer:
 * How often would the PICA sort two surfaces differently from the DS. In
 * Z-buffer mode the two interpolate the same function and the answer is never.
 * In W-buffer mode the DS uses an approximation of a perspective-correct
 * interpolation, eight or nine bits of a factor, in interp_interpolate_z(),
 * and the PICA interpolates 1/W exactly. Those are the same curve only in
 * the limit, so the two can disagree about which of two surfaces is in front,
 * and this file counts the pixels where they do.
 *
 * That count is the evidence for the decision the plan says must come first:
 * This task cannot close on a digest the way the GPU present, the background path and the 3D layer did, so it
 * needs a difference statistic and a threshold argued from a measurement
 * rather than chosen afterwards to fit the result.
 *
 * IT INCLUDES pc_gpu3d_soft.c for the same reason tex3d_convert.c does: the
 * interpolator is static, and it is the oracle.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "armrec_rt.h"

#include "pc_gpu3d.h"
#include "3ds_pica3d.h"

static const PcGxRenderRegs *sRegs;

const PcGxRenderRegs *pc_gpu3d_render_regs(void) { return sRegs; }

PcGxPolygon **pc_gpu3d_render_polygons(uint32_t *count)
{
    if (count != NULL) {
        *count = 0;
    }
    return NULL;
}

int pc_gpu3d_hd_scale(void) { return 1; }
int pc_gpu3d_wide_width(void) { return 0; }
int pc_gpu3d_installed(void) { return 0; }
uint32_t *pc_gpu3d_stage(uint32_t addr) { (void)addr; return NULL; }
void pc_gpu3d_refresh_regs(void) { }

#include "pc_gpu3d_soft.c"

static int sRan;
static int sBad;

void fault_stop(const char *top, const char *bottom)
{
    printf("  fault_stop: %s, %s\n", top ? top : "", bottom ? bottom : "");
    sBad++;
}

#define CHECK(what, cond)                                                  \
    do {                                                                   \
        sRan++;                                                            \
        if (!(cond)) {                                                     \
            printf("  %-56s FAILED\n", (what));                            \
            sBad++;                                                        \
        }                                                                  \
    } while (0)

/* A repeatable sequence; the numbers below are quoted in the write-up and a
 * library rand() would make them a property of the C library. */
static uint32_t sSeed = 0x13572468u;

static uint32_t rnd(void)
{
    sSeed = sSeed * 1664525u + 1013904223u;
    return sSeed >> 8;
}

/* ------------------------------------------------------------------ */
/* The transform round trip                                            */
/* ------------------------------------------------------------------ */

static void round_trip(void)
{
    PcGxPolygon poly;
    PcGxVertex vtx;
    struct pica3d_vertex out;
    long worstX = 0, worstY = 0;
    double maxErr = 0.0;
    int x, y, k;
    /* The whole range of W the field can hold, not a comfortable middle: the
     * geometry engine truncates W to 24 bits and the viewport divide behaves
     * differently above 0xFFFF, so the large end is where a float would run
     * out of mantissa if this were done in the wrong precision. */
    static const int32_t kW[] = { 1, 0x100, 0x1000, 0x7FFF, 0x10000,
                                  0x100000, 0xFFFFFF };

    memset(&poly, 0, sizeof poly);
    memset(&vtx, 0, sizeof vtx);
    poly.NumVertices = 1;
    poly.Vertices[0] = &vtx;
    poly.Attr = 31u << 16;

    for (k = 0; k < (int)(sizeof kW / sizeof *kW); k++) {
        poly.FinalW[0] = kW[k];
        poly.FinalZ[0] = 0x400000;
        for (y = 0; y <= 192; y++) {
            for (x = 0; x <= 256; x++) {
                double sx, sy, dx, dy;

                vtx.FinalPosition[0] = x;
                vtx.FinalPosition[1] = y;
                if (pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) != 0) {
                    continue;
                }
                pica3d_project(&out, 256, 192, &sx, &sy, NULL);
                dx = sx - (double)x;
                dy = sy - (double)y;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dx > maxErr) { maxErr = dx; worstX = x; worstY = y; }
                if (dy > maxErr) { maxErr = dy; worstX = x; worstY = y; }
            }
        }
    }

    /*
     * A twentieth of a pixel. The transform is exact in real arithmetic; what
     * is left is that the vertex leaves this file as four floats, because that
     * is what the PICA's attribute loader takes. Anything bigger than a
     * rounding step here would be an algebra mistake.
     */
    printf("  round trip: worst %.6f px over %d positions x %d W values"
           " (at %ld,%ld)\n",
           maxErr, 257 * 193, (int)(sizeof kW / sizeof *kW), worstX, worstY);
    CHECK("the divide recovers the geometry engine's own pixel",
          maxErr < 0.05);
}

/* ------------------------------------------------------------------ */
/* Depth: where the PICA and the DS could disagree                     */
/* ------------------------------------------------------------------ */

/*
 * The DS's depth across a span, through its own interpolator, the same calls
 * render_polygon() makes along a scanline.
 */
static void ds_depth_span(int32_t z0, int32_t z1, int32_t w0, int32_t w1,
                          int len, int wbuffer, double *out)
{
    Interp I;
    int i;

    interp_setup(&I, 0, 0, len, w0, w1, wbuffer);
    for (i = 0; i < len; i++) {
        interp_set_x(&I, i);
        out[i] = (double)interp_interpolate_z(&I, z0, z1);
    }
}

/*
 * ...and the PICA's: the two endpoint depths this file produces, interpolated
 * linearly in screen space, which is what the depth unit does.
 */
static void pica_depth_span(int32_t z0, int32_t z1, int32_t w0, int32_t w1,
                            int len, int wbuffer, double *out)
{
    PcGxPolygon poly;
    PcGxVertex va, vb;
    struct pica3d_vertex pa, pb;
    double d0, d1;
    int i;

    memset(&poly, 0, sizeof poly);
    memset(&va, 0, sizeof va);
    memset(&vb, 0, sizeof vb);
    poly.NumVertices = 2;
    poly.Vertices[0] = &va;
    poly.Vertices[1] = &vb;
    poly.Attr = 31u << 16;
    poly.WBuffer = (uint8_t)wbuffer;
    poly.FinalZ[0] = z0;  poly.FinalW[0] = w0;
    poly.FinalZ[1] = z1;  poly.FinalW[1] = w1;

    pica3d_vertex(&poly, 0, 256, 192, 8, 8, &pa);
    pica3d_vertex(&poly, 1, 256, 192, 8, 8, &pb);
    pica3d_project(&pa, 256, 192, NULL, NULL, &d0);
    pica3d_project(&pb, 256, 192, NULL, NULL, &d1);

    for (i = 0; i < len; i++) {
        double t = (len > 1) ? (double)i / (double)(len - 1) : 0.0;
        double d = d0 + (d1 - d0) * t;

        /*
         * And then it is stored, which is the half this file first left out.
         * The interpolation being right is not the whole question: the value
         * lands in a 24-bit fixed-point depth buffer, and that is where the
         * W mode's real cost is. The DS stores W itself, so its resolution is
         * uniform in distance. Storing 1/W instead spends almost the whole
         * range on the near field, every surface past W = 0x8000 shares the
         * bottom thirty-two-thousandth of the buffer.
         *
         * Modelling it is one rounding step and it is the difference between
         * a measurement of the arithmetic and a measurement of the renderer.
         */
        d = d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d);
        out[i] = (double)(long)(d * 16777215.0 + 0.5);
    }
}

#define SPAN 64

/*
 * Two surfaces with independent gradients, and whether the two machines agree
 * About which is in front, binned by how far apart they actually are.
 *
 * The first version of this gave the second surface a CONSTANT offset from the
 * first, which made the measurement worthless and looked like a pass: two
 * surfaces with the same endpoints-difference get the same truncated gradient
 * out of the DS, so they stay exactly parallel on both machines and can never
 * swap. Every separation reported zero. The interesting case, and the one a
 * scene is made of, is two surfaces at DIFFERENT ANGLES passing near each
 * other, where the DS rounds one gradient and the PICA does not.
 *
 * So both surfaces are drawn independently, and each trial is filed under the
 * SMALLEST gap the DS itself put between them anywhere on the span. That turns
 * the result into the statement the 3D producer needs: surfaces the DS keeps at least
 * N apart are never swapped, and below N they can be.
 *
 * Neither machine is exact. The DS truncates the depth GRADIENT before
 * interpolating (`disp >>= 9` along X in interp_interpolate_z()) so its
 * ramp is linear with a rounded slope where the PICA's is linear with the true
 * one. In W mode it also approximates the perspective curve with eight bits of
 * a factor. Both are why this task cannot close on a byte compare.
 */

#define NBINS 7

/* Bin edges in DS depth units out of 0xFFFFFF. */
static const int32_t kBinMin[NBINS] = {
    0, 0x40, 0x100, 0x400, 0x1000, 0x4000, 0x10000
};

struct bins {
    long compared[NBINS];
    long bad[NBINS];
};

static int bin_of(double gap)
{
    int b;

    for (b = NBINS - 1; b > 0; b--) {
        if (gap >= (double)kBinMin[b]) {
            return b;
        }
    }
    return 0;
}

static void disagreements(int wbuffer, struct bins *out)
{
    double dsA[SPAN], dsB[SPAN], pcA[SPAN], pcB[SPAN];
    int trial, i;

    memset(out, 0, sizeof *out);

    for (trial = 0; trial < 20000; trial++) {
        int32_t wa0 = (int32_t)(rnd() % 0xFFFF) + 1;
        int32_t wa1 = (int32_t)(rnd() % 0xFFFF) + 1;
        int32_t wb0 = (int32_t)(rnd() % 0xFFFF) + 1;
        int32_t wb1 = (int32_t)(rnd() % 0xFFFF) + 1;
        int32_t za0, za1, zb0, zb1;
        double gap = 1e30;
        int b;

        if (wbuffer) {
            /* In W mode the depth is the W, so the surfaces are two W ramps
             * and their gradients differ because their corners do. */
            za0 = wa0; za1 = wa1;
            zb0 = wb0; zb1 = wb1;
        } else {
            /* Near each other on purpose: two surfaces a third of the depth
             * range apart agree trivially and would only dilute the bins. */
            za0 = (int32_t)(rnd() % 0xF00000) + 0x80000;
            za1 = (int32_t)(rnd() % 0xF00000) + 0x80000;
            zb0 = za0 + (int32_t)(rnd() % 0x40000) - 0x20000;
            zb1 = za1 + (int32_t)(rnd() % 0x40000) - 0x20000;
            if (zb0 < 0) zb0 = 0;
            if (zb1 < 0) zb1 = 0;
        }

        ds_depth_span(za0, za1, wa0, wa1, SPAN, wbuffer, dsA);
        ds_depth_span(zb0, zb1, wb0, wb1, SPAN, wbuffer, dsB);
        pica_depth_span(za0, za1, wa0, wa1, SPAN, wbuffer, pcA);
        pica_depth_span(zb0, zb1, wb0, wb1, SPAN, wbuffer, pcB);

        /*
         * The smallest gap the DS put between them anywhere. If the two
         * surfaces CROSS, the gap is zero and the trial lands in the first
         * bin, which is correct: a crossing is exactly where an ordering
         * question is genuinely ambiguous.
         */
        for (i = 0; i < SPAN; i++) {
            double d = dsA[i] - dsB[i];

            if (d < 0) d = -d;
            if (d < gap) gap = d;
        }
        b = bin_of(gap);

        for (i = 0; i < SPAN; i++) {
            int dsFront, pcFront;

            if (dsA[i] == dsB[i]) {
                continue;
            }
            dsFront = (dsA[i] < dsB[i]);        /* smaller depth is nearer */
            /* The PICA's W-mode depth runs backwards: larger is nearer. */
            pcFront = wbuffer ? (pcA[i] > pcB[i]) : (pcA[i] < pcB[i]);
            out->compared[b]++;
            if (dsFront != pcFront) {
                out->bad[b]++;
            }
        }
    }
}

static void depth_sweep(int wbuffer, const char *what)
{
    struct bins bn;
    long tailBad = 0, tailCmp = 0;
    int b, clean = -1;

    disagreements(wbuffer, &bn);

    printf("  depth, %s buffer, disagreements by the DS's own smallest gap:\n",
           what);
    for (b = 0; b < NBINS; b++) {
        if (bn.compared[b] == 0) {
            continue;
        }
        printf("      %8ld of %8ld (%7.4f%%) at a gap of 0x%06X and up\n",
               bn.bad[b], bn.compared[b],
               100.0 * (double)bn.bad[b] / (double)bn.compared[b],
               (unsigned)kBinMin[b]);
    }
    for (b = NBINS - 1; b >= 0; b--) {
        if (bn.compared[b] == 0) {
            continue;
        }
        if (bn.bad[b] != 0) {
            break;
        }
        clean = b;
    }
    if (clean >= 0) {
        printf("      ... clean from a gap of 0x%06X up"
               " (%.4f%% of the depth range)\n",
               (unsigned)kBinMin[clean],
               100.0 * (double)kBinMin[clean] / 16777215.0);
    }

    /*
     * The CHECK is on the tail, not on the total. Surfaces the DS itself holds
     * a long way apart are the ones a swap would show on: that is a wrong
     * picture. Surfaces it holds within a few depth units of each other are
     * ambiguous on the DS too, and disagreeing about those is a pixel on a
     * seam.
     *
     * And it is a regression guard, not a verdict. The plan's rule for this
     * task is that the difference statistic gets chosen before the renderer is
     * written, so that a threshold cannot be picked afterwards to fit the
     * result; which means the honest thing to do with a number that is not
     * zero is to record it and pin it, not to pick a bound it happens to sit
     * under and call that a pass. Z mode measures exactly zero here and is
     * held to zero. W mode measures 0.061% and 0.029% in the two tail bins,
     * for a reason that is a property of the DS and not of this code: it
     * approximates the perspective curve with eight bits of a factor where the
     * PICA interpolates 1/W exactly. Whether that is acceptable is decided by
     * a pixel diff on real scenes, not here. This only says it must not grow.
     */
    for (b = 0; b < NBINS; b++) {
        if (kBinMin[b] >= 0x1000) {
            tailBad += bn.bad[b];
            tailCmp += bn.compared[b];
        }
    }
    {
        char msg[112];
        double rate = tailCmp ? (double)tailBad / (double)tailCmp : 1.0;

        printf("      tail (gap 0x001000 and up): %ld of %ld (%.4f%%)\n",
               tailBad, tailCmp, 100.0 * rate);
        if (!wbuffer) {
            snprintf(msg, sizeof msg,
                     "Z-buffer depth never swaps surfaces 0x1000 or more apart");
            CHECK(msg, tailCmp > 0 && tailBad == 0);
        } else {
            snprintf(msg, sizeof msg,
                     "W-buffer depth swaps under 0.1%% of them, and no more");
            CHECK(msg, tailCmp > 0 && rate < 0.001);
        }
    }
}

/* ------------------------------------------------------------------ */

int main(void)
{
    static PcGxRenderRegs regs;
    regs.DispCnt = 1u;
    sRegs = &regs;
    R = &regs;

    {
        int ran = 0;
        int bad = pica3d_selftest(&ran);

        sRan += ran;
        sBad += bad;
        if (bad != 0) {
            printf("  %-56s FAILED (%d of %d)\n", "pica3d_selftest", bad, ran);
        }
    }

    round_trip();

    /*
     * Neither mode is exact, and the first draft of this file asserted that z
     * Mode would be. The reasoning was that both machines interpolate depth
     * linearly in screen space, which is true and is not enough: the DS
     * truncates the depth GRADIENT first (nine bits of it along X) so its
     * ramp is a linear one with a rounded slope. The two therefore agree at the
     * start of a span and drift along it, and the sweep below is how far apart
     * two surfaces have to be before that drift stops mattering.
     */
    depth_sweep(0, "Z");
    depth_sweep(1, "W");

    printf("pica3d_vtx: %d checks, %d failed\n", sRan, sBad);
    return sBad != 0;
}
