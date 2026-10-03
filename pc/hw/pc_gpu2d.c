/*
 * The 2D engines' background, sprite and effect layers.
 *
 * Derived from melonDS's src/GPU2D_Soft.cpp and src/GPU2D.cpp (Copyright
 * 2016-2026 melonDS team, GPLv3-or-later). This port's licence permits lifting
 * melonDS's hardware cores and asks for them to be quarantined with their
 * provenance recorded, which is what pc/hw/ is. See pc/hw/README for what
 * "derived" means here, and for why this is C against our own VRAM rather than
 * their C++ against theirs: SoftRenderer2D wants a GPU2D&, a SoftRenderer& and
 * a GPU&, so lifting it means instantiating a melonDS::NDS inside the port,
 * which is a second DS in the process with a second VRAM model to keep in step.
 *
 * The oracle is `pcdiff-melon --bg-selftest`, which drives melonDS's own 2D
 * engine over 76 configurations and reports the pixels; pc/tests/test_bg.c
 * replays every one of them here and compares. That is the only way these
 * rules can be checked: a register's meaning is console behaviour and leaves
 * no trace in a ROM image.
 *
 * Nothing here is a guess about something untested. The places the model could
 * have invented an answer all abort with a message. Two are left, and both are
 * the same missing piece: DISPCNT's main-memory display mode and DISPCAPCNT's
 * bit 25, which each read the display FIFO.
 *
 * What is covered: BG0 as the 3D layer, read at the rasterizer's own
 * precision; the capture unit's two sources, four sizes, offsets and blend;
 * 128 sprites per scanline with both character mappings, all sixteen
 * shape/size pairs, both colour depths, the bitmap mappings, rotation and
 * scaling; and the three effect units, window, blend and mosaic. Every one of
 * those is in the sweep.
 *
 * Three of those are state machines across scanlines, which is the one thing a
 * per-frame renderer has to think about: a window's vertical range, its
 * horizontal range and the mosaic counters are latches the hardware advances
 * line by line. With the registers held still for a frame, each has a closed
 * form, and the sweep is what says the closed forms are right.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "armrec_rt.h"
#include "pc_gpu2d.h"
#include "../src/pc_bench.h"
#include "pc_workers.h"
#include "pc_gpu3d_soft.h"
#include "pc_video.h"

/* ------------------------------------------------------------------ */
/* Guest state, read where it is                                       */
/* ------------------------------------------------------------------ */

/*
 * Identity mapping means a register is a load from its
 * own address, so this needs neither an accessor layer nor a copy of the
 * register file. Engine B's block is engine A's plus 0x1000, which is the
 * whole of the difference between them in the address map, the differences
 * that matter are in the code below and every one is a `e->num` test.
 */
#define IO_A 0x04000000u
#define IO_B 0x04001000u

#define PAL_BASE 0x05000000u
#define PAL_MASK 0x000007FFu

/*
 * Guest address to host pointer, and the one place this file is not the same
 * on both hosts.
 *
 * On PC a guest address IS a host address, so every accessor below is a cast
 * the compiler folds away and the address space does the work, including the
 * VRAM window aliasing and the read-only floor under it. The 3DS can do none
 * of that: userland will not map 0x06000000, and which bank answers a VRAM
 * address depends on VRAMCNT. So there the accessors go through
 * 3ds/src/3ds_hostmap.c, which is armrec_host_ptr() memoised by 16 KB block,
 * VRAM's own placement grain, because a rasterizer asks tens of thousands of
 * times a frame. It never answers NULL: an unmapped address reads out of a
 * shared zero block, which is the floor this file already assumes.
 *
 * The macro is the whole of the difference. Nothing below it branches on a
 * host, so the two ports run the same renderer over the same state, which is
 * the only way the PC port can go on being this one's oracle.
 */
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
#define G2D_HOST(a) hostmap_fast((uint32_t)(a))
#else
#define G2D_HOST(a) ((void *)(uintptr_t)(a))
#endif

static inline uint8_t io8(uint32_t a) {
    return *(volatile uint8_t *)G2D_HOST(a);
}
static inline uint16_t io16(uint32_t a) {
    return *(volatile uint16_t *)G2D_HOST(a);
}
static inline uint32_t io32(uint32_t a) {
    return *(volatile uint32_t *)G2D_HOST(a);
}

/* Palette RAM is 2 KB and mirrors; engine B's half starts at 0x400. */
static inline uint16_t pal16(uint32_t off) {
    return *(volatile uint16_t *)G2D_HOST(PAL_BASE + (off & PAL_MASK));
}

/*
 * The 2D engines see VRAM through their own windows rather than through the
 * banks, which is exactly armrec's model: engine A's backgrounds are the
 * 512 KB at 0x06000000 and engine B's the 128 KB at 0x06200000, and an address
 * with no bank behind it reads zero because the window's floor is a read-only
 * zero mapping. So there is no bank lookup here at all, one masked load, and
 * on the 3DS one masked load and a memoised translation, which is G2D_HOST's
 * whole job above.
 */
#define ABG_BASE 0x06000000u
#define ABG_MASK 0x0007FFFFu
#define BBG_BASE 0x06200000u
#define BBG_MASK 0x0001FFFFu

/*
 * ...and the sprites see it through two more, which is the whole of what makes
 * an OBJ fetch different from a BG one: 256 KB at 0x06400000 for engine A and
 * 128 KB at 0x06600000 for engine B. The masks are the windows' own content
 * sizes in tools/armrec/armrec_rt.c's vram_win[], so a tile number that runs
 * off the end wraps exactly where the hardware's does.
 */
#define AOBJ_BASE 0x06400000u
#define AOBJ_MASK 0x0003FFFFu
#define BOBJ_BASE 0x06600000u
#define BOBJ_MASK 0x0001FFFFu

/* OAM is 2 KB of plain guest memory, engine A's 1 KB then engine B's. */
#define OAM_BASE 0x07000000u

/* ------------------------------------------------------------------ */
/* One engine's per-frame state                                        */
/* ------------------------------------------------------------------ */

/*
 * A pixel in `line` is six-bit r, g, b in three eight-bit lanes plus a layer
 * flag in the top byte, melonDS's packing, kept because the blend unit
 * needs to know which layer a pixel came from and reconstructing that later
 * would mean touching every DrawPixel again. `line[256 + i]` is the pixel the
 * top one covered, which is the second operand of every blend.
 */
/*
 * A pixel in `objline` is the sprite layer's own half-finished answer, and it
 * is *not* a colour: the OBJ layer is drawn once per scanline for all four
 * priorities at once, then interleaved between the backgrounds priority by
 * priority, so what a sprite leaves behind has to carry the palette lookup it
 * has not done yet along with the priority that decides when to do it. These
 * are melonDS's own bit assignments, kept for the same reason the BG flags are:
 * The blend unit reads the top byte, and renumbering them would mean
 * touching every store below twice.
 */
#define OBJ_STANDARD_PAL (1u << 12)
#define OBJ_DIRECT_COLOR (1u << 15)   /* not set here: it is the colour's own
                                       * bit 15, which a bitmap sprite has and
                                       * a palette index cannot */
#define OBJ_BGPRIO_MASK  (3u << 16)
#define OBJ_IS_OPAQUE    (1u << 18)
#define OBJ_OPAPRIO_MASK (OBJ_BGPRIO_MASK | OBJ_IS_OPAQUE)
#define OBJ_IS_SPRITE    (1u << 19)
#define OBJ_MOSAIC       (1u << 20)

struct engine {
    int num;                    /* 0 = A, 1 = B */
    uint32_t io;
    uint32_t dispcnt;
    uint16_t bgcnt[4];
    uint32_t vram_base, vram_mask;
    uint32_t obj_base, obj_mask;
    uint32_t oam;               /* guest address of this engine's OAM */
    uint32_t palbase;           /* 0x000 for A, 0x400 for B */
    int32_t  rot_x0[2], rot_y0[2];   /* the frame's reload values, BGxX/BGxY */
    int32_t  rot_x[2], rot_y[2];     /* ...advanced to the line being drawn */
    int16_t  rot_a[2], rot_b[2], rot_c[2], rot_d[2];
    uint32_t line[512];
    uint32_t objline[256];
    uint8_t  objwindow[256];
    int      nsprites;

    /* 3.4. WININ's two bytes then WINOUT's, which is the order the hardware
     * numbers the four regions in: inside window 0, inside window 1, outside
     * both, and inside the OBJ window. */
    uint8_t  wincnt[4];
    /* [window][x1, x2, y1, y2], and x1/y1 are the *high* byte of WINxH/WINxV,
     * which is the one thing about these registers that is easy to get
     * backwards. */
    uint8_t  wincoord[2][4];
    /* bit 0 is "this scanline is inside the vertical range" and bit 1 "this
     * pixel is inside the horizontal one"; the hardware keeps both as latches
     * rather than recomputing them, and both survive into the next scanline. */
    uint8_t  winactive[2];
    uint8_t  winmask[256];

    uint16_t blendcnt;
    uint8_t  eva, evb, evy;

    /* Set when BG0-as-3D drew this scanline, read by the high-resolution
     * settle below it. Per line, and on the engine because a band of lines
     * may be composed on a thread of its own. */
    int      hd3d_drew;

    /* MOSAIC's four nibbles: BG horizontal and vertical, then OBJ. */
    uint8_t  mos_bg_x, mos_bg_y, mos_obj_x, mos_obj_y;
    /* ...and the two lines those vertical counters latch, which is what a
     * mosaic layer reads instead of the line being drawn. */
    uint32_t bg_mos_line, obj_mos_line;
};

/*
 * A mosaic of size m repeats every m+1 pixels, so the phase at x is x % (m+1):
 * melonDS's MosaicTable, computed once here rather than written out. Built
 * lazily because pc_gpu2d_render() is reached both through pc_gpu2d_install()
 * and directly from pc/tests/test_bg.c.
 */
static uint8_t mosaic_table[16][256];
static int mosaic_table_ready;

static const uint8_t *mosaic_row(unsigned size) {
    if (!mosaic_table_ready) {
        unsigned m, x;
        for (m = 0; m < 16; m++)
            for (x = 0; x < 256; x++)
                mosaic_table[m][x] = (uint8_t)(x % (m + 1));
        mosaic_table_ready = 1;
    }
    return mosaic_table[size & 15];
}

static inline uint8_t vram8(const struct engine *e, uint32_t off) {
    return *(volatile uint8_t *)G2D_HOST(e->vram_base +
                                        (off & e->vram_mask));
}

/*
 * A host pointer for one row of tile pixels, taken once and then indexed.
 *
 * Why this exists and why it is not a tidy-up. On the desktop G2D_HOST is a
 * cast and the compiler hoists it out of the pixel loop by itself. Here it is
 * a translation, and the loop asks for three different 16 KB blocks per pixel:
 * The tile map, the tile data and the palette, so the one-entry inline
 * cache in front of it misses on nearly every access. Measured on this
 * console: 330,292 calls into the translator per frame, each of them a
 * cross-object call into a function that pushes ten registers, which is about
 * a fifth of a frame. Taking the pointer once per tile row instead of once
 * per pixel is what removes them.
 *
 * Why indexing it is safe. A row of a tile is four bytes at 4bpp or eight at
 * 8bpp, and `off` is aligned to its own row size, so every byte of the row is
 * inside the same aligned group, and therefore inside whatever contiguous
 * run the pointer came from, and on the same side of the VRAM window's mask.
 * A pointer taken for a longer or unaligned span would not be.
 */
static inline const volatile uint8_t *vram_row(const struct engine *e,
                                               uint32_t off) {
    return (const volatile uint8_t *)G2D_HOST(e->vram_base +
                                              (off & e->vram_mask));
}

/*
 * ...and the row of map entries a text layer reads for one scanline, which is
 * the other half of the same cost. 32 entries is 64 bytes, and every term of
 * the address above is a multiple of 64, the two bases, the row offset and
 * the second screen block's 0x800, so the row is aligned to its own size and
 * the argument vram_row() makes applies to it unchanged.
 *
 * A 512-pixel-wide map's second screen block is a second row 0x800 further on
 * rather than an index into this one: the gap between the two belongs to
 * neither, and a pointer spanning it would be the unaligned case the argument
 * above excludes.
 */
static inline const volatile uint16_t *vram_map_row(const struct engine *e,
                                                    uint32_t off) {
    return (const volatile uint16_t *)G2D_HOST(e->vram_base +
                                               (off & e->vram_mask));
}

/* ...and the whole palette, which is 2 KB and never crosses a block, so one
 * pointer serves every lookup a layer makes. `pal_at` applies the same mirror
 * mask `pal16` does, so the two read the same halfword for the same offset. */
static inline const volatile uint16_t *palette_ram(void) {
    return (const volatile uint16_t *)G2D_HOST(PAL_BASE);
}
static inline uint16_t pal_at(const volatile uint16_t *pal, uint32_t off) {
    return pal[(off & PAL_MASK) >> 1];
}
static inline uint16_t vram16(const struct engine *e, uint32_t off) {
    return *(volatile uint16_t *)G2D_HOST(e->vram_base +
                                          (off & e->vram_mask));
}
static inline uint8_t objvram8(const struct engine *e, uint32_t off) {
    return *(volatile uint8_t *)G2D_HOST(e->obj_base + (off & e->obj_mask));
}
static inline uint16_t objvram16(const struct engine *e, uint32_t off) {
    return *(volatile uint16_t *)G2D_HOST(e->obj_base +
                                          (off & e->obj_mask));
}

/* Halfword `i` of this engine's OAM. An attribute is three of these plus one
 * belonging to a rotation/scaling group, which is why the hardware's own index
 * is halfwords rather than sprites. */
static inline uint16_t oam16(const struct engine *e, uint32_t i) {
    return *(volatile uint16_t *)G2D_HOST(e->oam + i * 2u);
}

/*
 * An extended-palette slot that no bank is mapped to reads as zeros rather
 * than being skipped, an 8bpp pixel with a nonzero index still draws, and it
 * draws black. Handing back a shared zero page says that in the type system
 * instead of in a branch at every lookup.
 */
static const uint16_t extpal_zero[16 * 256];

static const uint16_t *bg_extpal(const struct engine *e, int slot, uint32_t pal) {
    void *p = armrec_vram_extpal(e->num ? ARMREC_EXTPAL_BBG
                                        : ARMREC_EXTPAL_ABG, slot);
    if (!p) return extpal_zero + pal * 256;
    return (const uint16_t *)((const char *)p + pal * 512);
}

/* The OBJ side has one 8 KB slot rather than four, and the palette number is
 * carried in the pixel rather than applied here, InterleaveSprites indexes
 * all 4,096 entries at once. Same zero page when no bank is mapped. */
