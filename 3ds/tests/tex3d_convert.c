/*
 * 3ds/tests/tex3d_convert.c: 3ds_tex3d.c against the software renderer's own
 * texture unit, texel by texel, on a build machine.
 *
 * The oracle is render_pixel() and not a table. pc_gpu3d_soft.c is what the
 * console currently draws with, so the question worth asking is not "does the
 * converter match GBAtek"; it is "does it match the renderer this port already
 * ships". This file therefore includes that .c rather than linking it, because
 * texture_lookup() and render_pixel() are static and reaching them is the
 * point.
 *
 * render_pixel() and not texture_lookup(), because the lookup answers in RGB555
 * with a 0 to 31 alpha and the conversion to what reaches a surface happens
 * further down. Asking the lookup would leave this test checking half the
 * chain, with the half most likely to be wrong written twice. render_pixel()
 * with a white vertex colour and polygon alpha 31 is the identity over both of
 * the DS's combining steps, so its answer is the texel itself.
 *
 * What is covered: all eight formats, four image shapes including a
 * non-square one, the colour-zero-transparent bit both ways, and the palette
 * reached out of two different slots. Every texel of every one of them, which
 * is what makes this a proof rather than a sample: the formats that pack
 * several texels into a byte are exactly the ones where an off-by-one in the
 * shift draws a picture that looks almost right.
 *
 * And the address spaces. Texture memory is not a guest address; it is a
 * 512 KB space that VRAM banks are mapped into, so the test writes through the
 * LCDC window with the banks in LCDC, then remaps them to texture and palette
 * roles, which is the sequence the SDK's own texture load performs.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "armrec_rt.h"

#include "armrec_mem_3ds.h"
#include "pc_gpu3d.h"
#include "3ds_guest.h"
#include "3ds_tex3d.h"
#include "3ds_tile.h"
#include "3ds_vram.h"

/*
 * The renderer, included whole. It needs the geometry engine's two accessors,
 * which are pc_gpu3d.c's; stubs are enough here because nothing in this test
 * renders a frame, only the texture unit and the shading are exercised, and
 * neither reads a polygon list.
 */
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

/* The rest of what armrec_rt_3ds.c reaches for. Nothing in this test issues a
 * geometry command, so the engine is not installed and nothing is staged;
 * which is what gpu2d_render.c answers for the same three names. */
int pc_gpu3d_installed(void) { return 0; }
uint32_t *pc_gpu3d_stage(uint32_t addr) { (void)addr; return NULL; }
void pc_gpu3d_refresh_regs(void) { }

#include "pc_gpu3d_soft.c"

static int sRan;
static int sBad;

/* armrec_trap()'s fatal path. 3ds_fault.c has one, and it opens with <3ds.h>
 * and cannot be compiled here; nothing in this test should reach it. */
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

/* ------------------------------------------------------------------ */
/* Guest memory, written the way the SDK writes it                     */
/* ------------------------------------------------------------------ */

#define VRAMCNT_A   0x04000240u
#define LCDC_A      0x06800000u
#define LCDC_E      0x06880000u
#define LCDC_F      0x06890000u

/* Bank A is texture slot 0, bank B slot 1; E covers palette slots 0-3 and F
 * sits in slot 4. Two texture slots and two palette sources is what makes the
 * slot arithmetic testable; one of each would pass with the index wired to
 * zero. */
static void bank_lcdc(int bank)
{
    uint8_t *p = (uint8_t *)armrec_host_ptr(VRAMCNT_A + (uint32_t)bank);

    if (p != NULL) {
        *p = 0x80u;             /* enabled, mst 0 */
    }
    armrec_vram_touch();
}

static void bank_role(int bank, unsigned ofs)
{
    uint8_t *p = (uint8_t *)armrec_host_ptr(VRAMCNT_A + (uint32_t)bank);

    if (p != NULL) {
        *p = (uint8_t)(0x83u | (ofs << 3));   /* enabled, mst 3 */
    }
    armrec_vram_touch();
}

/* A deterministic fill: every byte value appears, and the sequence differs
 * between the two banks so a converter reading the wrong slot cannot pass. */
