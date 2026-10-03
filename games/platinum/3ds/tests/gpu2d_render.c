/*
 * 3ds/tests/gpu2d_render.c: the real 2D renderer, over this console's memory
 * model, on a build machine.
 *
 * The change is one macro in pc/hw/pc_gpu2d.c: every read of guest memory
 * that used to be `*(volatile uint16_t *)(uintptr_t)addr` now goes through
 * 3ds/src/3ds_hostmap.c. The claim worth checking is not that the macro
 * compiles; it is that the renderer still draws the same picture when the
 * address space is not doing the work.
 *
 * That is checkable here and NOT on the console, which is the opposite of most
 * of this port's checks and worth saying why. pc_gpu2d.c includes no DS SDK
 * header, stdint, armrec_rt.h, pc_video.h and the 3D rasterizer's interface,
 * and nothing else, so it compiles for the host with -D__3DS__ and links
 * against the same 3ds/src model files every other host test uses. The
 * .3dsx cannot run it: the self-test binary links no pc/hw at all, and the
 * game link is waiting on sound. So the oracle for the host map is here until then.
 *
 * What it draws. The smallest picture that exercises the whole read path: one
 * 256-colour text background, whose tile map, tile pixels and palette are
 * three different regions reached three different ways. A white palette entry
 * makes the expected output a fixed number rather than a rounding argument,
 * 0x7FFF through rgb15to18() is 62 per channel, and expand() replicates the
 * top two bits, so every pixel must be exactly 0x00FBFBFB.
 *
 * And what happens with no bank mapped, which is the case the PC port answers
 * with a read-only floor under its windows and this console has to answer some
 * other way. Not a fault, and not a skipped frame: zeros, and therefore the
 * backdrop colour, which is what hardware shows.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "armrec_rt.h"

#include "armrec_mem_3ds.h"
#include "3ds_gpu.h"
#include "3ds_effect.h"
#include "3ds_gpu2d.h"
#include "3ds_guest.h"
#include "3ds_hostmap.h"
#include "3ds_layer3d.h"
#include "3ds_tile.h"
#include "3ds_vram.h"

#include "pc_video.h"

static int sRan;
static int sBad;

#define CHECK(what, cond)                                                  \
    do {                                                                   \
        sRan++;                                                            \
        if (!(cond)) {                                                     \
            printf("  %-56s FAILED\n", (what));                            \
            sBad++;                                                        \
        }                                                                  \
    } while (0)

/* ------------------------------------------------------------------ */
/* What pc_gpu2d.c expects to be linked against                        */
/* ------------------------------------------------------------------ */
/*
 * Stubs, and deliberately so: what is under test is the renderer's reads, not
 * pc_video.c's dump machinery or the 3D rasterizer. Both are real files in the
 * game link; neither changes what pc_gpu2d.c asks guest memory for.
 */

static uint32_t sSurface[2][PC_VIDEO_PIXELS];
static pc_video_renderer_fn sRenderer;
static const char *sRendererName;

uint32_t *pc_video_surface(int engine)
{
    return sSurface[engine & 1];
}

void pc_video_set_renderer(pc_video_renderer_fn fn, const char *name)
{
    sRenderer = fn;
    sRendererName = name;
}

/*
 * The 3D layer's 3D layer needs a rasterizer under it, and the point of the seam is
 * that the compositor cannot tell which one. So this file is one: 192 rows of
 * whatever the scene below wants, handed over in exactly the format
 * pc_gpu3d_soft.c hands its own over in, six bits a channel, alpha 0 to 31
 * in the top byte, and alpha 0 meaning the pixel was never drawn.
 */
static uint32_t sLine3D[PC_VIDEO_HEIGHT][PC_VIDEO_WIDTH];
static int sHave3D;

int pc_gpu3d_soft_present(void)
{
    return sHave3D;
}

/* The 3D engine is the and is not installed, which is also its state in the
 * game link today. armrec_rt_3ds.c asks before it stages anything. */
int pc_gpu3d_installed(void)
{
    return 0;
}

uint32_t *pc_gpu3d_stage(uint32_t addr)
{
    (void)addr;
    return NULL;
}

void pc_gpu3d_refresh_regs(void)
{
}

/* --bench is a PC command line and there is none here. */
int pc_bench_on;

unsigned long long pc_bench_enter(void)
{
    return 0ull;
}

void pc_bench_leave(int span, unsigned long long t0)
{
    (void)span;
    (void)t0;
}

/* armrec_trap()'s fatal path. 3ds_fault.c has a host definition of this, but
 * that file opens with <3ds.h> and cannot be compiled here; nothing in this
 * test should reach it, so a print and a non-zero exit is the whole of it. */
void fault_stop(const char *top, const char *bottom)
{
    printf("  fault_stop: %s, %s\n", top ? top : "", bottom ? bottom : "");
    sBad++;
}

const uint32_t *pc_gpu3d_soft_line(int line)
{
    if (!sHave3D || line < 0 || line >= PC_VIDEO_HEIGHT) {
        return NULL;
    }
    return sLine3D[line];
}

/* ------------------------------------------------------------------ */
/* Guest state, written the way the game writes it                     */
/* ------------------------------------------------------------------ */

#define VRAMCNT_A   0x04000240u
#define VRAMCNT_B   0x04000241u
#define DISPCNT_A   0x04000000u
#define DISPCAPCNT  0x04000064u
#define BG0CNT_A    0x04000008u
#define POWCNT1     0x04000304u
#define PAL_A       0x05000000u
#define PAL_OBJ_A   0x05000200u
#define BG_WINDOW   0x06000000u
#define OBJ_WINDOW  0x06400000u
#define OAM_A       0x07000000u

#define TILESET_OFF 0x4000u     /* character base block 1 */
#define WHITE_18    0x00FBFBFBu /* 0x7FFF, expanded */

static void poke8(uint32_t a, uint8_t v)
{
    uint8_t *p = (uint8_t *)armrec_host_ptr(a);

    if (p != NULL) {
        *p = v;
    }
}

static void poke16(uint32_t a, uint16_t v)
{
    uint16_t *p = (uint16_t *)armrec_host_ptr(a);

    if (p != NULL) {
        *p = v;
    }
}

static void poke32(uint32_t a, uint32_t v)
{
    uint32_t *p = (uint32_t *)armrec_host_ptr(a);

    if (p != NULL) {
        *p = v;
    }
}

/* One 8x8 tile of colour index 1, and a map that is nothing but tile 0. */
static void write_picture(void)
{
    uint32_t i;

    for (i = 0; i < 64u; i++) {
        poke8(BG_WINDOW + TILESET_OFF + i, 1u);
    }
    /* 32x32 entries, all zero, which is already what a cleared bank holds,
     * written anyway so the test does not depend on that. */
    for (i = 0; i < 32u * 32u; i++) {
        poke16(BG_WINDOW + i * 2u, 0u);
    }
    poke16(PAL_A + 2u, 0x7FFFu);       /* index 1: white */
    poke16(PAL_A + 0u, 0x0000u);       /* index 0 and the backdrop: black */
}

static void write_registers(void)
{
    /* Graphics display mode, BG0 on, BG mode 0, both DISPCNT bases zero. */
    poke32(DISPCNT_A, 0x00010100u);
    /* 256 colours (bit 7), character base block 1 (bits 2-5), screen base 0. */
    poke16(BG0CNT_A, 0x0084u);
    /* LCDs on, engine A on. */
    poke16(POWCNT1, 0x0003u);
    poke16(0x04000010u, 0u);           /* BG0HOFS */
    poke16(0x04000012u, 0u);           /* BG0VOFS */
    poke16(0x04000050u, 0u);           /* BLDCNT */
    poke16(0x0400006Cu, 0u);           /* MASTER_BRIGHT */
}

static void map_bank_a(int on)
{
    /* MST 1 is the main BG window, offset 0. Bit 7 enables. */
    poke8(VRAMCNT_A, on ? 0x81u : 0x00u);
    armrec_vram_touch();
}