static const uint16_t *obj_extpal(const struct engine *e) {
    void *p = armrec_vram_extpal(e->num ? ARMREC_EXTPAL_BOBJ
                                        : ARMREC_EXTPAL_AOBJ, 0);
    return p ? (const uint16_t *)p : extpal_zero;
}

/* ------------------------------------------------------------------ */
/* Pixels                                                              */
/* ------------------------------------------------------------------ */

/*
 * Fifteen-bit BGR to the six-bit lanes. Green picks up bit 15 as its low bit,
 * which is not a rounding choice: the 3D engine's output is six bits per
 * channel and the 2D path carries the sixth green bit in the halfword's top
 * bit, so every palette entry with bit 15 set is one step greener. Half the
 * entries in the sweep have it, so this is measured rather than inherited.
 */
static inline uint32_t rgb15to18(uint16_t c) {
    uint32_t r = (uint32_t)(c & 0x001F) << 1;
    uint32_t g = (uint32_t)((c & 0x03E0) >> 4) | (uint32_t)((c & 0x8000) >> 15);
    uint32_t b = (uint32_t)(c & 0x7C00) >> 9;
    return r | (g << 8) | (b << 16);
}

/*
 * ...and the same conversion without that green bit, which is what the VRAM
 * display mode uses. The two differ by one step on half of all halfwords, so
 * this is a real distinction rather than a tidy-up: the display mode scans a
 * bank out as a picture, where bit 15 is nothing at all.
 */
static inline uint32_t rgb15to18_flat(uint16_t c) {
    return ((uint32_t)(c & 0x001F) << 1)
         | ((uint32_t)((c & 0x03E0) >> 4) << 8)
         | ((uint32_t)((c & 0x7C00) >> 9) << 16);
}

static inline void draw_pixel(uint32_t *dst, uint16_t color, uint32_t flag) {
    *(dst + 256) = *dst;
    *dst = rgb15to18(color) | flag;
}

/* Six bits per channel out to eight, replicating the top two bits, the same
 * expansion melonDS's ExpandColor does, so a comparison against it is exact
 * rather than within a rounding step. */
static inline uint32_t expand(uint32_t c) {
    uint32_t r = c & 0x3F, g = (c >> 8) & 0x3F, b = (c >> 16) & 0x3F;
    r = (r << 2) | (r >> 4);
    g = (g << 2) | (g >> 4);
    b = (b << 2) | (b >> 4);
    return (r << 16) | (g << 8) | b;
}

static void gpu2d_trap(const char *what, uint32_t reg, uint32_t val);

/* ------------------------------------------------------------------ */
/* Windows.                                           */
/* ------------------------------------------------------------------ */

/*
 * A window is two coordinate pairs and a six-bit enable mask, and the unit's
 * whole output is one byte per pixel saying which layers may draw there: bits
 * 0-3 the four backgrounds, bit 4 the sprites, bit 5 the colour effect. That is
 * this tree's own account of it, not melonDS's, registers.h gives
 * REG_G2_WINOUT_WINOUT_SIZE as 6 and GX_g2.h's GXWndPlane spells the five plane
 * bits and the effect bit separately.
 *
 * What is not a function of the line number. Both halves of "is this pixel
 * inside" are latches. The vertical one flips at the two scanlines WINxV names
 * and is updated at the top of *every* scanline, visible or not, whether or not
 * the window is enabled; the horizontal one flips at the two pixels WINxH names
 * as the mask is scanned left to right, and it is *not* reset per scanline.
 * That is what makes Y1 > Y2 and X1 > X2 wrap rather than draw nothing, and it
 * is why engine_begin() warms both up before line 0 instead of starting from
 * zero: the frame inherits whatever the frame before it left.
 */
static void win_update_y(struct engine *e, uint32_t line) {
    int w;
    for (w = 0; w < 2; w++) {
        /* Ordered: at a line that is both bounds the clear wins, so a window
         * whose two coordinates are equal is off rather than full height. */
        if ((line & 0xFFu) == e->wincoord[w][3]) e->winactive[w] &= (uint8_t)~1u;
        else if ((line & 0xFFu) == e->wincoord[w][2]) e->winactive[w] |= 1u;
    }
}

/* One window's contribution, left to right. `mask` is NULL for the warm-up
 * pass, which advances the latch and writes nothing, the same loop, so the
 * state line 0 starts from is produced by the code that produces every other
 * line's rather than by a rule about it. */
static void win_scan(struct engine *e, int w, uint8_t *mask) {
    int i;
    for (i = 0; i < 256; i++) {
        if (i == (int)e->wincoord[w][1]) e->winactive[w] &= (uint8_t)~2u;
        else if (i == (int)e->wincoord[w][0]) e->winactive[w] |= 2u;
        if (mask && e->winactive[w] == 3) mask[i] = e->wincnt[w];
    }
}

/*
 * Precedence is window 0, then window 1, then the OBJ window, then outside,
 * expressed as "write the weakest first and let the stronger overwrite it",
 * which is also how the hardware's priority encoder reads.
 */
static void calculate_window_mask(struct engine *e) {
    int i;

    memset(e->winmask, e->wincnt[2], sizeof e->winmask);

    if (e->dispcnt & (1u << 15)) {
        for (i = 0; i < 256; i++)
            if (e->objwindow[i]) e->winmask[i] = e->wincnt[3];
    }
    if (e->dispcnt & (1u << 14)) win_scan(e, 1, e->winmask);
    if (e->dispcnt & (1u << 13)) win_scan(e, 0, e->winmask);
}

/* ------------------------------------------------------------------ */
/* The blend unit.                                    */
/* ------------------------------------------------------------------ */

/*
 * `line[i]` is the top pixel and `line[256 + i]` the one it covered, which is
 * what draw_pixel() keeps for exactly this. The top byte
 * of each says which layer it came from: 0x01..0x08 the four backgrounds, 0x10
 * a sprite, 0x20 the backdrop, 0x40 the 3D layer, 0x80 a semi-transparent
 * sprite, and a bitmap sprite is 0xC0 plus its own alpha in the low five
 * bits, which is the one case where the flag carries a number.
 *
 * The blends themselves are fixed-point with a rounding bias, and the biases
 * are not symmetric: 8 for a 4-bit blend, 8 up and 7 down for brightness.
 * Getting one of them wrong is a one-step error on half the inputs, which is
 * exactly what a pixel-for-pixel sweep can see and a screenshot cannot.
 */
static uint32_t color_blend4(uint32_t v1, uint32_t v2, uint32_t eva,
                             uint32_t evb) {
    uint32_t r =  (((v1 & 0x00003Fu) * eva) + ((v2 & 0x00003Fu) * evb)
                   + 0x000008u) >> 4;
    uint32_t g = ((((v1 & 0x003F00u) * eva) + ((v2 & 0x003F00u) * evb)
                   + 0x000800u) >> 4) & 0x007F00u;
    uint32_t b = ((((v1 & 0x3F0000u) * eva) + ((v2 & 0x3F0000u) * evb)
                   + 0x080000u) >> 4) & 0x7F0000u;

    if (r > 0x00003Fu) r = 0x00003Fu;
    if (g > 0x003F00u) g = 0x003F00u;
    if (b > 0x3F0000u) b = 0x3F0000u;
    return r | g | b | 0xFF000000u;
}

static uint32_t color_bright_up(uint32_t v, uint32_t factor, uint32_t bias) {
    uint32_t rb = v & 0x3F003Fu, g = v & 0x003F00u;
    rb += ((((0x3F003Fu - rb) * factor) + (bias * 0x010001u)) >> 4) & 0x3F003Fu;
    g  += ((((0x003F00u - g)  * factor) + (bias * 0x000100u)) >> 4) & 0x003F00u;
    return rb | g | 0xFF000000u;
}

static uint32_t color_bright_down(uint32_t v, uint32_t factor, uint32_t bias) {
    uint32_t rb = v & 0x3F003Fu, g = v & 0x003F00u;
    rb -= (((rb * factor) + (bias * 0x010001u)) >> 4) & 0x3F003Fu;
    g  -= (((g  * factor) + (bias * 0x000100u)) >> 4) & 0x003F00u;
    return rb | g | 0xFF000000u;
}

/*
 * The blend the 3D layer does with what is under it, which is the one effect
 * whose coefficients are not in a register: a 3D pixel carries five bits of
 * its own alpha and blends at 32 steps rather than the other four effects'
 * 16, so the rounding constant and the shift are both one larger. Alpha 31 is
 * opaque and returns without touching the second operand, which is not an
 * optimisation, 32/32 and 0/32 would round the same way but the flag byte
 * would not survive.
 */
static uint32_t color_blend5(uint32_t val1, uint32_t val2) {
    uint32_t eva = ((val1 >> 24) & 0x1Fu) + 1u, evb = 32u - eva;
    uint32_t r, g, b;

    if (eva == 32u) return val1;

    r =  (((val1 & 0x00003Fu) * eva) + ((val2 & 0x00003Fu) * evb) + 0x000010u) >> 5;
    g = ((((val1 & 0x003F00u) * eva) + ((val2 & 0x003F00u) * evb) + 0x001000u) >> 5)
        & 0x007F00u;
    b = ((((val1 & 0x3F0000u) * eva) + ((val2 & 0x3F0000u) * evb) + 0x100000u) >> 5)
        & 0x7F0000u;

    if (r > 0x00003Fu) r = 0x00003Fu;
    if (g > 0x003F00u) g = 0x003F00u;
    if (b > 0x3F0000u) b = 0x3F0000u;
    return r | g | b | 0xFF000000u;
}

/*
 * The blend unit's five inputs, spelled out rather than reached through the
 * engine, so that the high-resolution 3D path can run this same function over
 * a scanline's recorded state after the engine has moved on. color_composite()
 * below is the wrapper every ordinary pixel still goes through, and there is
 * one implementation of the arithmetic.
 */
static uint32_t color_composite_at(uint32_t blendcnt, uint32_t e_eva,
                                   uint32_t e_evb, uint32_t e_evy,
                                   uint32_t winmask, uint32_t val1,
                                   uint32_t val2) {
    uint32_t coloreffect = 0, eva = 0, evb = 0;
    uint32_t flag1 = val1 >> 24, flag2 = val2 >> 24;
    uint32_t target2;

    /* The second operand's BLDCNT bit. A semi-transparent sprite counts as the
     * OBJ target and the 3D layer as BG0's, whatever they are actually over. */
    if      (flag2 & 0x80u) target2 = 0x1000u;
    else if (flag2 & 0x40u) target2 = 0x0100u;
    else                    target2 = flag2 << 8;

    if ((flag1 & 0x80u) && (blendcnt & target2)) {
        /*
         * A semi-transparent sprite blends whatever BLDCNT's *effect* field
         * says and whether or not OBJ is a target-1 layer; it only needs the
         * layer underneath to be a target-2 one. That asymmetry is the whole
         * reason this is not one branch, and it is also the reason the sweep
         * was blind to a renderer that dropped the alpha flag.
         */
        coloreffect = 1;
        if (flag1 & 0x40u) { eva = flag1 & 0x1Fu; evb = 16u - eva; }
        else               { eva = e_eva; evb = e_evb; }
    } else if ((flag1 & 0x40u) && (blendcnt & target2)) {
        /*
         * The 3D layer over a target-2 layer, which blends by the 3D pixel's
         * own alpha. Note what is *not* tested here: neither BLDCNT's effect
         * field nor its BG0 target-1 bit, and not the window's colour-effect
         * bit either. A translucent 3D pixel blends with what it covers
         * whatever the blend unit has been told to do, and the only thing it
         * needs is a layer underneath that BLDCNT admits as a second operand.
         */
        coloreffect = 4;
    } else {
        if      (flag1 & 0x80u) flag1 = 0x10u;
        else if (flag1 & 0x40u) flag1 = 0x01u;

        if ((blendcnt & flag1) && (winmask & 0x20u)) {
            coloreffect = (blendcnt >> 6) & 3u;
            if (coloreffect == 1) {
                /* Alpha with nothing blendable underneath is not a blend
                 * against the backdrop; it is no effect at all. */
                if (blendcnt & target2) { eva = e_eva; evb = e_evb; }
                else coloreffect = 0;
            }
        }
    }

    switch (coloreffect) {
    case 0: return val1;
    case 1: return color_blend4(val1, val2, eva, evb);
    case 2: return color_bright_up(val1, e_evy, 8);
    case 3: return color_bright_down(val1, e_evy, 7);
    default: return color_blend5(val1, val2);
    }
}

static uint32_t color_composite(struct engine *e, int i, uint32_t val1,
                                uint32_t val2) {
    return color_composite_at(e->blendcnt, e->eva, e->evb, e->evy,
                              e->winmask[i], val1, val2);
}

/* ------------------------------------------------------------------ */
/* Text backgrounds                                                    */
/* ------------------------------------------------------------------ */