static void fill(uint32_t base, uint32_t bytes, uint32_t seed)
{
    uint32_t i;

    for (i = 0; i < bytes; i++) {
        uint8_t *p = (uint8_t *)armrec_host_ptr(base + i);

        if (p != NULL) {
            seed = seed * 1103515245u + 12345u;
            *p = (uint8_t)(seed >> 16);
        }
    }
}

/* ------------------------------------------------------------------ */
/* The comparison                                                      */
/* ------------------------------------------------------------------ */

/*
 * What the DS would put on a surface for this texel: render_pixel() with a
 * white vertex and an opaque polygon, then the six-to-eight expansion every
 * surface in this port goes through.
 */
static uint32_t oracle(uint32_t texparam, uint32_t texpal, int si, int ti)
{
    PcGxPolygon poly;
    uint32_t c, r, g, b, a;

    memset(&poly, 0, sizeof poly);
    poly.Attr = 31u << 16;              /* modulation, polygon alpha 31 */
    poly.TexParam = texparam;
    poly.TexPalette = texpal;

    if (((texparam >> 26) & 7u) == 0u) {
        /* An untextured polygon is the vertex colour, which says nothing about
         * a texture. The converter answers a transparent texel here and this
         * is the one case the oracle cannot be asked about. */
        return 0;
    }

    c = render_pixel(&poly, 63, 63, 63, (int16_t)(si << 4), (int16_t)(ti << 4));

    r = c & 0x3Fu;
    g = (c >> 8) & 0x3Fu;
    b = (c >> 16) & 0x3Fu;
    a = (c >> 24) & 0x3Fu;

    r = (r << 2) | (r >> 4);
    g = (g << 2) | (g >> 4);
    b = (b << 2) | (b >> 4);
    a = (a << 3) | (a >> 2);
    return (r << 24) | (g << 16) | (b << 8) | a;
}

/* Every texel of one image, through both the single-texel entry point and the
 * whole-image converter, the second of which also has to land each texel in
 * the swizzled place the tile cache's atlas arithmetic expects. */
