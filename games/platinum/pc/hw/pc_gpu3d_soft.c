/*
 * The rasterizer.
 *
 * Derived from melonDS, which is GPLv3-or-later, so this file is too.
 * Upstream: src/GPU3D_Soft.cpp (the depth tests, the alpha blend, the
 * per-pixel shading, the shadow-mask and polygon scanline loops, the fog
 * density curve, the final pass and the buffer clear) and src/GPU3D_Soft.h
 * (the interpolator, the slope walker and the buffer geometry). Copyright
 * 2016-2026 melonDS team. See pc/hw/README for why the algorithms are lifted
 * into C here rather than linking melonDS's C++ against melonDS's state.
 *
 * Theirs: every shift, truncation, division and comparison. The interpolator
 * is a theory about the hardware, and a formulation that is algebraically
 * equal but rounds differently is a wrong pixel, so the expressions below are
 * transcribed rather than rearranged. Ours: where the registers come from, the
 * trap for the texture unit, and the storage.
 *
 * Deliberately absent: upstream's render thread and its FrameIdentical cache.
 * Both are about when a frame is drawn rather than what it contains, and guest
 * time here only moves when the guest yields, so there is nothing for a
 * background thread to overlap with. When rebasing, check that new upstream
 * code in RenderThreadFunc, FinishRendering and RestartFrame really is only
 * about scheduling.
 *
 * The texture unit reads VRAM rather than a copy. Upstream flattens the
 * texture and palette spaces into two buffers kept coherent with a dirty-page
 * set; this port resolves the four texture slots and eight palette slots to
 * pointers into the one bank store at the frame boundary and reads through
 * them. Two banks on one slot, which hardware ORs and a pointer cannot
 * express, traps exactly as vram_overlap() does.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "armrec_rt.h"
#include "pc_gpu3d.h"
#include "pc_gpu3d_gl.h"
#include "pc_gpu3d_gl_tex.h"
#include "pc_gpu3d_soft.h"
#include "pc_view.h"
#include "pc_workers.h"

/* GCC and Clang both honour this; anything else gets a plain hint. */
#if defined(__GNUC__)
#define PC_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define PC_ALWAYS_INLINE inline
#endif

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/*
 * Buffer geometry, upstream's. 258x194 rather than 256x192: edge marking reads
 * the four neighbours of every pixel, so a one-pixel border removes the
 * boundary cases from the inner loop. The buffers are doubled because
 * anti-aliasing needs the pixel that was pushed down underneath an edge.
 */
#if defined(__3DS__)
/*
 * The console renders at the DS's own resolution and nothing else. The three
 * buffers below are 2.12 MB each at the desktop maximum, 6.35 MB of a 64 MB
 * machine, for two enhancements it cannot use: its top screen is 400 pixels
 * wide, so a 684-pixel render would be downscaled to fit, and HD 2x is four
 * times the fill rate of a 268 MHz CPU that is already the frame's cost.
 * Sizing them here rather than allocating at run time keeps the maximum a
 * compile-time constant, which is what the address arithmetic below assumes.
 */
#define RENDER_W_MAX      256
#define RENDER_H_MAX      192
#else
/* The page's own cap, so a build that widens PC_VIEW_WIDE_MAX widens this
 * surface with it, a rasterizer narrower than the published frame hands
 * the compose rows it must refuse, which reads as black margins and a
 * frustum that disagrees with its own picture. */
#define RENDER_W_MAX      ((int)PC_VIEW_WIDE_MAX * PC_GPU3D_HD_MAX)
#define RENDER_H_MAX      (192 * PC_GPU3D_HD_MAX)
#endif
#define SCANLINE_W        (RENDER_W_MAX + 2)
#define NUM_SCANLINES     (RENDER_H_MAX + 2)
#define BUFFER_SIZE       (SCANLINE_W * NUM_SCANLINES)
#define FIRST_PIXEL_OFF   (SCANLINE_W + 1)

/*
 * The render width. 256 native; pc_gpu3d_soft_set_width() widens it for the
 * wide-rendering enhancement; the buffers are laid out for the maximum
 * whatever the width, so widening changes no address arithmetic, only how
 * many columns of each line carry pixels and where the right border cell
 * sits (always at RW + 1, beside the last visible column, which is what
 * edge marking's neighbour reads rely on).
 */
static int RW = 256;
static int RH = 192;            /* the render height, RW's other axis     */
static int RS = 1;              /* the HD scale both grew by: RW/RS and
                                 * RH/RS are the display-resolution size  */

/* Engine A's BG0 horizontal scroll, which is the 3D layer's X position. */
#define REG_BG0HOFS       0x04000010u

/* The same translator pc/hw/pc_gpu2d.c and pc/hw/pc_gpu3d.c use: on PC a guest
 * address is a host address and this folds away; on the 3DS it is a number. */
#if defined(__3DS__)
/*
 * The inline fast path. hostmap_ptr() is a memoised translation in another
 * object, and this macro sits in the innermost per-pixel accessors: a 2D frame
 * makes about 470,000 of these lookups and the whole frame is 46 million ARM11
 * cycles, so the call, the eight-way scan behind it and its counter are a
 * large part of the frame by themselves. On the desktop the macro is a cast,
 * which is why the cost only exists here.
 *
 * 3ds/src/3ds_hostmap.c publishes its most recent answer in the three
 * variables below. A scanline renderer stays inside one block for a long run,
 * so testing that one entry inline catches nearly every access and the real
 * cache stays behind it. hostmap_fast_tag is 0 whenever there is nothing safe
 * to use (an unmapped block, a flushed cache) so the test falls through to
 * the call rather than needing to encode those cases.
 *
 * HOSTMAP_FAST_SHIFT must equal HOSTMAP_BLK_SHIFT in 3ds/src/3ds_hostmap.c.
 * It cannot be included from here (this file is on the DS SDK's include chain,
 * not the port's), so 3ds/tests/run.sh greps both and fails if they differ,
 * the same arrangement the keypad constants already have.
 */
#define HOSTMAP_FAST_SHIFT 14
extern unsigned int hostmap_fast_tag;
extern unsigned char *hostmap_fast_base;
extern unsigned int hostmap_fast_limit;
void *hostmap_ptr(uint32_t guest);      /* 3ds/src/3ds_hostmap.h */

static inline void *hostmap_fast(uint32_t a)
{
    uint32_t tag = (a >> HOSTMAP_FAST_SHIFT) + 1u;
    uint32_t off = a & ((1u << HOSTMAP_FAST_SHIFT) - 1u);

    if (tag == hostmap_fast_tag && off < hostmap_fast_limit) {
        return hostmap_fast_base + off;
    }
    return hostmap_ptr(a);
}
#define G3D_HOST(a) hostmap_fast((uint32_t)(a))
#else
#define G3D_HOST(a) ((void *)(uintptr_t)(a))
#endif

static u32 ColorBuffer[BUFFER_SIZE * 2];
static u32 DepthBuffer[BUFFER_SIZE * 2];
static u32 AttrBuffer[BUFFER_SIZE * 2];
static u8  StencilBuffer[RENDER_W_MAX * 2];
static int PrevIsShadowMask;
/* Set for a frame that carries a shadow mask or a shadow polygon; such a
 * frame is drawn serially, because the stencil is state that crosses a
 * scanline boundary. */
static int StencilFrame;
/*
 * Where each row was drawn: the union of every span's clamped x-range on
 * the row, packed (max << 16) | min. The final pass walks only this window:
 * A pixel outside it was written by nobody but the clear, and a clear
 * pixel has no edge flag in bits 0-3, so edge marking and anti-aliasing
 * skip it by the same test they always ran. Fog is the one pass that
 * reaches pixels no span wrote (the clear plane carries its own fog flag),
 * so a fogged frame keeps the full-width walk.
 *
 * One store per row, from the thread that drew the row: the extent is
 * accumulated in render_scanline's locals and stored when the row is done.
 * The first shape of this (two u16 arrays widened per span) lost 2-3%
 * at eight threads while winning 1.5% at one: thirty-two rows share a
 * cache line, a chunk is as few as eight, so every span's compare-and-store
 * ping-ponged lines between the threads that owned neighbouring chunks.
 * Per-row stores make the traffic 768 stores a frame instead of one per
 * span, and the band barrier publishes them to the final pass the same way
 * it publishes the pixels.
 */
static u32 RowExt[RENDER_H_MAX];
static u32 ScrolledLine[RENDER_W_MAX];
/* The wide and high-resolution rows scroll into buffers of their own: the
 * compositor holds a native row and a wide row of the same scanline at the
 * same time, and one buffer between them would alias. */
static u32 ScrolledWide[RENDER_W_MAX];
static u32 ScrolledHd[RENDER_W_MAX];
static u32 PolysDrawn;

/*
 * What the final pass touched, over the run. Two counters, because the 3DS work has
 * two candidate answers for the difference a GPU rasterizer leaves on the
 * silhouettes and DISP3DCNT has both bits set on nearly every frame this game
 * draws: edge marking paints an outline colour and anti-aliasing blends the
 * edge with the pixel it covered. A count of each is what says which one is
 * worth building on a machine that has neither.
 */
static unsigned long EdgeMarkedPixels;
static unsigned long AABlendedPixels;
/* ...and the third full-width pass's own count, kept the same way: how many
 * topmost pixels the fog pass actually blended, which is a different number
 * from how many polygons carried the fog attribute. */
static unsigned long FoggedPixels;

/*
 * PC_SURVEY3D, what this game's 3D frames actually use, counted with the
 * renderer's own predicates. The 3DS port's own survey halved a
 * renderer task by finding half the feature space dead; this is the desktop
 * twin, and it prices every POLY_SPAN specialization before one is written
 * before one is written. Everything here is poly-, span- or frame-grain: the
 * pixel loop is not touched, so a survey run still measures the real spans.
 *
 * The span-grain counters are written from the band threads, so they
 * accumulate per slice, padded a cache line apart, and are summed at the
 * report. The poly- and frame-grain ones are only ever written on the
 * dispatching thread.
 */
static int SurveyOn;

typedef struct Sv3dBand {
    unsigned long long spans;          /* polygon-scanlines walked          */
    unsigned long long nonlinear;      /* ...whose interpolator divides     */
    unsigned long long div_px;         /* pixels of those spans (= divides) */
    unsigned long long aaedge_px;      /* pixels in AA edge spans (6-word
                                        * push-down candidates)             */
    unsigned char pad[32];
} Sv3dBand;
static Sv3dBand SvBand[PC_WORKERS_MAX];

static struct {
    unsigned long long frames, polyframes, polys;
    unsigned long long mode_modulate, mode_decal, mode_toon, mode_highlight,
                       mode_shadow;
    unsigned long long textured, wireframe, translucent, wbuffer;
    unsigned long long fog_attr, clip4, shadowpoly, shadowmask;
    unsigned long long fmt[8];
    unsigned long long f_edge, f_fog, f_aa, f_alpha;
} Sv;

static void survey_report(void)
{
    static const char *const fmtname[8] = {
        "none", "a3i5", "pal4", "pal16", "pal256", "cmp4x4", "a5i3", "direct"
    };
    unsigned long long spans = 0, nonlinear = 0, div_px = 0, aaedge = 0;
    int i;

    for (i = 0; i < PC_WORKERS_MAX; i++) {
        spans     += SvBand[i].spans;
        nonlinear += SvBand[i].nonlinear;
        div_px    += SvBand[i].div_px;
        aaedge    += SvBand[i].aaedge_px;
    }
    fprintf(stderr, "\npc-survey3d: %llu frames, %llu with polygons, "
            "%llu polygons (%.1f per poly-frame)\n",
            Sv.frames, Sv.polyframes, Sv.polys,
            Sv.polyframes ? (double)Sv.polys / (double)Sv.polyframes : 0.0);
    fprintf(stderr, "pc-survey3d: blend modulate %llu decal %llu toon %llu "
            "highlight %llu shadow %llu\n", Sv.mode_modulate, Sv.mode_decal,
            Sv.mode_toon, Sv.mode_highlight, Sv.mode_shadow);
    fprintf(stderr, "pc-survey3d: textured %llu wireframe %llu translucent "
            "%llu wbuffer %llu fog-attr %llu clip>4 %llu shadow-poly %llu "
            "shadow-mask %llu\n", Sv.textured, Sv.wireframe, Sv.translucent,
            Sv.wbuffer, Sv.fog_attr, Sv.clip4, Sv.shadowpoly, Sv.shadowmask);
    fprintf(stderr, "pc-survey3d: texfmt");
    for (i = 0; i < 8; i++) {
        fprintf(stderr, " %s %llu", fmtname[i], Sv.fmt[i]);
    }
    fprintf(stderr, "\npc-survey3d: frames with edge-marking %llu fog %llu "
            "aa %llu alpha-test %llu\n", Sv.f_edge, Sv.f_fog, Sv.f_aa,
            Sv.f_alpha);
    fprintf(stderr, "pc-survey3d: spans %llu nonlinear %llu (%.1f%%) "
            "divide-pixels %llu aa-edge-pixels %llu\n", spans, nonlinear,
            spans ? 100.0 * (double)nonlinear / (double)spans : 0.0,
            div_px, aaedge);
    fprintf(stderr, "pc-survey3d: final-pass edge-marked %lu aa-blended %lu "
            "fogged %lu\n", EdgeMarkedPixels, AABlendedPixels, FoggedPixels);
    fflush(stderr);
}

void pc_gpu3d_soft_survey_init(void)
{
    if (getenv("PC_SURVEY3D") == NULL) {
        return;
    }
    SurveyOn = 1;
    atexit(survey_report);
}

static const PcGxRenderRegs *R;   /* the latched registers, for this frame */