static int surface_uniform(int engine, uint32_t want)
{
    uint32_t i;

    for (i = 0; i < (uint32_t)PC_VIDEO_PIXELS; i++) {
        if (sSurface[engine][i] != want) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* The background path; what 3ds_gpu.c would have drawn              */
/* ------------------------------------------------------------------ */
/*
 * 3ds_gpu2d.c turns an engine's registers into quads over an atlas of
 * expanded tiles, and the console draws them. On a build machine there is no
 * PICA, but there is no need for one either: the quads are axis-aligned,
 * integer-sized, unfiltered and unblended, which is exactly the set of
 * conditions under which "what the hardware draws" is a copy. So the emitter
 * is checked here against the software renderer's own surface, pixel for
 * pixel, and what is left for the console to answer is only the two things
 * this cannot know, whether a texture block is where this file thinks it is
 * in memory, and which way the sampler's v axis runs.
 *
 * Those two are the GPU present's PRESENT_VERIFY, which compares whole framebuffers on
 * the console. This is the check that says a wrong picture there is a layout
 * question and not a map-addressing bug, which is the difference between an
 * afternoon and a week.
 */

/*
 * Deliberately not square: 256 across and 128 down. The console's atlas is
 * 1024 by 512, and a check over a square one cannot tell a texture coordinate
 * that divides by the width from one that divides by the height; which is a
 * mistake that draws a picture out of whatever else is in the atlas.
 */
#define ATLAS_TEST_W     256
#define ATLAS_TEST_SLOTS ((ATLAS_TEST_W / TILE_SIDE) * 16)
#define VERTS_MAX        (6 * (4 * 33 * 25 + 128 * 16))

static uint32_t sAtlasPool[ATLAS_TEST_SLOTS * TILE_TEXELS];
static struct gpu_vertex sVertBuf[VERTS_MAX];
static int sVertN;

struct gpu_vertex *gpu_vertex_alloc(int count)
{
    struct gpu_vertex *v;

    if (sVertN + count > VERTS_MAX) {
        return NULL;
    }
    v = sVertBuf + sVertN;
    sVertN += count;
    return v;
}

int gpu_vertex_count(void)
{
    return sVertN;
}

void gpu_cache_flush(const void *p, size_t bytes)
{
    (void)p;
    (void)bytes;
}

/*
 * The 3D rasterizer's producer, which is citro3d from end to end and cannot be compiled on
 * a build machine. 3ds_layer3d.c calls it whenever `3d.txt` says `pica`; here
 * the mode file does not exist, so the soft packer is what runs and these
 * exist only to let the link close.
 */
int pica3d_gpu_init(void)
{
    return -1;
}

unsigned pica3d_gpu_build(void)
{
    return 0u;
}

void pica3d_gpu_draw(void)
{
}

void *pica3d_gpu_texture(void)
{
    return NULL;
}

uint32_t pica3d_gpu_texel(int x, int y)
{
    (void)x;
    (void)y;
    return 0u;
}

int pica3d_gpu_diff(void)
{
    return 0;
}

int pica3d_gpu_no_split(void)
{
    return 0;
}

void pica3d_gpu_report(FILE *f)
{
    (void)f;
}

/* The two 3ds_gpu.c answers the survey asks for. Here the GPU is always up
 * and never verifying, so eligibility is the register state alone. */
int gpu_ready(void)
{
    return 1;
}

int gpu_mode(void)
{
    return PRESENT_GPU;
}

/*
 * One texel out of the atlas, addressed the way this port lays it out: 8x8
 * blocks in raster order, Morton order inside a block, and memory row 0 at
 * v = 1.
 */
static uint32_t pool_texel(const uint32_t *pool, int w, int col, int row)
{
    int block = (row / TILE_SIDE) * (w / TILE_SIDE) + (col / TILE_SIDE);

    return pool[block * TILE_TEXELS
                + tile_swizzle(col % TILE_SIDE, row % TILE_SIDE)];
}

/* The 3D layer's texture, the size the console gives it: a power of two on both sides
 * with the 256x192 picture in the top rows. */
#define L3D_TEST_SIDE 256
static uint32_t sLayerPool[L3D_TEST_SIDE * L3D_TEST_SIDE];
static uint32_t sVramHost[L3D_TEST_SIDE * L3D_TEST_SIDE];

/*
 * THE PICA's own blend, as an eight-bit model. A coefficient the DS writes in
 * sixteenths is a byte over 255 here (3ds_gpu.c's coefficient()), so this is
 * not the software renderer's arithmetic and is not meant to be: what the
 * check below asks is how far apart the two are, which is the number the effect pass has
 * to report rather than the zero the tasks before it reported.
 */
static uint32_t blend8(uint32_t src, uint32_t dst, unsigned fs, unsigned fd)
{
    uint32_t out = 0;
    int c;

    for (c = 0; c < 3; c++) {
        unsigned a = (src >> (c * 8)) & 0xFFu;
        unsigned b = (dst >> (c * 8)) & 0xFFu;
        unsigned v = (a * fs + b * fd + 127u) / 255u;

        if (v > 255u) {
            v = 255u;
        }
        out |= v << (c * 8);
    }
    return out;
}

static unsigned coefficient(unsigned k)
{
    return (k * 255u + 8u) / 16u;
}

/* How many pixels the blend unit ran on in the last scene. A blend scene that
 * never blended would otherwise pass by drawing the same picture the software
 * renderer drew without one. */
static unsigned long sBlended;

/*
 * The draw list, drawn. `out` is 0x00RRGGBB like a pc_video surface, and the
 * stencil beside it is the eight bits 3ds_gpu.c's targets carry, the window
 * mask in 0-5 and the target-2 bit in 7.
 */
static void raster_quads(uint32_t *out, const struct bg_draw *list, int n)
{
    int atlasH = (ATLAS_TEST_SLOTS / (ATLAS_TEST_W / TILE_SIDE)) * TILE_SIDE;
    static uint8_t stencil[PC_VIDEO_PIXELS];
    int i, q, d;

    sBlended = 0;
    for (i = 0; i < PC_VIDEO_PIXELS; i++) {
        out[i] = 0u;
        stencil[i] = 0u;
    }

    for (d = 0; d < n; d++) {
        const struct bg_draw *w = &list[d];
        const uint32_t *pool;
        int poolW, poolH;
        unsigned athresh;

        if (w->tex == BG_TEX_LAYER3D) {
            pool = sLayerPool;
            poolW = L3D_TEST_SIDE;
            poolH = L3D_TEST_SIDE;
            athresh = 0u;
        } else if (w->tex == BG_TEX_VRAM) {
            pool = gpu2d_vram_tex();
            poolW = gpu2d_vram_side();
            poolH = poolW;
            athresh = 0x80u;
            if (pool == NULL || poolW < PC_VIDEO_WIDTH) {
                continue;
            }
        } else {
            pool = sAtlasPool;
            poolW = ATLAS_TEST_W;
            poolH = atlasH;
            athresh = 0x80u;
        }

        for (q = w->base; q + 6 <= w->base + w->count; q += 6) {
            const struct gpu_vertex *a = &sVertBuf[q];       /* top left     */
            const struct gpu_vertex *b = &sVertBuf[q + 2];   /* bottom right */
            int x, y;

            for (y = (int)a->y; y < (int)b->y; y++) {
                for (x = (int)a->x; x < (int)b->x; x++) {
                    uint32_t texel;
                    uint8_t *st;

                    if (x < 0 || x >= PC_VIDEO_WIDTH
                        || y < 0 || y >= PC_VIDEO_HEIGHT) {
                        continue;
                    }
                    st = &stencil[y * PC_VIDEO_WIDTH + x];

                    if (w->colour) {
                        texel = w->constant;
                    } else if (w->tex == BG_TEX_NONE) {
                        texel = 0xFFFFFFFFu;    /* stencil only */
                    } else {
                        float fx = ((float)x + 0.5f - a->x) / (b->x - a->x);
                        float fy = ((float)y + 0.5f - a->y) / (b->y - a->y);
                        float u = a->u + (b->u - a->u) * fx;
                        float v = a->v + (b->v - a->v) * fy;

                        texel = pool_texel(pool, poolW,
                                           (int)(u * (float)poolW),
                                           (int)((1.0f - v) * (float)poolH));
                    }
                    /* The alpha test, then the stencil test, then the write;
                     * the PICA's own order, and what makes palette index 0
                     * leave the stencil alone as well as the colour. */
                    if ((texel & 0xFFu) <= athresh) {
                        continue;
                    }
                    if (w->test != 0u
                        && (w->ref & w->test) != (*st & w->test)) {
                        continue;
                    }
                    if (w->write != 0u) {
                        *st = (uint8_t)((*st & ~w->write)
                                        | (w->ref & w->write));
                    }
                    if (w->nocolour
                        || (w->tex == BG_TEX_NONE && !w->colour)) {
                        continue;               /* no colour written */
                    }
                    {
                        uint32_t src = texel >> 8;
                        uint32_t *dst = &out[y * PC_VIDEO_WIDTH + x];

                        switch (w->blend) {
                        case BG_BLEND_ALPHA:
                            *dst = blend8(src, *dst, coefficient(w->eva),
                                          coefficient(w->evb));
                            sBlended++;
                            break;
                        case BG_BLEND_FADE:
                            *dst = blend8(src, *dst, coefficient(w->eva),
                                          255u - coefficient(w->eva));
                            break;
                        default:
                            *dst = src;
                            break;
                        }
                    }
                }
            }
        }
    }
}

/*
 * One scene: write the registers, let both renderers loose on it, compare.
 * Returns the number of differing pixels and prints the first one, which is
 * the thing a wrong flip or a wrong map row is diagnosed from.
 *
 * `slack` is how far apart a channel may be. It is 0 for everything the background path and
 * The 3D layer drew and for everything the effect pass folds into a palette, because those are
 * the software renderer's own arithmetic moved to the CPU's palette pass; it
 * is not 0 for a frame with an alpha blend on it, where the PICA works in
 * eighths of a byte and the DS in sixteenths. A scene that asks for slack and
 * needs none is reported too; see sSlackUsed.
 */
static int sSlackUsed;
static unsigned sSceneFrame;

static int scene_compare_slack(const char *what, int slack)
{
    static uint32_t drawn[PC_VIDEO_PIXELS];
    struct bg_frame bg[2];
    const struct bg_draw *list;
    int bad = 0, worst = 0;
    int i, n;

    hostmap_flush();
    memset(sSurface, 0xAA, sizeof sSurface);
    sRenderer(0);

    sVertN = 0;
    (void)gpu2d_survey(bg);
    if (bg[0].reasons != 0u) {
        printf("  %-56s NOT ELIGIBLE (%04X)\n", what, bg[0].reasons);
        return -1;
    }
    /*
     * A frame number that moves, because the tile cache will not evict a slot
     * drawn in the frame it is being asked about, so a test that says "frame
     * 1" for every scene fills the atlas with keys it may never reuse and the
     * twentieth scene has nowhere to expand into. On the console this counter
     * is the frame counter.
     */
    if (gpu2d_build(0, ++sSceneFrame, 0.0f, 0.0f) < 0) {
        printf("  %-56s BUILD FAILED\n", what);
        return -1;
    }
    n = gpu2d_drawlist(0, &list);
    raster_quads(drawn, list, n);

    for (i = 0; i < PC_VIDEO_PIXELS; i++) {
        int c, off = 0;

        for (c = 0; c < 3; c++) {
            int a = (int)((drawn[i] >> (c * 8)) & 0xFFu);
            int b = (int)((sSurface[0][i] >> (c * 8)) & 0xFFu);
            int diff = a > b ? a - b : b - a;

            if (diff > off) {
                off = diff;
            }
        }
        if (off > worst) {
            worst = off;
        }
        if (off > slack) {
            if (bad == 0) {
                printf("  %s: first at %d,%d want %06X got %06X\n", what,
                       i % PC_VIDEO_WIDTH, i / PC_VIDEO_WIDTH,
                       sSurface[0][i], drawn[i]);
            }
            bad++;
        }
    }
    sSlackUsed = worst;
    return bad;
}

static int scene_compare(const char *what)
{
    return scene_compare_slack(what, 0);
}

/*
 * The scenes. Each one is a thing the emitter can get wrong on its own: the
 * map row arithmetic, the sub-palette nibble, the two mirror bits, a scroll
 * that puts a partial tile against each edge, the second screen block of a
 * 512-wide map, two layers whose priorities decide which is on top, and an
 * extended palette.
 */
static void check_quads(void)
{
    uint32_t i;

    if (gpu2d_init(sAtlasPool, ATLAS_TEST_SLOTS, ATLAS_TEST_W) != 0) {
        CHECK("the tile atlas takes the test pool", 0);
        return;
    }
    gpu2d_set_vramtex(sVramHost, L3D_TEST_SIDE);

    /* A tileset with four distinguishable tiles: a solid one, a left half, a
     * top half, and a diagonal, so a horizontal flip, a vertical flip and a
     * transpose are all three different pictures. 4bpp. */
    for (i = 0; i < 32u; i++) {
        poke8(BG_WINDOW + TILESET_OFF + 0x00u + i, 0x11u);
    }
    for (i = 0; i < 8u; i++) {
        poke8(BG_WINDOW + TILESET_OFF + 0x20u + i * 4u + 0u, 0x22u);
        poke8(BG_WINDOW + TILESET_OFF + 0x20u + i * 4u + 1u, 0x00u);
        poke8(BG_WINDOW + TILESET_OFF + 0x20u + i * 4u + 2u, 0x00u);
        poke8(BG_WINDOW + TILESET_OFF + 0x20u + i * 4u + 3u, 0x00u);
        poke8(BG_WINDOW + TILESET_OFF + 0x40u + i * 4u + 0u,
              (uint8_t)(i < 4u ? 0x33u : 0x00u));
        poke8(BG_WINDOW + TILESET_OFF + 0x40u + i * 4u + 1u,
              (uint8_t)(i < 4u ? 0x33u : 0x00u));
        poke8(BG_WINDOW + TILESET_OFF + 0x40u + i * 4u + 2u,
              (uint8_t)(i < 4u ? 0x33u : 0x00u));
        poke8(BG_WINDOW + TILESET_OFF + 0x40u + i * 4u + 3u,
              (uint8_t)(i < 4u ? 0x33u : 0x00u));
        /* tile 3: one pixel per row, walking right */
        poke8(BG_WINDOW + TILESET_OFF + 0x60u + i * 4u + (i >> 1),
              (uint8_t)((i & 1u) ? 0x40u : 0x04u));
    }
    /* Sixteen sub-palettes, each with four colours that are its own. */
    for (i = 0; i < 16u * 16u; i++) {
        poke16(PAL_A + i * 2u,
               (uint16_t)(i == 0u ? 0x0000u
                                  : (0x0421u * (i & 0xFu)) + (i >> 4)));
    }
    /* A map of tile 0 with sub-palette 1 everywhere, then the corners made
     * different so a row or column that is one out is visible. */
    for (i = 0; i < 64u * 64u; i++) {
        poke16(BG_WINDOW + i * 2u, (uint16_t)(0x1000u | (i & 3u)));
    }

    poke32(DISPCNT_A, 0x00010100u);
    poke16(BG0CNT_A, 0x0004u);          /* 4bpp, char base 1, screen base 0 */
    poke16(0x04000010u, 0u);
    poke16(0x04000012u, 0u);

    CHECK("the quads draw what the renderer drew: 4bpp, unscrolled",
          scene_compare("4bpp unscrolled") == 0);

    poke16(0x04000010u, 3u);
    poke16(0x04000012u, 5u);
    CHECK("...scrolled by three and five, so both edges are part tiles",
          scene_compare("4bpp scrolled") == 0);

    /* The two mirror bits, on every tile of the map. */
    for (i = 0; i < 64u * 64u; i++) {
        poke16(BG_WINDOW + i * 2u,
               (uint16_t)(0x1000u | (i & 3u)
                          | ((i & 4u) ? 0x0400u : 0u)
                          | ((i & 8u) ? 0x0800u : 0u)));
    }
    CHECK("...with the two mirror bits", scene_compare("4bpp flipped") == 0);

    /* 512 pixels wide: the second screen block is a second map 0x800 on, and
     * a scroll of 300 is inside it. */
    for (i = 0; i < 32u * 32u; i++) {
        poke16(BG_WINDOW + 0x800u + i * 2u, (uint16_t)(0x2000u | 1u));
    }
    poke16(BG0CNT_A, 0x4004u);
    poke16(0x04000010u, 300u);
    CHECK("...across the seam of a 512-wide map",
          scene_compare("4bpp wide") == 0);

    /* 512 rows: the same second block, reached by y instead. */
    poke16(BG0CNT_A, 0x8004u);
    poke16(0x04000010u, 0u);
    poke16(0x04000012u, 260u);
    CHECK("...and of a 512-row one", scene_compare("4bpp tall") == 0);

    /* Two layers, and the priority field decides which is on top. BG1's map
     * is the second screen block, holding a tile that is solid where BG0's is
     * half, so a priority read backwards changes the picture. */
    poke16(BG0CNT_A, 0x0004u);          /* BG0: priority 0, screen base 0 */
    poke16(0x0400000Au, 0x0105u);       /* BG1: priority 1, screen base 1 */
    poke32(DISPCNT_A, 0x00010300u);     /* BG0 and BG1 enabled */
    poke16(0x04000012u, 0u);
    CHECK("...under a second layer whose priority puts it behind",
          scene_compare("two layers") == 0);

    /* 256 colours out of the standard palette. */
    poke32(DISPCNT_A, 0x00010100u);
    poke16(BG0CNT_A, 0x0084u);
    for (i = 0; i < 64u * 64u; i++) {
        poke16(BG_WINDOW + i * 2u, (uint16_t)(i & 3u));
    }
    CHECK("...8bpp out of the standard palette",
          scene_compare("8bpp") == 0);

    /* Put the registers and the map back the way the checks above this file's
     * The tile cache section expect to find them. */
    poke16(BG0CNT_A, 0x0084u);
    poke32(DISPCNT_A, 0x00010100u);
    poke16(0x04000010u, 0u);
    poke16(0x04000012u, 0u);
    poke16(0x0400000Au, 0u);
    for (i = 0; i < 64u * 64u; i++) {
        poke16(BG_WINDOW + i * 2u, 0u);
    }
    for (i = 0; i < 64u; i++) {
        poke8(BG_WINDOW + TILESET_OFF + i, 1u);
    }
    poke16(PAL_A + 2u, 0x7FFFu);
    hostmap_flush();
}

/* ------------------------------------------------------------------ */
/* The 3D layer, the 3D layer against the renderer                   */
/* ------------------------------------------------------------------ */

/*
 * The DS composites the 3D engine as engine A's BG0, and the 3D layer draws that layer
 * as one quad out of a texture 3ds_layer3d.c packed. Everything that can go
 * wrong with it is a disagreement with draw_bg_3d(), and all of it is visible
 * here: the expansion from six bits to eight, the pixels alpha 0 leaves alone,
 * and (the one that a picture would not obviously show) where in the draw
 * ORDER the quad goes. So the same scene is drawn with the layer over the
 * tiles and under them, which puts the quad at both ends of the range the
 * console splits.
 */
static void write_3d_layer(int alpha)
{
    int x, y;

    for (y = 0; y < PC_VIDEO_HEIGHT; y++) {
        for (x = 0; x < PC_VIDEO_WIDTH; x++) {
            /* Two shapes of hole, not one: a column pattern and a band, so a
             * layer that is drawn but in the wrong order and a layer that is
             * not drawn at all are different pictures. */
            int hole = (x % 8) == 3 || y < 8;
            uint32_t a = hole ? 0u : (uint32_t)alpha;
            uint32_t r = (uint32_t)(x & 0x3Fu);
            uint32_t g = (uint32_t)(y & 0x3Fu);
            uint32_t b = (uint32_t)((x + y) & 0x3Fu);

            sLine3D[y][x] = (a << 24) | (b << 16) | (g << 8) | r;
        }
    }
}

static void check_layer3d(void)
{
    struct bg_frame bg[2];
    uint32_t i;

    if (layer3d_init(sLayerPool, L3D_TEST_SIDE, L3D_TEST_SIDE) != 0) {
        CHECK("the 3D layer takes the test texture", 0);
        return;
    }
    sHave3D = 1;
    write_3d_layer(31);

    /* A tile layer under it that is half holes: 8bpp, tile 0, every other
     * column palette index 0. A layer that were opaque everywhere would hide
     * a 3D layer drawn in the wrong order behind it. */
    for (i = 0; i < 64u; i++) {
        poke8(BG_WINDOW + TILESET_OFF + i, (uint8_t)((i & 1u) ? 1u : 0u));
    }
    for (i = 0; i < 64u * 64u; i++) {
        poke16(BG_WINDOW + i * 2u, 0u);
    }
    poke16(PAL_A + 2u, 0x7FFFu);
    poke16(0x04000050u, 0u);            /* BLDCNT: no effect, no targets   */
    poke16(0x0400000Au, 0x0184u);       /* BG1: priority 1, 8bpp, base 1   */
    poke16(BG0CNT_A, 0x0000u);          /* BG0: priority 0: over BG1     */
    poke32(DISPCNT_A, 0x00010308u);     /* mode 1, BG0 is 3D, BG0+BG1 on   */

    CHECK("the 3D layer draws what the renderer composited",
          scene_compare("3d over tiles") == 0);

    poke16(BG0CNT_A, 0x0003u);          /* BG0: priority 3: under BG1    */
    CHECK("...and behind the tiles when its priority says so",
          scene_compare("3d under tiles") == 0);

    /*
     * A translucent layer with nothing for it to blend with is not a blend:
     * color_composite() needs BLDCNT to admit the layer underneath, and with
     * no target bits at all the pixel replaces what it covers whatever its
     * alpha is. So this scene must still be drawn, and drawn identically.
     */
    write_3d_layer(15);
    poke16(BG0CNT_A, 0x0000u);
    CHECK("...translucent, with a BLDCNT that admits nothing",
          scene_compare("3d translucent, no target") == 0);

    /*
     * ...and with one that does, the frame is refused, on a target-2 bit
     * alone, with BLDCNT's effect field still zero, which is the case the
     * effect test cannot see (bg_classify).
     */
    poke16(0x04000050u, 0x0200u);       /* BG1 is a second operand         */
    (void)gpu2d_survey(bg);
    CHECK("...and refused when BLDCNT would blend it",
          bg[0].reasons == BG_R_3DBLEND);

    /* An opaque layer over the same BLDCNT is drawn again: it is the pixel
     * and the register together, and neither on its own. */
    write_3d_layer(31);
    CHECK("...but an opaque one over that same BLDCNT is not",
          scene_compare("3d opaque, blend target set") == 0);

    /* And with no rasterizer in the binary there is no layer to draw, which
     * is the state every build before the 3D layer was in. */
    sHave3D = 0;
    (void)gpu2d_survey(bg);
    CHECK("...and with no rasterizer linked the frame is not ours",
          bg[0].reasons == BG_R_3D);

    /* Put back what the tile cache checks below expect to find. */
    poke16(0x04000050u, 0u);
    poke16(0x0400000Au, 0u);
    poke16(BG0CNT_A, 0x0084u);
    poke32(DISPCNT_A, 0x00010100u);
    for (i = 0; i < 64u; i++) {
        poke8(BG_WINDOW + TILESET_OFF + i, 1u);
    }
    hostmap_flush();
}

/* ------------------------------------------------------------------ */
/* The effect pass, the window, the blend and the fade               */
/* ------------------------------------------------------------------ */
/*
 * What this CHECK is for, and where its slack comes from. Three of the effect pass's
 * four effects are folded into the palette a tile is expanded through, so
 * they are the software renderer's own six-bit arithmetic run on the CPU and
 * the two pictures must agree to the byte, the same standard the background path and the 3D layer
 * closed on. The fourth, BLDCNT's alpha blend, has a second operand that is
 * another layer's pixel and can only be the PICA's blend unit, which works in
 * 255ths where the DS works in sixteenths. So the blend scenes below ask how
 * far apart the two are instead of whether they differ, and the number they
 * allow is the finding: see 3ds_effect.h.
 *
 * The scenes are built so that every side of every rule appears in one
 * picture. The blend one puts a quarter-width layer over a half-height one,
 * which leaves four regions in every cell, the target-1 layer over a
 * target-2 layer, over the backdrop, the target-2 layer alone, and the
 * backdrop alone, so a stencil bit that is never cleared and one that is
 * never set both fail.
 */
#define BLDCNT_A   0x04000050u
#define BLDALPHA_A 0x04000052u
#define BLDY_A     0x04000054u
#define WIN0H_A    0x04000040u
#define WIN0V_A    0x04000044u
#define WININ_A    0x04000048u
#define WINOUT_A   0x0400004Au
#define MASTER_A   0x0400006Cu

static void effects_scene(void)
{
    uint32_t i;

    /*
     * Its own tileset, because check_layer3d() put a solid one back over the
     * first two tiles, and reported dirty, because a poke here goes straight
     * through armrec_host_ptr() where the console's writes go through the
     * store path that reports them. A cache that cannot see a write is what
     * this test would otherwise be measuring.
     *
     * Tile 1 is the left two columns of a cell and tile 2 the top four rows,
     * so the two layers below overlap on a quarter of every cell and leave
     * the other three quarters as three different pictures.
     */
    for (i = 0; i < 8u; i++) {
        poke8(BG_WINDOW + TILESET_OFF + 0x20u + i * 4u + 0u, 0x22u);
        poke8(BG_WINDOW + TILESET_OFF + 0x20u + i * 4u + 1u, 0x00u);
        poke8(BG_WINDOW + TILESET_OFF + 0x20u + i * 4u + 2u, 0x00u);
        poke8(BG_WINDOW + TILESET_OFF + 0x20u + i * 4u + 3u, 0x00u);
        poke8(BG_WINDOW + TILESET_OFF + 0x40u + i * 4u + 0u,
              (uint8_t)(i < 4u ? 0x33u : 0x00u));
        poke8(BG_WINDOW + TILESET_OFF + 0x40u + i * 4u + 1u,
              (uint8_t)(i < 4u ? 0x33u : 0x00u));
        poke8(BG_WINDOW + TILESET_OFF + 0x40u + i * 4u + 2u,
              (uint8_t)(i < 4u ? 0x33u : 0x00u));
        poke8(BG_WINDOW + TILESET_OFF + 0x40u + i * 4u + 3u,
              (uint8_t)(i < 4u ? 0x33u : 0x00u));
    }
    tile_cache_dirty(BG_WINDOW + TILESET_OFF, 0x60u);

    /*
     * BG1 on top out of tile 1 (two columns of every cell) over BG2 out
     * of tile 2, which is the top four rows of every cell. THE SUB-PALETTE
     * WALKS with the column on one and with the row on the other, so one
     * picture puts all sixteen of one layer's colours over all sixteen of the
     * other's: 256 pairs through the blend, compared against the renderer's
     * own answer for each, rather than the one pair a fixed scene would give.
     */
    for (i = 0; i < 32u * 32u; i++) {
        poke16(BG_WINDOW + 0x0800u + i * 2u,
               (uint16_t)(((i & 15u) << 12) | 1u));      /* BG1: tile 1 */
        poke16(BG_WINDOW + 0x1000u + i * 2u,
               (uint16_t)((((i >> 5) & 15u) << 12) | 2u)); /* BG2: tile 2 */
    }
    poke16(0x0400000Au, 0x0105u);       /* BG1: priority 1, screen base 1 */
    poke16(0x0400000Cu, 0x0206u);       /* BG2: priority 2, screen base 2 */
    poke16(0x04000014u, 0u);            /* BG1HOFS/VOFS, BG2HOFS/VOFS     */
    poke16(0x04000016u, 0u);
    poke16(0x04000018u, 0u);
    poke16(0x0400001Au, 0u);
    poke16(BLDCNT_A, 0u);
    poke16(MASTER_A, 0u);
    poke32(DISPCNT_A, 0x00010600u);     /* BG1 and BG2, mode 0, display 1 */
}

static void check_effects(void)
{
    struct bg_frame bg[2];

    /* raster_quads() carries the eight stencil bits, so this build has one
     * the way a console with VRAM for a depth-stencil buffer has one. */
    gpu2d_set_stencil(1);

    effects_scene();
    CHECK("11.6: two layers, no effect at all",
          scene_compare("no effect") == 0);

    /*
     * MASTER_BRIGHT, folded into every palette and into the backdrop. Both
     * directions and both ends: a factor of 16 upward is white whatever the
     * colour was, and downward is black, and the two round differently,
     * which is what the +15 in master_bright() is.
     */
    poke16(MASTER_A, 0x4008u);
    CHECK("...faded halfway to white, and it is folded into the palettes",
          scene_compare("fade up 8") == 0);
    poke16(MASTER_A, 0x8008u);
    CHECK("...and halfway to black", scene_compare("fade down 8") == 0);
    poke16(MASTER_A, 0x4010u);
    CHECK("...all the way up", scene_compare("fade up 16") == 0);
    poke16(MASTER_A, 0x8010u);
    CHECK("...and all the way down", scene_compare("fade down 16") == 0);
    poke16(MASTER_A, 0xC010u);
    CHECK("...mode 3, which is not a fade", scene_compare("fade mode 3") == 0);
    poke16(MASTER_A, 0x4000u);
    CHECK("...factor zero, which is not one either",
          scene_compare("fade factor 0") == 0);
    poke16(MASTER_A, 0u);

    /*
     * BLDCNT's two brightness effects, which apply to the layers BLDCNT names
     * and to nothing else, so BG1 changes and BG2 does not, and a fold that
     * reached every palette would fail here rather than look plausible.
     */
    poke16(BLDY_A, 6u);
    poke16(BLDCNT_A, 0x0082u);          /* effect 2, target 1 = BG1 */
    CHECK("11.6: a brightness effect on one layer and not the other",
          scene_compare("bright up") == 0);
    poke16(BLDCNT_A, 0x00C2u);          /* effect 3 */
    CHECK("...downward", scene_compare("bright down") == 0);
    /* ...and with the fade after it, which is the order color_composite() and
     * master_bright() run in. */
    poke16(MASTER_A, 0x4006u);
    CHECK("...with a fade after it, in that order",
          scene_compare("bright down then fade") == 0);
    poke16(MASTER_A, 0u);

    /*
     * An effect with nothing to apply itself to is the identity. BG0 is not
     * enabled in this scene, so a BLDCNT naming it as the only target changes
     * no pixel, and the frame is drawable, which is 262 engine-frames of
     * the survey's 1,198.
     */
    poke16(BLDCNT_A, 0x0081u);          /* effect 2, target 1 = BG0 (off) */
    CHECK("...and an effect whose only target is not on screen is none",
          scene_compare("effect with no target") == 0);

    /*
     * The alpha blend. Target 1 is BG1 and target 2 is BG2, so the blend runs
     * on the top left of every cell and nowhere else, and the slack is the
     * PICA's 255ths against the DS's sixteenths.
     */
    poke16(BLDALPHA_A, 0x0808u);        /* EVA 8, EVB 8 */
    poke16(BLDCNT_A, 0x0442u);          /* effect 1, target 1 BG1, 2 BG2 */
    CHECK("11.6: the alpha blend, within a step of the DS's own",
          scene_compare_slack("alpha blend", 4) == 0);
    CHECK("...and it really did blend, so the check is not empty",
          sBlended > 1000ul);
    printf("      blend: %lu pixels through the blend unit, worst channel "
           "%d of 255\n", sBlended, sSlackUsed);
    /* Uneven coefficients, which is what says the two are two registers. */
    poke16(BLDALPHA_A, 0x040Cu);        /* EVA 12, EVB 4 */
    CHECK("...with EVA and EVB different",
          scene_compare_slack("alpha 12/4", 4) == 0);

    /*
     * The same layer over a BLDCNT that admits nothing underneath is not a
     * BLEND, and this is the check the stencil bit exists for: the register
     * differs by one bit and every pixel must come out byte-identical, blend
     * unit or no blend unit.
     */
    poke16(BLDCNT_A, 0x0042u);          /* target 2 names nothing */
    CHECK("...and with no target-2 layer it is not a blend at all",
          scene_compare("alpha, no second operand") == 0);
    /* ...and with the backdrop as the second operand instead, which is a
     * different quarter of the cell. */
    poke16(BLDCNT_A, 0x2042u);
    CHECK("...with the backdrop as the second operand",
          scene_compare_slack("alpha over backdrop", 4) == 0);

    /*
     * The blended layer named as a second operand as well, which is the case
     * where the marker draw writes the bit rather than clearing it. Nothing
     * above it reads that bit, the predicate allows one target-1 layer,
     * so what this asks is that the third draw disturbs no pixel.
     */
    poke16(BLDCNT_A, 0x0642u);          /* target 2 = BG1 and BG2 */
    CHECK("...and one that is its own second operand's target too",
          scene_compare_slack("alpha, self in target 2", 4) == 0);

    /* A fade cannot be folded into a palette on a frame that blends after it,
     * so it becomes a pass over the finished picture. */
    poke16(BLDCNT_A, 0x0442u);
    poke16(MASTER_A, 0x8008u);
    CHECK("...and a fade on a blended frame is a pass of its own",
          scene_compare_slack("blend then fade", 6) == 0);
    poke16(MASTER_A, 0u);

    /* Two target-1 layers would blend one against the other's already-blended
     * pixel, which is not what draw_pixel() keeps. Refused. */
    poke16(BLDCNT_A, 0x0446u);          /* target 1 = BG1 and BG2 */
    (void)gpu2d_survey(bg);
    CHECK("...and two target-1 layers are refused rather than approximated",
          bg[0].reasons == BG_R_EFFECT);
    poke16(BLDCNT_A, 0u);

    /*
     * The window unit. WIN0 over a rectangle with BG1 inside and BG2 outside,
     * which is a mask the stencil has to carry per layer rather than per
     * frame.
     */
    poke16(WIN0H_A, 0x40C0u);           /* x 64 to 191 */
    poke16(WIN0V_A, 0x2060u);           /* y 32 to 95  */
    poke16(WININ_A, 0x0022u);           /* inside: BG1 and the effect */
    poke16(WINOUT_A, 0x0024u);          /* outside: BG2 and the effect */
    poke32(DISPCNT_A, 0x00012600u);     /* ...and WIN0 on */
    CHECK("11.6: a window, with a different layer inside it and outside",
          scene_compare("window") == 0);

    /* X1 > X2 and Y1 > Y2 wrap rather than draw nothing, because neither
     * latch is reset (win_scan). Two rectangles each way, four in all. */
    poke16(WIN0H_A, 0xC040u);
    poke16(WIN0V_A, 0x6020u);
    CHECK("...and one whose coordinates wrap round the screen",
          scene_compare("window wrapped") == 0);

    /* A window whose two coordinates are equal is off, not full width: the
     * clear is tested before the set. */
    poke16(WIN0H_A, 0x4040u);
    poke16(WIN0V_A, 0x2060u);
    CHECK("...and one whose edges are equal is off",
          scene_compare("window empty") == 0);

    /*
     * The colour effect inside a window and not outside it. The layer would
     * need two palettes and two sets of quads, so the frame is refused and
     * counted; which is a reason in the report and not a wrong picture.
     */
    poke16(WIN0H_A, 0x40C0u);
    poke16(WININ_A, 0x0022u);
    poke16(WINOUT_A, 0x0004u);          /* outside: no effect */
    poke16(BLDCNT_A, 0x0082u);
    (void)gpu2d_survey(bg);
    CHECK("...and a window that runs the effect on part of the screen is "
          "refused",
          bg[0].reasons == BG_R_EFFECT);
    /* ...but the same window with the two regions agreeing is drawn. */
    poke16(WINOUT_A, 0x0024u);
    CHECK("...while one where the regions agree is drawn",
          scene_compare("window with effect") == 0);
    /* ...and with both regions saying no, the effect does not run at all. */
    poke16(WININ_A, 0x0002u);
    poke16(WINOUT_A, 0x0004u);
    CHECK("...and with both regions refusing it, it does not run",
          scene_compare("window without effect") == 0);

    /* The OBJ window is drawn by the sprite unit and is the sprite path's. */
    poke32(DISPCNT_A, 0x0001A600u);
    (void)gpu2d_survey(bg);
    CHECK("...and the OBJ window is not this task's",
          (bg[0].reasons & BG_R_WINDOW) != 0u);

    /* Put back what the checks below expect to find. */
    poke32(DISPCNT_A, 0x00010100u);
    poke16(BLDCNT_A, 0u);
    poke16(BLDALPHA_A, 0u);
    poke16(BLDY_A, 0u);
    poke16(MASTER_A, 0u);
    poke16(WIN0H_A, 0u);
    poke16(WIN0V_A, 0u);
    poke16(WININ_A, 0u);
    poke16(WINOUT_A, 0u);
    poke16(0x0400000Au, 0u);
    poke16(0x0400000Cu, 0u);
    poke16(BG0CNT_A, 0x0084u);
    hostmap_flush();
}

/* ------------------------------------------------------------------ */
/* The sprite path, sprites as atlas quads                           */
/* ------------------------------------------------------------------ */

static void oam_disable_all(void)
{
    int i;

    for (i = 0; i < 128; i++) {
        poke16(OAM_A + (uint32_t)i * 8u + 0u, 0x0200u);
        poke16(OAM_A + (uint32_t)i * 8u + 2u, 0);
        poke16(OAM_A + (uint32_t)i * 8u + 4u, 0);
    }
}

static void oam_spr(int i, uint16_t a0, uint16_t a1, uint16_t a2)
{
    poke16(OAM_A + (uint32_t)i * 8u + 0u, a0);
    poke16(OAM_A + (uint32_t)i * 8u + 2u, a1);
    poke16(OAM_A + (uint32_t)i * 8u + 4u, a2);
}

static void obj_tile4(uint32_t tile, uint8_t b0, uint8_t b1, uint8_t b2,
                      uint8_t b3)
{
    uint32_t base = OBJ_WINDOW + tile * 32u;
    int y;

    for (y = 0; y < 8; y++) {
        poke8(base + (uint32_t)y * 4u + 0u, b0);
        poke8(base + (uint32_t)y * 4u + 1u, b1);
        poke8(base + (uint32_t)y * 4u + 2u, b2);
        poke8(base + (uint32_t)y * 4u + 3u, b3);
    }
}

static void check_sprites(void)
{
    struct bg_frame bg[2];
    uint32_t i;

    poke8(VRAMCNT_B, 0x82u);            /* bank B: main OBJ, offset 0 */
    armrec_vram_touch();
    oam_disable_all();
    for (i = 0; i < 256u; i++) {
        poke16(PAL_OBJ_A + i * 2u, 0);
    }
    poke16(PAL_OBJ_A + 2u, 0x7FFFu);    /* index 1: white */
    poke16(PAL_OBJ_A + 4u, 0x001Fu);    /* index 2: red   */
    obj_tile4(0, 0x11u, 0x11u, 0x11u, 0x11u);
    obj_tile4(1, 0x22u, 0x22u, 0x00u, 0x00u);   /* left half, index 2 */
    /* 1D 16x16 at tile 2 uses 2,3,4,5 consecutive. 2D 16x16 at tile 16 uses
     * 16,17 and the next row at 48,49. */
    obj_tile4(2, 0x11u, 0x11u, 0x11u, 0x11u);
    obj_tile4(3, 0x11u, 0x11u, 0x11u, 0x11u);
    obj_tile4(4, 0x11u, 0x11u, 0x11u, 0x11u);
    obj_tile4(5, 0x11u, 0x11u, 0x11u, 0x11u);
    obj_tile4(16, 0x11u, 0x11u, 0x11u, 0x11u);
    obj_tile4(17, 0x11u, 0x11u, 0x11u, 0x11u);
    obj_tile4(48, 0x11u, 0x11u, 0x11u, 0x11u);
    obj_tile4(49, 0x11u, 0x11u, 0x11u, 0x11u);

    poke16(PAL_A + 0u, 0);              /* backdrop black */
    poke16(BLDCNT_A, 0);
    poke16(0x0400006Cu, 0);
    poke32(DISPCNT_A, 0x00011010u);     /* mode 1, OBJ, 1D, no BG */

    oam_spr(0, 40u, 80u, 0u);           /* 8x8 at (80,40), tile 0 */
    CHECK("an 8x8 4bpp sprite", scene_compare("obj 8x8") == 0);

    oam_spr(0, 40u, 80u | (1u << 12), 1u);  /* tile 1, H-flip */
    CHECK("...horizontally flipped", scene_compare("obj hflip") == 0);
    oam_spr(0, 40u, 80u | (1u << 13), 1u);
    CHECK("...and vertically flipped", scene_compare("obj vflip") == 0);

    /* Square 16x16 is shape 0 size 1: size is a1 bits 14-15. */
    oam_spr(0, 40u, 80u | (1u << 14), 2u);
    CHECK("a 16x16 1D sprite", scene_compare("obj 16x16 1d") == 0);

    /* 2D mapping: the second row of a 16x16 is 32 tiles on, not 2. */
    poke32(DISPCNT_A, 0x00011000u);     /* 2D */
    oam_spr(0, 40u, 80u | (1u << 14), 16u);
    CHECK("...and the same sprite in 2D mapping",
          scene_compare("obj 16x16 2d") == 0);
    poke32(DISPCNT_A, 0x00011010u);

    /* Y wrap: top at 250, 16 tall, shows on lines 0-9. */
    oam_spr(0, 250u, 80u | (1u << 14), 2u);
    CHECK("a sprite that wraps the top of the screen",
          scene_compare("obj y wrap") == 0);

    /* Same priority, overlapping: lower OAM index wins. Sprite 0 is white,
     * sprite 1 is the left-half red tile covering the same pixels. If the
     * quads went out in OAM order, 1 would paint over 0. */
    oam_spr(0, 40u, 80u, 0u);
    oam_spr(1, 40u, 80u, 1u);
    CHECK("the lower OAM index wins a tie",
          scene_compare("obj oam prio") == 0);
    oam_spr(1, 0x0200u, 0, 0);

    /* Over a background: the sprite is last within its priority, so it
     * covers a BG0 that shares priority 0. The BG is a green field so a
     * white sprite is a different colour, not a missed layer. */
    {
        uint32_t t;

        for (t = 0; t < 32u; t++) {
            poke8(BG_WINDOW + TILESET_OFF + t, 0x11u);
        }
        for (t = 0; t < 32u * 32u; t++) {
            poke16(BG_WINDOW + t * 2u, 0u);
        }
        poke16(PAL_A + 2u, 0x03E0u);    /* BG index 1: green */
    }
    poke32(DISPCNT_A, 0x00011110u);
    poke16(BG0CNT_A, 0x0004u);          /* 4bpp, char block 1 */
    oam_spr(0, 40u, 80u, 0u);
    CHECK("a sprite covers a background of the same priority",
          scene_compare("obj over bg") == 0);

    /* Bitmap sprites stay in software. Mode 3 is bits 10-11 of a0 = 3. */
    poke32(DISPCNT_A, 0x00011010u);
    poke16(BG0CNT_A, 0x0084u);
    oam_spr(0, 40u | (3u << 10), 80u, 0u);
    (void)gpu2d_survey(bg);
    CHECK("a bitmap sprite is still refused",
          (bg[0].reasons & BG_R_OBJ) != 0u);

    /* OBJ window still is, via DISPCNT bit 15, the effect pass's bit, the sprite path's unit. */
    oam_disable_all();
    poke32(DISPCNT_A, 0x0001A010u);
    (void)gpu2d_survey(bg);
    CHECK("the OBJ window is still refused",
          (bg[0].reasons & BG_R_WINDOW) != 0u);

    oam_disable_all();
    poke8(VRAMCNT_B, 0);
    armrec_vram_touch();
    poke32(DISPCNT_A, 0x00010100u);
    poke16(BG0CNT_A, 0x0084u);
    poke16(BLDCNT_A, 0);
    hostmap_flush();
}

/* ------------------------------------------------------------------ */
/* The display-mode work, forced blank, VRAM display, capture of a fill               */
/* ------------------------------------------------------------------ */

static void check_display(void)
{
    struct bg_frame bg[2];
    uint16_t *bank;
    uint32_t i;
    int x, y;

    poke16(POWCNT1, 0x0003u);
    poke16(BLDCNT_A, 0);
    poke16(0x0400006Cu, 0);             /* MASTER_BRIGHT */

    poke32(DISPCNT_A, 0x00010180u);     /* mode 1, BG0, forced blank */
    CHECK("a forced blank is eligible", scene_compare("forced blank") == 0);

    poke16(0x0400006Cu, 0x4010u);       /* fade all the way to white */
    CHECK("...and a fade still reaches it",
          scene_compare("blank fade up") == 0);
    poke16(0x0400006Cu, 0);

    poke32(DISPCNT_A, 0x00000100u);     /* display off, BG0 leftover */
    CHECK("display off is a white fill", scene_compare("display off") == 0);

    poke8(VRAMCNT_A, 0x80u);            /* bank A: LCDC */
    armrec_vram_touch();
    bank = (uint16_t *)armrec_vram_bank_ptr(0);
    CHECK("LCDC bank A is mapped", bank != NULL);
    if (bank != NULL) {
        for (i = 0; i < 256u * 192u; i++) {
            bank[i] = 0;
        }
        /* A red 16x16 at (80,40). Bit 15 set on half of them so a conversion
         * that kept it as green would fail the compare. */
        for (y = 40; y < 56; y++) {
            for (x = 80; x < 96; x++) {
                bank[y * 256 + x] = (uint16_t)(0x001Fu | ((x & 1) ? 0x8000u : 0u));
            }
        }
    }
    poke32(DISPCNT_A, 0x00020000u);     /* mode 2, bank A */
    CHECK("VRAM display scans the bank", scene_compare("vram display") == 0);

    poke16(0x0400006Cu, 0x8010u);       /* fade all the way to black */
    CHECK("...and a fade reaches that too",
          scene_compare("vram fade down") == 0);
    poke16(0x0400006Cu, 0);

    poke8(VRAMCNT_A, 0);                /* bank not in LCDC: black */
    armrec_vram_touch();
    CHECK("a bank not in LCDC is black", scene_compare("vram unmapped") == 0);

    poke8(VRAMCNT_A, 0x80u);
    armrec_vram_touch();
    poke32(DISPCNT_A, 0x00010180u);     /* blank, so capture is a fill */
    poke32(DISPCAPCNT, 0x80000000u | (3u << 20));  /* 256x192, dest A, source A */
    (void)gpu2d_survey(bg);
    CHECK("blank plus capture is eligible", bg[0].reasons == 0u);
    gpu2d_capture();
    {
        uint32_t *cap = (uint32_t *)armrec_host_ptr(DISPCAPCNT);

        CHECK("capture cleared DISPCAPCNT bit 31",
              cap != NULL && (*cap & 0x80000000u) == 0u);
    }
    if (bank != NULL) {
        CHECK("a captured blank is 15-bit white with alpha",
              bank[0] == 0xFFFFu && bank[256 * 100 + 128] == 0xFFFFu);
    }

    poke32(DISPCNT_A, 0x00010100u);
    poke32(DISPCAPCNT, 0x80000000u | (3u << 20));
    (void)gpu2d_survey(bg);
    CHECK("compose plus capture still stays in software",
          (bg[0].reasons & BG_R_CAPTURE) != 0u);

    poke32(DISPCAPCNT, 0);
    poke8(VRAMCNT_A, 0x81u);            /* bank A back on the BG window */
    armrec_vram_touch();
    poke16(BG0CNT_A, 0x0084u);
    hostmap_flush();
}

/*
 * Colours with bit 15 set and clear, and none of them grey: a conversion that
 * dropped a channel or mixed two up would still pass over a grey ramp.
 */
static uint16_t test_colour(int i)
{
    uint16_t c = (uint16_t)(((i * 7u) & 0x1Fu)
                            | ((((unsigned)i * 3u) & 0x1Fu) << 5)
                            | ((((unsigned)i * 11u) & 0x1Fu) << 10));

    return (uint16_t)(c | ((i & 1) ? 0x8000u : 0u));
}

static void tile_compare_8bpp(void)
{
    uint32_t pal[TILE_PAL8];
    uint32_t texels[TILE_TEXELS];
    uint16_t pal15[TILE_PAL8];
    int x, y, i;
    int bad = 0;

    /* One tile whose 64 pixels are 64 different indices, and never index 0;
     * that one is transparent and the renderer answers it with the backdrop,
     * which is not what this compares. */
    for (i = 0; i < TILE_TEXELS; i++) {
        poke8(BG_WINDOW + TILESET_OFF + (uint32_t)i, (uint8_t)(i + 1));
    }
    for (i = 0; i < TILE_PAL8; i++) {
        pal15[i] = test_colour(i);
        poke16(PAL_A + (uint32_t)i * 2u, pal15[i]);
    }
    poke16(BG0CNT_A, 0x0084u);          /* 256 colours, char base block 1 */
    hostmap_flush();

    memset(sSurface, 0xAA, sizeof sSurface);
    sRenderer(3);

    tile_palette(pal, pal15, TILE_PAL8);
    tile_expand8(texels, (const uint8_t *)armrec_host_ptr(BG_WINDOW
                                                          + TILESET_OFF), pal);

    for (y = 0; y < TILE_SIDE; y++) {
        for (x = 0; x < TILE_SIDE; x++) {
            if (sSurface[0][y * 256 + x] != (texels[tile_swizzle(x, y)] >> 8)) {
                bad++;
            }
        }
    }
    CHECK("8bpp: the expanded tile is the renderer's own pixels", bad == 0);
    if (bad) {
        printf("  8bpp: %d of %d texels differ, first pixel %06X vs %06X\n",
               bad, TILE_TEXELS, sSurface[0][0], texels[0] >> 8);
    }

    /* And through the cache, which must not change a single texel. */
    {
        static uint32_t pool[8 * TILE_TEXELS];
        int slot;

        tile_cache_init(pool, 8);
        slot = tile_cache_get(BG_WINDOW + TILESET_OFF, PAL_A, 8,
                              (const uint8_t *)armrec_host_ptr(BG_WINDOW
                                                               + TILESET_OFF),
                              pal, 1u, NULL);
        CHECK("the cache hands back the same expansion",
              slot >= 0 && memcmp(tile_cache_slot(slot), texels,
                                  sizeof texels) == 0);
    }
}

static void tile_compare_4bpp(void)
{
    uint32_t pal[TILE_PAL4];
    uint32_t texels[TILE_TEXELS];
    uint16_t pal15[TILE_PAL8];
    int x, y, i;
    int bad = 0;
    /* Sub-palette 3: the map entry's top nibble picks a 16-entry block, so
     * this exercises the offset as well as the depth. */
    const uint32_t subpal = 3u * 32u;

    for (i = 0; i < TILE_BYTES4; i++) {
        /* Nibble pairs 1..15, never 0, low nibble the even pixel. */
        uint8_t lo = (uint8_t)((i % 15) + 1);
        uint8_t hi = (uint8_t)(((i + 7) % 15) + 1);

        poke8(BG_WINDOW + TILESET_OFF + (uint32_t)i,
              (uint8_t)(lo | (hi << 4)));
    }
    for (i = 0; i < TILE_PAL8; i++) {
        pal15[i] = test_colour(i + 5);
        poke16(PAL_A + (uint32_t)i * 2u, pal15[i]);
    }
    poke16(BG_WINDOW, (uint16_t)0x3000u);   /* map entry 0: tile 0, palette 3 */
    poke16(BG0CNT_A, 0x0004u);              /* 16 colours, char base block 1 */
    hostmap_flush();

    memset(sSurface, 0xAA, sizeof sSurface);
    sRenderer(4);

    tile_palette(pal, pal15 + subpal / 2u, TILE_PAL4);
    tile_expand4(texels, (const uint8_t *)armrec_host_ptr(BG_WINDOW
                                                          + TILESET_OFF), pal);

    for (y = 0; y < TILE_SIDE; y++) {
        for (x = 0; x < TILE_SIDE; x++) {
            if (sSurface[0][y * 256 + x] != (texels[tile_swizzle(x, y)] >> 8)) {
                bad++;
            }
        }
    }
    CHECK("4bpp: the expanded tile is the renderer's own pixels", bad == 0);
    if (bad) {
        printf("  4bpp: %d of %d texels differ, first pixel %06X vs %06X\n",
               bad, TILE_TEXELS, sSurface[0][0], texels[0] >> 8);
    }

    /* The map entry is put back, because the checks above this one run again
     * in whatever order a later edit leaves them. */
    poke16(BG_WINDOW, 0u);
    poke16(BG0CNT_A, 0x0084u);
    hostmap_flush();
}

int main(void)
{
    extern void pc_gpu2d_install(void);
    unsigned long hits, misses;

    if (armrec_mem_init() != 0) {
        printf("gpu2d_render: init failed: %s\n", armrec_mem_strerror());
        return 2;
    }

    /* The memoising translator's own checks first: it is the thing under the
     * renderer, and a failure there would show up above as a wrong picture
     * with no indication of why. */
    {
        int ran = 0;
        int bad = hostmap_selftest(&ran);

        sRan += ran;
        sBad += bad;
        if (bad != 0) {
            printf("  %-56s FAILED (%d of %d)\n", "hostmap_selftest", bad, ran);
        }
    }

    /* The install the crt now makes, through 3ds_init.c's table. */
    pc_gpu2d_install();
    CHECK("pc_gpu2d_install() registered a renderer", sRenderer != NULL);
    CHECK("and named what it covers",
          sRendererName != NULL && strstr(sRendererName, "bg") != NULL);
    if (sRenderer == NULL) {
        printf("gpu2d_render: %d checks, %d failed\n", sRan, sBad + 1);
        return 1;
    }

    /* ---------------------------------------------------------- mapped */
    map_bank_a(1);
    write_picture();
    write_registers();
    hostmap_flush();

    memset(sSurface, 0xAA, sizeof sSurface);
    sRenderer(0);

    CHECK("every pixel is the white the palette holds",
          surface_uniform(0, WHITE_18));

    /* The reads really went through the memoising translator, and it really
     * memoised: a frame that missed on every access would be correct and
     * pointless. */
    hits = hostmap_hits();
    misses = hostmap_misses();
    CHECK("the renderer read through hostmap_ptr()", hits + misses > 0);
    CHECK("and almost never missed", misses < 64 && hits > 1000);
    CHECK("no block was left untranslatable", hostmap_uncacheable() == 0);

    /*
     * Engine B is off in POWCNT1, and a disabled engine B is WHITE, hardware's
     * answer, not a convention: 0x3F3F3F through expand() is 0xFFFFFF. (A
     * disabled engine A is black.) Checked because "the other surface is
     * untouched" and "the other surface is right" look the same from here
     * otherwise, and this test memsets both to 0xAA first.
     */
    CHECK("the disabled engine B is white", surface_uniform(1, 0x00FFFFFFu));

    /* ------------------------------------------------------- not mapped */
    /*
     * The bank goes away and nothing else changes. On hardware the window
     * reads zeros, so every tile is index 0 and the screen is the backdrop.
     * The point of the check is that this does not fault: on PC a floor under
     * the window answers, and here 3ds_hostmap.c's zero block does.
     */
    map_bank_a(0);
    memset(sSurface, 0xAA, sizeof sSurface);
    sRenderer(1);
    CHECK("an unmapped bank draws the backdrop, and does not fault",
          surface_uniform(0, 0u));

    /* ------------------------------------------------- and back again */
    /*
     * The cache is flushed by the model, not by this test: map_bank_a() only
     * writes VRAMCNT and calls armrec_vram_touch(). If 3ds_vram.c's weak call
     * to hostmap_flush() were not there, the white would come back from stale
     * pointers above and this check would pass for the wrong reason, so it
     * is the unmapped frame above that proves the flush, and this one proves
     * the remap.
     */
    map_bank_a(1);
    memset(sSurface, 0xAA, sizeof sSurface);
    sRenderer(2);
    CHECK("mapping it again brings the picture back",
          surface_uniform(0, WHITE_18));

    printf("  surface[0][0] = %06X, hostmap %lu hit / %lu miss / %lu uncached\n",
           sSurface[0][0], hostmap_hits(), hostmap_misses(),
           hostmap_uncacheable());

    /* ------------------------------------------- the PICA's own copy */
    /*
     * The tile cache, and last because it rewrites the palette and the tile the checks
     * above depend on, and because its own renders would otherwise clear the
     * translator's counters before the line above reads them.
     *
     * The tile expander turns a DS tile into texels for the GPU, and the only
     * question worth asking about it is whether its colours are the
     * *renderer's* colours: a palette entry's bit 15 is green's sixth bit
     * here, and six bits become eight by replicating the top two, so a
     * plausible-looking conversion is wrong by up to four counts a channel and
     * still looks like a picture. This is the oracle, the same tile, drawn
     * by pc_gpu2d.c and expanded by 3ds_tile.c, pixel for pixel.
     */
    tile_compare_8bpp();
    tile_compare_4bpp();

    /*
     * ...and the background path after them, because it rewrites the same tileset and
     * palette again for scenes of its own. What is under test here is the
     * quad emitter: the same frame, composed by pc_gpu2d.c and by
     * 3ds_gpu2d.c, over the memory model this console really has.
     */
    check_quads();
    /*
     * ...and the 3D layer after the background path, for the same reason in the other direction: it
     * writes a tileset of its own and puts it back, and what it adds to the
     * frame is a layer neither of the two above has.
     */
    check_layer3d();
    /*
     * ...and the effect pass after both, because its scenes are those layers with a
     * blend, a fade and a window over them, and because it leaves the
     * registers it found, which the tile cache checks below still read.
     */
    check_effects();
    check_sprites();
    check_display();
    {
        int ran = 0;
        int bad = tile_selftest(&ran);

        sRan += ran;
        sBad += bad;
        if (bad != 0) {
            printf("  %-56s FAILED (%d of %d)\n", "tile_selftest", bad, ran);
        }
    }
    /*
     * ...and the two self-tests the console also runs, here as well, because
     * a check that only a boot can run costs a boot to fail. The effect pass's predicate
     * changed what several of the survey's assertions should say and the
     * console found that out with a red corner block and a game that never
     * started; a build machine says it in a second. They run last because
     * gpu2d_selftest() puts registers in states no scene above would want.
     */
    {
        int ran = 0;
        int bad = gpu2d_selftest(&ran);

        sRan += ran;
        sBad += bad;
        if (bad != 0) {
            printf("  %-56s FAILED (%d of %d)\n", "gpu2d_selftest", bad, ran);
        }
    }
    {
        int ran = 0;
        int bad = effect_selftest(&ran);

        sRan += ran;
        sBad += bad;
        if (bad != 0) {
            printf("  %-56s FAILED (%d of %d)\n", "effect_selftest", bad, ran);
        }
    }

    armrec_mem_free();
    printf("gpu2d_render: %d checks, %d failed\n", sRan, sBad);
    return sBad != 0;
}