static int compare_image(const char *what, uint32_t texparam, uint32_t texpal,
                         uint32_t *scratch)
{
    unsigned w = tex3d_width(texparam);
    unsigned h = tex3d_height(texparam);
    unsigned blockCols = w / TILE_SIDE;
    unsigned si, ti;
    int firstBad = -1;
    unsigned long bad = 0;

    if (tex3d_convert(scratch, texparam, texpal) != 0) {
        printf("  %-56s REFUSED\n", what);
        sRan++;
        sBad++;
        return 0;
    }

    for (ti = 0; ti < h; ti++) {
        for (si = 0; si < w; si++) {
            uint32_t want = oracle(texparam, texpal, (int)si, (int)ti);
            uint32_t got = tex3d_texel(texparam, texpal, (int)si, (int)ti);
            uint32_t packed = scratch[(size_t)((ti / TILE_SIDE) * blockCols
                                               + si / TILE_SIDE) * TILE_TEXELS
                                      + (unsigned)tile_swizzle((int)(si & 7),
                                                               (int)(ti & 7))];

            if (got != want || packed != want) {
                if (firstBad < 0) {
                    printf("    %s: (%u,%u) want %08X texel %08X packed %08X\n",
                           what, si, ti, want, got, packed);
                    firstBad = (int)(ti * w + si);
                }
                bad++;
            }
        }
    }

    sRan++;
    if (bad != 0) {
        printf("  %-56s FAILED (%lu of %u texels)\n", what, bad, w * h);
        sBad++;
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    static uint32_t scratch[TEX3D_MAX_TEXELS];
    static PcGxRenderRegs regs;
    unsigned long texels = 0;
    int i;

    if (armrec_mem_init() != 0) {
        printf("tex3d_convert: init failed: %s\n", armrec_mem_strerror());
        return 2;
    }

    /* DISP3DCNT bit 0: without it render_pixel() draws untextured and the
     * oracle would answer white for every format. */
    regs.DispCnt = 1u;
    sRegs = &regs;
    R = &regs;

    /* The banks in LCDC, filled, then put in their roles, GX_BeginLoadTex,
     * a DMA, GX_EndLoadTex. */
    bank_lcdc(0);
    bank_lcdc(1);
    bank_lcdc(4);
    bank_lcdc(5);
    fill(LCDC_A, 0x10000u, 0x1234u);
    fill(LCDC_A + 0x20000u, 0x10000u, 0x9ABCu);   /* bank B */
    fill(LCDC_E, 0x10000u, 0x5555u);
    fill(LCDC_F, 0x4000u, 0xAAAAu);
    bank_role(0, 0);        /* A -> texture slot 0 */
    bank_role(1, 1);        /* B -> texture slot 1 */
    bank_role(4, 0);        /* E -> palette slots 0-3 */
    bank_role(5, 2);        /* F -> palette slot 4 */
    latch_texture_slots();

    CHECK("bank A is texture slot 0", vram_texture(0) != NULL);
    CHECK("bank B is texture slot 1", vram_texture(1) != NULL);
    CHECK("bank E fills palette slots 0 to 3",
          vram_texpal(0) != NULL && vram_texpal(3) != NULL);
    CHECK("bank F is palette slot 4", vram_texpal(4) != NULL);
    CHECK("and slot 6 has no bank that can reach it", vram_texpal(6) == NULL);

    /*
     * Every format, over a 64x64 image out of texture slot 0 with its palette
     * in slot 0. 64x64 is 4,096 texels: enough that every packing repeats many
     * times and small enough that eight of them stay inside a second.
     */
    for (i = 1; i <= 7; i++) {
        char name[64];
        uint32_t texparam = ((uint32_t)i << 26) | (3u << 20) | (3u << 23);

        snprintf(name, sizeof name, "format %d, 64x64, palette slot 0", i);
        if (compare_image(name, texparam, 0x0000u, scratch)) {
            texels += 64u * 64u;
        }
    }

    /* Colour zero transparent, which is a bit of TexParam and not of the
     * palette, and only the three indexed formats read it. */
    for (i = 2; i <= 4; i++) {
        char name[64];
        uint32_t texparam = ((uint32_t)i << 26) | (3u << 20) | (3u << 23)
                          | (1u << 29);

        snprintf(name, sizeof name, "format %d, colour 0 transparent", i);
        if (compare_image(name, texparam, 0x0000u, scratch)) {
            texels += 64u * 64u;
        }
    }

    /*
     * A non-square image, which is where a width/height mix-up hides: a square
     * one indexes correctly under either. 128x32 in 16 colours.
     */
    if (compare_image("format 3, 128x32", (3u << 26) | (4u << 20) | (2u << 23),
                      0x0000u, scratch)) {
        texels += 128u * 32u;
    }

    /* The smallest and the largest this converter accepts. */
    if (compare_image("format 3, 8x8", (3u << 26), 0x0000u, scratch)) {
        texels += 64u;
    }
    if (compare_image("format 2, 256x256",
                      (2u << 26) | (5u << 20) | (5u << 23), 0x0000u, scratch)) {
        texels += 256u * 256u;
    }

    /* Out of the second texture slot, and against a palette in slot 4, the
     * one that comes from a different bank. Palette slot 4 is 0x10000 into
     * the palette space, which is `texpal << 4` for 0x1000. */
    if (compare_image("format 1, texture slot 1, palette slot 4",
                      (1u << 26) | (3u << 20) | (3u << 23) | 0x4000u,
                      0x1000u, scratch)) {
        texels += 64u * 64u;
    }

    /* Past the ceiling: refused, not truncated. */
    CHECK("a 512x512 image is refused",
          tex3d_convert(scratch, (3u << 26) | (6u << 20) | (6u << 23), 0) == -1);

    /* ------------------------------------------------------------ cache */
    {
        static uint32_t pool[TEX3D_MAX_TEXELS * 2];
        uint32_t tp = (3u << 26) | (3u << 20) | (3u << 23);
        struct tex3d_cache_stats st;
        int a, b, conv;

        CHECK("a pool too small for one image is refused",
              tex3d_cache_init(pool, 16) == -1);
        CHECK("and one that fits is taken",
              tex3d_cache_init(pool, sizeof pool / sizeof *pool) == 0);

        a = tex3d_cache_get(tp, 0, 1u, &conv);
        CHECK("the first ask converts", a >= 0 && conv == 1);
        CHECK("and the texels are what the converter produces",
              tex3d_cache_texels(a) != NULL
              && tex3d_convert(scratch, tp, 0) == 0
              && memcmp(tex3d_cache_texels(a), scratch,
                        64u * 64u * sizeof *scratch) == 0);
        CHECK("its size is the image's", tex3d_cache_size(a) == 64u * 64u);

        b = tex3d_cache_get(tp, 0, 1u, &conv);
        CHECK("the second ask hits", b == a && conv == 0);

        /* The wrap bits are sampler state, so they must not split an entry. */
        b = tex3d_cache_get(tp | (0xFu << 16) | (1u << 30), 0, 1u, &conv);
        CHECK("wrap and transform bits share one entry", b == a && conv == 0);

        /* A different palette is a different image. */
        b = tex3d_cache_get(tp, 0x0040u, 1u, &conv);
        CHECK("a different palette is its own entry", b != a && conv == 1);

        /* A write into a bank that is not this image's leaves it alone; one
         * into the bank holding its texture does not. */
        tex3d_cache_dirty(0x02000000u, 0x1000u);
        CHECK("a write outside VRAM invalidates nothing",
              tex3d_cache_get(tp, 0, 2u, &conv) == a && conv == 0);

        bank_lcdc(0);
        tex3d_cache_dirty(LCDC_A, 0x1000u);
        bank_role(0, 0);
        CHECK("a write into its texture bank re-converts",
              tex3d_cache_get(tp, 0, 3u, &conv) == a && conv == 1);

        bank_lcdc(4);
        tex3d_cache_dirty(LCDC_E + 0x8000u, 0x100u);
        bank_role(4, 0);
        CHECK("...and one into its palette bank does too",
              tex3d_cache_get(tp, 0, 4u, &conv) == a && conv == 1);

        tex3d_cache_stats(&st);
        CHECK("the counters add up", st.hits >= 3 && st.misses == 2
              && st.stale == 2 && st.refused == 0);

        /*
         * Overflow, which is the subtle rule. The pool is a bump allocator, so
         * emptying it in the middle of a frame would move images the caller
         * already holds entries for. It refuses for the rest of the frame
         * instead, and empties at the top of the next one.
         */
        tex3d_cache_reset();
        {
            const uint32_t *first;
            uint32_t firstTexels[16];
            int n = 0, refused = 0;

            first = NULL;
            while (n < 64) {
                /* Distinct images: the texture address is TexParam's low 16
                 * bits, moved a 64x64 4bpp image at a time. */
                uint32_t t = tp | (uint32_t)(n * 0x100);
                int e = tex3d_cache_get(t, 0, 10u, &conv);

                if (e < 0) {
                    refused = 1;
                    break;
                }
                if (n == 0) {
                    first = tex3d_cache_texels(e);
                    memcpy(firstTexels, first, sizeof firstTexels);
                }
                n++;
            }
            CHECK("the pool fills and then refuses", refused && n > 1);
            CHECK("and nothing already handed out moved",
                  first != NULL
                  && memcmp(first, firstTexels, sizeof firstTexels) == 0);

            /* The next frame empties it and the first image converts again. */
            CHECK("the next frame starts empty",
                  tex3d_cache_get(tp, 0, 11u, &conv) == 0 && conv == 1);
            tex3d_cache_stats(&st);
            CHECK("and the rebuild is counted once", st.evictions == 1);
        }

        tex3d_cache_reset();
        CHECK("a reset empties it", tex3d_cache_entries() == 0);
    }

    {
        int ran = 0;
        int bad = tex3d_selftest(&ran);

        sRan += ran;
        sBad += bad;
        if (bad != 0) {
            printf("  %-56s FAILED (%d of %d)\n", "tex3d_selftest", bad, ran);
        }
    }

    armrec_mem_free();
    printf("tex3d_convert: %d checks, %d failed, %lu texels compared\n",
           sRan, sBad, texels);
    return sBad != 0;
}