#if defined(__GNUC__)
__attribute__((noreturn, format(printf, 1, 2)))
static void gxr_fatal(const char *fmt, ...);
#endif
static void gxr_fatal(const char *fmt, ...)
{
    va_list ap;
#if defined(__3DS__)
    /* Same reason as pc_gpu3d.c's: the console has no stderr, so the message
     * goes over the last frame rather than into a silent abort. */
    char what[192];

    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    armrec_trap("pc_gpu3d_soft", what);
#else
    va_start(ap, fmt);
    fprintf(stderr, "pc_gpu3d_soft: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
#endif
}

/* ------------------------------------------------------------------ */
/* The interpolator                                                    */
/* ------------------------------------------------------------------ */
/*
 * Upstream's comment is worth keeping, because it is the reason nothing here
 * is rearranged: this is a *theory* of how the DS interpolates, matching the
 * hardware on the tests its author could run. The GPU approximates a
 * perspective-correct interpolation between 0 and 1 and then uses that as a
 * linear factor, with 9 bits of precision along Y and 8 along X, and takes a
 * separate exactly-linear path when the two W values are equal with their low
 * bits clear; which is what makes the GPU usable for 2D.
 *
 * `dir` is upstream's template parameter: 1 along Y, 0 along X.
 */
typedef struct Interp {
    s32 x0, x1, xdiff, x;
    int shift;
    int linear;
    int wbuffer;
    s32 xrecip_z;
    s32 w0n, w0d, w1d;
    u32 yfactor;
    int dir;
} Interp;

static void interp_setup(Interp *I, int dir, s32 x0, s32 x1, s32 w0, s32 w1,
                         int wbuffer)
{
    u32 mask;

    I->dir     = dir;
    I->x0      = x0;
    I->x1      = x1;
    I->xdiff   = x1 - x0;
    I->wbuffer = wbuffer;
    I->x       = 0;
    I->yfactor = 0;

    /* reciprocal for Z interpolation */
    if (I->xdiff != 0) I->xrecip_z = (1 << 22) / I->xdiff;
    else               I->xrecip_z = 0;

    /* linear mode when both W values are equal with their low bits cleared,
     * bits 0-6 along X, 1-6 along Y */
    mask = dir ? 0x7E : 0x7F;
    I->linear = ((w0 == w1) && !(w0 & mask) && !(w1 & mask)) ? 1 : 0;

    if (dir) {
        I->w0n   = w0 >> 1;
        I->w0d   = (w0 + ((w0 & ~w1) & 1)) >> 1;
        I->w1d   = w1 >> 1;
        I->shift = 9;
    } else {
        I->w0n   = w0;
        I->w0d   = w0;
        I->w1d   = w1;
        I->shift = 8;
    }
}

static void interp_set_x(Interp *I, s32 x)
{
    x -= I->x0;
    I->x = x;
    if ((I->xdiff != 0) && ((!I->linear) || I->wbuffer)) {
        u32 num = (u32)(x * I->w0n) << I->shift;
        u32 den = (u32)((x * I->w0d) + ((I->xdiff - x) * I->w1d));

        /* upstream: "this seems to be a proper division on hardware" */
        if (den == 0) I->yfactor = 0;
        else          I->yfactor = num / den;
    }
}

static s32 interp_interpolate(const Interp *I, s32 y0, s32 y1)
{
    if (I->xdiff == 0 || y0 == y1) return y0;

    if (!I->linear) {
        /* perspective-correct approximation */
        if (y0 < y1) return y0 + (s32)((u32)(y1 - y0) * I->yfactor >> I->shift);
        else         return y1 + (s32)((u32)(y0 - y1) *
                                       ((1u << I->shift) - I->yfactor) >> I->shift);
    } else {
        if (y0 < y1) return y0 + (s32)((s64)(y1 - y0) * I->x / I->xdiff);
        else         return y1 + (s32)((s64)(y0 - y1) * (I->xdiff - I->x) / I->xdiff);
    }
}

PC_ALWAYS_INLINE static s32 interp_interpolate_z(const Interp *I, s32 z0, s32 z1)
{
    if (I->xdiff == 0 || z0 == z1) return z0;

    if (I->wbuffer) {
        /* W-buffering: perspective-correct approximation */
        if (z0 < z1) return z0 + (s32)(((s64)(z1 - z0) * I->yfactor) >> I->shift);
        else         return z1 + (s32)(((s64)(z0 - z1) *
                                        (u32)((1u << I->shift) - I->yfactor)) >> I->shift);
    } else {
        /* Z-buffering: linear. Upstream: "still doesn't quite match hardware" */
        s32 base = 0, disp = 0, factor = 0;

        if (z0 < z1) { base = z0; disp = z1 - z0; factor = I->x; }
        else         { base = z1; disp = z0 - z1; factor = I->xdiff - I->x; }

        if (I->dir) {
            int shift = 0;
            while (disp > 0x3FF) { disp >>= 1; shift++; }
            return base + (s32)((((s64)disp * factor * I->xrecip_z) >> 22) << shift);
        } else {
            disp >>= 9;
            return base + (s32)(((s64)disp * factor * I->xrecip_z) >> 13);
        }
    }
}

/* ------------------------------------------------------------------ */
/* The slope walker                                                    */
/* ------------------------------------------------------------------ */
/* `side` is upstream's template parameter: 0 left, 1 right. */
typedef struct Slope {
    s32   Increment;
    int   Negative;
    int   XMajor;
    Interp Interp;      /* Interpolator<1> */

    s32   x0, xmin, xmax;
    s32   xlen, ylen;
    s32   dx;
    s32   y;
    s32   xcov_incr;
    int   side;
} Slope;

static s32 slope_xval(const Slope *S)
{
    s32 ret;
    if (S->Negative) ret = S->x0 - (S->dx >> 18);
    else             ret = S->x0 + (S->dx >> 18);

    if (ret < S->xmin)      ret = S->xmin;
    else if (ret > S->xmax) ret = S->xmax;
    return ret;
}

static s32 slope_setup_dummy(Slope *S, int side, s32 x0, int wbuffer)
{
    S->side = side;
    S->dx = 0;
    S->x0 = x0;
    S->xmin = x0;
    S->xmax = x0;
    S->xlen = 0;
    S->ylen = 0;
    S->y = 0;
    S->Negative = 0;
    S->Increment = 0;
    S->XMajor = 0;

    interp_setup(&S->Interp, 1, 0, 0, 0, 0, wbuffer);
    interp_set_x(&S->Interp, 0);

    S->xcov_incr = 0;
    return x0;
}

static s32 slope_setup(Slope *S, int side, s32 x0, s32 x1, s32 y0, s32 y1,
                       s32 w0, s32 w1, s32 y, int wbuffer)
{
    s32 x;
    int interpoffset;

    S->side = side;
    S->x0 = x0;
    S->y = y;

    if (x1 > x0) {
        S->xmin = x0;
        S->xmax = x1 - 1;
        S->Negative = 0;
    } else if (x1 < x0) {
        S->xmin = x1;
        S->xmax = x0 - 1;
        S->Negative = 1;
    } else {
        S->xmin = x0;
        S->xmax = S->xmin;
        S->Negative = 0;
    }

    S->xlen = S->xmax + 1 - S->xmin;
    S->ylen = y1 - y0;

    /* The increment has an 18-bit fraction, and upstream notes that x/y is not
     * computed directly: 1/y is, and is then multiplied by x. The truncation
     * that leaves is the hardware's own answer, and the abs below is load
     * bearing: it covers a negative x span AND a negative ylen, and an
     * increment that comes out negative walks the edge the wrong way.
     *
     * A version that divided once instead, more precise above the DS's
     * resolution, where this truncation's error grows as the square of the
     * scale, lived here on 2026-08-19 and was taken back out. It took the
     * abs of the span only, so a negative ylen produced a negative increment,
     * and the port hung in the intro at --hd3d 2 and above. It bought nothing
     * visible, so it is not worth a second attempt without a test that drives
     * a negative ylen. */
    if (S->ylen == 0)
        S->Increment = 0;
    else if (S->ylen == S->xlen && S->xlen != 1)
        S->Increment = 0x40000;
    else {
        s32 yrecip = (1 << 18) / S->ylen;
        S->Increment = (x1 - x0) * yrecip;
        if (S->Increment < 0) S->Increment = -S->Increment;
    }

    S->XMajor = (S->Increment > 0x40000);

    if (side) {
        if (S->XMajor)            S->dx = S->Negative ? (0x20000 + 0x40000)
                                                      : (S->Increment - 0x20000);
        else if (S->Increment != 0) S->dx = S->Negative ? 0x40000 : 0;
        else                        S->dx = 0;
    } else {
        if (S->XMajor)            S->dx = S->Negative ? ((S->Increment - 0x20000) + 0x40000)
                                                      : 0x20000;
        else if (S->Increment != 0) S->dx = S->Negative ? 0x40000 : 0;
        else                        S->dx = 0;
    }

    S->dx += (y - y0) * S->Increment;

    x = slope_xval(S);

    interpoffset = (S->Increment >= 0x40000) && (side ^ S->Negative);
    interp_setup(&S->Interp, 1, y0 - interpoffset, y1 - interpoffset, w0, w1, wbuffer);
    interp_set_x(&S->Interp, y);

    /* used for AA coverage. XMajor means Increment > 0x40000, which cannot
     * happen with x1 == x0, so xlen is at least 1 here; the trap is because a
     * division by zero on this path would be a silent wrong picture on some
     * hosts rather than a crash. */
    if (S->XMajor) {
        if (S->xlen == 0) gxr_fatal("an X-major slope with zero X length");
        S->xcov_incr = (S->ylen << 10) / S->xlen;
    }

    return x;
}

static s32 slope_step(Slope *S)
{
    s32 x;
    S->dx += S->Increment;
    S->y++;
    x = slope_xval(S);
    interp_set_x(&S->Interp, S->y);
    return x;
}

static void slope_edge_params_xmajor(const Slope *S, int swapped,
                                     s32 *length, s32 *coverage)
{
    s32 startx, startcov;

    *length = 1;

    /* upstream only computes the length for the right side when swapped: the
     * spans are broken there and it is wanted for AA alone. It leaves *length
     * untouched in the other case and never reads it there; the 1 above is so
     * that this file has no path that reads an unset one. */
    if (!swapped || S->side) {
        if (S->side ^ S->Negative)
            *length = (S->dx >> 18) - ((S->dx - S->Increment) >> 18);
        else
            *length = ((S->dx + S->Increment) >> 18) - (S->dx >> 18);
    }

    /* for X-major edges the coverage of the first pixel is returned, with the
     * increment for the rest of the scanline packed beside it */
    startx = S->dx >> 18;
    if (S->Negative) startx = S->xlen - startx;
    if (S->side)     startx = startx - *length + 1;

    if (S->xlen == 0) gxr_fatal("an X-major slope with zero X length");
    startcov = (((startx << 10) + 0x1FF) * S->ylen) / S->xlen;
    *coverage = (s32)(1u << 31) | ((startcov & 0x3FF) << 12) | (S->xcov_incr & 0x3FF);

    if (swapped) *length = 1;
}

static void slope_edge_params_ymajor(const Slope *S, int swapped,
                                     s32 *length, s32 *coverage)
{
    *length = 1;

    if (S->Increment == 0) {
        /* vertical edges' AA values are inverted too when swapped */
        *coverage = swapped ? 0 : 31;
    } else {
        s32 cov = ((S->dx >> 9) + (S->Increment >> 10)) >> 4;
        if ((cov >> 5) != (S->dx >> 18)) cov = 31;
        cov &= 0x1F;
        if (swapped) {
            if (S->side ^ S->Negative) cov = 0x1F - cov;
        } else {
            if (!(S->side ^ S->Negative)) cov = 0x1F - cov;
        }
        *coverage = cov;
    }
}

static void slope_edge_params(const Slope *S, int swapped,
                              s32 *length, s32 *coverage)
{
    if (S->XMajor) slope_edge_params_xmajor(S, swapped, length, coverage);
    else           slope_edge_params_ymajor(S, swapped, length, coverage);
}

/* ------------------------------------------------------------------ */
/* Depth tests                                                         */
/* ------------------------------------------------------------------ */
/*
 * The test is "less or equal" rather than "less than" when a front-facing
 * pixel is drawn over an opaque back-facing one, and the equal window differs
 * with the buffering mode: +-0x200 for Z, +-0xFF for W.
 */
/*
 * Why a mode and not a function pointer. The test is chosen per polygon and
 * run per pixel, and through a pointer that is an indirect call inside the
 * innermost loop, one the compiler cannot inline and the branch predictor
 * has to learn. As a small enum hoisted out of the loop it is a switch the
 * predictor gets right every time and whose arms are three comparisons.
 */
enum {
    DEPTH_EQUAL_Z, DEPTH_EQUAL_W, DEPTH_LESS_FRONT, DEPTH_LESS
};

static int depth_equal_z(s32 dstz, s32 z, u32 dstattr)
{
    s32 diff = dstz - z;
    (void)dstattr;
    return (u32)(diff + 0x200) <= 0x400;
}

static int depth_equal_w(s32 dstz, s32 z, u32 dstattr)
{
    s32 diff = dstz - z;
    (void)dstattr;
    return (u32)(diff + 0xFF) <= 0x1FE;
}

static int depth_less_than(s32 dstz, s32 z, u32 dstattr)
{
    (void)dstattr;
    return z < dstz;
}

static int depth_less_than_front(s32 dstz, s32 z, u32 dstattr)
{
    if ((dstattr & 0x00400010) == 0x00000010)   /* opaque, back facing */
        return z <= dstz;
    return z < dstz;
}

/* ------------------------------------------------------------------ */
/* The texture unit                                                    */
/* ------------------------------------------------------------------ */

/*
 * The two spaces, resolved once per frame. Upstream reads
 * VRAMFlat_Texture[addr & 0x7FFFF] and VRAMFlat_TexPal[addr & 0x1FFFF] out of
 * flattened copies; a slot table indexed the same way is the same arithmetic
 * against the banks themselves, and an unmapped slot answers zero, which is
 * what both the flat copy and the hardware give.
 *
 * Every 16-bit read below is at an even address; a direct-colour texel is
 * `(t*width+s) << 1` from a base that is `<< 3`, a compressed palette index is
 * `((vramaddr & 0x1FFFC) >> 1)` from 0x20000, and a palette entry is an even
 * offset from `texpal << 3` or `<< 4`, so none of them can straddle a
 * 16 KB slot boundary. They are assembled from two byte reads anyway, because
 * that costs one extra load and makes the boundary a non-question rather than
 * a comment claiming it cannot happen.
 */
static const u8 *TexSlot[4];      /* 128 KB apiece, NULL when unmapped */
static const u8 *TexPalSlot[8];   /* 16 KB apiece, and 6 and 7 are never
                                   * mapped: no bank can reach them */

static void latch_texture_slots(void)
{
    int i;
    for (i = 0; i < 4; i++) TexSlot[i] = (const u8 *)armrec_vram_texture(i);
    for (i = 0; i < 8; i++) TexPalSlot[i] = (const u8 *)armrec_vram_texpal(i);
}

static u32 tex_read8(u32 addr)
{
    const u8 *p = TexSlot[(addr >> 17) & 3];
    return p ? p[addr & 0x1FFFF] : 0;
}

/*
 * The 16-bit reads resolve their slot ONCE. Two byte reads pick the slot
 * twice and test it for NULL twice, and the texture unit is the single
 * largest cost in this file, half the rasterizer, measured by removing it.
 * The straddling case is still the byte pair, so the answer does not depend
 * on the claim above that it cannot arise; it is a branch that never fires
 * rather than a comment.
 */
static u32 tex_read16(u32 addr)
{
    u32 off = addr & 0x1FFFF;
    const u8 *p;

    if (off == 0x1FFFF) return tex_read8(addr) | (tex_read8(addr + 1) << 8);
    p = TexSlot[(addr >> 17) & 3];
    return p ? (u32)p[off] | ((u32)p[off + 1] << 8) : 0;
}

static u32 texpal_read8(u32 addr)
{
    const u8 *p = TexPalSlot[(addr >> 14) & 7];
    return p ? p[addr & 0x3FFF] : 0;
}

static u32 texpal_read16(u32 addr)
{
    u32 off = addr & 0x3FFF;
    const u8 *p;

    if (off == 0x3FFF) return texpal_read8(addr) | (texpal_read8(addr + 1) << 8);
    p = TexPalSlot[(addr >> 14) & 7];
    return p ? (u32)p[off] | ((u32)p[off + 1] << 8) : 0;
}

/*
 * The texture spaces, as latched for this frame, for the gl producer's
 * converter (pc/src/pc_gpu3d_gl_tex.c) and its oracle suite: thin exports
 * of the readers above, so the two producers cannot disagree about where
 * texture memory is. Valid between the frame's latch (render_frame_now)
 * and its end, the same window the spans read in.
 */
u32 pc_gpu3d_soft_tex_read8(u32 addr)     { return tex_read8(addr); }
u32 pc_gpu3d_soft_tex_read16(u32 addr)    { return tex_read16(addr); }
u32 pc_gpu3d_soft_texpal_read16(u32 addr) { return texpal_read16(addr); }

/*
 * Everything the shader can know before the first pixel. TEXIMAGE_PARAM,
 * TEXPLTT_BASE, POLYGON_ATTR and the latched DISPCNT do not change inside a
 * polygon, and deriving the texture's size, its two base addresses, its wrap
 * rules, its format and the blend mode from them is about twenty operations.
 * Doing that per texel was most of the texture unit's cost, and removing the
 * texture unit entirely was measured at half the rasterizer, so it is
 * derived once, in setup_polygon(), and the per-pixel path reads fields.
 *
 * Nothing here changes an expression: every shift, mask and comparison below
 * is the one that used to sit inside the loop, moved to where its inputs stop
 * changing.
 */
typedef struct PolyShade {
    u32 vrambase;      /* the texel base, (TexParam & 0xFFFF) << 3          */
    u32 palbase;       /* TexPalette shifted by the format's own amount     */
    /*
     * The two bases resolved to host pointers, or NULL when they cannot be.
     * A texel read is otherwise three or four operations of address decode,
     * pick the 128 KB slot out of a table, test it, mask the offset, before
     * it reads anything, and it happens once or twice per pixel. Almost every
     * texture sits inside one slot with its palette inside one palette slot,
     * and then all of that is a constant the polygon can carry. When it does
     * not (a texture that straddles a slot, an unmapped bank, the compressed
     * format, whose second read is in a different slot by design) these stay
     * NULL and the decode below runs as before.
     */
    const u8 *texptr;
    const u8 *palptr;
    /* The frame's decoded texels for this texture, color | alpha << 16,
     * or NULL, and NULL is the shipped path. See the cache below. */
    const u32 *cache;
    s32 width, height;
    u32 fmt;           /* (TexParam >> 26) & 7; 0 is "no texture"           */
    u32 polyalpha;
    u32 blendmode;
    u8  alpha0;        /* colour 0's alpha: 0 when the transparency bit set */
    u8  wrap_s, flip_s, wrap_t, flip_t;
    u8  textured;      /* DISPCNT bit 0 and a format that is not 0          */
    u8  wireframe;     /* alpha 0                                           */
    u8  toon, highlight;
} PolyShade;

/* A slot pointer that covers [base, base + span), or NULL. */
static const u8 *tex_direct(u32 base, u32 span)
{
    const u8 *p = TexSlot[(base >> 17) & 3];
    u32 off = base & 0x1FFFF;

    if (p == NULL || span == 0 || off + span > 0x20000u) return NULL;
    return p + off;
}

static const u8 *pal_direct(u32 base, u32 span)
{
    const u8 *p = TexPalSlot[(base >> 14) & 7];
    u32 off = base & 0x3FFF;

    if (p == NULL || span == 0 || off + span > 0x4000u) return NULL;
    return p + off;
}

static u32 tex_at8(const PolyShade *P, u32 off)
{
    if (P->texptr != NULL) return P->texptr[off];
    return tex_read8(P->vrambase + off);
}

static u32 tex_at16(const PolyShade *P, u32 off)
{
    if (P->texptr != NULL)
        return (u32)P->texptr[off] | ((u32)P->texptr[off + 1] << 8);
    return tex_read16(P->vrambase + off);
}

static u32 pal_at16(const PolyShade *P, u32 off)
{
    if (P->palptr != NULL)
        return (u32)P->palptr[off] | ((u32)P->palptr[off + 1] << 8);
    return texpal_read16(P->palbase + off);
}

static const u32 *texcache_get(PolyShade *P, u32 texparam, u32 texpal);
static int texcache_wanted(void);

static void poly_shade_setup(PolyShade *P, const PcGxPolygon *polygon)
{
    u32 texparam = polygon->TexParam;
    u32 texels, palbytes;

    P->cache     = NULL;
    P->vrambase  = (texparam & 0xFFFF) << 3;
    P->width     = 8 << ((texparam >> 20) & 0x7);
    P->height    = 8 << ((texparam >> 23) & 0x7);
    P->fmt       = (texparam >> 26) & 0x7;
    P->alpha0    = (u8)((texparam & (1 << 29)) ? 0 : 31);
    P->wrap_s    = (u8)((texparam & (1 << 16)) != 0);
    P->flip_s    = (u8)((texparam & (1 << 18)) != 0);
    P->wrap_t    = (u8)((texparam & (1 << 17)) != 0);
    P->flip_t    = (u8)((texparam & (1 << 19)) != 0);
    /* the four-colour format's palettes are 8-byte aligned; the rest are 16 */
    P->palbase   = (P->fmt == 2) ? (polygon->TexPalette << 3)
                                 : (polygon->TexPalette << 4);

    /*
     * How much of each space this format reads, so the two spans above can be
     * checked. The compressed format is deliberately zero: its texel and its
     * palette index live in different slots by construction, which is the one
     * case the pointer pair cannot express.
     */
    texels = (u32)P->width * (u32)P->height;
    switch (P->fmt) {
    case 1:  palbytes = 32 * 2;                    break;  /* A3I5   */
    case 2:  palbytes = 4 * 2;   texels >>= 2;     break;  /* 4       */
    case 3:  palbytes = 16 * 2;  texels >>= 1;     break;  /* 16      */
    case 4:  palbytes = 256 * 2;                   break;  /* 256     */
    case 6:  palbytes = 8 * 2;                     break;  /* A5I3    */
    case 7:  palbytes = 0;       texels <<= 1;     break;  /* direct  */
    default: palbytes = 0;       texels = 0;       break;  /* 0 and 5 */
    }
    P->texptr = tex_direct(P->vrambase, texels);
    P->palptr = (palbytes != 0) ? pal_direct(P->palbase, palbytes) : NULL;

    P->polyalpha = (polygon->Attr >> 16) & 0x1F;
    P->blendmode = (polygon->Attr >> 4) & 0x3;
    P->wireframe = (u8)(P->polyalpha == 0);
    P->textured  = (u8)((R->DispCnt & (1 << 0)) && P->fmt != 0);
    P->toon      = (u8)(P->blendmode == 2 && !(R->DispCnt & (1 << 1)));
    P->highlight = (u8)(P->blendmode == 2 && (R->DispCnt & (1 << 1)) != 0);

    /* Attached last: the decode below reads through this very shade with
     * cache still NULL, so the texels it stores are the shipped path's own
     * answers. Serial by construction; the one caller runs before the
     * frame fans out to the pool. */
    if (P->textured && texcache_wanted()) {
        P->cache = texcache_get(P, texparam, polygon->TexPalette);
    }
}

/* The survey's poly-grain row, taken from the shade the frame will actually
 * draw with, the predicates above, not a second reading of the registers. */
static void survey_poly(const PcGxPolygon *polygon, const PolyShade *sh)
{
    Sv.polys++;
    if (sh->toon)                Sv.mode_toon++;
    else if (sh->highlight)      Sv.mode_highlight++;
    else if (sh->blendmode == 3) Sv.mode_shadow++;
    else if (sh->blendmode & 1)  Sv.mode_decal++;
    else                         Sv.mode_modulate++;
    if (sh->textured)  Sv.textured++;
    if (sh->wireframe) Sv.wireframe++;
    if (polygon->Translucent) Sv.translucent++;
    if (polygon->WBuffer)     Sv.wbuffer++;
    if (polygon->Attr & (1u << 15)) Sv.fog_attr++;
    if (polygon->NumVertices > 4)   Sv.clip4++;
    if (polygon->IsShadow)     Sv.shadowpoly++;
    if (polygon->IsShadowMask) Sv.shadowmask++;
    Sv.fmt[sh->fmt & 7]++;
}

/*
 * One texel, upstream's TextureLookup. `color` comes back as RGB555 and
 * `alpha` on the texture's own 0-31 scale, which is what render_pixel() below
 * expects to combine with the vertex colour and the polygon alpha.
 *
 * The compressed format is the one to read twice, and the plan says so: a
 * 4x4 block's two texel bits live in the slot the texture address selects and
 * the 16-bit word that names its palette lives at a fixed place in the *next*
 * slot, 0x20000 above the wrapped address, halved, and 0x10000 further up
 * again above 0x40000, so that slots 0 and 2 hold texels and slots 1 and 3
 * hold their indices. Reading the index out of the texel slot draws a
 * perfectly plausible wrong picture, which is why the sweep's compressed
 * configurations map two banks and vary the block modes rather than checking
 * one block.
 */
PC_ALWAYS_INLINE static void texture_lookup(const PolyShade *P, s16 s, s16 t,
                                            u16 *color, u8 *alpha)
{
    u32 vramaddr = P->vrambase;
    u32 texpal = P->palbase;
    s32 width = P->width;
    s32 height = P->height;
    s32 si = s >> 4, ti = t >> 4;
    u8 alpha0 = P->alpha0;

    /* wrapping: repeat, repeat-and-flip, or clamp to the edge */
    if (P->wrap_s) {
        if (P->flip_s) {
            if (si & width) si = (width - 1) - (si & (width - 1));
            else            si = (si & (width - 1));
        } else {
            si &= width - 1;
        }
    } else {
        if (si < 0) si = 0;
        else if (si >= width) si = width - 1;
    }

    if (P->wrap_t) {
        if (P->flip_t) {
            if (ti & height) ti = (height - 1) - (ti & (height - 1));
            else             ti = (ti & (height - 1));
        } else {
            ti &= height - 1;
        }
    } else {
        if (ti < 0) ti = 0;
        else if (ti >= height) ti = height - 1;
    }

    /* The frame's decoded copy, when the polygon carries one. The wrap and
     * clamp above already happened, so si/ti are in range and the read is
     * one load where the switch below is two or three plus the palette. */
    if (P->cache != NULL) {
        u32 v = P->cache[(u32)((ti * width) + si)];

        *color = (u16)v;
        *alpha = (u8)(v >> 16);
        return;
    }

    switch (P->fmt) {
    case 1: {   /* A3I5 */
        u32 pixel = tex_at8(P, (u32)((ti * width) + si));

        *color = (u16)pal_at16(P, (pixel & 0x1F) << 1);
        *alpha = (u8)(((pixel >> 3) & 0x1C) + (pixel >> 6));
        break;
    }
    case 2: {   /* 4-colour */
        u32 pixel = tex_at8(P, (u32)(((ti * width) + si) >> 2));

        pixel >>= ((si & 0x3) << 1);
        pixel &= 0x3;
        *color = (u16)pal_at16(P, pixel << 1);
        *alpha = (u8)((pixel == 0) ? alpha0 : 31);
        break;
    }
    case 3: {   /* 16-colour */
        u32 pixel = tex_at8(P, (u32)(((ti * width) + si) >> 1));

        if (si & 0x1) pixel >>= 4;
        else          pixel &= 0xF;
        *color = (u16)pal_at16(P, pixel << 1);
        *alpha = (u8)((pixel == 0) ? alpha0 : 31);
        break;
    }
    case 4: {   /* 256-colour */
        u32 pixel = tex_at8(P, (u32)((ti * width) + si));

        *color = (u16)pal_at16(P, pixel << 1);
        *alpha = (u8)((pixel == 0) ? alpha0 : 31);
        break;
    }
    case 5: {   /* 4x4 compressed */
        u32 slot1addr, val, palinfo, paloffset;

        vramaddr += (u32)((ti & 0x3FC) * (width >> 2)) + (u32)(si & 0x3FC);
        vramaddr += (u32)(ti & 0x3);
        vramaddr &= 0x7FFFF;   /* every calculation wraps after slot 3 */

        slot1addr = 0x20000 + ((vramaddr & 0x1FFFC) >> 1);
        if (vramaddr >= 0x40000) slot1addr += 0x10000;

        if (vramaddr >= 0x20000 && vramaddr < 0x40000) {
            val = 0;           /* reading slot 1 for texels always reads 0 */
        } else {
            val = tex_read8(vramaddr);
            val >>= (2 * (si & 0x3));
        }

        palinfo = tex_read16(slot1addr);
        paloffset = (palinfo & 0x3FFF) << 2;

        switch (val & 0x3) {
        case 0:
            *color = (u16)texpal_read16(texpal + paloffset);
            *alpha = 31;
            break;
        case 1:
            *color = (u16)texpal_read16(texpal + paloffset + 2);
            *alpha = 31;
            break;
        case 2:
            if ((palinfo >> 14) == 1 || (palinfo >> 14) == 3) {
                u32 c0 = texpal_read16(texpal + paloffset);
                u32 c1 = texpal_read16(texpal + paloffset + 2);
                u32 r0 = c0 & 0x001F, g0 = c0 & 0x03E0, b0 = c0 & 0x7C00;
                u32 r1 = c1 & 0x001F, g1 = c1 & 0x03E0, b1 = c1 & 0x7C00;
                u32 r, g, b;

                if ((palinfo >> 14) == 1) {
                    r = (r0 + r1) >> 1;
                    g = ((g0 + g1) >> 1) & 0x03E0;
                    b = ((b0 + b1) >> 1) & 0x7C00;
                } else {
                    r = (r0 * 5 + r1 * 3) >> 3;
                    g = ((g0 * 5 + g1 * 3) >> 3) & 0x03E0;
                    b = ((b0 * 5 + b1 * 3) >> 3) & 0x7C00;
                }
                *color = (u16)(r | g | b);
            } else {
                *color = (u16)texpal_read16(texpal + paloffset + 4);
            }
            *alpha = 31;
            break;
        default:
            if ((palinfo >> 14) == 2) {
                *color = (u16)texpal_read16(texpal + paloffset + 6);
                *alpha = 31;
            } else if ((palinfo >> 14) == 3) {
                u32 c0 = texpal_read16(texpal + paloffset);
                u32 c1 = texpal_read16(texpal + paloffset + 2);
                u32 r0 = c0 & 0x001F, g0 = c0 & 0x03E0, b0 = c0 & 0x7C00;
                u32 r1 = c1 & 0x001F, g1 = c1 & 0x03E0, b1 = c1 & 0x7C00;
                u32 r = (r0 * 3 + r1 * 5) >> 3;
                u32 g = ((g0 * 3 + g1 * 5) >> 3) & 0x03E0;
                u32 b = ((b0 * 3 + b1 * 5) >> 3) & 0x7C00;

                *color = (u16)(r | g | b);
                *alpha = 31;
            } else {
                *color = 0;
                *alpha = 0;
            }
            break;
        }
        break;
    }
    case 6: {   /* A5I3 */
        u32 pixel = tex_at8(P, (u32)((ti * width) + si));

        *color = (u16)pal_at16(P, (pixel & 0x7) << 1);
        *alpha = (u8)(pixel >> 3);
        break;
    }
    default: {  /* 7: direct colour */
        *color = (u16)tex_at16(P, (u32)(((ti * width) + si) << 1));
        *alpha = (u8)((*color & 0x8000) ? 31 : 0);
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* The frame's texture cache                                           */
/* ------------------------------------------------------------------ */
/*
 * Decode each texture once per frame, then sample the decoded copy. The
 * header's rejection of upstream's flattened-copy scheme stands; that was a
 * persistent copy needing dirty tracking to stay coherent. This one lives
 * exactly as long as the frame's own texture latch, so nothing survives to go
 * stale and a generation number is the whole invalidation story.
 *
 * On at 3x and up, measured: forced on at native it costs 8.6%, because the DS
 * samples a texture about as often as it has texels; at 2x the two heavy
 * scenes split; at 3x and 4x every texel is sampled nine to sixteen times and
 * it wins everywhere it was measured. PC_TEXCACHE=1/0 forces it either way,
 * and forcing it on at native is how the byte-exactness gate is run, since the
 * decode loop is texture_lookup itself.
 *
 * The console build keeps none of this: 8 MB of arena on a 64 MB machine that
 * renders at native only would be all cost.
 */
#if !defined(__3DS__)

#define TEXCACHE_ENTRIES 128
#define TEXCACHE_TEXELS  (2u << 20)    /* u32 words: 8 MB, ~500 64x64s */

typedef struct TexCacheEntry {
    u32 key_param, key_pal, gen;
    const u32 *texels;
} TexCacheEntry;

static TexCacheEntry TexCache[TEXCACHE_ENTRIES];
static u32 TexCacheArena[TEXCACHE_TEXELS];
static u32 TexCacheUsed;
static u32 TexCacheGen;
static int TexCacheMode = -2;    /* -2 unread; -1 auto; 0 off; 1 on */

static int texcache_wanted(void)
{
    if (TexCacheMode == -2) {
        const char *e = getenv("PC_TEXCACHE");

        TexCacheMode = (e == NULL) ? -1 : (e[0] != '0');
    }
    return TexCacheMode == 1 || (TexCacheMode == -1 && RS >= 3);
}

/* A new frame's latch invalidates every entry in O(1). */
static void texcache_frame(void)
{
    TexCacheGen++;
    TexCacheUsed = 0;
}

static const u32 *texcache_get(PolyShade *P, u32 texparam, u32 texpal)
{
    /* Bits 16-19 are wrap and flip: they choose how coordinates fold, not
     * what a texel holds, so two polygons differing only there share one
     * decode. */
    u32 key = texparam & ~0x000F0000u;
    u32 h = ((key >> 3) ^ (key >> 17) ^ texpal) & (TEXCACHE_ENTRIES - 1);
    TexCacheEntry *e = NULL;
    u32 probe, need;
    u32 *dst;
    s32 si, ti;

    for (probe = 0; probe < 8; probe++) {
        e = &TexCache[(h + probe) & (TEXCACHE_ENTRIES - 1)];
        if (e->gen == TexCacheGen && e->key_param == key
            && e->key_pal == texpal) {
            return e->texels;
        }
        if (e->gen != TexCacheGen) break;   /* free this frame */
    }
    if (probe == 8) return NULL;            /* bucket full: shipped path */

    need = (u32)P->width * (u32)P->height;
    if (TexCacheUsed + need > TEXCACHE_TEXELS) return NULL;

    dst = &TexCacheArena[TexCacheUsed];
    for (ti = 0; ti < P->height; ti++) {
        for (si = 0; si < P->width; si++) {
            u16 c;
            u8 a;

            /* In-range coordinates pass the wrap and clamp above untouched
             * in every mode, so this reads texel (si, ti) exactly. */
            texture_lookup(P, (s16)(si << 4), (s16)(ti << 4), &c, &a);
            dst[ti * P->width + si] = (u32)c | ((u32)a << 16);
        }
    }
    TexCacheUsed += need;
    e->gen = TexCacheGen;
    e->key_param = key;
    e->key_pal = texpal;
    e->texels = dst;
    return dst;
}

#else /* __3DS__ */

static int texcache_wanted(void) { return 0; }
static void texcache_frame(void) { }
static const u32 *texcache_get(PolyShade *P, u32 texparam, u32 texpal)
{
    (void)P; (void)texparam; (void)texpal;
    return NULL;
}

#endif

/* ------------------------------------------------------------------ */
/* Shading                                                             */
/* ------------------------------------------------------------------ */

static u32 alpha_blend(u32 srccolor, u32 dstcolor, u32 alpha)
{
    u32 dstalpha = dstcolor >> 24;
    u32 srcR, srcG, srcB;

    if (dstalpha == 0) return srccolor;

    srcR = srccolor & 0x3F;
    srcG = (srccolor >> 8) & 0x3F;
    srcB = (srccolor >> 16) & 0x3F;

    if (R->DispCnt & (1 << 3)) {
        u32 dstR = dstcolor & 0x3F;
        u32 dstG = (dstcolor >> 8) & 0x3F;
        u32 dstB = (dstcolor >> 16) & 0x3F;

        alpha++;
        srcR = ((srcR * alpha) + (dstR * (32 - alpha))) >> 5;
        srcG = ((srcG * alpha) + (dstG * (32 - alpha))) >> 5;
        srcB = ((srcB * alpha) + (dstB * (32 - alpha))) >> 5;
        alpha--;
    }

    if (alpha > dstalpha) dstalpha = alpha;

    return srcR | (srcG << 8) | (srcB << 16) | (dstalpha << 24);
}

/*
 * Inlined on purpose. This is called once per pixel from three spans, and on
 * a 32-bit x86 the call alone is six arguments pushed to the stack, a call,
 * and a frame, about as much work as the shading. The compiler will not do
 * it unasked because the body is large.
 */
PC_ALWAYS_INLINE static u32 render_pixel(const PolyShade *P, u8 vr, u8 vg, u8 vb,
                                         s16 s, s16 t)
{
    u8 r = 0, g = 0, b = 0, a = 0;
    u32 blendmode = P->blendmode;
    u32 polyalpha = P->polyalpha;
    int wireframe = P->wireframe;

    if (P->highlight) {
        /* highlight: the colour is computed normally except that every
         * vertex colour component takes the red one, and the toon colour
         * is added at the end */
        vg = vr;
        vb = vr;
    } else if (P->toon) {
        /* toon: the vertex colour is replaced by the toon colour */
        u16 tooncolor = R->ToonTable[vr >> 1];

        vr = (tooncolor << 1) & 0x3E; if (vr) vr++;
        vg = (tooncolor >> 4) & 0x3E; if (vg) vg++;
        vb = (tooncolor >> 9) & 0x3E; if (vb) vb++;
    }

    if (P->textured) {
        u16 tcolor;
        u8 talpha, tr, tg, tb;

        texture_lookup(P, s, t, &tcolor, &talpha);

        tr = (tcolor << 1) & 0x3E; if (tr) tr++;
        tg = (tcolor >> 4) & 0x3E; if (tg) tg++;
        tb = (tcolor >> 9) & 0x3E; if (tb) tb++;

        if (blendmode & 0x1) {
            /* decal: the texture alpha chooses between the two colours
             * rather than scaling the polygon's */
            if (talpha == 0) {
                r = vr; g = vg; b = vb;
            } else if (talpha == 31) {
                r = tr; g = tg; b = tb;
            } else {
                r = (u8)(((tr * talpha) + (vr * (31 - talpha))) >> 5);
                g = (u8)(((tg * talpha) + (vg * (31 - talpha))) >> 5);
                b = (u8)(((tb * talpha) + (vb * (31 - talpha))) >> 5);
            }
            a = (u8)polyalpha;
        } else {
            /* modulate */
            r = (u8)(((tr + 1) * (vr + 1) - 1) >> 6);
            g = (u8)(((tg + 1) * (vg + 1) - 1) >> 6);
            b = (u8)(((tb + 1) * (vb + 1) - 1) >> 6);
            a = (u8)(((talpha + 1) * (polyalpha + 1) - 1) >> 5);
        }
    } else {
        r = vr;
        g = vg;
        b = vb;
        a = (u8)polyalpha;
    }

    if (P->highlight) {
        u16 tooncolor = R->ToonTable[vr >> 1];
        u32 rr, gg, bb;

        vr = (tooncolor << 1) & 0x3E; if (vr) vr++;
        vg = (tooncolor >> 4) & 0x3E; if (vg) vg++;
        vb = (tooncolor >> 9) & 0x3E; if (vb) vb++;

        rr = (u32)r + vr;
        gg = (u32)g + vg;
        bb = (u32)b + vb;

        if (rr > 63) rr = 63;
        if (gg > 63) gg = 63;
        if (bb > 63) bb = 63;
        r = (u8)rr; g = (u8)gg; b = (u8)bb;
    }

    if (wireframe) a = 31;

    return (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
}

/*
 * The modulate-textured-opaque leg of render_pixel and NOTHING else, for the
 * span specialization below. The survey measured this class at every polygon
 * of every scene on the protocol (no toon, no highlight, no decal, no
 * wireframe anywhere in this game's frames), so the general body's branches
 * were being carried per pixel for cases that never occur. The arithmetic is
 * the general body's own, kept expression for expression; polyalpha is 31 in
 * this class and ((talpha+1)*32 - 1) >> 5 is exactly talpha, so the pixel's
 * alpha is the texture's own, 0 or 31 for the palette formats this class
 * admits, which is why the caller's translucent path is provably dead.
 */
PC_ALWAYS_INLINE static u32 render_pixel_fast(const PolyShade *P, u8 vr, u8 vg,
                                              u8 vb, s16 s, s16 t)
{
    u16 tcolor;
    u8 talpha, tr, tg, tb, r, g, b;

    texture_lookup(P, s, t, &tcolor, &talpha);

    tr = (tcolor << 1) & 0x3E; if (tr) tr++;
    tg = (tcolor >> 4) & 0x3E; if (tg) tg++;
    tb = (tcolor >> 9) & 0x3E; if (tb) tb++;

    r = (u8)(((tr + 1) * (vr + 1) - 1) >> 6);
    g = (u8)(((tg + 1) * (vg + 1) - 1) >> 6);
    b = (u8)(((tb + 1) * (vb + 1) - 1) >> 6);

    return (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)talpha << 24);
}

static void plot_translucent_pixel(u32 pixeladdr, u32 color, u32 z,
                                   u32 polyattr, u32 shadow)
{
    u32 dstattr = AttrBuffer[pixeladdr];
    u32 attr = (polyattr & 0xE0F0) | ((polyattr >> 8) & 0xFF0000) | (1 << 22) |
               (dstattr & 0xFF001F0F);

    if (shadow) {
        /* for shadows the opaque pixels are checked too */
        if (dstattr & (1 << 22)) {
            if ((dstattr & 0x007F0000) == (attr & 0x007F0000)) return;
        } else {
            if ((dstattr & 0x3F000000) == (polyattr & 0x3F000000)) return;
        }
    } else {
        /* skip when the translucent polygon IDs are equal */
        if ((dstattr & 0x007F0000) == (attr & 0x007F0000)) return;
    }

    /* fog flag */
    if (!(dstattr & (1 << 15))) attr &= ~(1u << 15);

    color = alpha_blend(color, ColorBuffer[pixeladdr], color >> 24);

    if (z != (u32)-1) DepthBuffer[pixeladdr] = z;

    ColorBuffer[pixeladdr] = color;
    AttrBuffer[pixeladdr] = attr;
}

/* ------------------------------------------------------------------ */
/* Polygon setup                                                       */
/* ------------------------------------------------------------------ */

typedef struct RendererPolygon {
    PcGxPolygon *PolyData;
    Slope SlopeL, SlopeR;
    s32   XL, XR;
    u32   CurVL, CurVR;
    u32   NextVL, NextVR;
    PolyShade Shade;    /* what the shader can know before the first pixel */
} RendererPolygon;

static RendererPolygon PolygonList[PC_GX_MAX_POLYGONS];

static void setup_polygon_left_edge(RendererPolygon *rp, s32 y)
{
    PcGxPolygon *polygon = rp->PolyData;

    while (y >= polygon->Vertices[rp->NextVL]->FinalPosition[1] &&
           rp->CurVL != polygon->VBottom) {
        rp->CurVL = rp->NextVL;

        if (polygon->FacingView) {
            rp->NextVL = rp->CurVL + 1;
            if (rp->NextVL >= polygon->NumVertices) rp->NextVL = 0;
        } else {
            rp->NextVL = rp->CurVL - 1;
            if ((s32)rp->NextVL < 0) rp->NextVL = polygon->NumVertices - 1;
        }
    }

    rp->XL = slope_setup(&rp->SlopeL, 0,
                         polygon->Vertices[rp->CurVL]->FinalPosition[0],
                         polygon->Vertices[rp->NextVL]->FinalPosition[0],
                         polygon->Vertices[rp->CurVL]->FinalPosition[1],
                         polygon->Vertices[rp->NextVL]->FinalPosition[1],
                         polygon->FinalW[rp->CurVL], polygon->FinalW[rp->NextVL],
                         y, polygon->WBuffer);
}

static void setup_polygon_right_edge(RendererPolygon *rp, s32 y)
{
    PcGxPolygon *polygon = rp->PolyData;

    while (y >= polygon->Vertices[rp->NextVR]->FinalPosition[1] &&
           rp->CurVR != polygon->VBottom) {
        rp->CurVR = rp->NextVR;

        if (polygon->FacingView) {
            rp->NextVR = rp->CurVR - 1;
            if ((s32)rp->NextVR < 0) rp->NextVR = polygon->NumVertices - 1;
        } else {
            rp->NextVR = rp->CurVR + 1;
            if (rp->NextVR >= polygon->NumVertices) rp->NextVR = 0;
        }
    }

    rp->XR = slope_setup(&rp->SlopeR, 1,
                         polygon->Vertices[rp->CurVR]->FinalPosition[0],
                         polygon->Vertices[rp->NextVR]->FinalPosition[0],
                         polygon->Vertices[rp->CurVR]->FinalPosition[1],
                         polygon->Vertices[rp->NextVR]->FinalPosition[1],
                         polygon->FinalW[rp->CurVR], polygon->FinalW[rp->NextVR],
                         y, polygon->WBuffer);
}

static void setup_polygon(RendererPolygon *rp, PcGxPolygon *polygon)
{
    u32 nverts = polygon->NumVertices;
    u32 vtop = polygon->VTop, vbot = polygon->VBottom;
    s32 ytop = polygon->YTop, ybot = polygon->YBottom;

    rp->PolyData = polygon;
    poly_shade_setup(&rp->Shade, polygon);

    rp->CurVL = vtop;
    rp->CurVR = vtop;

    if (polygon->FacingView) {
        rp->NextVL = rp->CurVL + 1;
        if (rp->NextVL >= nverts) rp->NextVL = 0;
        rp->NextVR = rp->CurVR - 1;
        if ((s32)rp->NextVR < 0) rp->NextVR = nverts - 1;
    } else {
        rp->NextVL = rp->CurVL - 1;
        if ((s32)rp->NextVL < 0) rp->NextVL = nverts - 1;
        rp->NextVR = rp->CurVR + 1;
        if (rp->NextVR >= nverts) rp->NextVR = 0;
    }

    if (ybot == ytop) {
        int i;
        vtop = 0; vbot = 0;

        i = 1;
        if (polygon->Vertices[i]->FinalPosition[0] <
            polygon->Vertices[vtop]->FinalPosition[0]) vtop = i;
        if (polygon->Vertices[i]->FinalPosition[0] >
            polygon->Vertices[vbot]->FinalPosition[0]) vbot = i;

        i = (int)nverts - 1;
        if (polygon->Vertices[i]->FinalPosition[0] <
            polygon->Vertices[vtop]->FinalPosition[0]) vtop = i;
        if (polygon->Vertices[i]->FinalPosition[0] >
            polygon->Vertices[vbot]->FinalPosition[0]) vbot = i;

        rp->CurVL = vtop; rp->NextVL = vtop;
        rp->CurVR = vbot; rp->NextVR = vbot;

        rp->XL = slope_setup_dummy(&rp->SlopeL, 0,
                                   polygon->Vertices[rp->CurVL]->FinalPosition[0],
                                   polygon->WBuffer);
        rp->XR = slope_setup_dummy(&rp->SlopeR, 1,
                                   polygon->Vertices[rp->CurVR]->FinalPosition[0],
                                   polygon->WBuffer);
    } else {
        setup_polygon_left_edge(rp, ytop);
        setup_polygon_right_edge(rp, ytop);
    }
}

/* ------------------------------------------------------------------ */
/* The scanline loops                                                  */
/* ------------------------------------------------------------------ */
/*
 * The shape both loops share: walk the two edges to this scanline, work out
 * which of the two is on the left, decide whether each edge pixel is filled,
 * then run three spans, left edge, inside, right edge. Upstream writes the
 * three spans out three times in each function rather than looping, because
 * the AA coverage arithmetic differs between them; that is kept.
 */

typedef struct SpanSetup {
    PcGxVertex *vlcur, *vlnext, *vrcur, *vrnext;
    s32 xstart, xend;
    int l_filledge, r_filledge;
    s32 l_edgelen, r_edgelen;
    s32 l_edgecov, r_edgecov;
    Interp *interp_start, *interp_end;
    s32 wl, wr, zl, zr;
    int swapped;
} SpanSetup;

static void span_setup(RendererPolygon *rp, s32 y, SpanSetup *S)
{
    PcGxPolygon *polygon = rp->PolyData;
    u32 polyalpha = (polygon->Attr >> 16) & 0x1F;
    int wireframe = (polyalpha == 0);
    s32 t;

    S->xstart = rp->XL;
    S->xend   = rp->XR;

    S->wl = interp_interpolate(&rp->SlopeL.Interp, polygon->FinalW[rp->CurVL],
                               polygon->FinalW[rp->NextVL]);
    S->wr = interp_interpolate(&rp->SlopeR.Interp, polygon->FinalW[rp->CurVR],
                               polygon->FinalW[rp->NextVR]);

    S->zl = interp_interpolate_z(&rp->SlopeL.Interp, polygon->FinalZ[rp->CurVL],
                                 polygon->FinalZ[rp->NextVL]);
    S->zr = interp_interpolate_z(&rp->SlopeR.Interp, polygon->FinalZ[rp->CurVR],
                                 polygon->FinalZ[rp->NextVR]);

    /* A right vertical edge is pushed one pixel left as long as either the
     * left edge's slope is not 0 or the span is not empty, and it is not
     * already at the leftmost pixel */
    if (rp->SlopeR.Increment == 0 &&
        (rp->SlopeL.Increment != 0 || S->xstart != S->xend) && (S->xend != 0))
        S->xend--;

    /*
     * If the two edges are swapped, the span is rendered backwards. Upstream
     * records that hardware seems to break the edge-length calculation there,
     * which is why the two branches are not each other's mirror image.
     */
    if (S->xstart > S->xend) {
        S->swapped = 1;

        S->vlcur  = polygon->Vertices[rp->CurVR];
        S->vlnext = polygon->Vertices[rp->NextVR];
        S->vrcur  = polygon->Vertices[rp->CurVL];
        S->vrnext = polygon->Vertices[rp->NextVL];

        S->interp_start = &rp->SlopeR.Interp;
        S->interp_end   = &rp->SlopeL.Interp;

        slope_edge_params(&rp->SlopeR, 1, &S->l_edgelen, &S->l_edgecov);
        slope_edge_params(&rp->SlopeL, 1, &S->r_edgelen, &S->r_edgecov);

        t = S->xstart; S->xstart = S->xend; S->xend = t;
        t = S->wl;     S->wl = S->wr;       S->wr = t;
        t = S->zl;     S->zl = S->zr;       S->zr = t;

        /* edges are always filled when AA or edge marking is on, when the
         * pixels are translucent and blending is on, or when wireframe */
        if ((R->DispCnt & ((1 << 4) | (1 << 5))) ||
            ((polyalpha < 31) && (R->DispCnt & (1 << 3))) || wireframe) {
            S->l_filledge = 1;
            S->r_filledge = 1;
        } else {
            S->l_filledge = (rp->SlopeR.Negative || !rp->SlopeR.XMajor)
                || ((y == polygon->YBottom - 1) && rp->SlopeR.XMajor &&
                    (S->vlnext->FinalPosition[0] != S->vrnext->FinalPosition[0]));
            S->r_filledge = (!rp->SlopeL.Negative && rp->SlopeL.XMajor)
                || (!(rp->SlopeL.Negative && rp->SlopeL.XMajor) && rp->SlopeR.Increment == 0)
                || ((y == polygon->YBottom - 1) && rp->SlopeL.XMajor &&
                    (S->vlnext->FinalPosition[0] != S->vrnext->FinalPosition[0]));
        }
    } else {
        S->swapped = 0;

        S->vlcur  = polygon->Vertices[rp->CurVL];
        S->vlnext = polygon->Vertices[rp->NextVL];
        S->vrcur  = polygon->Vertices[rp->CurVR];
        S->vrnext = polygon->Vertices[rp->NextVR];

        S->interp_start = &rp->SlopeL.Interp;
        S->interp_end   = &rp->SlopeR.Interp;

        slope_edge_params(&rp->SlopeL, 0, &S->l_edgelen, &S->l_edgecov);
        slope_edge_params(&rp->SlopeR, 0, &S->r_edgelen, &S->r_edgecov);

        if ((R->DispCnt & ((1 << 4) | (1 << 5))) ||
            ((polyalpha < 31) && (R->DispCnt & (1 << 3))) || wireframe) {
            S->l_filledge = 1;
            S->r_filledge = 1;
        } else {
            S->l_filledge = ((rp->SlopeL.Negative || !rp->SlopeL.XMajor)
                || ((y == polygon->YBottom - 1) && rp->SlopeL.XMajor &&
                    (S->vlnext->FinalPosition[0] != S->vrnext->FinalPosition[0])))
                || ((rp->SlopeL.Increment == rp->SlopeR.Increment) &&
                    (S->xstart + S->l_edgelen == S->xend + 1));
            S->r_filledge = (!rp->SlopeR.Negative && rp->SlopeR.XMajor)
                || (rp->SlopeR.Increment == 0)
                || ((y == polygon->YBottom - 1) && rp->SlopeR.XMajor &&
                    (S->vlnext->FinalPosition[0] != S->vrnext->FinalPosition[0]));
        }
    }
}

static int depth_mode_for(const PcGxPolygon *polygon)
{
    if (polygon->Attr & (1 << 14))
        return polygon->WBuffer ? DEPTH_EQUAL_W : DEPTH_EQUAL_Z;
    if (polygon->FacingView) return DEPTH_LESS_FRONT;
    return DEPTH_LESS;
}

static inline int depth_test_by(int mode, s32 dstz, s32 z, u32 dstattr)
{
    switch (mode) {
    case DEPTH_EQUAL_Z:    return depth_equal_z(dstz, z, dstattr);
    case DEPTH_EQUAL_W:    return depth_equal_w(dstz, z, dstattr);
    case DEPTH_LESS_FRONT: return depth_less_than_front(dstz, z, dstattr);
    default:               return depth_less_than(dstz, z, dstattr);
    }
}

static void render_shadow_mask_scanline(RendererPolygon *rp, s32 y)
{
    PcGxPolygon *polygon = rp->PolyData;
    u32 polyalpha = (polygon->Attr >> 16) & 0x1F;
    int wireframe = (polyalpha == 0);
    int depth_mode = depth_mode_for(polygon);
    SpanSetup S;
    Interp interpX;
    s32 x, xlimit;
    int yedge;

    if (!PrevIsShadowMask)
        memset(&StencilBuffer[RENDER_W_MAX * (y & 0x1)], 0, (size_t)RW);

    PrevIsShadowMask = 1;

    if (polygon->YTop != polygon->YBottom) {
        if (y >= polygon->Vertices[rp->NextVL]->FinalPosition[1] &&
            rp->CurVL != polygon->VBottom)
            setup_polygon_left_edge(rp, y);
        if (y >= polygon->Vertices[rp->NextVR]->FinalPosition[1] &&
            rp->CurVR != polygon->VBottom)
            setup_polygon_right_edge(rp, y);
    }

    span_setup(rp, y, &S);

    /* colour and texcoord attributes are not needed for a shadow mask: every
     * pixel has the same alpha even with a texture, since shadows use decal
     * blending. The alpha test can therefore be done once, up front. */
    if (wireframe) polyalpha = 31;
    if (polyalpha <= R->AlphaRef) return;

    yedge = 0;
    if (y == polygon->YTop)             yedge = 0x4;
    else if (y == polygon->YBottom - 1) yedge = 0x8;

    x = S.xstart;
    interp_setup(&interpX, 0, S.xstart, S.xend + 1, S.wl, S.wr, polygon->WBuffer);
    if (x < 0) x = 0;

#define SHADOW_SPAN                                                          \
        for (; x < xlimit; x++) {                                            \
            u32 pixeladdr = FIRST_PIXEL_OFF + (y * SCANLINE_W) + x;          \
            s32 z;                                                           \
            u32 dstattr;                                                     \
                                                                             \
            interp_set_x(&interpX, x);                                       \
            z = interp_interpolate_z(&interpX, S.zl, S.zr);                  \
            dstattr = AttrBuffer[pixeladdr];                                 \
                                                                             \
            if (!depth_test_by(depth_mode, DepthBuffer[pixeladdr], z,        \
                               dstattr))                                     \
                StencilBuffer[RENDER_W_MAX * (y & 0x1) + x] = 1;             \
                                                                             \
            if (dstattr & 0xF) {                                             \
                pixeladdr += BUFFER_SIZE;                                    \
                if (!depth_test_by(depth_mode, DepthBuffer[pixeladdr], z,    \
                                   AttrBuffer[pixeladdr]))                   \
                    StencilBuffer[RENDER_W_MAX * (y & 0x1) + x] |= 0x2;      \
            }                                                                \
        }

    /* part 1: left edge. A shadow mask sets stencil bits where the depth test
     * fails and draws nothing at all. */
    xlimit = S.xstart + S.l_edgelen;
    if (xlimit > S.xend + 1) xlimit = S.xend + 1;
    if (xlimit > RW) xlimit = RW;
    if (!S.l_filledge) x = xlimit;
    else SHADOW_SPAN

    /* part 2: inside */
    xlimit = S.xend - S.r_edgelen + 1;
    if (xlimit > S.xend + 1) xlimit = S.xend + 1;
    if (xlimit > RW) xlimit = RW;
    if (wireframe && !yedge) {
        if (x < xlimit) x = xlimit;
    } else SHADOW_SPAN

    /* part 3: right edge */
    xlimit = S.xend + 1;
    if (xlimit > RW) xlimit = RW;
    if (S.r_filledge) SHADOW_SPAN

#undef SHADOW_SPAN

    rp->XL = slope_step(&rp->SlopeL);
    rp->XR = slope_step(&rp->SlopeR);
}

/*
 * `fast` is a compile-time constant at both call sites and PC_ALWAYS_INLINE
 * makes the compiler emit the two specializations the templates upstream
 * would have: the fast copy carries no toon, no decal, no wireframe, no
 * shadow stencil and no translucent plot, the branches the survey measured
 * dead on every polygon of every scene, and the general copy is this
 * function exactly as it always was. The arithmetic of the pixels either
 * copy draws is identical, which the pinned digests hold.
 */
PC_ALWAYS_INLINE
static void render_polygon_scanline_body(RendererPolygon *rp, s32 y, int slice,
                                         s32 *extmin, s32 *extmax,
                                         const int fast)
{
    PcGxPolygon *polygon = rp->PolyData;
    u32 polyattr = (polygon->Attr & 0x3F008000);
    u32 polyalpha = fast ? 31u : (polygon->Attr >> 16) & 0x1F;
    int wireframe = fast ? 0 : (polyalpha == 0);
    int depth_mode = depth_mode_for(polygon);
    /*
     * Copies, not field reads. The span below stores to three buffers on
     * every pixel, and the build is -fno-strict-aliasing, so the compiler has
     * to assume each of those stores could have changed anything it reached
     * through a pointer, the polygon record, the latched registers, the
     * shading state. Reading them once into locals whose address never
     * escapes the loop is what lets them stay in registers, and it is worth
     * about a tenth of the rasterizer.
     */
    PolyShade shade = rp->Shade;
    int is_shadow = fast ? 0 : polygon->IsShadow;
    int keep_z = (polygon->Attr & (1 << 11)) != 0;
    u32 alpharef = R->AlphaRef;
    int aa_on = (R->DispCnt & (1 << 4)) != 0;
    SpanSetup S;
    Interp interpX;
    s32 rl, gl, bl, sl, tl, rr, gr, br, sr, tr;
    s32 x, xlimit, xcov = 0;
    int yedge, edge;

    if (!polygon->FacingView) polyattr |= (1 << 4);

    /* Only a frame that has a shadow volume in it reads this, and such a
     * frame is drawn on one thread; a banded frame must not write it at all
     * or several bands would be storing to the same byte. */
    if (StencilFrame) PrevIsShadowMask = 0;

    if (polygon->YTop != polygon->YBottom) {
        if (y >= polygon->Vertices[rp->NextVL]->FinalPosition[1] &&
            rp->CurVL != polygon->VBottom)
            setup_polygon_left_edge(rp, y);
        if (y >= polygon->Vertices[rp->NextVR]->FinalPosition[1] &&
            rp->CurVR != polygon->VBottom)
            setup_polygon_right_edge(rp, y);
    }

    span_setup(rp, y, &S);

    /* interpolate the attributes along Y */
    rl = interp_interpolate(S.interp_start, S.vlcur->FinalColor[0], S.vlnext->FinalColor[0]);
    gl = interp_interpolate(S.interp_start, S.vlcur->FinalColor[1], S.vlnext->FinalColor[1]);
    bl = interp_interpolate(S.interp_start, S.vlcur->FinalColor[2], S.vlnext->FinalColor[2]);
    sl = interp_interpolate(S.interp_start, S.vlcur->TexCoords[0], S.vlnext->TexCoords[0]);
    tl = interp_interpolate(S.interp_start, S.vlcur->TexCoords[1], S.vlnext->TexCoords[1]);

    rr = interp_interpolate(S.interp_end, S.vrcur->FinalColor[0], S.vrnext->FinalColor[0]);
    gr = interp_interpolate(S.interp_end, S.vrcur->FinalColor[1], S.vrnext->FinalColor[1]);
    br = interp_interpolate(S.interp_end, S.vrcur->FinalColor[2], S.vrnext->FinalColor[2]);
    sr = interp_interpolate(S.interp_end, S.vrcur->TexCoords[0], S.vrnext->TexCoords[0]);
    tr = interp_interpolate(S.interp_end, S.vrcur->TexCoords[1], S.vrnext->TexCoords[1]);

    yedge = 0;
    if (y == polygon->YTop)                yedge = 0x4;
    else if (y == polygon->YBottom - 1)    yedge = 0x8;

    x = S.xstart;
    interp_setup(&interpX, 0, S.xstart, S.xend + 1, S.wl, S.wr, polygon->WBuffer);
    if (x < 0) x = 0;

/*
 * `cov_mode` selects which of the three coverage rules the span uses: 1 is the
 * left edge's running counter, 2 the right edge's, 0 the inside's flat 0x1F.
 * Upstream spells the three spans out separately for exactly this reason.
 */
#define POLY_SPAN(cov_mode)                                                   \
        for (; x < xlimit; x++) {                                             \
            u32 pixeladdr = FIRST_PIXEL_OFF + (y * SCANLINE_W) + x;           \
            u32 dstattr = AttrBuffer[pixeladdr];                              \
            u32 vr, vg, vb, color;                                            \
            s16 sc, tc;                                                       \
            s32 z;                                                            \
            u8 alpha;                                                         \
                                                                              \
            if (is_shadow) {                                                  \
                u8 stencil = StencilBuffer[RENDER_W_MAX * (y & 0x1) + x];     \
                if (!stencil) continue;                                       \
                if (!(stencil & 0x1)) pixeladdr += BUFFER_SIZE;               \
                if (!(stencil & 0x2)) dstattr &= ~0xFu;                       \
            }                                                                 \
                                                                              \
            interp_set_x(&interpX, x);                                        \
            z = interp_interpolate_z(&interpX, S.zl, S.zr);                   \
                                                                              \
            /* if the topmost pixel fails, test the one underneath */         \
            if (!depth_test_by(depth_mode, DepthBuffer[pixeladdr], z,         \
                               dstattr)) {                                    \
                if (!(dstattr & 0xF) || pixeladdr >= BUFFER_SIZE) continue;   \
                pixeladdr += BUFFER_SIZE;                                     \
                dstattr = AttrBuffer[pixeladdr];                              \
                if (!depth_test_by(depth_mode, DepthBuffer[pixeladdr], z,     \
                                   dstattr)) continue;                        \
            }                                                                 \
                                                                              \
            vr = (u32)interp_interpolate(&interpX, rl, rr);                   \
            vg = (u32)interp_interpolate(&interpX, gl, gr);                   \
            vb = (u32)interp_interpolate(&interpX, bl, br);                   \
            sc = (s16)interp_interpolate(&interpX, sl, sr);                   \
            tc = (s16)interp_interpolate(&interpX, tl, tr);                   \
                                                                              \
            color = fast ? render_pixel_fast(&shade, (u8)(vr >> 3),           \
                                             (u8)(vg >> 3), (u8)(vb >> 3),    \
                                             sc, tc)                          \
                         : render_pixel(&shade, (u8)(vr >> 3), (u8)(vg >> 3), \
                                        (u8)(vb >> 3), sc, tc);               \
            alpha = (u8)(color >> 24);                                        \
                                                                              \
            if (alpha <= alpharef) continue;                                  \
                                                                              \
            if (fast || alpha == 31) {                                        \
                u32 attr = polyattr | (u32)edge;                              \
                                                                              \
                if ((cov_mode) == 0) {                                        \
                    if (aa_on && (attr & 0xF)) {                              \
                        attr |= (0x1Fu << 8);                                 \
                        if (pixeladdr < BUFFER_SIZE) {                        \
                            ColorBuffer[pixeladdr + BUFFER_SIZE] = ColorBuffer[pixeladdr]; \
                            DepthBuffer[pixeladdr + BUFFER_SIZE] = DepthBuffer[pixeladdr]; \
                            AttrBuffer[pixeladdr + BUFFER_SIZE]  = AttrBuffer[pixeladdr];  \
                        }                                                     \
                    }                                                         \
                } else if (aa_on) {                                           \
                    s32 cov = ((cov_mode) == 1) ? S.l_edgecov : S.r_edgecov;  \
                    if (cov & (1 << 31)) {                                    \
                        if ((cov_mode) == 1) {                                \
                            cov = xcov >> 5;                                  \
                            if (cov > 31) cov = 31;                           \
                            xcov += (S.l_edgecov & 0x3FF);                    \
                        } else {                                              \
                            cov = 0x1F - (xcov >> 5);                         \
                            if (cov < 0) cov = 0;                             \
                            xcov += (S.r_edgecov & 0x3FF);                    \
                        }                                                     \
                    }                                                         \
                    attr |= ((u32)cov << 8);                                  \
                                                                              \
                    if (pixeladdr < BUFFER_SIZE) {                            \
                        ColorBuffer[pixeladdr + BUFFER_SIZE] = ColorBuffer[pixeladdr]; \
                        DepthBuffer[pixeladdr + BUFFER_SIZE] = DepthBuffer[pixeladdr]; \
                        AttrBuffer[pixeladdr + BUFFER_SIZE]  = AttrBuffer[pixeladdr];  \
                    }                                                         \
                }                                                             \
                                                                              \
                DepthBuffer[pixeladdr] = (u32)z;                              \
                ColorBuffer[pixeladdr] = color;                               \
                AttrBuffer[pixeladdr]  = attr;                                \
            } else {                                                          \
                u32 zz = keep_z ? (u32)z : (u32)-1;                           \
                plot_translucent_pixel(pixeladdr, color, zz, polyattr,        \
                                       is_shadow);                            \
                                                                              \
                if ((dstattr & 0xF) && (pixeladdr < BUFFER_SIZE))             \
                    plot_translucent_pixel(pixeladdr + BUFFER_SIZE, color, zz,\
                                           polyattr, is_shadow);              \
            }                                                                 \
        }

    /* part 1: left edge */
    edge = yedge | 0x1;
    xlimit = S.xstart + S.l_edgelen;
    if (xlimit > S.xend + 1) xlimit = S.xend + 1;
    if (xlimit > RW) xlimit = RW;
    if (S.l_edgecov & (1 << 31)) {
        xcov = (S.l_edgecov >> 12) & 0x3FF;
        if (xcov == 0x3FF) xcov = 0;
    }
    if (!S.l_filledge) x = xlimit;
    else POLY_SPAN(1)

    /* part 2: inside */
    edge = yedge;
    xlimit = S.xend - S.r_edgelen + 1;
    if (xlimit > S.xend + 1) xlimit = S.xend + 1;
    if (xlimit > RW) xlimit = RW;
    if (wireframe && !edge) {
        if (x < xlimit) x = xlimit;
    } else POLY_SPAN(0)

    /* part 3: right edge */
    edge = yedge | 0x2;
    xlimit = S.xend + 1;
    if (xlimit > RW) xlimit = RW;
    if (S.r_edgecov & (1 << 31)) {
        xcov = (S.r_edgecov >> 12) & 0x3FF;
        if (xcov == 0x3FF) xcov = 0;
    }
    if (S.r_filledge) POLY_SPAN(2)

#undef POLY_SPAN

    {
        s32 sx = S.xstart < 0 ? 0 : S.xstart;
        s32 ex = S.xend + 1 > RW ? RW : S.xend + 1;

        /* This span's clamped bounds join the row's drawn extent, which is
         * all the final pass will walk, accumulated in the caller's locals
         * and stored once per row. Span grain on purpose: a pixel the depth
         * or alpha test rejected still widens the row, and costs the final
         * pass one attr load and a skip, the same skip it always took. */
        if (sx < ex) {
            if (sx < *extmin) *extmin = sx;
            if (ex > *extmax) *extmax = ex;
        }

        /* The survey's span-grain rows, counted OUTSIDE the pixel loop with
         * the span's own setup: the interpolator divides once per iterated
         * pixel of a span that is perspective-correct or W-buffered
         * (interp_set_x), and an AA edge pixel is a six-word push-down
         * candidate. Approximate at span grain on purpose, a wireframe
         * interior or an alpha-rejected pixel still counts, because
         * touching POLY_SPAN would perturb the loop this exists to price. */
        if (SurveyOn) {
            Sv3dBand *B = &SvBand[slice];
            s32 spanw = ex - sx;

            B->spans++;
            if (spanw > 0 && (!interpX.linear || polygon->WBuffer)) {
                B->nonlinear++;
                B->div_px += (unsigned long long)spanw;
            }
            if (aa_on && spanw > 0) {
                s32 aew = S.l_edgelen + S.r_edgelen;

                if (aew > spanw) aew = spanw;
                if (aew > 0) B->aaedge_px += (unsigned long long)aew;
            }
        }
    }

    rp->XL = slope_step(&rp->SlopeL);
    rp->XR = slope_step(&rp->SlopeR);
}

/* The dispatch, per polygon-scanline: the class test is five compares against
 * the shade the frame latched, and the fast class was measured at every
 * polygon this game draws on the protocol's scenes. The palette formats are
 * the ones whose texture alpha is only ever 0 or 31, a3i5/a5i3 carry real
 * per-pixel translucency and stay on the general copy. */
static void render_polygon_scanline(RendererPolygon *rp, s32 y, int slice,
                                    s32 *extmin, s32 *extmax)
{
    const PolyShade *sh = &rp->Shade;

    if (sh->blendmode == 0 && sh->textured && sh->polyalpha == 31
        && !rp->PolyData->IsShadow
        && (sh->fmt == 2 || sh->fmt == 3 || sh->fmt == 4)) {
        render_polygon_scanline_body(rp, y, slice, extmin, extmax, 1);
    } else {
        render_polygon_scanline_body(rp, y, slice, extmin, extmax, 0);
    }
}

static void render_scanline(RendererPolygon *list, int npolys, s32 y, int slice)
{
    s32 extmin = RW, extmax = 0;
    int i;
    for (i = 0; i < npolys; i++) {
        RendererPolygon *rp = &list[i];
        PcGxPolygon *polygon = rp->PolyData;

        if (y >= polygon->YTop &&
            (y < polygon->YBottom ||
             (y == polygon->YTop && polygon->YBottom == polygon->YTop))) {
            if (polygon->IsShadowMask) render_shadow_mask_scanline(rp, y);
            else                       render_polygon_scanline(rp, y, slice,
                                                               &extmin,
                                                               &extmax);
        }
    }
    /* the row is done; its extent is final. Every row of the frame passes
     * through here exactly once, so this store is also the reset. */
    RowExt[y] = ((u32)extmax << 16) | (u32)extmin;
}

/* ------------------------------------------------------------------ */
/* The final pass                                                      */
/* ------------------------------------------------------------------ */

static u32 calculate_fog_density(u32 pixeladdr)
{
    u32 z = DepthBuffer[pixeladdr];
    u32 densityid, densityfrac, density;

    if (z < R->FogOffset) {
        densityid = 0;
        densityfrac = 0;
    } else {
        /* the Z difference is shifted right by two then left by the fog shift;
         * bits 0-16 are the fraction and 17-31 the density index. On hardware
         * a big enough shift overflows 32 bits and the fog wraps around, which
         * is reproduced by leaving this in u32. */
        z -= R->FogOffset;
        z = (z >> 2) << R->FogShift;

        densityid = z >> 17;
        if (densityid >= 32) {
            densityid = 32;
            densityfrac = 0;
        } else {
            densityfrac = z & 0x1FFFF;
        }
    }

    density = ((R->FogDensityTable[densityid] * (0x20000 - densityfrac)) +
               (R->FogDensityTable[densityid + 1] * densityfrac)) >> 17;
    if (density >= 127) density = 128;

    return density;
}

/*
 * The full-width form, kept for the frames fog reaches: fog is applied by
 * attribute bit 15, which the CLEAR writes too (ClearAttr1 bit 15, or the
 * clear image's depth-and-fog map), so a fogged frame can blend pixels no
 * span ever drew and the drawn extent is not a bound on the pass. Edge
 * marking and anti-aliasing gate on bits 0-3, which only a span store sets,
 * which is what lets scanline_final_pass below trim them to the extent on
 * every unfogged frame, and this game never sets DISP3DCNT's fog enable
 * once, so this body is the cold path in practice while
 * staying the letter of the hardware.
 */
static void scanline_final_pass_full(s32 y, unsigned long *edgecount,
                                     unsigned long *aacount,
                                     unsigned long *fogcount)
{
    int x;

    if (R->DispCnt & (1 << 5)) {
        /* edge marking, on the topmost pixels only */
        for (x = 0; x < RW; x++) {
            u32 pixeladdr = FIRST_PIXEL_OFF + (y * SCANLINE_W) + x;
            u32 attr = AttrBuffer[pixeladdr];
            u32 polyid, z;

            if (!(attr & 0xF)) continue;

            polyid = attr >> 24;        /* opaque IDs are used for edge marking */
            z = DepthBuffer[pixeladdr];

            if (((polyid != (AttrBuffer[pixeladdr - 1] >> 24)) &&
                 (z < DepthBuffer[pixeladdr - 1])) ||
                ((polyid != (AttrBuffer[pixeladdr + 1] >> 24)) &&
                 (z < DepthBuffer[pixeladdr + 1])) ||
                ((polyid != (AttrBuffer[pixeladdr - SCANLINE_W] >> 24)) &&
                 (z < DepthBuffer[pixeladdr - SCANLINE_W])) ||
                ((polyid != (AttrBuffer[pixeladdr + SCANLINE_W] >> 24)) &&
                 (z < DepthBuffer[pixeladdr + SCANLINE_W]))) {
                u16 edgecolor = R->EdgeTable[polyid >> 3];
                u32 edgeR = (edgecolor << 1) & 0x3E; if (edgeR) edgeR++;
                u32 edgeG = (edgecolor >> 4) & 0x3E; if (edgeG) edgeG++;
                u32 edgeB = (edgecolor >> 9) & 0x3E; if (edgeB) edgeB++;

                ColorBuffer[pixeladdr] = edgeR | (edgeG << 8) | (edgeB << 16) |
                                         (ColorBuffer[pixeladdr] & 0xFF000000);
                (*edgecount)++;

                /* break the AA coverage */
                AttrBuffer[pixeladdr] = (AttrBuffer[pixeladdr] & 0xFFFFE0FF) | 0x00001000;
            }
        }
    }

    if (R->DispCnt & (1 << 7)) {
        /*
         * Fog. The step is 0x80000 >> shift, i.e. GBAtek's depth values times
         * 0x200 to reach Z-buffer values. It is applied to the topmost two
         * pixels, which anti-aliasing needs.
         */
        int fogcolor = !(R->DispCnt & (1 << 6));
        u32 fogR = (R->FogColor << 1) & 0x3E; if (fogR) fogR++;
        u32 fogG = (R->FogColor >> 4) & 0x3E; if (fogG) fogG++;
        u32 fogB = (R->FogColor >> 9) & 0x3E; if (fogB) fogB++;
        u32 fogA = (R->FogColor >> 16) & 0x1F;

        for (x = 0; x < RW; x++) {
            u32 pixeladdr = FIRST_PIXEL_OFF + (y * SCANLINE_W) + x;
            u32 density, srccolor, srcR, srcG, srcB, srcA;
            u32 attr = AttrBuffer[pixeladdr];

            if (attr & (1 << 15)) {
                density = calculate_fog_density(pixeladdr);
                (*fogcount)++;

                srccolor = ColorBuffer[pixeladdr];
                srcR = srccolor & 0x3F;
                srcG = (srccolor >> 8) & 0x3F;
                srcB = (srccolor >> 16) & 0x3F;
                srcA = (srccolor >> 24) & 0x1F;

                if (fogcolor) {
                    srcR = ((fogR * density) + (srcR * (128 - density))) >> 7;
                    srcG = ((fogG * density) + (srcG * (128 - density))) >> 7;
                    srcB = ((fogB * density) + (srcB * (128 - density))) >> 7;
                }
                srcA = ((fogA * density) + (srcA * (128 - density))) >> 7;

                ColorBuffer[pixeladdr] = srcR | (srcG << 8) | (srcB << 16) | (srcA << 24);
            }

            /* The pixel underneath */
            if (!(attr & 0xF)) continue;
            pixeladdr += BUFFER_SIZE;

            attr = AttrBuffer[pixeladdr];
            if (!(attr & (1 << 15))) continue;

            density = calculate_fog_density(pixeladdr);

            srccolor = ColorBuffer[pixeladdr];
            srcR = srccolor & 0x3F;
            srcG = (srccolor >> 8) & 0x3F;
            srcB = (srccolor >> 16) & 0x3F;
            srcA = (srccolor >> 24) & 0x1F;

            if (fogcolor) {
                srcR = ((fogR * density) + (srcR * (128 - density))) >> 7;
                srcG = ((fogG * density) + (srcG * (128 - density))) >> 7;
                srcB = ((fogB * density) + (srcB * (128 - density))) >> 7;
            }
            srcA = ((fogA * density) + (srcA * (128 - density))) >> 7;

            ColorBuffer[pixeladdr] = srcR | (srcG << 8) | (srcB << 16) | (srcA << 24);
        }
    }

    if (R->DispCnt & (1 << 4)) {
        /* anti-aliasing: the edges were flagged and their coverage computed
         * while rendering; this is where they blend with what is underneath */
        for (x = 0; x < RW; x++) {
            u32 pixeladdr = FIRST_PIXEL_OFF + (y * SCANLINE_W) + x;
            u32 attr = AttrBuffer[pixeladdr];
            u32 coverage, topcolor, botcolor;
            u32 topR, topG, topB, topA, botR, botG, botB, botA;

            if (!(attr & 0xF)) continue;

            coverage = (attr >> 8) & 0x1F;
            if (coverage == 0x1F) continue;

            (*aacount)++;
            if (coverage == 0) {
                ColorBuffer[pixeladdr] = ColorBuffer[pixeladdr + BUFFER_SIZE];
                continue;
            }

            topcolor = ColorBuffer[pixeladdr];
            topR = topcolor & 0x3F;
            topG = (topcolor >> 8) & 0x3F;
            topB = (topcolor >> 16) & 0x3F;
            topA = (topcolor >> 24) & 0x1F;

            botcolor = ColorBuffer[pixeladdr + BUFFER_SIZE];
            botR = botcolor & 0x3F;
            botG = (botcolor >> 8) & 0x3F;
            botB = (botcolor >> 16) & 0x3F;
            botA = (botcolor >> 24) & 0x1F;

            coverage++;

            /* The colour is only blended when the bottom pixel is not fully
             * transparent; the alpha always is */
            if (botA > 0) {
                topR = ((topR * coverage) + (botR * (32 - coverage))) >> 5;
                topG = ((topG * coverage) + (botG * (32 - coverage))) >> 5;
                topB = ((topB * coverage) + (botB * (32 - coverage))) >> 5;
            }
            topA = ((topA * coverage) + (botA * (32 - coverage))) >> 5;

            ColorBuffer[pixeladdr] = topR | (topG << 8) | (topB << 16) | (topA << 24);
        }
    }
}

/*
 * The pass the game actually runs: the same two walks, each bounded to the
 * row's drawn extent. On a row nothing drew, a fifth of every protocol
 * run's frames draw no polygon at all; both walks vanish; on a drawn
 * row they skip the margins no span reached, which is also where the wide
 * enhancement's 86 extra columns per side will sit when a scene letterboxes.
 *
 * NOT a single folded walk, though the passes fuse exactly (neither reads
 * what the other writes anywhere but the pixel in hand): that was built and
 * measured 2026-08-27, battle station, --hd3d 3, interleaved same-block
 * triples against this shape and HEAD, and the fused loop cost the 3D
 * span a consistent 3% at default threads where these two loops matched
 * HEAD to 0.5%. Half the attr loads, and slower: the -m32 hot skip path
 * turned a fall-through-plus-short-back branch into two far taken branches
 * across a body too big to keep tight. The extent bound is the part of the
 * idea that survived its own measurement.
 */
static void scanline_final_pass(s32 y, unsigned long *edgecount,
                                unsigned long *aacount,
                                unsigned long *fogcount)
{
    int edge_on, aa_on;
    s32 x, x0, x1;

    if (R->DispCnt & (1 << 7)) {
        scanline_final_pass_full(y, edgecount, aacount, fogcount);
        return;
    }

    edge_on = (R->DispCnt & (1 << 5)) != 0;
    aa_on   = (R->DispCnt & (1 << 4)) != 0;
    if (!edge_on && !aa_on) return;

    x0 = (s32)(RowExt[y] & 0xFFFF);
    x1 = (s32)(RowExt[y] >> 16);

    if (edge_on)
    for (x = x0; x < x1; x++) {
        u32 pixeladdr = FIRST_PIXEL_OFF + (y * SCANLINE_W) + x;
        u32 attr = AttrBuffer[pixeladdr];

        if (!(attr & 0xF)) continue;

        {
            u32 polyid = attr >> 24;    /* opaque IDs mark edges */
            u32 z = DepthBuffer[pixeladdr];

            if (((polyid != (AttrBuffer[pixeladdr - 1] >> 24)) &&
                 (z < DepthBuffer[pixeladdr - 1])) ||
                ((polyid != (AttrBuffer[pixeladdr + 1] >> 24)) &&
                 (z < DepthBuffer[pixeladdr + 1])) ||
                ((polyid != (AttrBuffer[pixeladdr - SCANLINE_W] >> 24)) &&
                 (z < DepthBuffer[pixeladdr - SCANLINE_W])) ||
                ((polyid != (AttrBuffer[pixeladdr + SCANLINE_W] >> 24)) &&
                 (z < DepthBuffer[pixeladdr + SCANLINE_W]))) {
                u16 edgecolor = R->EdgeTable[polyid >> 3];
                u32 edgeR = (edgecolor << 1) & 0x3E; if (edgeR) edgeR++;
                u32 edgeG = (edgecolor >> 4) & 0x3E; if (edgeG) edgeG++;
                u32 edgeB = (edgecolor >> 9) & 0x3E; if (edgeB) edgeB++;

                ColorBuffer[pixeladdr] = edgeR | (edgeG << 8) | (edgeB << 16) |
                                         (ColorBuffer[pixeladdr] & 0xFF000000);
                (*edgecount)++;

                /* break the AA coverage */
                attr = (attr & 0xFFFFE0FF) | 0x00001000;
                AttrBuffer[pixeladdr] = attr;
            }
        }
    }

    if (!aa_on) return;

    for (x = x0; x < x1; x++) {
        u32 pixeladdr = FIRST_PIXEL_OFF + (y * SCANLINE_W) + x;
        u32 attr = AttrBuffer[pixeladdr];
        u32 coverage, topcolor, botcolor;
        u32 topR, topG, topB, topA, botR, botG, botB, botA;

        if (!(attr & 0xF)) continue;

        coverage = (attr >> 8) & 0x1F;
        if (coverage == 0x1F) continue;

        (*aacount)++;
        if (coverage == 0) {
            ColorBuffer[pixeladdr] = ColorBuffer[pixeladdr + BUFFER_SIZE];
            continue;
        }

        topcolor = ColorBuffer[pixeladdr];
        topR = topcolor & 0x3F;
        topG = (topcolor >> 8) & 0x3F;
        topB = (topcolor >> 16) & 0x3F;
        topA = (topcolor >> 24) & 0x1F;

        botcolor = ColorBuffer[pixeladdr + BUFFER_SIZE];
        botR = botcolor & 0x3F;
        botG = (botcolor >> 8) & 0x3F;
        botB = (botcolor >> 16) & 0x3F;
        botA = (botcolor >> 24) & 0x1F;

        coverage++;

        /* The colour is only blended when the bottom pixel is not fully
         * transparent; the alpha always is */
        if (botA > 0) {
            topR = ((topR * coverage) + (botR * (32 - coverage))) >> 5;
            topG = ((topG * coverage) + (botG * (32 - coverage))) >> 5;
            topB = ((topB * coverage) + (botB * (32 - coverage))) >> 5;
        }
        topA = ((topA * coverage) + (botA * (32 - coverage))) >> 5;

        ColorBuffer[pixeladdr] = topR | (topG << 8) | (topB << 16) | (topA << 24);
    }
}

/* ------------------------------------------------------------------ */
/* The frame                                                           */
/* ------------------------------------------------------------------ */

/*
 * The clear is per band, not a pass of its own: it writes three buffers over
 * every pixel of the frame (12 MB at the largest internal resolution) and
 * a serial clear was a fixed eighth of the render whatever the machine had.
 * Row y's clear is read by nothing but row y until the whole frame is drawn,
 * so it belongs to whichever thread owns that row. The screen borders are the
 * two edges no band owns; the first and last band take them.
 */
static void clear_rows(s32 y0, s32 y1)
{
    u32 clearz = ((R->ClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;
    u32 polyid = R->ClearAttr1 & 0x3F000000;   /* the opaque polygon ID */
    s32 x, y;

    /* The screen borders, which is what edge marking reads off the edges */
    if (y0 == 0) {
        for (x = 0; x < SCANLINE_W; x++) {
            ColorBuffer[x] = 0;
            DepthBuffer[x] = clearz;
            AttrBuffer[x]  = polyid;
        }
    }
    if (y1 == RH && y0 < y1) {
        for (x = SCANLINE_W * (RH + 1); x < SCANLINE_W * (RH + 2); x++) {
            ColorBuffer[x] = 0;
            DepthBuffer[x] = clearz;
            AttrBuffer[x]  = polyid;
        }
    }
    for (y = y0; y < y1; y++) {
        u32 e = (u32)SCANLINE_W * (u32)(y + 1);

        ColorBuffer[e] = 0;
        DepthBuffer[e] = clearz;
        AttrBuffer[e]  = polyid;
        ColorBuffer[e + (u32)RW + 1] = 0;
        DepthBuffer[e + (u32)RW + 1] = clearz;
        AttrBuffer[e + (u32)RW + 1]  = polyid;
    }

    if (R->DispCnt & (1 << 14)) {
        /*
         * The clear image: a 256x256 bitmap in texture slot 2 and a 256x256
         * depth-and-fog map in slot 3, scrolled by the two bytes above
         * CLEAR_DEPTH. The offsets are bytes and wrap on their own, which is
         * what makes the image tile; there is no clamp here and adding one
         * would be a plausible wrong picture. X does not reset at the start of
         * a line: it carries on from where the line above left it, so a band
         * that does not start at row 0 works out where that is.
         */
        u32 xoff0 = (R->ClearAttr2 >> 16) & 0xFF;
        u32 yoff0 = (R->ClearAttr2 >> 24) & 0xFF;

        for (y = y0; y < y1; y++) {
            u32 xoff = (xoff0 + (u32)y * (u32)RW) & 0xFF;
            u32 yoff = (yoff0 + (u32)y) & 0xFF;
            u32 row = FIRST_PIXEL_OFF + (u32)y * SCANLINE_W;

            for (x = 0; x < RW; x++) {
                u32 val2 = tex_read16(0x40000 + (yoff << 9) + (xoff << 1));
                u32 val3 = tex_read16(0x60000 + (yoff << 9) + (xoff << 1));
                u32 r = (val2 << 1) & 0x3E; if (r) r++;
                u32 g = (val2 >> 4) & 0x3E; if (g) g++;
                u32 b = (val2 >> 9) & 0x3E; if (b) b++;
                u32 a = (val2 & 0x8000) ? 0x1F000000u : 0u;
                u32 pixeladdr = row + (u32)x;

                ColorBuffer[pixeladdr] = r | (g << 8) | (b << 16) | a;
                DepthBuffer[pixeladdr] = ((val3 & 0x7FFF) * 0x200) + 0x1FF;
                AttrBuffer[pixeladdr]  = polyid | (val3 & 0x8000);

                xoff = (xoff + 1) & 0xFF;
            }
        }
    } else {
        u32 r = (R->ClearAttr1 << 1) & 0x3E; if (r) r++;
        u32 g = (R->ClearAttr1 >> 4) & 0x3E; if (g) g++;
        u32 b = (R->ClearAttr1 >> 9) & 0x3E; if (b) b++;
        u32 a = (R->ClearAttr1 >> 16) & 0x1F;
        u32 color = r | (g << 8) | (b << 16) | (a << 24);

        polyid |= (R->ClearAttr1 & 0x8000);   /* the fog flag */

        for (y = y0; y < y1; y++) {
            u32 row = FIRST_PIXEL_OFF + (u32)y * SCANLINE_W;

            for (x = 0; x < RW; x++) {
                ColorBuffer[row + (u32)x] = color;
                DepthBuffer[row + (u32)x] = clearz;
                AttrBuffer[row + (u32)x]  = polyid;
            }
        }
    }
}

/*
 * Bands: one frame across several cores.
 *
 * A scanline's pixels are written by that scanline alone, so cutting the
 * screen into bands of rows and giving each band to a thread writes disjoint
 * memory, and the picture cannot depend on how many bands there were.
 *
 * What is not per-row is the edge walker: PolygonList[i] carries the two
 * slopes forward one scanline at a time, so a band that does not start at the
 * polygon's top needs that state at its own first row. It is seekable, because
 * slope_setup() takes the scanline as an argument, so each band seeds a
 * private copy and no band walks another's rows.
 *
 * The final pass is a second job, not the tail of the first. Edge marking
 * reads the four neighbours of every pixel, two of which are in other bands,
 * so every row has to be drawn before any row is marked. Within the pass the
 * rows are independent.
 *
 * Shadow polygons go down the serial path: the stencil is two rows indexed by
 * y & 1 and a mask deliberately keeps the previous scanline's contents, which
 * is state that crosses a band boundary. This game draws no shadow volume at
 * all, so it costs nothing measurable.
 */
typedef struct RenderJob {
    RendererPolygon *band[PC_WORKERS_MAX];
    int              nband[PC_WORKERS_MAX];
    unsigned long    edge[PC_WORKERS_MAX];
    unsigned long    aa[PC_WORKERS_MAX];
    unsigned long    fog[PC_WORKERS_MAX];
    int              npolys;
} RenderJob;

/* Grown once and kept: a frame may not allocate on the drawing path. */
static RendererPolygon *BandStore[PC_WORKERS_MAX];
static int              BandStoreCap[PC_WORKERS_MAX];

/*
 * Where the bands are cut: nowhere in advance. The precut answer, equal
 * cost per slice, measured in bounding-box row coverage, balanced the
 * frame across cores that were all the same speed, and the handheld's are
 * not: four big cores and four little ones, and a precut band that lands on
 * a little core finishes last and decides the frame. So rows are handed out
 * in small chunks from a shared cursor instead, and a fast core simply comes
 * back for more while a slow one is still on its first, balanced against
 * uneven rows AND uneven cores, with nothing measured and nothing guessed.
 *
 * A chunk is a scheduling decision and not a rendering one: the picture is
 * the same wherever the chunk edges land, which is what makes this safe.
 * The chunk is a few per thread so the tail is short, and never under eight
 * rows so the per-chunk seed walk stays a rounding error.
 */
static volatile s32 SpanCursor;
static volatile s32 FinalCursor;
static s32          ChunkRows;

static void bands_open(int nslices)
{
    ChunkRows = (s32)(RH / (nslices * 4));
    if (ChunkRows < 8) ChunkRows = 8;
    SpanCursor = 0;
    FinalCursor = 0;
}

static int bands_reserve(int nslices, int npolys)
{
    int b;

    for (b = 0; b < nslices; b++) {
        if (BandStoreCap[b] >= npolys) continue;
        {
            RendererPolygon *p = (RendererPolygon *)realloc(
                BandStore[b], (size_t)npolys * sizeof *p);

            if (p == NULL) return 0;
            BandStore[b] = p;
            BandStoreCap[b] = npolys;
        }
    }
    return 1;
}

/* The polygons this band's rows can touch, in the list's own order, each one
 * wound forward to the band's first scanline. */
static int band_seed(RendererPolygon *dst, int npolys, s32 y0, s32 y1)
{
    int i, n = 0;

    for (i = 0; i < npolys; i++) {
        PcGxPolygon *polygon = PolygonList[i].PolyData;

        if (polygon->YTop >= y1) continue;
        if (polygon->YTop == polygon->YBottom) {
            /* A polygon with no height draws on its top scanline alone. */
            if (polygon->YTop < y0) continue;
        } else if (polygon->YBottom <= y0) {
            continue;
        }

        dst[n] = PolygonList[i];
        if (polygon->YTop != polygon->YBottom && y0 > polygon->YTop) {
            setup_polygon_left_edge(&dst[n], y0);
            setup_polygon_right_edge(&dst[n], y0);
        }
        n++;
    }
    return n;
}

/*
 * ONE dispatch, three phases. Waking the pool and waiting for it costs about
 * half a band, so the clear, the spans and the final pass ride the same job
 * and the only thing between them is a spun barrier. There is exactly one:
 * The clear a row needs is its own, and the final pass a row needs is every
 * row's spans.
 */
static void draw_band(void *ctx, int slice, int nslices)
{
    RenderJob *J = (RenderJob *)ctx;
    RendererPolygon *list = J->band[slice];
    s32 y0, y1, y;
    int n;

    /*
     * EVERY chunk seeds its own copy, into this thread's store. Rendering
     * advances the edge walkers in the records it draws from, so a chunk
     * that drew straight out of PolygonList would be editing what other
     * threads are still copying, a race that shows up as one wrong frame
     * in a few thousand, which is exactly how it was found. PolygonList is
     * the master and is read-only for the length of the job.
     */
    for (;;) {
        y0 = __atomic_fetch_add(&SpanCursor, ChunkRows, __ATOMIC_ACQ_REL);
        if (y0 >= RH) break;
        y1 = y0 + ChunkRows;
        if (y1 > RH) y1 = RH;
        clear_rows(y0, y1);
        n = band_seed(list, J->npolys, y0, y1);
        for (y = y0; y < y1; y++) render_scanline(list, n, y, slice);
    }

    /* A thread reaches this only after finishing every chunk it claimed, so
     * the barrier opening means every claimed row is drawn; which is what
     * the final pass needs of its neighbours. */
    pc_workers_barrier(nslices);

    for (;;) {
        y0 = __atomic_fetch_add(&FinalCursor, ChunkRows, __ATOMIC_ACQ_REL);
        if (y0 >= RH) break;
        y1 = y0 + ChunkRows;
        if (y1 > RH) y1 = RH;
        for (y = y0; y < y1; y++)
            scanline_final_pass(y, &J->edge[slice], &J->aa[slice],
                                &J->fog[slice]);
    }
}

static void render_polygons(PcGxPolygon **polygons, int npolys)
{
    static RenderJob job;
    int i, j = 0, slices, stencilled = 0;
    s32 y;

    for (i = 0; i < npolys; i++) {
        if (polygons[i]->Degenerate) continue;
        if (polygons[i]->IsShadowMask || polygons[i]->IsShadow) stencilled = 1;
        setup_polygon(&PolygonList[j], polygons[i]);
        if (SurveyOn) survey_poly(polygons[i], &PolygonList[j].Shade);
        j++;
    }
    PolysDrawn = (u32)j;
    StencilFrame = stencilled;

    slices = pc_workers_slices();
    if (slices > RH) slices = (int)RH;
    if (slices > 1 && !stencilled && bands_reserve(slices, j)) {
        bands_open(slices);
        job.npolys = j;
        for (i = 0; i < slices; i++) {
            job.band[i] = BandStore[i];
            job.edge[i] = 0;
            job.aa[i] = 0;
            job.fog[i] = 0;
        }
        pc_workers_run_n(draw_band, &job, slices);
        for (i = 0; i < slices; i++) {
            EdgeMarkedPixels += job.edge[i];
            AABlendedPixels += job.aa[i];
            FoggedPixels += job.fog[i];
        }
        return;
    }

    clear_rows(0, RH);
    render_scanline(PolygonList, j, 0, 0);
    for (y = 1; y < RH; y++) {
        render_scanline(PolygonList, j, y, 0);
        scanline_final_pass(y - 1, &EdgeMarkedPixels, &AABlendedPixels,
                            &FoggedPixels);
    }
    scanline_final_pass(RH - 1, &EdgeMarkedPixels, &AABlendedPixels,
                        &FoggedPixels);
}

#if !defined(__3DS__)
/* ------------------------------------------------------------------ */
/* PC_GPU3D_DIFF, the produced picture against the software oracle   */
/* ------------------------------------------------------------------ */

/*
 * The instrument before the renderer: every rendered frame, the picture the
 * selected producer left is diffed against the software rasterizer's own
 * picture of the same latched frame. It exists before the gl renderer does, so
 * the renderer is measured by an instrument that was proved without it. Under
 * the soft producer the diff renders the frame twice and compares the
 * rasterizer with itself, which must be zero everywhere.
 *
 * The claim classes come off the rasterizer's own per-pixel state, because
 * "may these two pictures differ here" has three answers:
 *
 *   exact    nothing but the clear touched the pixel. Byte-equal, always.
 *   blend    the value went through a blend the GPU would do in its own
 *            arithmetic: a translucent polygon, an edge pixel, or a fog
 *            candidate on a frame whose fog enable is set. Within one six-bit
 *            step.
 *   raster   everything else a polygon drew. The GPU's interpolation against
 *            ours is a distribution, reported as gap buckets and dumps, never
 *            a tolerance.
 *
 * Anything past its class's threshold is a bug. The thresholds live in
 * diff_bad() and nowhere else, and the selftest pins them as vectors.
 *
 * Where, not how much: differing pixels accumulate per row and per column over
 * the run, written out as a table at exit beside the pictures. The pictures
 * are a triple, the oracle, the producer and a map of which kind of wrong, for
 * the worst frame so far and a typical recent one.
 *
 * It costs a second render per frame, so it is a mode and never a default.
 */
#define DIFF_WORST_DUMPS 8          /* pictures of the worst frame, at most */
#define DIFF_TYPICAL_EVERY 240u     /* ...and of a recent one, overwriting  */

static int DiffOn;
static const char *DiffDir;         /* the env string, which outlives us    */
static u32 *DiffRunPic;             /* the producer's picture, RW x RH      */

static unsigned long long DiffPairs;    /* frames rendered by both          */
static unsigned long long DiffEqual;    /* ...identical over the whole grid */
static unsigned long long DiffPx;       /* differing pixels, over the run   */
static unsigned long long DiffOnlyRun;  /* the producer drew, soft did not  */
static unsigned long long DiffOnlySoft; /* soft drew, the producer did not  */
static unsigned long long DiffColour;   /* both drew and disagreed          */
/* Colour disagreements by how far apart, in six-bit steps: one step, four,
 * sixteen, further. A ramp neighbour and a wrong object look the same in a
 * total and nothing alike here. */
static unsigned long long DiffGapHist[4];
static unsigned long long DiffGapSum;   /* the one rankable number's numerator */
static int DiffWorstGap;
/* Per class: pixels seen, pixels differing, pixels past the threshold. */
static unsigned long long DiffClassPx[3];
static unsigned long long DiffClassDiff[3];
static unsigned long long DiffClassBad[3];
/* Frames by how much of them differs, as a fraction of the grid, the bins
 * are 0, <=0.01%, 0.1%, 1%, 5%, 10%, 25% and the rest, because a total
 * cannot tell a handful of ruined frames from every frame slightly wrong. */
static unsigned long long DiffFrameHist[8];
static unsigned long long DiffWorstFrame, DiffWorstFramePx;
static unsigned long long DiffLastFrame, DiffLastPx;
static int DiffDumps;
/* WHERE, accumulated over the run. */
static unsigned long long DiffRowPx[RENDER_H_MAX];
static unsigned long long DiffColPx[RENDER_W_MAX];

enum { DIFF_EXACT = 0, DIFF_BLEND = 1, DIFF_RASTER = 2 };

static int diff_class(u32 attr, int x, int fogframe, u32 ext)
{
    /* Blend outranks the extent test: fog blends clear pixels the spans
     * never reached (the full-width final pass exists for exactly them). */
    if ((attr & (1u << 22)) || (attr & 0xFu)
        || (fogframe && (attr & (1u << 15)))) {
        return DIFF_BLEND;
    }
    if (x < (int)(ext & 0xFFFFu) || x >= (int)(ext >> 16)) {
        return DIFF_EXACT;
    }
    return DIFF_RASTER;
}

/*
 * The pixel gap in six-bit steps (alpha counts its own five-bit steps the
 * same way), and whether it is a coverage disagreement. A pixel neither
 * drew is equal whatever its colour bits hold; the compositor's own test
 * is a nonzero alpha, so nothing else is observable. Drawn against not
 * drawn is the whole scale.
 */
static int diff_pixel(u32 s, u32 p, int *cover)
{
    int drewS = (s >> 24) != 0u, drewP = (p >> 24) != 0u;
    int gap = 0, ch, d;

    *cover = 0;
    if (!drewS && !drewP) {
        return 0;
    }
    if (drewS != drewP) {
        *cover = 1;
        return 63;
    }
    for (ch = 0; ch <= 16; ch += 8) {
        d = (int)((s >> ch) & 0x3Fu) - (int)((p >> ch) & 0x3Fu);
        if (d < 0) d = -d;
        if (d > gap) gap = d;
    }
    d = (int)((s >> 24) & 0x1Fu) - (int)((p >> 24) & 0x1Fu);
    if (d < 0) d = -d;
    if (d > gap) gap = d;
    return gap;
}

/* The thresholds, in one place: exact byte-equal, blended one six-bit step,
 * rasterized reported. The selftest below holds these as vectors. */
static int diff_bad(int cls, int gap)
{
    if (cls == DIFF_EXACT) return gap != 0;
    if (cls == DIFF_BLEND) return gap > 1;
    return 0;
}

/*
 * The three pictures of one frame, the 3DS's own scheme: the map is the
 * point and the other two are its caption. Red is the producer drawing
 * where the oracle did not, blue the oracle where the producer did not,
 * green a pixel both drew and disagreed about, brighter the further apart.
 */
static void diff_dump(const char *tag)
{
    static const char *const kWhich[3] = { "soft", "run", "map" };
    int which, x, y;

    for (which = 0; which < 3; which++) {
        char path[512];
        FILE *f;

        snprintf(path, sizeof path, "%s/p3d-%s-%s.ppm", DiffDir, tag,
                 kWhich[which]);
        f = fopen(path, "wb");
        if (f == NULL) {
            return;
        }
        fprintf(f, "P6\n%d %d\n255\n", RW, RH);
        for (y = 0; y < RH; y++) {
            const u32 *soft = &ColorBuffer[FIRST_PIXEL_OFF
                                           + (size_t)y * SCANLINE_W];
            const u32 *run = &DiffRunPic[(size_t)y * (size_t)RW];

            for (x = 0; x < RW; x++) {
                u32 c = which == 0 ? soft[x] : run[x];
                unsigned char rgb[3];

                if (which == 2) {
                    int cover, gap = diff_pixel(soft[x], run[x], &cover);
                    int drewS = (soft[x] >> 24) != 0u;

                    rgb[0] = (unsigned char)(cover && !drewS ? 255 : 0);
                    rgb[2] = (unsigned char)(cover && drewS ? 255 : 0);
                    rgb[1] = (unsigned char)(!cover && gap
                                             ? (gap > 15 ? 255 : 64 + gap * 12)
                                             : 0);
                } else {
                    u32 r = c & 0x3Fu, g = (c >> 8) & 0x3Fu,
                        b = (c >> 16) & 0x3Fu;

                    if ((c >> 24) == 0u) r = g = b = 0;
                    rgb[0] = (unsigned char)((r << 2) | (r >> 4));
                    rgb[1] = (unsigned char)((g << 2) | (g >> 4));
                    rgb[2] = (unsigned char)((b << 2) | (b >> 4));
                }
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
    }
}

/* The oracle's picture is in the buffers, the producer's in DiffRunPic; the
 * classification state (attributes, extents, the latched fog enable) is
 * the oracle's own, read in place. */
static void diff_compare(void)
{
    int fogframe = (R->DispCnt & (1u << 7)) != 0;
    unsigned long long differing = 0;
    unsigned long long grid = (unsigned long long)RW * (unsigned long long)RH;
    int x, y;

    for (y = 0; y < RH; y++) {
        const u32 *soft = &ColorBuffer[FIRST_PIXEL_OFF
                                       + (size_t)y * SCANLINE_W];
        const u32 *attr = &AttrBuffer[FIRST_PIXEL_OFF
                                      + (size_t)y * SCANLINE_W];
        const u32 *run = &DiffRunPic[(size_t)y * (size_t)RW];
        u32 ext = RowExt[y];
        unsigned long long rowdiff = 0;

        for (x = 0; x < RW; x++) {
            int cls = diff_class(attr[x], x, fogframe, ext);
            int cover, gap;

            DiffClassPx[cls]++;
            gap = diff_pixel(soft[x], run[x], &cover);
            if (gap == 0) continue;

            rowdiff++;
            DiffColPx[x]++;
            DiffGapSum += (unsigned long long)gap;
            DiffClassDiff[cls]++;
            if (diff_bad(cls, gap)) DiffClassBad[cls]++;
            if (cover) {
                if ((run[x] >> 24) != 0u) DiffOnlyRun++;
                else DiffOnlySoft++;
            } else {
                DiffColour++;
                DiffGapHist[gap <= 1 ? 0 : gap <= 4 ? 1 : gap <= 16 ? 2 : 3]++;
                if (gap > DiffWorstGap) DiffWorstGap = gap;
            }
        }
        DiffRowPx[y] += rowdiff;
        differing += rowdiff;
    }

    DiffPairs++;
    DiffPx += differing;
    DiffFrameHist[differing == 0 ? 0
                  : differing * 10000u <= grid ? 1
                  : differing * 1000u <= grid ? 2
                  : differing * 100u <= grid ? 3
                  : differing * 20u <= grid ? 4
                  : differing * 10u <= grid ? 5
                  : differing * 4u <= grid ? 6 : 7]++;
    if ((DiffPairs % DIFF_TYPICAL_EVERY) == 0u) {
        DiffLastFrame = DiffPairs;
        DiffLastPx = differing;
        diff_dump("last");
    }
    if (differing == 0) {
        DiffEqual++;
        return;
    }
    /* Pictures come out on the frame that beats the record, a handful of
     * times: a decision wants the worst case, not a write per second. */
    if (differing > DiffWorstFramePx) {
        DiffWorstFramePx = differing;
        DiffWorstFrame = DiffPairs;
        if (DiffDumps < DIFF_WORST_DUMPS) {
            DiffDumps++;
            diff_dump("worst");
        }
    }
}

static void diff_frame(PcGxPolygon **polys, int npolys)
{
    unsigned long em, aa, fg;
    int sv, y;

    if (DiffRunPic == NULL) {
        DiffRunPic = (u32 *)malloc((size_t)RW * (size_t)RH
                                   * sizeof *DiffRunPic);
        if (DiffRunPic == NULL) {
            DiffOn = 0;
            fprintf(stderr, "pc-gpu3d-diff: no room for the run's picture"
                            " (%d x %d); the diff is off\n", RW, RH);
            return;
        }
    }

    /* 1. Keep the picture the selected producer left. */
    for (y = 0; y < RH; y++) {
        memcpy(&DiffRunPic[(size_t)y * (size_t)RW],
               &ColorBuffer[FIRST_PIXEL_OFF + (size_t)y * SCANLINE_W],
               (size_t)RW * sizeof *DiffRunPic);
    }

    /*
     * 2. The oracle renders the same latch again. The survey and the final-
     * pass counters would double off the second walk, so both are held
     * across it; the run's numbers stay the run's.
     */
    sv = SurveyOn;
    SurveyOn = 0;
    em = EdgeMarkedPixels;
    aa = AABlendedPixels;
    fg = FoggedPixels;
    render_polygons(polys, npolys);
    EdgeMarkedPixels = em;
    AABlendedPixels = aa;
    FoggedPixels = fg;
    SurveyOn = sv;

    /* 3. Judge, then 4. put the producer's picture back where a producer
     * other than soft answered; the run is playing that producer, and
     * under soft the re-render just proved itself byte-identical (or the
     * counters are about to say it did not, which is the null test doing
     * its job). */
    diff_compare();
    if (pc_gpu3d_gl_active()) {
        for (y = 0; y < RH; y++) {
            memcpy(&ColorBuffer[FIRST_PIXEL_OFF + (size_t)y * SCANLINE_W],
                   &DiffRunPic[(size_t)y * (size_t)RW],
                   (size_t)RW * sizeof *DiffRunPic);
        }
    }
}

static void diff_report(void)
{
    unsigned long long grid = (unsigned long long)RW * (unsigned long long)RH;
    unsigned long long bad = DiffClassBad[DIFF_EXACT]
                           + DiffClassBad[DIFF_BLEND];
    char path[512];
    FILE *f;
    int i;

    fprintf(stderr, "\npc-gpu3d-diff: producer %s; pairs %llu identical %llu"
            " differing-px %llu of %llu\n",
            pc_gpu3d_gl_active() ? "gl" : "soft (the null test)",
            DiffPairs, DiffEqual, DiffPx, DiffPairs * grid);
    fprintf(stderr, "pc-gpu3d-diff: class exact px %llu diff %llu BAD %llu;"
            " blend px %llu diff %llu BAD %llu; raster px %llu diff %llu\n",
            DiffClassPx[DIFF_EXACT], DiffClassDiff[DIFF_EXACT],
            DiffClassBad[DIFF_EXACT],
            DiffClassPx[DIFF_BLEND], DiffClassDiff[DIFF_BLEND],
            DiffClassBad[DIFF_BLEND],
            DiffClassPx[DIFF_RASTER], DiffClassDiff[DIFF_RASTER]);
    fprintf(stderr, "pc-gpu3d-diff: cover only-run %llu only-soft %llu;"
            " colour %llu step %llu near %llu far %llu wild %llu worst %d\n",
            DiffOnlyRun, DiffOnlySoft, DiffColour, DiffGapHist[0],
            DiffGapHist[1], DiffGapHist[2], DiffGapHist[3], DiffWorstGap);
    /* The one number two renderings can be ranked by: the mean gap per
     * pixel of surface, identical pixels included. */
    fprintf(stderr, "pc-gpu3d-diff: mean-gap %.6f; frames none %llu u0.01%%"
            " %llu u0.1%% %llu u1%% %llu u5%% %llu u10%% %llu u25%% %llu"
            " more %llu\n",
            DiffPairs ? (double)DiffGapSum / ((double)DiffPairs * (double)grid)
                      : 0.0,
            DiffFrameHist[0], DiffFrameHist[1], DiffFrameHist[2],
            DiffFrameHist[3], DiffFrameHist[4], DiffFrameHist[5],
            DiffFrameHist[6], DiffFrameHist[7]);
    fprintf(stderr, "pc-gpu3d-diff: worst frame %llu pixels %llu dumps %d;"
            " last frame %llu pixels %llu\n",
            DiffWorstFrame, DiffWorstFramePx, DiffDumps,
            DiffLastFrame, DiffLastPx);
    fprintf(stderr, "pc-gpu3d-diff: %s, %llu pixel(s) past a class"
            " threshold\n", bad == 0 ? "CLEAN" : "BAD", bad);

    snprintf(path, sizeof path, "%s/p3d-where.tsv", DiffDir);
    f = fopen(path, "wb");
    if (f != NULL) {
        for (i = 0; i < RH; i++) {
            if (DiffRowPx[i] != 0)
                fprintf(f, "row\t%d\t%llu\n", i, DiffRowPx[i]);
        }
        for (i = 0; i < RW; i++) {
            if (DiffColPx[i] != 0)
                fprintf(f, "col\t%d\t%llu\n", i, DiffColPx[i]);
        }
        fclose(f);
    }
    fflush(stderr);
}

void pc_gpu3d_soft_diff_init(void)
{
    const char *dir = getenv("PC_GPU3D_DIFF");

    if (dir == NULL || dir[0] == '\0') {
        return;
    }
    DiffDir = dir;
    DiffOn = 1;
    atexit(diff_report);
}

/*
 * The classes and the thresholds as vectors, pinned before any GL code
 * exists: a renderer must not be able to move them by being built. Runs
 * from --selftest; touches nothing but its own arguments.
 */
int pc_gpu3d_diff_selftest(void)
{
    /* Drawn columns [2, 8) of a row. */
    const u32 ext = (8u << 16) | 2u;
    const u32 opaque = 0x0F000000u;      /* an opaque poly id, nothing else */
    int cover, bad = 0;

#define DIFF_CHECK(cond)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "pc-gpu3d-diff selftest: line %d failed\n",      \
                    __LINE__);                                               \
            bad++;                                                           \
        }                                                                    \
    } while (0)

    /* The classes, off the renderer's own attribute bits. */
    DIFF_CHECK(diff_class(opaque, 0, 0, ext) == DIFF_EXACT);   /* margin    */
    DIFF_CHECK(diff_class(opaque, 8, 0, ext) == DIFF_EXACT);   /* max is
                                                                * exclusive */
    DIFF_CHECK(diff_class(opaque, 2, 0, ext) == DIFF_RASTER);  /* drawn     */
    DIFF_CHECK(diff_class(opaque, 7, 0, ext) == DIFF_RASTER);
    DIFF_CHECK(diff_class(opaque | (1u << 22), 4, 0, ext) == DIFF_BLEND);
    DIFF_CHECK(diff_class(opaque | 0x1u, 4, 0, ext) == DIFF_BLEND);
    DIFF_CHECK(diff_class(opaque | (1u << 15), 4, 1, ext) == DIFF_BLEND);
    DIFF_CHECK(diff_class(opaque | (1u << 15), 4, 0, ext) == DIFF_RASTER);
    DIFF_CHECK(diff_class(opaque | (1u << 15), 0, 1, ext) == DIFF_BLEND);
    DIFF_CHECK(diff_class(opaque | (1u << 15), 0, 0, ext) == DIFF_EXACT);

    /* The gap, in six-bit steps, coverage the whole scale, invisible
     * pixels equal whatever their colour bits hold. */
    DIFF_CHECK(diff_pixel(0x1F202122u, 0x1F202122u, &cover) == 0 && !cover);
    DIFF_CHECK(diff_pixel(0x00131415u, 0x00313233u, &cover) == 0 && !cover);
    DIFF_CHECK(diff_pixel(0x1F202122u, 0x00202122u, &cover) == 63 && cover);
    DIFF_CHECK(diff_pixel(0x1F202122u, 0x1F202123u, &cover) == 1 && !cover);
    DIFF_CHECK(diff_pixel(0x1F202122u, 0x1F212122u, &cover) == 1 && !cover);
    DIFF_CHECK(diff_pixel(0x1F202122u, 0x1E202122u, &cover) == 1 && !cover);
    DIFF_CHECK(diff_pixel(0x1F202122u, 0x1F202126u, &cover) == 4 && !cover);

    /* The thresholds: exact byte-equal, blended one six-bit step,
     * rasterized reported and never bad. */
    DIFF_CHECK(!diff_bad(DIFF_EXACT, 0));
    DIFF_CHECK(diff_bad(DIFF_EXACT, 1));
    DIFF_CHECK(!diff_bad(DIFF_BLEND, 0));
    DIFF_CHECK(!diff_bad(DIFF_BLEND, 1));
    DIFF_CHECK(diff_bad(DIFF_BLEND, 2));
    DIFF_CHECK(!diff_bad(DIFF_RASTER, 63));

#undef DIFF_CHECK
    return bad == 0;
}
#endif /* !__3DS__ */

#if !defined(__3DS__)
/*
 * The gl producer's landing strip (pc_gpu3d_gl.h): the same buffers, so
 * every reader, the compositor's native lines, the viewer's wide and
 * high-resolution rows, the diff instrument, serves a gl frame exactly
 * as it serves a soft one, scroll included. Owned here because the
 * buffers are this file's statics, the same reason the gltex oracle is.
 */
uint32_t *pc_gpu3d_gl_row(int y)
{
    return &ColorBuffer[FIRST_PIXEL_OFF + (size_t)y * SCANLINE_W];
}

int pc_gpu3d_gl_grid_w(void)
{
    return RW;
}

int pc_gpu3d_gl_grid_h(void)
{
    return RH;
}
#endif /* !__3DS__ */

/* See pc_gpu3d_soft.h. Off by default, so the desktop port's timing is what it
 * always was. */
static int Deferred;
static int RenderDue;
static int SkipNext;

static void render_frame_now(void);

void pc_gpu3d_soft_reset(void)
{
    memset(ColorBuffer, 0, sizeof(ColorBuffer));
    memset(DepthBuffer, 0, sizeof(DepthBuffer));
    memset(AttrBuffer, 0, sizeof(AttrBuffer));
    memset(StencilBuffer, 0, sizeof(StencilBuffer));
    memset(ScrolledLine, 0, sizeof(ScrolledLine));
    memset(TexSlot, 0, sizeof(TexSlot));
    memset(TexPalSlot, 0, sizeof(TexPalSlot));
    PrevIsShadowMask = 0;
    PolysDrawn = 0;
    RenderDue = 0;
    SkipNext = 0;
}

/*
 * Skip the next frame outright: the vblank's render call becomes a no-op and
 * every reader, the 2D compositor's native lines, the viewer's wide and
 * high-resolution rows, keeps the previous frame's picture. The polygon list
 * the geometry engine published for the skipped frame is simply never drawn;
 * the next unskipped vblank latches and draws its own list as always, so
 * nothing here goes stale for longer than the caller asked for.
 *
 * This exists for the pacer's frame skip (pc_view.c): at --hd3d 2..4 the
 * rasterizer is the frame, measured on the new-game replay at scale 4,
 * 15.4 ms of a 19.3 ms frame, 79.8%, and a machine that cannot afford it
 * every frame used to run the whole simulation slow, which starved the
 * viewer's audio ring (the mixer emits one frame's audio per simulated
 * frame). Dropping the picture's refresh instead keeps the game and its
 * sound at full speed. One-shot by design: every skipped frame is a fresh
 * decision by the pacer, never a mode this file is left in.
 */
void pc_gpu3d_soft_skip_next(void)
{
    SkipNext = 1;
}

void pc_gpu3d_soft_defer(int on)
{
    Deferred = on != 0;
    if (!Deferred && RenderDue) {
        /* Turned off with a frame outstanding: draw it now rather than leave
         * a reader holding the frame before it. */
        RenderDue = 0;
        render_frame_now();
    }
}

static void render_frame_now(void)
{
    PcGxPolygon **polys;
    u32 count = 0;

    R = pc_gpu3d_render_regs();
    polys = pc_gpu3d_render_polygons(&count);

    /*
     * The texture slots are resolved here rather than at the store, for the
     * same reason the rendering registers are: VRAMCNT
     * is plain guest memory with armrec_vram_touch() behind it, and this is
     * the instant upstream makes its flattened copies coherent; the start
     * of the frame's rendering, after the polygon list has been published.
     *
     * Deferred, that instant moves later in the frame and the slots are
     * whatever VRAMCNT says when the first reader asks. The published list and
     * the latched registers do not move with it, the geometry engine swapped
     * banks at that vblank and the registers were latched into a copy, so
     * the only thing that shifts is which texture a bank remapped mid-frame
     * would hand over. Hardware rasterizes during display and reads its
     * textures then, so if anything this is the closer answer; it is written
     * down because it is a difference and not because it is a known fault.
     */
    latch_texture_slots();
    texcache_frame();

#if !defined(__3DS__)
    /*
     * The producer dispatch: whichever is selected
     * answers, from the same latch. A frame the gl producer refuses,
     * shadow polygons, a shading mode, fog, its ceilings, falls through
     * to the rasterizer below, and the refusal is counted in the gl
     * side's own report.
     */
    if (pc_gpu3d_gl_active()) {
        int drawn = pc_gpu3d_gl_frame(polys, (int)count);

        if (drawn >= 0) {
            PolysDrawn = (u32)drawn;
            if (SurveyOn) {
                Sv.frames++;
                if (PolysDrawn > 0) {
                    Sv.polyframes++;
                    if (R->DispCnt & (1 << 5)) Sv.f_edge++;
                    if (R->DispCnt & (1 << 7)) Sv.f_fog++;
                    if (R->DispCnt & (1 << 4)) Sv.f_aa++;
                    if ((R->DispCnt & (1 << 2)) && R->AlphaRef > 0)
                        Sv.f_alpha++;
                }
            }
            if (DiffOn) diff_frame(polys, (int)count);
            return;
        }
    }
#endif

    PolysDrawn = 0;
    render_polygons(polys, (int)count);

    if (SurveyOn) {
        Sv.frames++;
        if (PolysDrawn > 0) {
            Sv.polyframes++;
            if (R->DispCnt & (1 << 5)) Sv.f_edge++;
            if (R->DispCnt & (1 << 7)) Sv.f_fog++;
            if (R->DispCnt & (1 << 4)) Sv.f_aa++;
            if ((R->DispCnt & (1 << 2)) && R->AlphaRef > 0) Sv.f_alpha++;
        }
    }
#if !defined(__3DS__)
    if (DiffOn) diff_frame(polys, (int)count);
#endif
}

void pc_gpu3d_soft_render_frame(void)
{
    if (SkipNext) {
        /* One frame, decided before this vblank; see the setter above. */
        SkipNext = 0;
        return;
    }
    if (Deferred) {
        RenderDue = 1;
        return;
    }
    render_frame_now();
}

/* Every way out of this file goes through here first. One int in the common
 * case, which is the price of not being able to hand out a stale picture. */
static void render_if_due(void)
{
    if (RenderDue) {
        RenderDue = 0;
        render_frame_now();
    }
}

#if defined(__3DS__)
/*
 * Neither enhancement exists on this console; the buffers above are the DS's
 * size, so both setters refuse rather than clamp silently: a width past the
 * maximum would index off the end of every buffer. Said once, because a caller
 * that asks is likely to ask every frame.
 */
static void soft_refused(const char *what)
{
    static int said;

    if (!said) {
        said = 1;
        fprintf(stderr, "pc_gpu3d_soft: %s is not available at this render "
                        "size; staying at 256x192\n", what);
    }
}

void pc_gpu3d_soft_set_width(int w)
{
    if (w != 256) {
        soft_refused("wide rendering");
    }
}

void pc_gpu3d_soft_set_scale(int s)
{
    if (s != 1) {
        soft_refused("high-resolution 3D");
    }
}
#else
void pc_gpu3d_soft_set_width(int w)
{
    if (w < 256) w = 256;
    if (w > (int)PC_VIEW_WIDE_MAX) w = (int)PC_VIEW_WIDE_MAX;
    RW = w * RS;
    RH = 192 * RS;
}

/*
 * True high-resolution 3D: multiply both axes. Called once at startup,
 * after any wide width; the display-resolution size stays RW/RS x RH/RS
 * and pc_gpu3d_soft_line() keeps answering at it.
 *
 * The buffers above are laid out for RENDER_W_MAX x RENDER_H_MAX whatever
 * the scale, so this changes no address arithmetic, only how much of each
 * row and how many rows carry pixels. That is why a scale past 2 cost
 * nothing here beyond the ceiling.
 */
void pc_gpu3d_soft_set_scale(int s)
{
    int disp_w = RW / RS;

    RS = (s >= 2 && s <= PC_GPU3D_HD_MAX) ? s : 1;
    RW = disp_w * RS;
    RH = 192 * RS;
}
#endif

/*
 * THE 3D layer's horizontal scroll, as a signed shift.
 *
 * BG0HOFS is nine bits and the native path below spells its two halves out:
 * below 256 the picture slides left and the tail goes black, at or above 256
 * it slides right and the head does. Both are one rule; the layer sits at
 * -xpos in a 512-wide space and anything outside the visible columns is
 * black, and written that way it generalizes to a row of any width, which
 * is what the wide margins need. Verified against the native path: xpos=10
 * gives raw[i+10] with a black tail, xpos=0x110 gives 240 black columns then
 * raw from 0, which is what the two branches produce.
 */
static int scroll_shift(void)
{
    int xpos = (int)(*(volatile u16 *)G3D_HOST(REG_BG0HOFS) & 0x01FF);

    return xpos < 256 ? xpos : xpos - 512;
}

static const u32 *scroll_row(const u32 *raw, u32 *dst, int w, int step,
                             int shift)
{
    int i;

    if (shift == 0 && step == 1) return raw;
    for (i = 0; i < w; i++) {
        int j = i + shift;

        dst[i] = (j >= 0 && j < w) ? raw[j * step] : 0;
    }
    return dst;
}

/*
 * The full rendered line, RW pixels, what the viewer's wide compose reads
 * for the margins. The 2D compositor keeps reading pc_gpu3d_soft_line()
 * below, which slices the centre 256 out of this, so everything the
 * instruments compare stays the native picture whatever the width.
 *
 * The scroll is applied HERE as well as there. It used to be applied to the
 * centre 256 alone, which left the margins a few pixels out of step, so the
 * compositor blacked them whenever BG0HOFS was nonzero, and the game
 * scrolls this layer during screen shakes, so every shake blinked the
 * margins off. Measured on the recorded replay: 212 of 25,000 frames.
 */
const uint32_t *pc_gpu3d_soft_line_wide(int y, int *w)
{
    int cols = RW / RS;

    render_if_due();
    if (w != NULL) *w = cols;
    if (y < 0 || y >= 192) return ScrolledLine;
    return scroll_row(&ColorBuffer[(y * RS * SCANLINE_W) + FIRST_PIXEL_OFF],
                      ScrolledWide, cols, RS, scroll_shift());
}

/*
 * The full-resolution row for the viewer's high-definition compose: `hy` is
 * a rendered row (0 .. 192*scale), `w` its pixel count, `s` the scale. The
 * shift is in native pixels, so at scale S it is S times as many samples.
 */
const uint32_t *pc_gpu3d_soft_line_hd(int hy, int *w, int *s)
{
    return pc_gpu3d_soft_line_hd_r(hy, w, s, ScrolledHd);
}

/*
 * The re-entrant spelling: `scratch` is the caller's row buffer
 * (RENDER_W_MAX words), so several threads may read rows at once, the
 * banded HD compose does. The render itself is not re-entrant; the first
 * call of a frame renders, so a caller that fans out fetches one row
 * before it fans.
 */
const uint32_t *pc_gpu3d_soft_line_hd_r(int hy, int *w, int *s, u32 *scratch)
{
    render_if_due();
    if (w != NULL) *w = RW;
    if (s != NULL) *s = RS;
    if (hy < 0 || hy >= RH) return ScrolledLine;
    return scroll_row(&ColorBuffer[(hy * SCANLINE_W) + FIRST_PIXEL_OFF],
                      scratch, RW, 1, scroll_shift() * RS);
}

const uint32_t *pc_gpu3d_soft_line(int y)
{
    const u32 *rawline;
    u16 xpos;
    int i, j;

    render_if_due();
    if (y < 0 || y >= 192) return ScrolledLine;

    /* The centre 256 of a wide render is the native picture, the wide
     * projection and the wide viewport cancel exactly there. Under the HD
     * scale the native picture is a point-sample of the finer grid; the
     * compositor and every instrument read this, the viewer alone reads
     * the fine one. */
    rawline = &ColorBuffer[(y * RS * SCANLINE_W) + FIRST_PIXEL_OFF
                           + (RW / RS - 256) / 2 * RS];

    /*
     * The 3D layer's X position is engine A's BG0 horizontal scroll, upstream
     * routes a write to 0x04000010 into GPU3D::SetRenderXPos. It is plain guest
     * memory here, so it is read at use rather than
     * latched; upstream latches at write and ignores writes made while the
     * rendering engine is powered down, which is the one difference and which
     * nothing in this tree exercises.
     */
    xpos = (u16)(*(volatile u16 *)G3D_HOST(REG_BG0HOFS) & 0x01FF);
    /* The native path stays spelled out rather than folded into scroll_row():
     * Every instrument and the whole 2D compositor read this function, and
     * its output is pinned frame by frame. */
    if (xpos == 0 && RS == 1) return rawline;

    /* Under the HD scale a native pixel is every RS-th sample. */
    if (xpos == 0) {
        for (i = 0; i < 256; i++) ScrolledLine[i] = rawline[i * RS];
    } else if (xpos & 0x100) {
        i = 0; j = xpos;
        for (; j < 512; i++, j++) ScrolledLine[i] = 0;
        for (j = 0; i < 256; i++, j++) ScrolledLine[i] = rawline[j * RS];
    } else {
        i = 0; j = xpos;
        for (; j < 256; i++, j++) ScrolledLine[i] = rawline[j * RS];
        for (; i < 256; i++) ScrolledLine[i] = 0;
    }

    return ScrolledLine;
}

/*
 * The scroll, checked against itself, because no scene this project can
 * replay ever scrolls the 3D layer.
 *
 * Measured before this was written: the recorded session has 212 frames with
 * BG0HOFS set and the hand-scripted one none, and in every one of the 212
 * BG0 is an ordinary background rather than the 3D layer, so the margins
 * were never actually blinking, and the wide row's scroll cannot be reached
 * by driving the game. What can still be wrong is the arithmetic, so that is
 * what this checks: over the whole nine-bit range, wherever the native path
 * shows a pixel, the wide row's corresponding column must show the same one.
 * A seam that shifts under scroll is exactly the defect this generalization
 * could introduce, and it would be invisible until a game did it.
 *
 * Runs from --selftest, before any guest code, and puts the colour buffer
 * and the register back the way it found them.
 */
int pc_gpu3d_soft_scroll_selftest(void)
{
    static const u16 kXpos[] = { 0, 1, 7, 100, 255, 256, 257, 400, 511 };
    volatile u16 *reg = (volatile u16 *)G3D_HOST(REG_BG0HOFS);
    const u16 saved = *reg;
    const int y = 96;
    int saved_rw = RW, saved_rs = RS;
    u32 *row;
    int k, i, w, margin, bad = 0;

    /* A wide render at 340 columns, native scale, and a row whose pixel
     * value names its own column, so a shifted pixel is identifiable and
     * zero cannot be mistaken for content. */
    RS = 1;
    RW = 340;
    row = &ColorBuffer[(y * SCANLINE_W) + FIRST_PIXEL_OFF];
    for (i = 0; i < RW; i++) row[i] = 0xFF000000u | (u32)(i + 1);
    margin = (RW - 256) / 2;

    for (k = 0; k < (int)(sizeof kXpos / sizeof kXpos[0]); k++) {
        const u32 *nat, *wide;

        *reg = kXpos[k];
        nat = pc_gpu3d_soft_line(y);
        wide = pc_gpu3d_soft_line_wide(y, &w);
        if (w != RW) {
            fprintf(stderr, "pc-3d: wide row is %d columns, expected %d\n",
                    w, RW);
            bad++;
            continue;
        }
        for (i = 0; i < 256; i++) {
            if (nat[i] == 0) continue;      /* the native path blanked it */
            if (wide[margin + i] == nat[i]) continue;
            fprintf(stderr,
                    "pc-3d: xpos %u: native column %d is %08X and the wide "
                    "row's column %d is %08X; the seam moves under "
                    "scroll\n",
                    (unsigned)kXpos[k], i, nat[i], margin + i,
                    wide[margin + i]);
            bad++;
            break;
        }
    }

    *reg = saved;
    RW = saved_rw;
    RS = saved_rs;
    memset(row, 0, (size_t)RENDER_W_MAX * sizeof *row);
    return bad == 0;
}

/*
 * The GL converter, checked against this file's own texture unit, texel by
 * texel, the 3DS pilot's texture-converter proof re-run in-binary. It
 * lives here and not with the converter because the oracle is
 * this file's statics: render_pixel() with a white vertex and polygon
 * alpha 31 is the identity over both DS combining steps, so its answer IS
 * the texel, in the six-bit form the surface expansion then widens. The
 * converter under test reads through the exported slot readers above, so
 * the slot walk is exercised, not bypassed.
 *
 * The banks are loaded the way the SDK loads them, written through LCDC,
 * then given their roles, because that sequence is what found the 3DS's
 * cache-keying bug. Two texture slots and two palette sources, so an index
 * wired to zero cannot pass. Runs from --selftest, before any guest code;
 * VRAMCNT and the latch are put back the way it found them.
 */
static u32 GltexOracle(u32 texparam, u32 texpal, int si, int ti)
{
    PcGxPolygon poly;
    PolyShade S;
    u32 c, r, g, b, a;

    memset(&poly, 0, sizeof poly);
    poly.Attr = 31u << 16;              /* modulation, polygon alpha 31 */
    poly.TexParam = texparam;
    poly.TexPalette = texpal;

    poly_shade_setup(&S, &poly);
    /* The general decode path, which is the arithmetic the converter
     * mirrors: the direct-pointer fast path and the frame cache read the
     * same bytes, but the proof should run the code the derivation names. */
    S.cache = NULL;
    S.texptr = NULL;
    S.palptr = NULL;

    c = render_pixel(&S, 63, 63, 63, (s16)(si << 4), (s16)(ti << 4));

    r = c & 0x3Fu;
    g = (c >> 8) & 0x3Fu;
    b = (c >> 16) & 0x3Fu;
    a = (c >> 24) & 0x1Fu;

    r = (r << 2) | (r >> 4);
    g = (g << 2) | (g >> 4);
    b = (b << 2) | (b >> 4);
    a = (a << 3) | (a >> 2);
    return r | (g << 8) | (b << 16) | (a << 24);    /* the GL word */
}

static u32 GltexScratch[PC_GLTEX_MAX_TEXELS];

static int gltex_compare(const char *what, u32 texparam, u32 texpal,
                         unsigned long *texels)
{
    unsigned w = pc_gltex_width(texparam);
    unsigned h = pc_gltex_height(texparam);
    unsigned si, ti;
    unsigned long bad = 0;
    int said = 0;

    if (pc_gltex_convert(GltexScratch, texparam, texpal) != 0) {
        fprintf(stderr, "pc-selftest gltex: %s: refused\n", what);
        return 0;
    }

    for (ti = 0; ti < h; ti++) {
        for (si = 0; si < w; si++) {
            u32 want = GltexOracle(texparam, texpal, (int)si, (int)ti);
            u32 got = pc_gltex_texel(texparam, texpal, (int)si, (int)ti);
            u32 image = GltexScratch[(size_t)ti * w + si];

            if (got != want || image != want) {
                if (!said) {
                    said = 1;
                    fprintf(stderr, "pc-selftest gltex: %s: (%u,%u) want"
                            " %08X texel %08X image %08X\n",
                            what, si, ti, want, got, image);
                }
                bad++;
            }
        }
    }
    if (bad != 0) {
        fprintf(stderr, "pc-selftest gltex: %s: %lu of %u texels differ\n",
                what, bad, w * h);
        return 0;
    }
    *texels += (unsigned long)w * h;
    return 1;
}

/* The SDK's own load sequence: the bank into LCDC, the write, the role. */
#define GLTEX_VRAMCNT   0x04000240u
#define GLTEX_LCDC_A    0x06800000u
#define GLTEX_LCDC_E    0x06880000u
#define GLTEX_LCDC_F    0x06890000u

static void gltex_vramcnt(int bank, u8 val)
{
    *(volatile u8 *)G3D_HOST(GLTEX_VRAMCNT + (u32)bank) = val;
    armrec_vram_touch();
}

static void gltex_fill(u32 base, u32 bytes, u32 seed)
{
    u32 i;

    for (i = 0; i < bytes; i++) {
        seed = seed * 1103515245u + 12345u;
        *(volatile u8 *)G3D_HOST(base + i) = (u8)(seed >> 16);
    }
}

int pc_gpu3d_gltex_selftest(void)
{
    const PcGxRenderRegs *savedR = R;
    int savedCacheMode = TexCacheMode;
    u8 savedCnt[6];
    static PcGxRenderRegs regs;
    unsigned long texels = 0;
    int i, ok = 1;

    /* DISP3DCNT bit 0: without it render_pixel() draws untextured and the
     * oracle would answer white for every format. */
    memset(&regs, 0, sizeof regs);
    regs.DispCnt = 1u;
    R = &regs;
    TexCacheMode = 0;

    for (i = 0; i < 6; i++) {
        savedCnt[i] = *(volatile u8 *)G3D_HOST(GLTEX_VRAMCNT + (u32)i);
    }

    /* A -> texture slot 0, B -> slot 1, E -> palette slots 0-3, F -> slot
     * 4; each written with a bank-distinct sequence first, so a converter
     * reading the wrong slot cannot pass. */
    gltex_vramcnt(0, 0x80);
    gltex_vramcnt(1, 0x80);
    gltex_vramcnt(4, 0x80);
    gltex_vramcnt(5, 0x80);
    gltex_fill(GLTEX_LCDC_A, 0x10000u, 0x1234u);
    gltex_fill(GLTEX_LCDC_A + 0x20000u, 0x10000u, 0x9ABCu);   /* bank B */
    gltex_fill(GLTEX_LCDC_E, 0x10000u, 0x5555u);
    gltex_fill(GLTEX_LCDC_F, 0x4000u, 0xAAAAu);
    gltex_vramcnt(0, 0x83);
    gltex_vramcnt(1, (u8)(0x83 | (1 << 3)));
    gltex_vramcnt(4, 0x83);
    gltex_vramcnt(5, (u8)(0x83 | (2 << 3)));
    latch_texture_slots();

    if (TexSlot[0] == NULL || TexSlot[1] == NULL
        || TexPalSlot[0] == NULL || TexPalSlot[4] == NULL) {
        fprintf(stderr, "pc-selftest gltex: the banks did not take their"
                        " roles\n");
        ok = 0;
    }

    /* Every format over a 64x64 image; the three indexed formats again with
     * colour 0 transparent; a non-square image, the smallest, the largest;
     * the second texture slot against the bank-F palette slot; a 512x512
     * refused rather than truncated. The 3DS proof's own list. */
    if (ok) {
        for (i = 1; i <= 7; i++) {
            char name[48];

            snprintf(name, sizeof name, "format %d, 64x64", i);
            ok &= gltex_compare(name, ((u32)i << 26) | (3u << 20) | (3u << 23),
                                0, &texels);
        }
        for (i = 2; i <= 4; i++) {
            char name[48];

            snprintf(name, sizeof name, "format %d, colour 0 transparent", i);
            ok &= gltex_compare(name, ((u32)i << 26) | (3u << 20) | (3u << 23)
                                      | (1u << 29), 0, &texels);
        }
        ok &= gltex_compare("format 3, 128x32",
                            (3u << 26) | (4u << 20) | (2u << 23), 0, &texels);
        ok &= gltex_compare("format 3, 8x8", (3u << 26), 0, &texels);
        ok &= gltex_compare("format 2, 256x256",
                            (2u << 26) | (5u << 20) | (5u << 23), 0, &texels);
        ok &= gltex_compare("format 1, slot 1, palette slot 4",
                            (1u << 26) | (3u << 20) | (3u << 23) | 0x4000u,
                            0x1000u, &texels);
        if (pc_gltex_convert(GltexScratch,
                             (3u << 26) | (6u << 20) | (6u << 23), 0) != -1) {
            fprintf(stderr, "pc-selftest gltex: a 512x512 image was not"
                            " refused\n");
            ok = 0;
        }
        if (ok && texels != 114752ul) {
            fprintf(stderr, "pc-selftest gltex: compared %lu texels, the"
                            " proof's count is 114752\n", texels);
            ok = 0;
        }
    }

    for (i = 0; i < 6; i++) {
        *(volatile u8 *)G3D_HOST(GLTEX_VRAMCNT + (u32)i) = savedCnt[i];
    }
    armrec_vram_touch();
    latch_texture_slots();
    R = savedR;
    TexCacheMode = savedCacheMode;
    return ok;
}

int pc_gpu3d_soft_present(void) { render_if_due(); return 1; }

uint32_t pc_gpu3d_soft_polygons_drawn(void) { return PolysDrawn; }

void pc_gpu3d_soft_final_counts(unsigned long *edgeMarked, unsigned long *aaBlended)
{
    if (edgeMarked != NULL) *edgeMarked = EdgeMarkedPixels;
    if (aaBlended != NULL)  *aaBlended  = AABlendedPixels;
}