static void draw_bg_text(struct engine *e, uint32_t line, int bgnum) {
    uint16_t bgcnt = e->bgcnt[bgnum];
    uint32_t tilesetaddr, tilemapaddr;
    uint32_t palofs;
    int extpal, extpalslot = 0;
    uint16_t xoff = io16(e->io + 0x010 + (uint32_t)bgnum * 4);
    /*
     * The two halves of mosaic are separately conditioned and that is not a
     * tidy-up: BGxCNT bit 6 alone makes the layer read the *latched* line, so
     * a layer with the mosaic bit set and MOSAIC's horizontal nibble at zero
     * is still vertically blocky. The horizontal half additionally needs a
     * nonzero size, because a size of zero would be an identity table.
     */
    uint16_t yoff = (uint16_t)(io16(e->io + 0x012 + (uint32_t)bgnum * 4)
                               + ((bgcnt & (1 << 6)) ? e->bg_mos_line : line));
    int mosaic = (bgcnt & (1 << 6)) && e->mos_bg_x;
    const uint8_t *mostab = mosaic ? mosaic_row(e->mos_bg_x) : NULL;
    uint32_t lastxpos = 0;
    uint32_t widexmask = (bgcnt & (1 << 14)) ? 0x100u : 0u;
    uint16_t curtile = 0;
    uint32_t pixelsaddr = 0;
    const uint16_t *curpal;
    /* Taken once per tile row and once per layer respectively; see
     * vram_row(). Both start where the offsets they stand in for start, so a
     * layer whose first tile fetch has not happened yet reads what
     * vram8(e, 0) and pal16(0) would have read. */
    const volatile uint8_t *pixels = vram_row(e, 0);
    const volatile uint16_t *palram = palette_ram();
    /* ...and the map row, once per layer per scanline; see vram_map_row().
     * Taken below, where the address is finished. */
    const volatile uint16_t *map0, *map1;
    int i;

    /*
     * BGxCNT bit 13 is the wrap bit on an affine layer and the extended-palette
     * slot select on a text one, and only on BG0 and BG1, BG2 and BG3 always
     * take slots 2 and 3. That asymmetry is why the sweep has two configs that
     * differ only in which layer is enabled.
     */
    extpal = (e->dispcnt & (1u << 30)) != 0;
    if (extpal)
        extpalslot = (bgnum < 2 && (bgcnt & 0x2000)) ? (2 + bgnum) : bgnum;

    if (e->num) {
        tilesetaddr = (uint32_t)(bgcnt & 0x003C) << 12;
        tilemapaddr = (uint32_t)(bgcnt & 0x1F00) << 3;
    } else {
        /* DISPCNT's two 64 KB bases are engine A's alone; engine B has neither
         * field, and applying them to it would put every sub-screen tile in
         * the wrong place. */
        tilesetaddr = ((e->dispcnt & 0x07000000u) >> 8)
                    + ((uint32_t)(bgcnt & 0x003C) << 12);
        tilemapaddr = ((e->dispcnt & 0x38000000u) >> 11)
                    + ((uint32_t)(bgcnt & 0x1F00) << 3);
    }
    palofs = e->palbase;

    /* Row of map entries. A 64-row map is 0x1F8 of y and carries the second
     * screen block; a 32-row one wraps at 0xF8. */
    if (bgcnt & (1 << 15)) {
        tilemapaddr += ((uint32_t)yoff & 0x1F8u) << 3;
        if (bgcnt & (1 << 14))
            tilemapaddr += ((uint32_t)yoff & 0x100u) << 3;
    } else {
        tilemapaddr += ((uint32_t)yoff & 0xF8u) << 3;
    }

    map0 = vram_map_row(e, tilemapaddr);
    map1 = widexmask ? vram_map_row(e, tilemapaddr + 0x800u) : map0;

    if (bgcnt & (1 << 7)) {
        /* 256 colours, one byte per pixel. */
        curpal = NULL;
        if ((xoff & 7) || mosaic) {
            curtile = (((uint32_t)xoff & widexmask) ? map1 : map0)
                          [((uint32_t)xoff & 0xF8u) >> 3];
            curpal = extpal ? bg_extpal(e, extpalslot, curtile >> 12) : NULL;
            pixelsaddr = tilesetaddr + ((uint32_t)(curtile & 0x03FF) << 6)
                       + ((uint32_t)((curtile & (1 << 11)) ? (7 - (yoff & 7))
                                                           : (yoff & 7)) << 3);
            pixels = vram_row(e, pixelsaddr);
        }
        lastxpos = xoff;
        for (i = 0; i < 256; i++) {
            /* Mosaic pulls the sample back to the block's first column, which
             * can take it below zero and wrap; the masks below are what the
             * hardware's address arithmetic does with that, so it is left to
             * wrap rather than clamped. */
            uint32_t xpos = mosaic ? (uint32_t)(xoff - mostab[i])
                                   : (uint32_t)xoff;
            if ((!mosaic && !(xpos & 7))
                || (mosaic && ((xpos >> 3) != (lastxpos >> 3)))) {
                curtile = ((xpos & widexmask) ? map1 : map0)
                              [(xpos & 0xF8u) >> 3];
                curpal = extpal ? bg_extpal(e, extpalslot, curtile >> 12)
                                : NULL;
                pixelsaddr = tilesetaddr + ((uint32_t)(curtile & 0x03FF) << 6)
                           + ((uint32_t)((curtile & (1 << 11))
                                             ? (7 - (yoff & 7))
                                             : (yoff & 7)) << 3);
                pixels = vram_row(e, pixelsaddr);
                if (mosaic) lastxpos = xpos;
            }
            if (e->winmask[i] & (1u << bgnum)) {
                uint32_t tx = (curtile & (1 << 10)) ? (7u - (xpos & 7u))
                                                    : (xpos & 7u);
                uint8_t color = pixels[tx];
                if (color)
                    draw_pixel(&e->line[i],
                               curpal ? curpal[color]
                                      : pal_at(palram,
                                               palofs + (uint32_t)color * 2),
                               0x01000000u << bgnum);
            }
            xoff++;
        }
    } else {
        /* 16 colours, one nibble per pixel; the map entry's top nibble picks
         * the palette and extended palettes do not apply. */
        uint32_t subpal = 0;
        if ((xoff & 7) || mosaic) {
            curtile = (((uint32_t)xoff & widexmask) ? map1 : map0)
                          [((uint32_t)xoff & 0xF8u) >> 3];
            subpal = ((uint32_t)(curtile & 0xF000) >> 12) * 32u;
            pixelsaddr = tilesetaddr + ((uint32_t)(curtile & 0x03FF) << 5)
                       + ((uint32_t)((curtile & (1 << 11)) ? (7 - (yoff & 7))
                                                           : (yoff & 7)) << 2);
            pixels = vram_row(e, pixelsaddr);
        }
        lastxpos = xoff;
        for (i = 0; i < 256; i++) {
            uint32_t xpos = mosaic ? (uint32_t)(xoff - mostab[i])
                                   : (uint32_t)xoff;
            if ((!mosaic && !(xpos & 7))
                || (mosaic && ((xpos >> 3) != (lastxpos >> 3)))) {
                curtile = ((xpos & widexmask) ? map1 : map0)
                              [(xpos & 0xF8u) >> 3];
                subpal = ((uint32_t)(curtile & 0xF000) >> 12) * 32u;
                pixelsaddr = tilesetaddr + ((uint32_t)(curtile & 0x03FF) << 5)
                           + ((uint32_t)((curtile & (1 << 11))
                                             ? (7 - (yoff & 7))
                                             : (yoff & 7)) << 2);
                pixels = vram_row(e, pixelsaddr);
                if (mosaic) lastxpos = xpos;
            }
            if (e->winmask[i] & (1u << bgnum)) {
                uint32_t tx = (curtile & (1 << 10)) ? (7u - (xpos & 7u))
                                                    : (xpos & 7u);
                uint8_t b = pixels[tx >> 1];
                uint8_t color = (tx & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 15);
                if (color)
                    draw_pixel(&e->line[i],
                               pal_at(palram,
                                      palofs + subpal + (uint32_t)color * 2),
                               0x01000000u << bgnum);
            }
            xoff++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Affine and extended backgrounds                                     */
/* ------------------------------------------------------------------ */

/*
 * BGxCNT's size field means four different things depending on the layer type,
 * which is the single largest source of "looks right, is wrong" in a 2D
 * engine. Each of the four tables below is one of them and the sweep drives
 * every entry of every one.
 */
static void affine_coord_mask(uint16_t bgcnt, uint32_t *coordmask,
                              uint32_t *yshift) {
    switch ((bgcnt >> 14) & 3) {
    case 0: *coordmask = 0x07800; *yshift = 7; break;
    case 1: *coordmask = 0x0F800; *yshift = 8; break;
    case 2: *coordmask = 0x1F800; *yshift = 9; break;
    default: *coordmask = 0x3F800; *yshift = 10; break;
    }
}

/*
 * The horizontal mosaic sample on a rotated layer, which is not "round the
 * screen x down"; it is the texel the block's first *screen* column maps to,
 * so it walks back along the layer's own axes. `im` is at most 15 and the two
 * matrix entries are 16-bit, so the products cannot overflow.
 */
static inline void mosaic_step_back(int im, int32_t rotA, int32_t rotC,
                                    int32_t *x, int32_t *y) {
    *x = (int32_t)((uint32_t)*x - (uint32_t)(im * rotA));
    *y = (int32_t)((uint32_t)*y - (uint32_t)(im * rotC));
}

static void draw_bg_affine(struct engine *e, int bgnum) {
    uint16_t bgcnt = e->bgcnt[bgnum];
    uint32_t tilesetaddr, tilemapaddr, coordmask, yshift, overflowmask;
    int32_t rotX = e->rot_x[bgnum - 2], rotY = e->rot_y[bgnum - 2];
    int32_t rotA = e->rot_a[bgnum - 2], rotC = e->rot_c[bgnum - 2];
    int mosaic = (bgcnt & (1 << 6)) && e->mos_bg_x;
    const uint8_t *mostab = mosaic ? mosaic_row(e->mos_bg_x) : NULL;
    int i;

    affine_coord_mask(bgcnt, &coordmask, &yshift);
    /* Bit 13 set wraps the map; clear leaves everything outside it
     * transparent, which is what the mask tests below say. */
    overflowmask = (bgcnt & (1 << 13)) ? 0u : ~(coordmask | 0x7FFu);

    if (e->num) {
        tilesetaddr = (uint32_t)(bgcnt & 0x003C) << 12;
        tilemapaddr = (uint32_t)(bgcnt & 0x1F00) << 3;
    } else {
        tilesetaddr = ((e->dispcnt & 0x07000000u) >> 8)
                    + ((uint32_t)(bgcnt & 0x003C) << 12);
        tilemapaddr = ((e->dispcnt & 0x38000000u) >> 11)
                    + ((uint32_t)(bgcnt & 0x1F00) << 3);
    }
    yshift -= 3;

    for (i = 0; i < 256; i++) {
        if (e->winmask[i] & (1u << bgnum)) {
            int32_t fx = rotX, fy = rotY;
            if (mosaic) mosaic_step_back(mostab[i], rotA, rotC, &fx, &fy);
            if (!(((uint32_t)fx | (uint32_t)fy) & overflowmask)) {
                uint8_t curtile = vram8(e, tilemapaddr
                    + (((((uint32_t)fy & coordmask) >> 11) << yshift)
                       + (((uint32_t)fx & coordmask) >> 11)));
                uint32_t tx = ((uint32_t)fx >> 8) & 7u;
                uint32_t ty = ((uint32_t)fy >> 8) & 7u;
                uint8_t color = vram8(e, tilesetaddr + ((uint32_t)curtile << 6)
                                         + (ty << 3) + tx);
                if (color)
                    draw_pixel(&e->line[i],
                               pal16(e->palbase + (uint32_t)color * 2),
                               0x01000000u << bgnum);
            }
        }
        rotX = (int32_t)((uint32_t)rotX + (uint32_t)rotA);
        rotY = (int32_t)((uint32_t)rotY + (uint32_t)rotC);
    }
}

static void draw_bg_extended(struct engine *e, int bgnum) {
    uint16_t bgcnt = e->bgcnt[bgnum];
    int32_t rotX = e->rot_x[bgnum - 2], rotY = e->rot_y[bgnum - 2];
    int32_t rotA = e->rot_a[bgnum - 2], rotC = e->rot_c[bgnum - 2];
    int extpal = (e->dispcnt & (1u << 30)) != 0;
    int mosaic = (bgcnt & (1 << 6)) && e->mos_bg_x;
    const uint8_t *mostab = mosaic ? mosaic_row(e->mos_bg_x) : NULL;
    int i;

    if (bgcnt & (1 << 7)) {
        /* The two bitmap forms. The size field is a pair of coordinate masks
         * here rather than a map size, and 2 and 3 differ only in y. */
        uint32_t xmask, ymask, yshift, ofxmask, ofymask, tilemapaddr;
        switch ((bgcnt >> 14) & 3) {
        case 0: xmask = 0x07FFF; ymask = 0x07FFF; yshift = 7; break;
        case 1: xmask = 0x0FFFF; ymask = 0x0FFFF; yshift = 8; break;
        case 2: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
        default: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
        }
        ofxmask = (bgcnt & (1 << 13)) ? 0u : ~xmask;
        ofymask = (bgcnt & (1 << 13)) ? 0u : ~ymask;
        tilemapaddr = (uint32_t)(bgcnt & 0x1F00) << 6;

        if (bgcnt & (1 << 2)) {
            /* Direct colour: a halfword per pixel, drawn only when its top bit
             * is set. That bit is the alpha, so it is not the green bit
             * rgb15to18() reads; the value is masked before it gets there. */
            for (i = 0; i < 256; i++) {
                if (e->winmask[i] & (1u << bgnum)) {
                    int32_t fx = rotX, fy = rotY;
                    if (mosaic)
                        mosaic_step_back(mostab[i], rotA, rotC, &fx, &fy);
                    if (!((uint32_t)fx & ofxmask)
                        && !((uint32_t)fy & ofymask)) {
                        uint16_t color = vram16(e, tilemapaddr
                            + (((((uint32_t)fy & ymask) >> 8) << yshift)
                               + (((uint32_t)fx & xmask) >> 8)) * 2u);
                        if (color & 0x8000)
                            draw_pixel(&e->line[i], (uint16_t)(color & 0x7FFF),
                                       0x01000000u << bgnum);
                    }
                }
                rotX = (int32_t)((uint32_t)rotX + (uint32_t)rotA);
                rotY = (int32_t)((uint32_t)rotY + (uint32_t)rotC);
            }
        } else {
            for (i = 0; i < 256; i++) {
                if (e->winmask[i] & (1u << bgnum)) {
                    int32_t fx = rotX, fy = rotY;
                    if (mosaic)
                        mosaic_step_back(mostab[i], rotA, rotC, &fx, &fy);
                    if (!((uint32_t)fx & ofxmask)
                        && !((uint32_t)fy & ofymask)) {
                        uint8_t color = vram8(e, tilemapaddr
                            + ((((uint32_t)fy & ymask) >> 8) << yshift)
                            + (((uint32_t)fx & xmask) >> 8));
                        if (color)
                            draw_pixel(&e->line[i],
                                       pal16(e->palbase + (uint32_t)color * 2),
                                       0x01000000u << bgnum);
                    }
                }
                rotX = (int32_t)((uint32_t)rotX + (uint32_t)rotA);
                rotY = (int32_t)((uint32_t)rotY + (uint32_t)rotC);
            }
        }
        return;
    }

    /* Mixed affine/text: an affine layer whose map entries are halfwords, so
     * it gets flips and a palette number that a plain affine layer has not.
     * Its extended-palette slot is the layer number with no bit 13 case. */
    {
        uint32_t tilesetaddr, tilemapaddr, coordmask, yshift, overflowmask;

        affine_coord_mask(bgcnt, &coordmask, &yshift);
        overflowmask = (bgcnt & (1 << 13)) ? 0u : ~(coordmask | 0x7FFu);

        if (e->num) {
            tilesetaddr = (uint32_t)(bgcnt & 0x003C) << 12;
            tilemapaddr = (uint32_t)(bgcnt & 0x1F00) << 3;
        } else {
            tilesetaddr = ((e->dispcnt & 0x07000000u) >> 8)
                        + ((uint32_t)(bgcnt & 0x003C) << 12);
            tilemapaddr = ((e->dispcnt & 0x38000000u) >> 11)
                        + ((uint32_t)(bgcnt & 0x1F00) << 3);
        }
        yshift -= 3;

        for (i = 0; i < 256; i++) {
            if (e->winmask[i] & (1u << bgnum)) {
                int32_t fx = rotX, fy = rotY;
                if (mosaic) mosaic_step_back(mostab[i], rotA, rotC, &fx, &fy);
                if (!(((uint32_t)fx | (uint32_t)fy) & overflowmask)) {
                    uint16_t curtile = vram16(e, tilemapaddr
                        + ((((((uint32_t)fy & coordmask) >> 11) << yshift)
                            + (((uint32_t)fx & coordmask) >> 11)) << 1));
                    const uint16_t *curpal =
                        extpal ? bg_extpal(e, bgnum, curtile >> 12) : NULL;
                    uint32_t tx = ((uint32_t)fx >> 8) & 7u;
                    uint32_t ty = ((uint32_t)fy >> 8) & 7u;
                    uint8_t color;
                    if (curtile & (1 << 10)) tx = 7u - tx;
                    if (curtile & (1 << 11)) ty = 7u - ty;
                    color = vram8(e, tilesetaddr
                        + ((uint32_t)(curtile & 0x03FF) << 6) + (ty << 3) + tx);
                    if (color)
                        draw_pixel(&e->line[i],
                                   curpal
                                       ? curpal[color]
                                       : pal16(e->palbase
                                               + (uint32_t)color * 2),
                                   0x01000000u << bgnum);
                }
            }
            rotX = (int32_t)((uint32_t)rotX + (uint32_t)rotA);
            rotY = (int32_t)((uint32_t)rotY + (uint32_t)rotC);
        }
    }
}

/* Mode 6's BG2: one 8-bit bitmap starting at the window's base, with its own
 * four sizes and no character or screen base at all. */
static void draw_bg_large(struct engine *e) {
    uint16_t bgcnt = e->bgcnt[2];
    uint32_t xmask, ymask, yshift, ofxmask, ofymask;
    int32_t rotX = e->rot_x[0], rotY = e->rot_y[0];
    int32_t rotA = e->rot_a[0], rotC = e->rot_c[0];
    int mosaic = (bgcnt & (1 << 6)) && e->mos_bg_x;
    const uint8_t *mostab = mosaic ? mosaic_row(e->mos_bg_x) : NULL;
    int i;

    switch ((bgcnt >> 14) & 3) {
    case 0: xmask = 0x1FFFF; ymask = 0x3FFFF; yshift = 9; break;
    case 1: xmask = 0x3FFFF; ymask = 0x1FFFF; yshift = 10; break;
    case 2: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
    default: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
    }
    ofxmask = (bgcnt & (1 << 13)) ? 0u : ~xmask;
    ofymask = (bgcnt & (1 << 13)) ? 0u : ~ymask;

    for (i = 0; i < 256; i++) {
        if (e->winmask[i] & (1u << 2)) {
            int32_t fx = rotX, fy = rotY;
            if (mosaic) mosaic_step_back(mostab[i], rotA, rotC, &fx, &fy);
            if (!((uint32_t)fx & ofxmask) && !((uint32_t)fy & ofymask)) {
                uint8_t color = vram8(e,
                    ((((uint32_t)fy & ymask) >> 8) << yshift)
                    + (((uint32_t)fx & xmask) >> 8));
                if (color)
                    draw_pixel(&e->line[i],
                               pal16(e->palbase + (uint32_t)color * 2),
                               0x01000000u << 2);
            }
        }
        rotX = (int32_t)((uint32_t)rotX + (uint32_t)rotA);
        rotY = (int32_t)((uint32_t)rotY + (uint32_t)rotC);
    }
}

/* ------------------------------------------------------------------ */
/* The 3D layer.                                      */
/* ------------------------------------------------------------------ */

/*
 * What the high-resolution 3D path needs from this file.
 *
 * At --hd3d S the rasterizer draws the 3D layer at S times the pixels on each
 * axis while everything on this page stays the DS's own 256x192 art. So the
 * viewer's compose has to answer, for every sub-pixel of every native pixel:
 * is the 3D layer what shows here, and what colour does the blend unit make of
 * it?
 *
 * Comparing the composed pixel against the native 3D pixel is wrong in both
 * directions. A 2D pixel that happens to match the 3D under it takes
 * sub-pixels it should not, and a 3D pixel the blend unit faded never matches,
 * so every translucent surface fell back to a replicated pixel.
 *
 * The engine already knows the answer, so it writes it down:
 *
 *   hd_u1  the pixel the 3D layer sat on, and hd_u2 the one under that: the
 *          two-deep stack the blend unit works with, as it stood before BG0
 *          got its turn. With both, a sub-pixel the polygon does not cover is
 *          composed as the hardware would have composed it with no 3D layer.
 *   hd_st  bit 0: the 3D layer is what shows here. Bit 1: the window's
 *          colour-effect bit.
 *   hd_bl  the blend registers, per scanline, because they are per scanline.
 *
 * Bit 0 cannot be read off the finished line, because a native pixel whose 3D
 * value happened to be transparent looks exactly like one where a sprite
 * covered the 3D. draw_bg_3d() settles it by writing at every column the
 * window admits: the real pixel where the rasterizer drew one, and otherwise a
 * phantom value the real path can never produce. Later layers overwrite a
 * phantom exactly as they would a real 3D pixel, so where one survives, a 3D
 * pixel would have. hd3d_settle() takes the phantoms back out before the blend
 * unit sees one.
 *
 * All of it is behind hd3d_on, which pc_view.c sets only for --hd3d 2 and
 * above.
 */
#if defined(__3DS__)
#define HD3D_RECORD 0
#else
#define HD3D_RECORD 1
#endif

#if HD3D_RECORD
#define HD3D_PHANTOM 0x40000000u        /* flag 0x40, alpha 0: impossible  */

struct hd3d_blend { uint32_t blendcnt, eva, evb, evy; };

static int hd3d_on;
static uint32_t hd3d_u1[192 * 256];
static uint32_t hd3d_u2[192 * 256];
static uint8_t  hd3d_st[192 * 256];
static struct hd3d_blend hd3d_bl[192];

void pc_gpu2d_set_hd(int scale)
{
    hd3d_on = scale > 1;
}

int pc_gpu2d_hd3d_covers(int x, int y)
{
    if (!hd3d_on || (unsigned)x >= 256u || (unsigned)y >= 192u) return 0;
    return hd3d_st[y * 256 + x] & 1u;
}

/* One sub-pixel of a native pixel the 3D layer is showing at. */
static uint32_t hd3d_sub(const struct hd3d_blend *b, uint32_t win,
                         uint32_t under1, uint32_t under2, uint32_t raw,
                         uint16_t mb)
{
    uint32_t val1, val2;

    if ((raw >> 24) != 0) {
        /* The polygon covers this sub-pixel: it is the top pixel, over what
         * the native pixel's 3D was over. The flag byte is the one
         * draw_bg_3d() builds, 0x40, and the rasterizer's own alpha, which
         * is the coefficient the blend unit uses. */
        val1 = raw | 0x40000000u;
        val2 = under1;
    } else {
        /* It does not: the stack under the 3D layer shows, exactly as it
         * would have with BG0 turned off. */
        val1 = under1;
        val2 = under2;
    }
    return pc_gpu2d_present_px(color_composite_at(b->blendcnt, b->eva, b->evb,
                                                  b->evy, win, val1, val2),
                               mb);
}

/*
 * A row at a time, and that is a measurement rather than a style. At --hd3d 4
 * a frame has 786,000 sub-pixels in the panel, and a per-sub-pixel entry
 * point across a translation unit boundary spends most of its time re-deriving
 * the same row pointer and the same per-pixel recipe. Per row it is 192 calls
 * a frame and the recipe is hoisted out of the inner loop.
 *
 * `dst` is expected to already hold the replicated native pixel, so the
 * pixels the 3D layer is not showing at are simply left alone, the caller
 * composes the whole frame by replication and this overwrites the 3D's share
 * of it.
 */
void pc_gpu2d_hd3d_row(int y, int s, const uint32_t *src, uint32_t *dst,
                       uint16_t mb)
{
    const struct hd3d_blend *b;
    const uint8_t *st;
    const uint32_t *u1, *u2;
    int x, k;

    if (!hd3d_on || (unsigned)y >= 192u || s < 1 || src == NULL
        || dst == NULL) {
        return;
    }
    b = &hd3d_bl[y];
    st = &hd3d_st[(uint32_t)y * 256u];
    u1 = &hd3d_u1[(uint32_t)y * 256u];
    u2 = &hd3d_u2[(uint32_t)y * 256u];
    for (x = 0; x < 256; x++) {
        uint32_t win, under1, under2;

        if (!(st[x] & 1u)) continue;
        win = (st[x] & 2u) ? 0x20u : 0u;
        under1 = u1[x];
        under2 = u2[x];
        for (k = 0; k < s; k++) {
            dst[x * s + k] = hd3d_sub(b, win, under1, under2,
                                      src[x * s + k], mb);
        }
    }
}

/* BG0's turn, with the recording armed: the rasterizer's pixel where it drew
 * one and a phantom where it did not, and the two values each displaced
 * written down before they are pushed. */
static void hd3d_lay(struct engine *e, uint32_t line, const uint32_t *src)
{
    uint32_t *u1 = &hd3d_u1[line * 256];
    uint32_t *u2 = &hd3d_u2[line * 256];
    int i;

    e->hd3d_drew = 1;
    for (i = 0; i < 256; i++) {
        uint32_t c = src[i];

        if (!(e->winmask[i] & 0x01u)) {
            /* The window turns BG0 off here, so no sub-pixel of this native
             * pixel can be the 3D layer either. */
            u1[i] = u2[i] = 0;
            continue;
        }
        u1[i] = e->line[i];
        u2[i] = e->line[256 + i];
        e->line[256 + i] = e->line[i];
        e->line[i] = (c >> 24) != 0 ? (c | 0x40000000u) : HD3D_PHANTOM;
    }
}

/* The phantoms out, the displaced values back, and bit 0 settled. Runs once
 * per engine-A scanline, after every layer and before the blend unit. */
static void hd3d_settle(struct engine *e, uint32_t line)
{
    uint32_t *u1 = &hd3d_u1[line * 256];
    uint32_t *u2 = &hd3d_u2[line * 256];
    uint8_t *st = &hd3d_st[line * 256];
    int i;

    hd3d_bl[line].blendcnt = e->blendcnt;
    hd3d_bl[line].eva = e->eva;
    hd3d_bl[line].evb = e->evb;
    hd3d_bl[line].evy = e->evy;

    if (!e->hd3d_drew) {
        /* No 3D layer on this line at all, BG0 is off, or is an ordinary
         * background, or this frame's DISPCNT does not select the engine. */
        memset(st, 0, 256);
        return;
    }
    for (i = 0; i < 256; i++) {
        uint32_t top = e->line[i], second = e->line[256 + i];
        int covers;

        if (top == HD3D_PHANTOM) {
            /* Nothing covered it, so a real 3D pixel would have shown. Put
             * back the two values it pushed down. */
            e->line[i] = u1[i];
            e->line[256 + i] = u2[i];
            covers = 1;
        } else {
            if (second == HD3D_PHANTOM) {
                /* Exactly one later layer covered it, and pushed the phantom
                 * into the second slot; the true second operand is what the
                 * phantom itself displaced. */
                e->line[256 + i] = u1[i];
            }
            /* 0x40 exactly. A bitmap sprite's flag is 0xC0 plus its own
             * alpha, so a bare bit test would call one a 3D pixel. */
            covers = ((top >> 24) & 0xC0u) == 0x40u ? 1 : 0;
        }
        st[i] = (uint8_t)(covers | ((e->winmask[i] & 0x20u) ? 2u : 0u));
    }
}
/*
 * The vector suite, because the five cases below cannot be driven from a game.
 *
 * Which native pixels the 3D layer is showing at is settled by what happens to
 * a phantom, and a phantom's fate depends on how many later layers wrote over
 * BG0 at that column. A recorded replay reaches "nothing covered it" and
 * "a sprite covered it" constantly and the other three almost never, and none
 * of them is visible in a screenshot; a wrong answer here shows up as one
 * sub-pixel block taking the wrong colour. So they are built by hand.
 *
 * Runs from --selftest, before any guest code, and leaves hd3d_on as it found
 * it. The recording arrays it scribbles on are rewritten by the first frame.
 */
int pc_gpu2d_hd3d_selftest(void)
{
    /* Six columns, one per case; the seventh value is the 3D pixel each is
     * handed. Colours are arbitrary and distinct so a swap is visible. */
    static const uint32_t U1[6] = { 0x01001122u, 0x01003344u, 0x01005566u,
                                    0x01007788u, 0x010099AAu, 0x0100BBCCu },
                          U2[6] = { 0x20112233u, 0x20334455u, 0x20556677u,
                                    0x20778899u, 0x2099AABBu, 0x20BBCCDDu },
                          THREE[6] = { 0x0F0A0B0Cu, 0u, 0x100D0E0Fu, 0u, 0u,
                                       0x1F101112u };
    static const uint32_t COVER[3] = { 0x10203040u, 0x08405060u, 0u };
    struct engine e;
    uint32_t src[256];
    int saved = hd3d_on, i, ok = 1;

    memset(&e, 0, sizeof e);
    e.num = 0;
    e.blendcnt = 0x03C1u;       /* alpha blend, BG0 and OBJ in, BG1 out    */
    e.eva = 9; e.evb = 7; e.evy = 5;
    memset(src, 0, sizeof src);
    for (i = 0; i < 6; i++) {
        /* Column 5 is the one the window shuts BG0 out of; every column has
         * the colour-effect bit so the blend unit is actually reached. */
        e.winmask[i] = (uint8_t)((i == 5 ? 0x00u : 0x01u) | 0x20u);
        e.line[i] = U1[i];
        e.line[256 + i] = U2[i];
        src[i] = THREE[i];
    }
    hd3d_on = 1;
    hd3d_lay(&e, 0, src);

    /* The later layers, exactly as draw_pixel() writes them. */
    e.line[256 + 2] = e.line[2]; e.line[2] = COVER[0];
    e.line[256 + 3] = e.line[3]; e.line[3] = COVER[0];
    e.line[256 + 4] = e.line[4]; e.line[4] = COVER[0];
    e.line[256 + 4] = e.line[4]; e.line[4] = COVER[1];

    hd3d_settle(&e, 0);

    for (i = 0; i < 6; i++) {
        /* Columns 0 and 1 are the two ways the 3D layer shows: it drew, and
         * it did not draw but nothing else claimed the pixel either. */
        int want = (i == 0 || i == 1);

        if (pc_gpu2d_hd3d_covers(i, 0) != want) {
            fprintf(stderr, "pc-gpu2d: hd3d column %d says covered=%d,"
                            " wanted %d\n", i, pc_gpu2d_hd3d_covers(i, 0),
                    want);
            ok = 0;
        }
    }
    /* ...and the line the blend unit is about to read must be the line it
     * would have read with no recording at all. */
    {
        static const uint32_t WANT1[6] = { 0x4F0A0B0Cu, 0x01003344u,
                                           0x10203040u, 0x10203040u,
                                           0x08405060u, 0x0100BBCCu };
        static const uint32_t WANT2[6] = { 0x01001122u, 0x20334455u,
                                           0x500D0E0Fu, 0x01007788u,
                                           0x10203040u, 0x20BBCCDDu };

        for (i = 0; i < 6; i++) {
            if (e.line[i] != WANT1[i] || e.line[256 + i] != WANT2[i]) {
                fprintf(stderr, "pc-gpu2d: hd3d column %d left the stack"
                                " %08X over %08X, wanted %08X over %08X\n",
                        i, e.line[i], e.line[256 + i], WANT1[i], WANT2[i]);
                ok = 0;
            }
        }
    }
    /*
     * And the sub-pixel answers. Two claims: a sub-pixel the polygon covers
     * is composed exactly as the native pixel was, so the sub-pixel the
     * native point-sample came from reproduces it bit for bit; and one the
     * polygon does not reach is the stack under BG0, composed as though the
     * layer were off.
     */
    {
        static const uint32_t UNTOUCHED = 0xDEADBEEFu;
        uint32_t row[256], out[256];
        uint32_t want;

        for (i = 0; i < 256; i++) { row[i] = 0; out[i] = UNTOUCHED; }
        row[0] = THREE[0];
        pc_gpu2d_hd3d_row(0, 1, row, out, 0);

        want = pc_gpu2d_present_px(
            color_composite_at(e.blendcnt, e.eva, e.evb, e.evy, 0x20u,
                               THREE[0] | 0x40000000u, U1[0]), 0);
        if (out[0] != want) {
            fprintf(stderr, "pc-gpu2d: hd3d covered sub-pixel %08X, wanted"
                            " %08X\n", out[0], want);
            ok = 0;
        }
        /* Column 1 is the other half of the same claim: the 3D layer shows
         * there, the polygon does not reach it, and what comes out is the
         * stack under BG0 composed as though the layer were off. */
        want = pc_gpu2d_present_px(
            color_composite_at(e.blendcnt, e.eva, e.evb, e.evy, 0x20u,
                               U1[1], U2[1]), 0);
        if (out[1] != want) {
            fprintf(stderr, "pc-gpu2d: hd3d uncovered sub-pixel %08X, wanted"
                            " %08X\n", out[1], want);
            ok = 0;
        }
        /* And a pixel the 3D layer is not showing at is left exactly as the
         * caller's replication left it. */
        for (i = 2; i < 6; i++) {
            if (out[i] != UNTOUCHED) {
                fprintf(stderr, "pc-gpu2d: hd3d wrote column %d, which the "
                                "3D layer does not show at\n", i);
                ok = 0;
            }
        }
    }
    hd3d_on = saved;
    return ok;
}

#else
void pc_gpu2d_set_hd(int scale) { (void)scale; }
int pc_gpu2d_hd3d_selftest(void) { return 1; }
int pc_gpu2d_hd3d_covers(int x, int y) { (void)x; (void)y; return 0; }
void pc_gpu2d_hd3d_row(int y, int s, const uint32_t *src, uint32_t *dst,
                       uint16_t mb)
{
    (void)y; (void)s; (void)src; (void)dst; (void)mb;
}
#endif

/*
 * BG0 as the rasterizer's output. This is the only layer that is not read out
 * of VRAM, and the only one that does not go through draw_pixel(): the 3D
 * engine works at six bits a channel already, so there is no 15-bit colour to
 * expand, and the flag byte it needs is `0x40 | alpha` rather than a plain
 * layer bit. color_composite() reads both halves of that, 0x40 says "this is
 * the 3D layer" and the low five bits are the coefficient it blends with.
 *
 * A pixel the rasterizer left with alpha 0 is *skipped* rather than drawn
 * transparent, which is what makes the layer under it show through unchanged,
 * and the window bit consulted is bit 0, BG0's own, so a window that turns
 * BG0 off turns the 3D layer off with it.
 *
 * The scroll is not applied here. pc_gpu3d_soft_line() has already done it,
 * because on hardware BG0HOFS is wired into the *rendering* engine rather than
 * into this layer; see and the note in that function.
 */
static void draw_bg_3d(struct engine *e, uint32_t line) {
    const uint32_t *src;
    int i;

    if (!pc_gpu3d_soft_present()) {
        /* A binary with no rasterizer cannot draw this layer, and drawing it
         * black or skipping it would both be a picture nothing here can
         * justify. The shipped port always has one; this is the wall a test
         * that links pc_gpu2d.c alone would hit. */
        gpu2d_trap("DISPCNT selects the 3D engine for BG0 and this binary has "
                   "no rasterizer linked in", e->io + 0, e->dispcnt);
    }

    src = pc_gpu3d_soft_line((int)line);
#if HD3D_RECORD
    if (hd3d_on) {
        hd3d_lay(e, line, src);
        return;
    }
#endif
    for (i = 0; i < 256; i++) {
        uint32_t c = src[i];
        if ((c >> 24) == 0) continue;
        if (!(e->winmask[i] & 0x01u)) continue;
        e->line[256 + i] = e->line[i];
        e->line[i] = c | 0x40000000u;
    }
}

/* ------------------------------------------------------------------ */
/* Sprites.                                           */
/* ------------------------------------------------------------------ */

/*
 * Two passes, not one, and the split is the hardware's. Every sprite that
 * touches this scanline is rasterised into `objline` first, resolving
 * sprite-against-sprite priority as it goes; the backgrounds are then composed
 * and the OBJ layer interleaved between them once per priority. That is why
 * `objline` holds a palette *index* and an attribute word rather than a colour:
 * The entry it will be looked up in is not known until the pass that knows
 * which BGs it lands between, and doing it earlier would mean 256 lookups that
 * a covered pixel throws away.
 *
 * The three sprite-vs-sprite rules are all in draw_sprite_pixel() and none of
 * them is "later wins": a lower OAM index wins ties, a lower BG-relative
 * priority beats an already-written pixel whatever its index, and a
 * *transparent* pixel of a higher-priority sprite still lowers the priority the
 * next sprite has to beat. The sweep drives all three.
 */

/* attribute 0 bits 14-15 (shape) and attribute 1 bits 14-15 (size), which
 * together are one 16-entry table rather than two fields, shape 3 is not a
 * shape, and the four entries it selects are all 8x8. */
static const int32_t sprite_width[16] = {
     8, 16,  8, 8,
    16, 32,  8, 8,
    32, 32, 16, 8,
    64, 64, 32, 8
};
static const int32_t sprite_height[16] = {
     8,  8, 16, 8,
    16,  8, 32, 8,
    32, 16, 32, 8,
    64, 32, 64, 8
};

/*
 * `color` is -1 for transparent rather than 0, because a bitmap sprite's
 * transparency is its top bit and 0x0000 is a legitimate black there.
 */
static void draw_sprite_pixel(struct engine *e, int color, uint32_t pixelattr,
                              int32_t xpos, int window) {
    uint32_t oldpixel;
    int oldisopaque, newisopaque, priocheck;

    if (window) {
        /* An OBJ-window sprite contributes a mask rather than a picture.
         * Nothing consumes `objwindow` until the window unit is built; it is
         * computed anyway because it costs one store and because a sprite in
         * this mode must not draw, which is the half that *is* observable. */
        if (color != -1) e->objwindow[xpos] = 1;
        return;
    }

    oldpixel = e->objline[xpos];
    oldisopaque = (oldpixel & OBJ_IS_OPAQUE) != 0;
    newisopaque = (color != -1);
    priocheck = (pixelattr & OBJ_BGPRIO_MASK) < (oldpixel & OBJ_BGPRIO_MASK);

    if (newisopaque && (!oldisopaque || priocheck)) {
        e->objline[xpos] = (uint32_t)color | pixelattr;
    } else if (!newisopaque && !oldisopaque) {
        e->objline[xpos] &= ~(OBJ_MOSAIC | OBJ_BGPRIO_MASK);
        e->objline[xpos] |= (pixelattr &
                             (OBJ_IS_SPRITE | OBJ_MOSAIC | OBJ_BGPRIO_MASK));
    }
}

static void draw_sprite_rotscale(struct engine *e, uint32_t num,
                                 uint32_t boundwidth, uint32_t boundheight,
                                 uint32_t width, uint32_t height,
                                 int32_t xpos, int32_t ypos, int window) {
    uint16_t a0 = oam16(e, num * 4 + 0);
    uint16_t a1 = oam16(e, num * 4 + 1);
    uint16_t a2 = oam16(e, num * 4 + 2);
    uint32_t rp = ((uint32_t)(a1 >> 9) & 0x1Fu) * 16u + 3u;
    uint32_t pixelattr = ((uint32_t)(a2 & 0x0C00) << 6) | OBJ_IS_SPRITE
                       | OBJ_IS_OPAQUE;
    uint32_t tilenum = a2 & 0x03FF;
    uint32_t spritemode = window ? 0u : (((uint32_t)a0 >> 10) & 3u);
    uint32_t ytilefactor = 0, xoff, pixelsaddr;
    int32_t centerX = (int32_t)boundwidth >> 1;
    int32_t centerY = (int32_t)boundheight >> 1;
    int16_t rotA, rotB, rotC, rotD;
    int32_t rotX, rotY;

    if ((a0 & (1 << 12)) && !window) pixelattr |= OBJ_MOSAIC;

    if (xpos >= 0) {
        xoff = 0;
        if ((uint32_t)xpos + boundwidth > 256u) boundwidth = 256u - (uint32_t)xpos;
    } else {
        xoff = (uint32_t)(-xpos);
        xpos = 0;
    }

    rotA = (int16_t)oam16(e, rp + 0);
    rotB = (int16_t)oam16(e, rp + 4);
    rotC = (int16_t)oam16(e, rp + 8);
    rotD = (int16_t)oam16(e, rp + 12);

    /* The bounding box's centre maps to the sprite's, so the walk starts at
     * whichever texel the top-left visible corner lands on. Everything here is
     * mod 2^32 on purpose: a wildly out-of-range matrix wraps rather than
     * saturating, and the unsigned compare below is what rejects it. */
    rotX = (int32_t)((xoff - (uint32_t)centerX) * (uint32_t)(int32_t)rotA
                   + ((uint32_t)ypos - (uint32_t)centerY)
                     * (uint32_t)(int32_t)rotB
                   + (width << 7));
    rotY = (int32_t)((xoff - (uint32_t)centerX) * (uint32_t)(int32_t)rotC
                   + ((uint32_t)ypos - (uint32_t)centerY)
                     * (uint32_t)(int32_t)rotD
                   + (height << 7));

    width <<= 8;
    height <<= 8;

    if (spritemode == 3) {
        /* Bitmap sprite: a halfword per texel, and the three mappings differ
         * only in where the sprite's own data starts and how long a row is. */
        uint32_t alpha = (uint32_t)a2 >> 12;
        if (!alpha) return;
        alpha++;
        pixelattr |= 0xC0000000u | (alpha << 24);

        if (e->dispcnt & 0x40u) {
            if (e->dispcnt & 0x20u) return;  /* reserved: draws nothing */
            pixelsaddr = tilenum << (7 + ((e->dispcnt >> 22) & 1u));
            ytilefactor = (width >> 8) * 2u;
        } else if (e->dispcnt & 0x20u) {
            pixelsaddr = ((tilenum & 0x01Fu) << 4) + ((tilenum & 0x3E0u) << 7);
            ytilefactor = 256u * 2u;
        } else {
            pixelsaddr = ((tilenum & 0x00Fu) << 4) + ((tilenum & 0x3F0u) << 7);
            ytilefactor = 128u * 2u;
        }

        for (; xoff < boundwidth; xoff++, xpos++) {
            if ((uint32_t)rotX < width && (uint32_t)rotY < height) {
                uint16_t color = objvram16(e, pixelsaddr
                    + (uint32_t)(rotY >> 8) * ytilefactor
                    + ((uint32_t)(rotX >> 8) << 1));
                draw_sprite_pixel(e, (color & 0x8000) ? (int)color : -1,
                                  pixelattr, xpos, window);
            }
            rotX = (int32_t)((uint32_t)rotX + (uint32_t)(int32_t)rotA);
            rotY = (int32_t)((uint32_t)rotY + (uint32_t)(int32_t)rotC);
        }
        return;
    }

    pixelsaddr = tilenum;
    if (e->dispcnt & (1u << 4)) {
        pixelsaddr <<= ((e->dispcnt >> 20) & 3u);
        ytilefactor = (width >> 11) << ((a0 & 0x2000) ? 1 : 0);
    } else {
        ytilefactor = 0x20u;
    }
    pixelattr |= (spritemode == 1) ? 0x80000000u : 0x10000000u;
    ytilefactor <<= 5;
    pixelsaddr <<= 5;

    if (a0 & (1 << 13)) {
        /* 256 colours. The palette number only exists when DISPCNT selects
         * extended palettes; otherwise every sprite shares the one at 0x200. */
        if (!window) {
            if (!(e->dispcnt & (1u << 31))) pixelattr |= OBJ_STANDARD_PAL;
            else pixelattr |= ((uint32_t)(a2 & 0xF000) >> 4);
        }
        for (; xoff < boundwidth; xoff++, xpos++) {
            if ((uint32_t)rotX < width && (uint32_t)rotY < height) {
                uint8_t color = objvram8(e, pixelsaddr
                    + (uint32_t)(rotY >> 11) * ytilefactor
                    + (((uint32_t)rotY & 0x700u) >> 5)
                    + (uint32_t)(rotX >> 11) * 64u
                    + (((uint32_t)rotX & 0x700u) >> 8));
                draw_sprite_pixel(e, color ? (int)color : -1, pixelattr,
                                  xpos, window);
            }
            rotX = (int32_t)((uint32_t)rotX + (uint32_t)(int32_t)rotA);
            rotY = (int32_t)((uint32_t)rotY + (uint32_t)(int32_t)rotC);
        }
    } else {
        /* 16 colours; the nibble is chosen by bit 8 of the texel coordinate
         * rather than by a loop counter, because a rotated walk can go
         * backwards through a byte. */
        if (!window) {
            pixelattr |= OBJ_STANDARD_PAL;
            pixelattr |= ((uint32_t)(a2 & 0xF000) >> 8);
        }
        for (; xoff < boundwidth; xoff++, xpos++) {
            if ((uint32_t)rotX < width && (uint32_t)rotY < height) {
                uint8_t b = objvram8(e, pixelsaddr
                    + (uint32_t)(rotY >> 11) * ytilefactor
                    + (((uint32_t)rotY & 0x700u) >> 6)
                    + (uint32_t)(rotX >> 11) * 32u
                    + (((uint32_t)rotX & 0x700u) >> 9));
                uint8_t color = (rotX & 0x100) ? (uint8_t)(b >> 4)
                                               : (uint8_t)(b & 0x0F);
                draw_sprite_pixel(e, color ? (int)color : -1, pixelattr,
                                  xpos, window);
            }
            rotX = (int32_t)((uint32_t)rotX + (uint32_t)(int32_t)rotA);
            rotY = (int32_t)((uint32_t)rotY + (uint32_t)(int32_t)rotC);
        }
    }
}

static void draw_sprite_normal(struct engine *e, uint32_t num,
                               uint32_t width, uint32_t height,
                               int32_t xpos, int32_t ypos, int window) {
    uint16_t a0 = oam16(e, num * 4 + 0);
    uint16_t a1 = oam16(e, num * 4 + 1);
    uint16_t a2 = oam16(e, num * 4 + 2);
    uint32_t pixelattr = ((uint32_t)(a2 & 0x0C00) << 6) | OBJ_IS_SPRITE
                       | OBJ_IS_OPAQUE;
    uint32_t tilenum = a2 & 0x03FF;
    uint32_t spritemode = window ? 0u : (((uint32_t)a0 >> 10) & 3u);
    uint32_t wmask = width - 8u;        /* really ((width - 1) & ~7) */
    uint32_t xoff, xend = width, pixelsaddr;
    int32_t pixelstride;

    if ((a0 & (1 << 12)) && !window) pixelattr |= OBJ_MOSAIC;

    if (a1 & (1 << 13)) ypos = (int32_t)height - 1 - ypos;   /* V flip */

    if (xpos >= 0) {
        xoff = 0;
        if ((uint32_t)xpos + xend > 256u) xend = 256u - (uint32_t)xpos;
    } else {
        xoff = (uint32_t)(-xpos);
        xpos = 0;
    }

    if (spritemode == 3) {
        uint32_t alpha = (uint32_t)a2 >> 12;
        if (!alpha) return;
        alpha++;
        pixelattr |= 0xC0000000u | (alpha << 24);

        pixelsaddr = tilenum;
        if (e->dispcnt & 0x40u) {
            if (e->dispcnt & 0x20u) return;  /* reserved: draws nothing */
            pixelsaddr <<= (7 + ((e->dispcnt >> 22) & 1u));
            pixelsaddr += (uint32_t)ypos * width * 2u;
        } else if (e->dispcnt & 0x20u) {
            pixelsaddr = ((tilenum & 0x01Fu) << 4) + ((tilenum & 0x3E0u) << 7);
            pixelsaddr += (uint32_t)ypos * 256u * 2u;
        } else {
            pixelsaddr = ((tilenum & 0x00Fu) << 4) + ((tilenum & 0x3F0u) << 7);
            pixelsaddr += (uint32_t)ypos * 128u * 2u;
        }

        if (a1 & (1 << 12)) {           /* H flip */
            pixelsaddr += (width - 1u) << 1;
            pixelsaddr -= xoff << 1;
            pixelstride = -2;
        } else {
            pixelsaddr += xoff << 1;
            pixelstride = 2;
        }

        for (; xoff < xend; xoff++, xpos++) {
            uint16_t color = objvram16(e, pixelsaddr);
            pixelsaddr = (uint32_t)((int32_t)pixelsaddr + pixelstride);
            draw_sprite_pixel(e, (color & 0x8000) ? (int)color : -1,
                              pixelattr, xpos, window);
        }
        return;
    }

    pixelsaddr = tilenum;
    if (e->dispcnt & (1u << 4)) {
        /* 1D mapping: the sprite's rows are consecutive, and DISPCNT's
         * boundary field scales the tile number. */
        pixelsaddr <<= ((e->dispcnt >> 20) & 3u);
        pixelsaddr += (((uint32_t)ypos >> 3) * (width >> 3))
                      << ((a0 & 0x2000) ? 1 : 0);
    } else {
        /* 2D mapping: the character area is a 32-tile-wide sheet, so the next
         * row of a sprite is 32 tiles on whatever the sprite's width is. */
        pixelsaddr += ((uint32_t)ypos >> 3) * 0x20u;
    }
    pixelattr |= (spritemode == 1) ? 0x80000000u : 0x10000000u;

    if (a0 & (1 << 13)) {
        pixelsaddr <<= 5;
        pixelsaddr += ((uint32_t)ypos & 7u) << 3;

        if (!window) {
            if (!(e->dispcnt & (1u << 31))) pixelattr |= OBJ_STANDARD_PAL;
            else pixelattr |= ((uint32_t)(a2 & 0xF000) >> 4);
        }

        if (a1 & (1 << 12)) {
            pixelsaddr += ((width - 1u) & wmask) << 3;
            pixelsaddr += (width - 1u) & 7u;
            pixelsaddr -= (xoff & wmask) << 3;
            pixelsaddr -= xoff & 7u;
            pixelstride = -1;
        } else {
            pixelsaddr += (xoff & wmask) << 3;
            pixelsaddr += xoff & 7u;
            pixelstride = 1;
        }

        for (; xoff < xend; xoff++, xpos++) {
            uint8_t color = objvram8(e, pixelsaddr);
            pixelsaddr = (uint32_t)((int32_t)pixelsaddr + pixelstride);
            draw_sprite_pixel(e, color ? (int)color : -1, pixelattr,
                              xpos, window);
            /* A tile is eight bytes wide and 64 long, so crossing a tile
             * boundary is a jump of the other 56. */
            if (!((xoff + 1u) & 7u))
                pixelsaddr = (uint32_t)((int32_t)pixelsaddr + 56 * pixelstride);
        }
    } else {
        pixelsaddr <<= 5;
        pixelsaddr += ((uint32_t)ypos & 7u) << 2;

        if (!window) {
            pixelattr |= OBJ_STANDARD_PAL;
            pixelattr |= ((uint32_t)(a2 & 0xF000) >> 8);
        }

        if (a1 & (1 << 12)) {
            pixelsaddr += ((width - 1u) & wmask) << 2;
            pixelsaddr += ((width - 1u) & 7u) >> 1;
            pixelsaddr -= (xoff & wmask) << 2;
            pixelsaddr -= (xoff & 7u) >> 1;
            pixelstride = -1;
        } else {
            pixelsaddr += (xoff & wmask) << 2;
            pixelsaddr += (xoff & 7u) >> 1;
            pixelstride = 1;
        }

        for (; xoff < xend; xoff++, xpos++) {
            uint8_t color;
            if (a1 & (1 << 12)) {
                if (xoff & 1) {
                    color = (uint8_t)(objvram8(e, pixelsaddr) & 0x0F);
                    pixelsaddr--;
                } else {
                    color = (uint8_t)(objvram8(e, pixelsaddr) >> 4);
                }
            } else {
                if (xoff & 1) {
                    color = (uint8_t)(objvram8(e, pixelsaddr) >> 4);
                    pixelsaddr++;
                } else {
                    color = (uint8_t)(objvram8(e, pixelsaddr) & 0x0F);
                }
            }
            draw_sprite_pixel(e, color ? (int)color : -1, pixelattr,
                              xpos, window);
            if (!((xoff + 1u) & 7u))
                pixelsaddr += (a1 & 0x1000) ? (uint32_t)-28 : 28u;
        }
    }
}

static void draw_sprites(struct engine *e, uint32_t line) {
    int sprnum;

    e->nsprites = 0;
    memset(e->objline, 0, sizeof e->objline);
    memset(e->objwindow, 0, sizeof e->objwindow);

    if (!(e->dispcnt & (1u << 12))) return;

    for (sprnum = 0; sprnum < 128; sprnum++) {
        uint16_t a0 = oam16(e, (uint32_t)sprnum * 4 + 0);
        uint16_t a1 = oam16(e, (uint32_t)sprnum * 4 + 1);
        uint32_t sprtype = ((uint32_t)a0 >> 8) & 3u;
        uint32_t sizeparam;
        int32_t width, height, boundwidth, boundheight, xpos, ypos;
        int iswin;

        if (sprtype == 2) continue;                  /* disabled */

        iswin = (((uint32_t)a0 >> 10) & 3u) == 2u;

        sizeparam = ((uint32_t)a0 >> 14) | (((uint32_t)a1 & 0xC000u) >> 12);
        width = sprite_width[sizeparam];
        height = sprite_height[sizeparam];
        boundwidth = width;
        boundheight = height;
        if (sprtype == 3) {                          /* double-size rotscale */
            boundwidth <<= 1;
            boundheight <<= 1;
        }

        /* Y is eight bits and wraps, so a sprite whose top is at 250 shows its
         * first rows on the last scanlines and the rest on the first. */
        ypos = (int32_t)(a0 & 0xFF);
        if ((int32_t)((line - (uint32_t)ypos) & 0xFFu) >= boundheight) continue;

        /* X is nine bits, signed: -256..255. */
        xpos = (int32_t)(((uint32_t)a1 & 0x1FFu) ^ 0x100u) - 0x100;
        if (xpos <= -boundwidth) continue;

        /*
         * The vertical half of sprite mosaic, which is not the same shape as
         * the background's: the sprite reads the row that was current at the
         * *latched* line, so a sprite whose top edge is below that line gets a
         * negative offset. The hardware wraps it into eight bits and the result
         * lands outside the sprite, which is the clamp below rather than a
         * guard against it. OBJ-window sprites are exempt.
         */
        if ((a0 & (1 << 12)) && !iswin) {
            ypos = (int32_t)((e->obj_mos_line - (uint32_t)ypos) & 0xFFu);
            if (ypos >= boundheight) ypos = 0;
        } else {
            ypos = (int32_t)((line - (uint32_t)ypos) & 0xFFu);
        }

        if (sprtype & 1)
            draw_sprite_rotscale(e, (uint32_t)sprnum, (uint32_t)boundwidth,
                                 (uint32_t)boundheight, (uint32_t)width,
                                 (uint32_t)height, xpos, ypos, iswin);
        else
            draw_sprite_normal(e, (uint32_t)sprnum, (uint32_t)width,
                               (uint32_t)height, xpos, ypos, iswin);

        e->nsprites++;
    }
}

/*
 * The horizontal half of sprite mosaic, applied to the finished OBJ line
 * rather than inside a sprite; which is the only place it can be, because
 * one mosaic block can span two sprites and the block takes its colour from
 * whichever of them owns its first column.
 *
 * The four latch conditions are the hardware's and only the first is obvious.
 * A pixel that is *not* a mosaic sprite's latches, so a plain sprite is never
 * smeared by a mosaic one beside it; crossing the boundary the other way
 * latches too; and a pixel of a better BG-relative priority latches whatever
 * the counter says, because the block cannot show a pixel the priority
 * resolution already lost.
 */
static void apply_sprite_mosaic_x(struct engine *e) {
    uint32_t mosw = e->mos_obj_x, mosx = 0, latch = 0;
    int i;

    if (!mosw) return;

    for (i = 0; i < 256; i++) {
        uint32_t cur = e->objline[i];

        if (mosx == 0
            || !(cur & OBJ_MOSAIC)
            || !(latch & OBJ_MOSAIC)
            || (cur & OBJ_BGPRIO_MASK) < (latch & OBJ_BGPRIO_MASK))
            latch = cur;

        e->objline[i] = latch;

        if (mosx == mosw) mosx = 0;
        else mosx++;
    }
}

/*
 * The OBJ layer's contribution at one priority. This is where the palette
 * lookup deferred above finally happens, and the three kinds of sprite differ
 * only in which table answers: a bitmap sprite carries its own colour, a
 * standard-palette sprite reads the 256 entries at 0x200 (0x600 for engine B),
 * and an extended-palette one reads all 4,096 of its own bank.
 */
static void interleave_sprites(struct engine *e, int prio) {
    uint32_t attrmask = ((uint32_t)prio << 16) | OBJ_IS_OPAQUE;
    const uint16_t *extpal = obj_extpal(e);
    int i;

    for (i = 0; i < 256; i++) {
        uint32_t pixel = e->objline[i];
        uint16_t color;

        if ((pixel & OBJ_OPAPRIO_MASK) != attrmask) continue;
        if (!(e->winmask[i] & 0x10u)) continue;

        if (pixel & OBJ_DIRECT_COLOR)
            color = (uint16_t)(pixel & 0x7FFFu);
        else if (pixel & OBJ_STANDARD_PAL)
            color = pal16(e->palbase + 0x200u + (pixel & 0xFFu) * 2u);
        else
            color = extpal[pixel & 0xFFFu];

        draw_pixel(&e->line[i], color, pixel & 0xFF000000u);
    }
}

/* ------------------------------------------------------------------ */
/* One scanline                                                        */
/* ------------------------------------------------------------------ */

static void gpu2d_trap(const char *what, uint32_t reg, uint32_t val) {
    fprintf(stderr,
            "pc_gpu2d: %s (register 0x%08X = 0x%08X).\n"
            "  This port has no model for it and will not invent one, a\n"
            "  picture that is plausible and wrong costs more to find later\n"
            "  than this stop costs now.\n",
            what, reg, val);
    abort();
}

static void draw_scanline(struct engine *e, uint32_t line) {
    uint32_t bgmode = e->dispcnt & 7u;
    uint32_t enable = (e->dispcnt >> 8) & 0x1Fu;
    uint16_t backdrop = pal16(e->palbase);
    int prio, i;

    for (i = 0; i < 256; i++) {
        e->line[i] = rgb15to18(backdrop) | 0x20000000u;
        e->line[256 + i] = 0;
    }
#if HD3D_RECORD
    e->hd3d_drew = 0;
#endif

    /*
     * The window mask is computed once for the whole scanline and every layer
     * below reads it, which is what makes "window" a unit rather than a flag
     * on each layer. With all three enable bits clear the unit is bypassed
     * entirely, and that is not the same as four regions that happen to
     * enable everything, because WINOUT would otherwise mask the layers even
     * with no window turned on.
     */
    if (e->dispcnt & 0xE000u) calculate_window_mask(e);
    else memset(e->winmask, 0xFF, sizeof e->winmask);

    apply_sprite_mosaic_x(e);

    /*
     * Back to front: priority 3 first, and within one priority the higher
     * layer number first, so that BG0 lands on top of a BG3 that shares its
     * priority. Composition is therefore just "the last write wins", which is
     * what makes draw_pixel() a store rather than a comparison. The OBJ layer
     * goes last within each priority, so a sprite covers every background that
     * shares its priority; which is why sprites are interleaved here rather
     * than drawn as a fifth layer at the end.
     */
    for (prio = 3; prio >= 0; prio--) {
        if ((e->bgcnt[3] & 3) == (uint32_t)prio && (enable & (1u << 3))) {
            if (bgmode == 6 || bgmode == 7) {
                /* Mode 6 has no BG3 and mode 7 is not a mode; neither draws. */
            } else if (bgmode >= 3) {
                draw_bg_extended(e, 3);
            } else if (bgmode >= 1) {
                draw_bg_affine(e, 3);
            } else {
                draw_bg_text(e, line, 3);
            }
        }
        if ((e->bgcnt[2] & 3) == (uint32_t)prio && (enable & (1u << 2))) {
            if (bgmode == 6) {
                draw_bg_large(e);
            } else if (bgmode == 7) {
                /* no BG2 */
            } else if (bgmode == 5) {
                draw_bg_extended(e, 2);
            } else if (bgmode == 4 || bgmode == 2) {
                draw_bg_affine(e, 2);
            } else {
                draw_bg_text(e, line, 2);
            }
        }
        if ((e->bgcnt[1] & 3) == (uint32_t)prio && (enable & (1u << 1))
            && bgmode != 6) {
            draw_bg_text(e, line, 1);
        }
        if ((e->bgcnt[0] & 3) == (uint32_t)prio && (enable & (1u << 0))) {
            if (!e->num && (e->dispcnt & 8u)) {
                draw_bg_3d(e, line);
            } else if (bgmode != 6) {
                draw_bg_text(e, line, 0);
            }
        }
        if ((enable & (1u << 4)) && e->nsprites) {
            interleave_sprites(e, prio);
        }
    }

#if HD3D_RECORD
    /* Before the blend unit, because the blend unit must never see a phantom
     * and the high-resolution path needs the stack as the blend unit will
     * read it. Engine A only: engine B has no 3D layer to record. */
    if (hd3d_on && !e->num) hd3d_settle(e, line);
#endif

    /* ...and the blend unit last, over the top pixel and the one it covered.
     * In place: each column is independent, and line[256 + i] is not read
     * again. */
    for (i = 0; i < 256; i++)
        e->line[i] = color_composite(e, i, e->line[i], e->line[256 + i]);
}

/* ------------------------------------------------------------------ */
/* One frame                                                           */
/* ------------------------------------------------------------------ */

static void engine_set_line(struct engine *e, uint32_t line);

static void engine_begin(struct engine *e, int num, uint32_t line0) {
    int b, v;
    uint16_t mosaic, bldalpha;

    e->num = num;
    e->io = num ? IO_B : IO_A;
    /*
     * Engine B has fewer DISPCNT fields than engine A and the hardware does not
     * merely ignore the others; it does not store them, which is what
     * melonDS's write mask says. The mask is applied here rather than at each
     * use so there is one answer to "does engine B have this bit": bit 3
     * (BG0-as-3D), bits 17-19 (the display mode's high bit and its VRAM block),
     * bit 22 (the bitmap-sprite 1D boundary) and bits 24-29 (the character and
     * screen bases) are engine A's alone. The first three were spelled out at
     * their use sites and those tests remain correct; the sprite unit needs bit 22 too and
     * a fourth hand-written case is a fourth place to forget one.
     */
    e->dispcnt = io32(e->io + 0x000) & (num ? 0xC0B1FFF7u : 0xFFFFFFFFu);
    for (b = 0; b < 4; b++) e->bgcnt[b] = io16(e->io + 0x008 + (uint32_t)b * 2);
    e->vram_base = num ? BBG_BASE : ABG_BASE;
    e->vram_mask = num ? BBG_MASK : ABG_MASK;
    e->obj_base = num ? BOBJ_BASE : AOBJ_BASE;
    e->obj_mask = num ? BOBJ_MASK : AOBJ_MASK;
    e->oam = OAM_BASE + (num ? 0x400u : 0x000u);
    e->palbase = num ? 0x400u : 0x000u;
    e->nsprites = 0;

    for (b = 0; b < 2; b++) {
        uint32_t base = 0x020u + (uint32_t)b * 0x10u;
        uint32_t x = io32(e->io + base + 8), y = io32(e->io + base + 12);
        /* BGxX and BGxY are 28-bit signed, so bit 27 is the sign. */
        if (x & 0x08000000u) x |= 0xF0000000u;
        if (y & 0x08000000u) y |= 0xF0000000u;
        e->rot_a[b] = (int16_t)io16(e->io + base + 0);
        e->rot_b[b] = (int16_t)io16(e->io + base + 2);
        e->rot_c[b] = (int16_t)io16(e->io + base + 4);
        e->rot_d[b] = (int16_t)io16(e->io + base + 6);
        e->rot_x0[b] = (int32_t)x;
        e->rot_y0[b] = (int32_t)y;
    }

    /* The three registers, and BLDALPHA's two coefficients are clamped as
     * they are written on hardware rather than as they are used, so a 0x1F
     * in either field is 16 and there is no 17th step. */
    mosaic       = io16(e->io + 0x04C);
    e->mos_bg_x  = (uint8_t)(mosaic & 0x0F);
    e->mos_bg_y  = (uint8_t)((mosaic >> 4) & 0x0F);
    e->mos_obj_x = (uint8_t)((mosaic >> 8) & 0x0F);
    e->mos_obj_y = (uint8_t)((mosaic >> 12) & 0x0F);

    e->blendcnt = (uint16_t)(io16(e->io + 0x050) & 0x3FFF);
    bldalpha    = io16(e->io + 0x052);
    e->eva = (uint8_t)(bldalpha & 0x1Fu);
    if (e->eva > 16) e->eva = 16;
    e->evb = (uint8_t)((bldalpha >> 8) & 0x1Fu);
    if (e->evb > 16) e->evb = 16;
    e->evy = (uint8_t)(io16(e->io + 0x054) & 0x1Fu);
    if (e->evy > 16) e->evy = 16;

    for (b = 0; b < 2; b++) {
        uint16_t h = io16(e->io + 0x040 + (uint32_t)b * 2);
        uint16_t vv = io16(e->io + 0x044 + (uint32_t)b * 2);
        e->wincoord[b][0] = (uint8_t)(h >> 8);      /* X1, the left edge */
        e->wincoord[b][1] = (uint8_t)(h & 0xFF);    /* X2, one past the right */
        e->wincoord[b][2] = (uint8_t)(vv >> 8);     /* Y1 */
        e->wincoord[b][3] = (uint8_t)(vv & 0xFF);   /* Y2 */
        e->winactive[b] = 0;
    }
    e->wincnt[0] = (uint8_t)(io16(e->io + 0x048) & 0xFF);
    e->wincnt[1] = (uint8_t)(io16(e->io + 0x048) >> 8);
    e->wincnt[2] = (uint8_t)(io16(e->io + 0x04A) & 0xFF);
    e->wincnt[3] = (uint8_t)(io16(e->io + 0x04A) >> 8);

    /*
     * Warming up the two window latches, which is the whole of what a
     * per-frame renderer owes the window unit. Neither is a function of the
     * line: the vertical one is stepped at the top of all 263 scanlines,
     * with the line number taken modulo 256, so 256..262 step it a second time
     * through 0..6, and the horizontal one at all 256 pixels of every
     * scanline that draws. Line 0 therefore inherits state from the frame
     * before it, and a renderer starting both at zero puts the first frame's
     * top rows outside a window that wraps. Registers held still for a frame
     * make one pass of each enough to reach the value every later frame has.
     */
    for (v = 0; v < 263; v++) win_update_y(e, (uint32_t)v);
    for (b = 0; b < 2; b++)
        if (e->dispcnt & (1u << (13 + b))) win_scan(e, b, NULL);

    engine_set_line(e, line0);
}

/*
 * Everything that moves from one scanline to the next, given one register
 * snapshot. The affine reference points advance by BGxPB/BGxPD per line, which
 * is a multiply here rather than an accumulate; the two mosaic counters latch
 * the first line of each block, which is that line rounded down; and a mosaic
 * affine layer therefore reads the *latched* line's reference point, which is
 * why the multiply is by bg_mos_line and not by line.
 */
/* The pure half: everything here is a function of the snapshot and the line
 * number alone, so a band can compute it for any line directly. The window's
 * vertical enable is the one latch, stepped separately below; which is what
 * lets a chunk seed itself from a recorded latch instead of replaying every
 * line since row 0. */
static void engine_line_values(struct engine *e, uint32_t line) {
    int b;

    e->bg_mos_line  = line - (line % (uint32_t)(e->mos_bg_y + 1));
    e->obj_mos_line = line - (line % (uint32_t)(e->mos_obj_y + 1));

    for (b = 0; b < 2; b++) {
        uint32_t l = (e->bgcnt[2 + b] & (1 << 6)) ? e->bg_mos_line : line;
        e->rot_x[b] = (int32_t)((uint32_t)e->rot_x0[b]
                                + l * (uint32_t)(int32_t)e->rot_b[b]);
        e->rot_y[b] = (int32_t)((uint32_t)e->rot_y0[b]
                                + l * (uint32_t)(int32_t)e->rot_d[b]);
    }
}

static void engine_set_line(struct engine *e, uint32_t line) {
    engine_line_values(e, line);
    win_update_y(e, line);
}

/*
 * MASTER_BRIGHT, applied to the finished screen rather than to a layer, so
 * it dims the VRAM display mode and the forced blank too, which is how a fade
 * to white covers everything. Mode 3 does nothing, which is melonDS's reading
 * and the sweep's `master-bright-mode3` config is what holds it.
 */
static uint32_t master_bright(uint32_t c, uint16_t reg) {
    uint32_t mode = (uint32_t)reg >> 14, factor = reg & 0x1Fu;
    uint32_t r = c & 0x3F, g = (c >> 8) & 0x3F, b = (c >> 16) & 0x3F;

    if (factor > 16) factor = 16;
    if (mode == 1) {
        r += ((0x3F - r) * factor) >> 4;
        g += ((0x3F - g) * factor) >> 4;
        b += ((0x3F - b) * factor) >> 4;
    } else if (mode == 2) {
        /* The +15 is not symmetry with the other direction and is not
         * roundable away: without it a full-strength fade to black leaves
         * every channel one step short of 0. */
        r -= ((r * factor) + 15) >> 4;
        g -= ((g * factor) + 15) >> 4;
        b -= ((b * factor) + 15) >> 4;
    } else {
        return c;
    }
    return (c & 0xFF000000u) | r | (g << 8) | (b << 16);
}

/*
 * The wide margins' way in. They are the rasterizer's own pixels, drawn
 * outside the 256 columns this engine composes, and they finish the way
 * every composed pixel finishes: the fade, then the expansion. Keeping the
 * two rules in one place is the point, a margin that expanded its own way
 * would drift from the panel the first time either rule changed.
 */
uint32_t pc_gpu2d_present_px(uint32_t bgr6, uint16_t master_bright_reg) {
    return expand(master_bright(bgr6 & 0x00FFFFFFu, master_bright_reg));
}

/* ------------------------------------------------------------------ */
/* The capture unit.                                  */
/* ------------------------------------------------------------------ */

/*
 * DISPCAPCNT, and it is engine A's alone. The unit takes one or two sources,
 * blends them if it was asked for two, and writes 15-bit halfwords into a
 * bank, so unlike everything else in this file it *changes guest state*, and
 * that is why the whole thing is behind a latch rather than behind a register
 * read at each line: bit 31 is cleared at the end of the frame it captured, so
 * a game that sets it gets exactly one frame of capture.
 *
 * The mask is the hardware's write mask, applied here rather than at the store
 * for the reason gives; this register is outside every
 * hooked window, so nothing sees the store and the value in guest memory is
 * whatever the game put there. Bits 5-7, 13-15, 22-23 and 28 do not exist, and
 * masking at the latch is what makes a read-back after a captured frame agree
 * with a console's.
 *
 * Why it does not matter that the port draws a whole engine at a time. Upstream
 * interleaves: it draws line L of engine A, then line L of engine B, then
 * captures line L, so a capture could in principle land under engine B's nose
 * mid-frame. It cannot: capture only writes a bank that is in LCDC, and a bank
 * in LCDC is by construction not mapped as anyone's background, sprite,
 * palette or texture memory. The one reader of an LCDC bank is engine A's own
 * VRAM display mode, and that is applied after the line loop here and after
 * DoCapture there; both of which read the frame before the capture, because
 * the capture bit is cleared before the frame that shows the result.
 */
#define DISPCAPCNT      0x04000064u
#define DISPCAPCNT_MASK 0xEF3F1F1Fu

static void do_capture(uint32_t line, const uint32_t *src2d, uint32_t capcnt) {
    uint32_t width, height, sz = (capcnt >> 20) & 3u;
    uint32_t dstbank, dstaddr, i;
    volatile uint16_t *dst;
    const uint32_t *srcA;
    const uint16_t *srcB = NULL;

    if (sz == 0u) { width = 128u; height = 128u; }
    else          { width = 256u; height = 64u * sz; }
    if (line >= height) return;

    /* A destination that is not in LCDC is not written at all, the capture
     * does not fall back to the bank's other role and does not stall. */
    dstbank = (capcnt >> 16) & 3u;
    if (!armrec_vram_bank_in_lcdc((int)dstbank)) return;

    /* Both offsets count *halfwords*, so the 0x4000 step is 32 KB and the
     * wrap is at 64K halfwords, which is one bank. */
    dst = (volatile uint16_t *)armrec_vram_bank_ptr((int)dstbank);
    dstaddr = (((capcnt >> 18) & 3u) << 14) + line * width;
    dst += (dstaddr & 0xFFFFu);

    if (capcnt & (1u << 24)) {
        if (!pc_gpu3d_soft_present())
            gpu2d_trap("DISPCAPCNT selects the 3D engine as capture source A "
                       "and this binary has no rasterizer linked in",
                       DISPCAPCNT, capcnt);
        srcA = pc_gpu3d_soft_line((int)line);
    } else {
        srcA = src2d;
    }

    if (capcnt & (1u << 25)) {
        gpu2d_trap("DISPCAPCNT selects the main-memory display FIFO as capture "
                   "source B, which needs the display FIFO this port has not "
                   "built", DISPCAPCNT, capcnt);
    } else {
        /*
         * Source B is the bank engine A's VRAM display mode would scan out,
         * named by DISPCNT rather than by DISPCAPCNT, one field, two uses.
         * The read offset applies only when that mode is *not* selected, which
         * is the hardware's rule and not a simplification: in VRAM display mode
         * the two units read the same line of the same bank.
         */
        uint32_t dispcnt = io32(IO_A + 0x000u);
        uint32_t srcvram = (dispcnt >> 18) & 3u;
        if (armrec_vram_bank_in_lcdc((int)srcvram)) {
            uint32_t offset = line * 256u;
            if (((dispcnt >> 16) & 3u) != 2u)
                offset += ((capcnt >> 26) & 3u) << 14;
            srcB = (const uint16_t *)armrec_vram_bank_ptr((int)srcvram)
                 + (offset & 0xFFFFu);
        }
    }

    switch ((capcnt >> 29) & 3u) {
    case 0:
        /* Source A alone. The alpha bit is "this pixel came from somewhere",
         * which for a 2D source is every pixel, even the backdrop carries a
         * flag byte, and for a 3D one is the polygon coverage. */
        for (i = 0; i < width; i++) {
            uint32_t v = srcA[i];
            dst[i] = (uint16_t)(((v >> 1) & 0x1Fu) | (((v >> 9) & 0x1Fu) << 5)
                                | (((v >> 17) & 0x1Fu) << 10)
                                | ((v >> 24) ? 0x8000u : 0u));
        }
        break;

    case 1:
        /* Source B alone: a straight halfword copy, alpha bit included. A
         * source bank that is not in LCDC reads as zero rather than as its
         * other role. */
        for (i = 0; i < width; i++) dst[i] = srcB ? srcB[i] : 0u;
        break;

    default:
        /*
         * A + B. Each side is multiplied by its *own* alpha as well as by its
         * coefficient, so a transparent 3D pixel contributes nothing however
         * large EVA is, and the result's alpha is set by whichever coefficient
         * is nonzero rather than by the sources.
         */
        {
            uint32_t eva = capcnt & 0x1Fu, evb = (capcnt >> 8) & 0x1Fu;
            if (eva > 16u) eva = 16u;
            if (evb > 16u) evb = 16u;

            for (i = 0; i < width; i++) {
                uint32_t v = srcA[i];
                uint32_t rA = (v >> 1) & 0x1Fu, gA = (v >> 9) & 0x1Fu;
                uint32_t bA = (v >> 17) & 0x1Fu, aA = (v >> 24) ? 1u : 0u;
                uint32_t rB = 0, gB = 0, bB = 0, aB = 0;
                uint32_t rD, gD, bD, aD;

                if (srcB) {
                    uint32_t w = srcB[i];
                    rB = w & 0x1Fu; gB = (w >> 5) & 0x1Fu;
                    bB = (w >> 10) & 0x1Fu; aB = w >> 15;
                }

                rD = ((rA * aA * eva) + (rB * aB * evb) + 8u) >> 4;
                gD = ((gA * aA * eva) + (gB * aB * evb) + 8u) >> 4;
                bD = ((bA * aA * eva) + (bB * aB * evb) + 8u) >> 4;
                aD = (eva ? aA : 0u) | (evb ? aB : 0u);

                if (rD > 0x1Fu) rD = 0x1Fu;
                if (gD > 0x1Fu) gD = 0x1Fu;
                if (bD > 0x1Fu) bD = 0x1Fu;

                dst[i] = (uint16_t)(rD | (gD << 5) | (bD << 10) | (aD << 15));
            }
        }
        break;
    }
}

/*
 * The engines another compositor has taken over for this frame; see
 * pc_gpu2d_set_external(). NULL, and therefore neither, unless a port
 * registers one.
 */
static pc_gpu2d_external_fn external_fn;

void pc_gpu2d_set_external(pc_gpu2d_external_fn fn) {
    external_fn = fn;
}

/* ------------------------------------------------------------------ */
/* The compose, across cores                                           */
/* ------------------------------------------------------------------ */
/*
 * An engine composes 192 scanlines from ONE register snapshot; that is what
 * engine_begin() latches, and it is what makes this renderer a frame at a
 * time rather than a line at a time. A scanline therefore depends on the
 * snapshot and on its own line number and on nothing another scanline
 * computed, so a band of lines is a job. Each band gets an engine struct of
 * its own to scribble in: that struct is the snapshot plus a line's worth of
 * scratch (the composed line, the sprite line, the window mask) so copies
 * do not interact.
 *
 * The one thing that is a latch rather than a function of the line is the
 * window's vertical enable, and engine_seek() winds it forward the way the
 * serial loop does.
 */
struct compose_job {
    uint32_t (*out)[PC_VIDEO_PIXELS];
    uint32_t capcnt;
    int capturing;
    int only;                   /* the engine this job composes */
};

static struct engine BandEng[PC_WORKERS_MAX];

/*
 * Rows off a cursor, the 3D render's own fix carried over: precut bands let
 * whichever band landed on the busiest rows (or the slowest core) decide
 * the frame, so rows are handed out in chunks from a shared cursor and a
 * fast thread simply comes back for more.
 *
 * What made precut bands cheap to seek was also what capped them at six:
 * engine_seek() replayed every line from row 0 to the band's first, so more
 * bands meant more replay. The replay's only real content is the window
 * unit's vertical latch (everything else engine_set_line computes is a pure
 * function of the line), so the frame prepares ONE template engine and
 * records that latch at every line, the same walk a serial compose makes,
 * made once, and a chunk seeds itself by copying the template and patching
 * the latch. Seek is O(1) wherever the chunk starts.
 */
static struct engine TemplateEng;
static uint8_t WinAtLine[PC_VIDEO_HEIGHT][2];
static volatile int32_t ComposeCursor;
static int32_t ComposeChunk;

static void compose_prepare(int num)
{
    struct engine *t = &TemplateEng;
    uint32_t line;

    engine_begin(t, num, 0);
    WinAtLine[0][0] = t->winactive[0];
    WinAtLine[0][1] = t->winactive[1];
    for (line = 1; line < PC_VIDEO_HEIGHT; line++) {
        win_update_y(t, line);
        WinAtLine[line][0] = t->winactive[0];
        WinAtLine[line][1] = t->winactive[1];
    }
    /* The walk above left the template holding the last line's latch; put
     * line 0's back so a copy starts where engine_begin() ended. */
    t->winactive[0] = WinAtLine[0][0];
    t->winactive[1] = WinAtLine[0][1];
}

static void compose_chunk_seek(struct engine *e, uint32_t l0)
{
    *e = TemplateEng;
    e->winactive[0] = WinAtLine[l0][0];
    e->winactive[1] = WinAtLine[l0][1];
    engine_line_values(e, l0);
}

static void compose_band(void *ctx, int slice, int nslices)
{
    struct compose_job *J = (struct compose_job *)ctx;
    struct engine *e = &BandEng[slice];
    uint32_t l0, l1, line;
    int i;

    (void)nslices;
    for (;;) {
        l0 = (uint32_t)__atomic_fetch_add(&ComposeCursor, ComposeChunk,
                                          __ATOMIC_ACQ_REL);
        if (l0 >= PC_VIDEO_HEIGHT) break;
        l1 = l0 + (uint32_t)ComposeChunk;
        if (l1 > PC_VIDEO_HEIGHT) l1 = PC_VIDEO_HEIGHT;
        compose_chunk_seek(e, l0);
        for (line = l0; line < l1; line++) {
            /* Sprites first: draw_scanline() interleaves the OBJ layer
             * between the backgrounds and needs this line's already
             * resolved. The hardware rasterises them one scanline ahead for
             * the same reason. */
            draw_sprites(e, line);
            draw_scanline(e, line);
            if (J->capturing && J->only == 0)
                do_capture(line, e->line, J->capcnt);
            for (i = 0; i < 256; i++)
                J->out[J->only][line * 256 + i] = e->line[i] & 0x00FFFFFFu;
            engine_set_line(e, line + 1);
        }
    }
}

static void compose_run(struct compose_job *J)
{
    int slices;

    compose_prepare(J->only);
    /*
     * A capturing frame composes on one thread. The capture unit writes guest
     * VRAM a line at a time, the one thing in a frame that is not simply
     * this engine's own pixels, and a game asks for it on the frame a screen
     * transition starts, which is not where the time is.
     */
    if (J->capturing && J->only == 0) {
        ComposeCursor = 0;
        ComposeChunk = PC_VIDEO_HEIGHT;
        compose_band(J, 0, 1);
        return;
    }
    slices = pc_workers_slices();
    if (slices > (int)PC_VIDEO_HEIGHT) slices = (int)PC_VIDEO_HEIGHT;
    ComposeChunk = (int32_t)PC_VIDEO_HEIGHT / (slices * 4);
    if (ComposeChunk < 8) ComposeChunk = 8;
    ComposeCursor = 0;
    pc_workers_run_n(compose_band, J, slices);
}

void pc_gpu2d_render(uint64_t frame) {
    /*
     * Static, not automatic: 384 KB of surfaces would be a large stack frame
     * anywhere and this runs on a guest fiber, whose stack is the size
     * OS_CreateThread was given. Every path below writes all of `out` before
     * anything reads it, so keeping it across calls carries no state.
     */
    static uint32_t out[2][PC_VIDEO_PIXELS];
    uint16_t powcnt;
    uint32_t capcnt;
    int screens_on, capturing, n, i;
    int external[2], compose[2] = { 0, 0 };
    uint32_t line;
    static struct compose_job job;

    PC_BENCH_BEGIN(bench_t);

    (void)frame;

    /*
     * "VRAMCNT may have changed", cheap when it has not, and it makes this
     * function a pure function of guest state rather than of guest state plus
     * whether a hook fired. armrec_vram_touch() is idempotent.
     */
    armrec_vram_touch();

    /* The mosaic table builds itself on first use, and a band of scanlines
     * may be composed on another thread, so it is built here, where there
     * is only one. Idempotent and a few thousand bytes. */
    (void)mosaic_row(0);

    powcnt = io16(0x04000304u);
    screens_on = (powcnt & 1u) != 0;

    /* The capture latch, read once for the whole frame. */
    capcnt = io32(DISPCAPCNT) & DISPCAPCNT_MASK;
    capturing = (capcnt & 0x80000000u) != 0;

    for (n = 0; n < 2; n++) {
        int enabled = (powcnt & (n ? (1u << 9) : (1u << 1))) != 0;
        uint32_t dispcnt = io32((n ? IO_B : IO_A) + 0x000u);
        uint32_t mode = n ? ((dispcnt >> 16) & 1u) : ((dispcnt >> 16) & 3u);

        external[n] = external_fn != NULL && external_fn(n);
        if (external[n]) continue;

        if (!enabled || (dispcnt & (1u << 7))) {
            /* A disabled engine A is black and a disabled engine B is white,
             * and a forced blank is white on both; the hardware's answers,
             * not a convention here. */
            uint32_t fill = (!enabled && n == 0) ? 0x000000u : 0x3F3F3Fu;
            for (i = 0; i < PC_VIDEO_PIXELS; i++) out[n][i] = fill;
            /*
             * ...and the capture unit still runs over it. What it sees is the
             * same fill with a full flag byte, because upstream's fixed colour
             * for these two cases is 0xFF000000 / 0xFF3F3F3F, so every
             * captured pixel of a blanked engine A has its alpha bit set,
             * which a plain `fill` would get wrong.
             */
            if (n == 0 && capturing) {
                static uint32_t blank[256];
                for (i = 0; i < 256; i++) blank[i] = fill | 0xFF000000u;
                for (line = 0; line < PC_VIDEO_HEIGHT; line++)
                    do_capture(line, blank, capcnt);
            }
        } else {
            compose[n] = 1;
        }

        /*
         * The display selector sits above all of that: mode 1 shows what was
         * just composed, and the other three do not look at it at all. Mode 0
         * is also the one that MASTER_BRIGHT does *not* reach, a display
         * that is off is off, and no fade dims it.
         */
        if (compose[n]) {
            job.out = out;
            job.capcnt = capcnt;
            job.capturing = capturing;
            job.only = n;
            compose_run(&job);
            compose[n] = 0;
        }
        if (mode == 0) {
            for (i = 0; i < PC_VIDEO_PIXELS; i++) out[n][i] = 0x3F3F3Fu;
        } else {
            if (mode == 2) {
                /* A bank scanned out directly, 256 halfwords a line. Not
                 * through a window: this mode reads the bank itself, and a
                 * bank that is not in LCDC shows black rather than showing
                 * whatever it holds in its other role. */
                int bank = (int)((dispcnt >> 18) & 3u);
                const uint16_t *src =
                    armrec_vram_bank_in_lcdc(bank)
                        ? (const uint16_t *)armrec_vram_bank_ptr(bank)
                        : NULL;
                for (i = 0; i < PC_VIDEO_PIXELS; i++)
                    out[n][i] = src ? rgb15to18_flat(src[i]) : 0u;
            } else if (mode == 3) {
                gpu2d_trap("DISPCNT selects the main-memory display mode, "
                           "which needs a display FIFO this port has not "
                           "built", (n ? IO_B : IO_A) + 0, dispcnt);
            }
            {
                uint16_t mb = io16((n ? IO_B : IO_A) + 0x06Cu);
                for (i = 0; i < PC_VIDEO_PIXELS; i++)
                    out[n][i] = master_bright(out[n][i], mb);
            }
        }
    }

    /*
     * ...and the capture bit falls at the end of the frame it captured, which
     * is why a game asking for a capture gets exactly one. The value written
     * back is the *masked* one, so the bits hardware never stored are gone
     * from a read-back too. This is the one write this function makes to guest
     * state and it is why test_bg's "the renderer changes nothing" section is
     * a claim about a frame with capture disabled.
     */
    if (capturing)
        *(volatile uint32_t *)G2D_HOST(DISPCAPCNT) = capcnt & ~0x80000000u;

    /* screens_on is loop-invariant and this loop runs 98,304 times a frame;
     * the test belongs outside it. Byte-identical output either way, what
     * changes is only where the branch is. */
    for (n = 0; n < 2; n++) {
        uint32_t *dst = pc_video_surface(n);
        if (!dst || external[n]) continue;
        if (screens_on) {
            for (i = 0; i < PC_VIDEO_PIXELS; i++) dst[i] = expand(out[n][i]);
        } else {
            memset(dst, 0, (size_t)PC_VIDEO_PIXELS * sizeof *dst);
        }
    }
    /* The last job of the frame: what follows is the guest's own code, so
     * the pool should sleep rather than spin waiting for the next one. */
    pc_workers_park();

    PC_BENCH_END(PC_BENCH_2D, bench_t);
}

void pc_gpu2d_install(void) {
    /* The manifest names what the renderer covers rather than that there is
     * one, because a screen with no sprites on it and a screen from a renderer
     * that cannot draw them look the same in a PNG. */
    pc_video_set_renderer(pc_gpu2d_render,
                          "gpu2d-bg-obj-win-blend-mosaic-3d-capture");
}
