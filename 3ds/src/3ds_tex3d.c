/*
 * 3ds/src/3ds_tex3d.c: see 3ds_tex3d.h.
 *
 * The converter is texture_lookup() with the wrapping taken out and the
 * Expansion put in. That is the whole of it, and saying it that way is the
 * point: pc_gpu3d_soft.c is the oracle for what a DS texel is, the same way
 * pc_gpu2d.c is the oracle for what a background pixel is, and
 * 3ds/tests/tex3d_convert.c checks this file against that one texel by texel
 * rather than against a table copied out of a document.
 *
 * The wrapping comes out because the PICA's sampler does it, three modes,
 * all three exact on power-of-two sides. The expansion goes in because the
 * software renderer does it later, in render_pixel(), and a converted image
 * has to already be in the eight-bit form a PICA texture holds.
 *
 * Why every format is here when the survey found four. The 3D survey counted six
 * million polygons of one replay: PLTT256, COMP4x4 and direct colour never
 * appear in the first three minutes of a new game. That is a statement about
 * three minutes and not about the game, and the three missing formats are a
 * dozen lines between them, a converter that silently drew the wrong picture
 * on a texture nobody had looked for would be a worse trade than writing them.
 * What the survey really bought is the CACHE's shape, which is sized from the
 * measured working set and not from the format list.
 */

#include "3ds_tex3d.h"

#include <string.h>

#include "3ds_tile.h"
#include "3ds_vram.h"

/* ------------------------------------------------------------------ */
/* The two address spaces                                              */
/* ------------------------------------------------------------------ */
/*
 * Latched, the way pc_gpu3d_soft.c latches them, and it is not only speed.
 * vram_texture() and vram_texpal() walk the nine VRAMCNT bytes and format a
 * role string so that two banks claiming one slot can be named, so they cost
 * an snprintf, and a converter calling one per byte would spend more time
 * deciding where memory is than reading it. They are taken once per image
 * instead.
 *
 * That is also the right answer for correctness and not a trade against it: an
 * image is converted inside one call, guest code cannot run underneath it, and
 * a mapping that changed halfway through would produce an image that is half
 * of each; which is the bug the latch makes impossible rather than unlikely.
 */
static const uint8_t *sTexSlot[4];
static const uint8_t *sPalSlot[8];

static void latch_slots(void)
{
    int i;

    for (i = 0; i < 4; i++) {
        sTexSlot[i] = (const uint8_t *)vram_texture(i);
    }
    for (i = 0; i < 8; i++) {
        sPalSlot[i] = (const uint8_t *)vram_texpal(i);
    }
}

static uint32_t tex_read8(uint32_t addr)
{
    const uint8_t *p = sTexSlot[(addr >> 17) & 3];
    return p != NULL ? p[addr & 0x1FFFF] : 0;
}

static uint32_t tex_read16(uint32_t addr)
{
    return tex_read8(addr) | (tex_read8(addr + 1) << 8);
}

static uint32_t texpal_read16(uint32_t addr)
{
    const uint8_t *p = sPalSlot[(addr >> 14) & 7];

    if (p == NULL) {
        return 0;
    }
    /* Every palette read is at an even offset from a `<< 3` or `<< 4` base, so
     * it cannot straddle a 16 KB slot, the same argument pc_gpu3d_soft.c
     * makes, and it is assembled from two bytes here for the same reason. */
    return p[addr & 0x3FFF] | ((uint32_t)p[(addr + 1) & 0x3FFF] << 8);
}

/* ------------------------------------------------------------------ */
/* Shape                                                               */
/* ------------------------------------------------------------------ */

unsigned tex3d_width(uint32_t texparam)
{
    return 8u << ((texparam >> 20) & 7);
}

unsigned tex3d_height(uint32_t texparam)
{
    return 8u << ((texparam >> 23) & 7);
}

unsigned tex3d_texels(uint32_t texparam)
{
    return tex3d_width(texparam) * tex3d_height(texparam);
}

int tex3d_wrap_s(uint32_t texparam)
{
    if (!(texparam & (1u << 16))) return TEX3D_WRAP_CLAMP;
    return (texparam & (1u << 18)) ? TEX3D_WRAP_MIRROR : TEX3D_WRAP_REPEAT;
}

int tex3d_wrap_t(uint32_t texparam)
{
    if (!(texparam & (1u << 17))) return TEX3D_WRAP_CLAMP;
    return (texparam & (1u << 19)) ? TEX3D_WRAP_MIRROR : TEX3D_WRAP_REPEAT;
}

/* ------------------------------------------------------------------ */
/* One texel                                                           */
/* ------------------------------------------------------------------ */

/*
 * RGB555 to the eight bits a PICA texture holds, through the six the DS
 * composites in. render_pixel() does the first step, `v * 2, and + 1 when
 * that is not zero`, and the surface expansion does the second by
 * replicating the top two bits. Both are exact and neither is a rounding
 * choice of this file's.
 */
static uint32_t expand555(uint32_t c15, uint32_t alpha5)
{
    uint32_t r = (c15 << 1) & 0x3Eu;
    uint32_t g = (c15 >> 4) & 0x3Eu;
    uint32_t b = (c15 >> 9) & 0x3Eu;

    if (r) r++;
    if (g) g++;
    if (b) b++;
    r = (r << 2) | (r >> 4);
    g = (g << 2) | (g >> 4);
    b = (b << 2) | (b >> 4);
    /* 0-31 to 0-255 the only way that keeps both ends: 0 stays transparent
     * and 31 reaches opaque. The DS's own blend weights an alpha as
     * (a + 1) / 32, which is a property of the blend unit and belongs with it
     * in the 3D producer, not with the image. */
    alpha5 = (alpha5 << 3) | (alpha5 >> 2);
    return (r << 24) | (g << 16) | (b << 8) | alpha5;
}

static uint32_t texel_at(uint32_t texparam, uint32_t texpal, int si, int ti)
{
    uint32_t vramaddr = (texparam & 0xFFFFu) << 3;
    uint32_t width = tex3d_width(texparam);
    uint32_t alpha0 = (texparam & (1u << 29)) ? 0u : 31u;
    uint32_t color = 0, alpha = 0;

    switch ((texparam >> 26) & 7) {
    case 0:
        /* No texture. render_pixel() never calls the unit at all in this
         * case; a converted image of it is one transparent texel, which is
         * the answer that composes to the same picture. */
        return 0;

    case 1: {   /* A3I5 */
        uint32_t pixel;

        vramaddr += (uint32_t)(ti * (int)width + si);
        pixel = tex_read8(vramaddr);
        texpal <<= 4;
        color = texpal_read16(texpal + ((pixel & 0x1Fu) << 1));
        alpha = ((pixel >> 3) & 0x1Cu) + (pixel >> 6);
        break;
    }

    case 2: {   /* 4-colour */
        uint32_t pixel;

        vramaddr += (uint32_t)((ti * (int)width + si) >> 2);
        pixel = (tex_read8(vramaddr) >> ((si & 3) << 1)) & 3u;
        texpal <<= 3;
        color = texpal_read16(texpal + (pixel << 1));
        alpha = (pixel == 0) ? alpha0 : 31u;
        break;
    }

    case 3: {   /* 16-colour */
        uint32_t pixel;

        vramaddr += (uint32_t)((ti * (int)width + si) >> 1);
        pixel = tex_read8(vramaddr);
        pixel = (si & 1) ? (pixel >> 4) : (pixel & 0xFu);
        texpal <<= 4;
        color = texpal_read16(texpal + (pixel << 1));
        alpha = (pixel == 0) ? alpha0 : 31u;
        break;
    }

    case 4: {   /* 256-colour */
        uint32_t pixel;

        vramaddr += (uint32_t)(ti * (int)width + si);
        pixel = tex_read8(vramaddr);
        texpal <<= 4;
        color = texpal_read16(texpal + (pixel << 1));
        alpha = (pixel == 0) ? alpha0 : 31u;
        break;
    }

    case 5: {   /* 4x4 compressed */
        uint32_t slot1addr, val, palinfo, paloffset;

        /*
         * The block's texels and the halfword naming its palette live in
         * DIFFERENT SLOTS, and reading the second out of the first draws a
         * plausible wrong picture. This is pc_gpu3d_soft.c's arithmetic
         * unchanged, down to the wrap after slot 3.
         */
        vramaddr += (uint32_t)((ti & 0x3FC) * ((int)width >> 2))
                  + (uint32_t)(si & 0x3FC);
        vramaddr += (uint32_t)(ti & 3);
        vramaddr &= 0x7FFFFu;

        slot1addr = 0x20000u + ((vramaddr & 0x1FFFCu) >> 1);
        if (vramaddr >= 0x40000u) {
            slot1addr += 0x10000u;
        }

        if (vramaddr >= 0x20000u && vramaddr < 0x40000u) {
            val = 0;    /* reading slot 1 for texels always reads 0 */
        } else {
            val = tex_read8(vramaddr) >> (2 * (si & 3));
        }

        palinfo = tex_read16(slot1addr);
        paloffset = (palinfo & 0x3FFFu) << 2;
        texpal <<= 4;
        alpha = 31u;

        switch (val & 3u) {
        case 0:
            color = texpal_read16(texpal + paloffset);
            break;
        case 1:
            color = texpal_read16(texpal + paloffset + 2);
            break;
        case 2:
            if ((palinfo >> 14) == 1 || (palinfo >> 14) == 3) {
                uint32_t c0 = texpal_read16(texpal + paloffset);
                uint32_t c1 = texpal_read16(texpal + paloffset + 2);
                uint32_t r0 = c0 & 0x001F, g0 = c0 & 0x03E0, b0 = c0 & 0x7C00;
                uint32_t r1 = c1 & 0x001F, g1 = c1 & 0x03E0, b1 = c1 & 0x7C00;

                if ((palinfo >> 14) == 1) {
                    color = ((r0 + r1) >> 1)
                          | (((g0 + g1) >> 1) & 0x03E0)
                          | (((b0 + b1) >> 1) & 0x7C00);
                } else {
                    color = ((r0 * 5 + r1 * 3) >> 3)
                          | (((g0 * 5 + g1 * 3) >> 3) & 0x03E0)
                          | (((b0 * 5 + b1 * 3) >> 3) & 0x7C00);
                }
            } else {
                color = texpal_read16(texpal + paloffset + 4);
            }
            break;
        default:
            if ((palinfo >> 14) == 2) {
                color = texpal_read16(texpal + paloffset + 6);
            } else if ((palinfo >> 14) == 3) {
                uint32_t c0 = texpal_read16(texpal + paloffset);
                uint32_t c1 = texpal_read16(texpal + paloffset + 2);
                uint32_t r0 = c0 & 0x001F, g0 = c0 & 0x03E0, b0 = c0 & 0x7C00;
                uint32_t r1 = c1 & 0x001F, g1 = c1 & 0x03E0, b1 = c1 & 0x7C00;

                color = ((r0 * 3 + r1 * 5) >> 3)
                      | (((g0 * 3 + g1 * 5) >> 3) & 0x03E0)
                      | (((b0 * 3 + b1 * 5) >> 3) & 0x7C00);
            } else {
                color = 0;
                alpha = 0;
            }
            break;
        }
        break;
    }

    case 6: {   /* A5I3 */
        uint32_t pixel;

        vramaddr += (uint32_t)(ti * (int)width + si);
        pixel = tex_read8(vramaddr);
        texpal <<= 4;
        color = texpal_read16(texpal + ((pixel & 7u) << 1));
        alpha = pixel >> 3;
        break;
    }

    default:    /* 7: direct colour */
        vramaddr += (uint32_t)((ti * (int)width + si) << 1);
        color = tex_read16(vramaddr);
        alpha = (color & 0x8000u) ? 31u : 0u;
        break;
    }

    return expand555(color, alpha);
}

/* One texel on its own, for a caller that is asking a question rather than
 * converting an image, the self-test and 3ds/tests/tex3d_convert.c. It pays
 * the latch every call, which is why the converter below does not use it. */
uint32_t tex3d_texel(uint32_t texparam, uint32_t texpal, int si, int ti)
{
    latch_slots();
    return texel_at(texparam, texpal, si, ti);
}

/* ------------------------------------------------------------------ */
/* The whole image                                                     */
/* ------------------------------------------------------------------ */

/* Morton order inside a block, taken once: an image can be 65,536 texels and
 * the shifts are worth more as one load. The tile cache's tile_swizzle() is the oracle
 * for what goes where, and this is a table of its answers. */
static uint8_t sSwizzle[TILE_TEXELS];
static int sSwizzleReady;

static void swizzle_table(void)
{
    int x, y;

    if (sSwizzleReady) {
        return;
    }
    for (y = 0; y < TILE_SIDE; y++) {
        for (x = 0; x < TILE_SIDE; x++) {
            sSwizzle[y * TILE_SIDE + x] = (uint8_t)tile_swizzle(x, y);
        }
    }
    sSwizzleReady = 1;
}

int tex3d_convert(uint32_t *out, uint32_t texparam, uint32_t texpal)
{
    unsigned w = tex3d_width(texparam);
    unsigned h = tex3d_height(texparam);
    unsigned blockCols = w / TILE_SIDE;
    unsigned by, bx, y, x;

    if (out == NULL || w * h > TEX3D_MAX_TEXELS) {
        return -1;
    }
    swizzle_table();
    latch_slots();

    /* Walked a block at a time rather than a row at a time: the destination is
     * then one contiguous run of 64 words per block, which is the order the
     * texture is read in and the order a DMA would want. The source reads jump
     * around instead, and they are the cheap side, one byte or one nibble. */
    for (by = 0; by < h / TILE_SIDE; by++) {
        for (bx = 0; bx < blockCols; bx++) {
            uint32_t *dst = out + (size_t)(by * blockCols + bx) * TILE_TEXELS;

            for (y = 0; y < TILE_SIDE; y++) {
                const uint8_t *sw = sSwizzle + y * TILE_SIDE;

                for (x = 0; x < TILE_SIDE; x++) {
                    dst[sw[x]] = texel_at(texparam, texpal,
                                          (int)(bx * TILE_SIDE + x),
                                          (int)(by * TILE_SIDE + y));
                }
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The cache                                                           */
/* ------------------------------------------------------------------ */

#define TEX3D_ENTRIES 256       /* the survey's ceiling; it saw 28 at peak */

struct entry {
    uint32_t key;               /* texparam & TEX3D_KEY_MASK              */
    uint32_t pal;
    uint32_t off;               /* words into the pool                     */
    uint32_t size;              /* ... and how many                        */
    uint16_t texgen;            /* the generation its texture bank had     */
    uint16_t palgen;            /* ... and its palette bank                */
    int8_t texbank, palbank;    /* ... and which banks those were          */
    uint8_t used;
};

static struct entry sEntry[TEX3D_ENTRIES];
static int sCount;
static uint32_t *sPool;
static size_t sPoolWords;
static size_t sPoolUsed;
static unsigned sLastFrame;
static int sHaveFrame;
static int sOverflowed;             /* this frame asked for more than fits */
static struct tex3d_cache_stats sStats;

/*
 * Nine generations, one per VRAM bank, and the slot is resolved at both ends.
 * The first version of this counted per texture and palette SLOT, and the test
 * showed why that cannot work: the SDK loads a texture by putting the bank in
 * LCDC, writing it, and only then giving it back to the texture engine
 * (GX_BeginLoadTex / GX_EndLoadTex). While the write is happening the bank is
 * not a texture slot at all, so a slot-keyed report has nothing to bump and
 * every cached image survived its own replacement.
 *
 * A bank keeps its identity across that, so the generation is the bank's. An
 * entry records which bank its texture slot and its palette slot were pointing
 * into, and a lookup re-derives both: a different bank is as stale as a
 * different generation, which is what makes a remap need no reset of its own.
 */
static uint16_t sBankGen[VRAM_BANKS];

/* Which bank a slot pointer lands in. The slot accessors hand back a bank's
 * storage, possibly at an offset, E's 64 KB fills four palette slots, so
 * this is a range test over the nine. -1 when nothing is mapped there. */
static int bank_of_ptr(const void *p)
{
    int b;

    if (p == NULL) {
        return -1;
    }
    for (b = 0; b < VRAM_BANKS; b++) {
        const char *base = (const char *)vram_bank_ptr(b);
        uint32_t size = vram_bank_size(b);

        if (base != NULL && (const char *)p >= base
            && (const char *)p < base + size) {
            return b;
        }
    }
    return -1;
}

/* Which slot an image's bytes live in. A texture address is `<< 3` into a
 * 512 KB space; a palette address is `<< 3` for the 4-colour format and
 * `<< 4` for every other one, into a 128 KB space. That single asymmetry is
 * the only arithmetic in this file with no second reader, so it has a check
 * of its own below. */
static int tex_slot_of(uint32_t texparam)
{
    return (int)((((texparam & 0xFFFFu) << 3) >> 17) & 3);
}

static int pal_slot_of(uint32_t texparam, uint32_t texpal)
{
    uint32_t shift = (((texparam >> 26) & 7) == 2) ? 3u : 4u;

    return (int)(((texpal << shift) >> 14) & 7);
}

static int tex_bank_of(uint32_t texparam)
{
    return bank_of_ptr(vram_texture(tex_slot_of(texparam)));
}

static int pal_bank_of(uint32_t texparam, uint32_t texpal)
{
    return bank_of_ptr(vram_texpal(pal_slot_of(texparam, texpal)));
}

int tex3d_cache_init(uint32_t *pool, size_t words)
{
    if (pool == NULL || words < TEX3D_MAX_TEXELS) {
        return -1;
    }
    sPool = pool;
    sPoolWords = words;
    tex3d_cache_reset();
    return 0;
}

void tex3d_cache_reset(void)
{
    memset(sEntry, 0, sizeof sEntry);
    sCount = 0;
    sPoolUsed = 0;
    sHaveFrame = 0;
    sOverflowed = 0;
}

void tex3d_cache_dirty(uint32_t addr, uint32_t bytes)
{
    int bank;

    if (sPool == NULL || bytes == 0) {
        return;
    }
    sStats.reports++;
    /*
     * One bank, not a range walk, and that is a deliberate floor. A bulk copy
     * into VRAM lands in one bank in every case this port has seen, the
     * SDK's texture load remaps a bank into LCDC and DMAs into it, and a
     * texture cannot straddle two banks on hardware either. The first address
     * is asked; a report that started outside VRAM entirely is ignored.
     */
    if (!vram_lookup(addr, &bank, NULL)) {
        return;
    }
    if (bank >= 0 && bank < VRAM_BANKS) {
        sBankGen[bank]++;
        sStats.bumps++;
    }
}

int tex3d_cache_get(uint32_t texparam, uint32_t texpal, unsigned frame,
                    int *converted)
{
    uint32_t key = texparam & TEX3D_KEY_MASK;
    unsigned size = tex3d_texels(texparam);
    int texbank = tex_bank_of(texparam);
    int palbank = pal_bank_of(texparam, texpal);
    uint16_t texgen = texbank >= 0 ? sBankGen[texbank] : 0;
    uint16_t palgen = palbank >= 0 ? sBankGen[palbank] : 0;
    int i, victim = -1;

    if (converted != NULL) {
        *converted = 0;
    }
    if (sPool == NULL) {
        return -1;
    }
    /*
     * A new frame, and the previous one asked for more than the pool holds.
     * This is the only safe moment to empty it: nothing this frame has been
     * handed an entry yet.
     */
    if (!sHaveFrame || frame != sLastFrame) {
        sLastFrame = frame;
        sHaveFrame = 1;
        if (sOverflowed) {
            sStats.evictions++;
            memset(sEntry, 0, sizeof sEntry);
            sCount = 0;
            sPoolUsed = 0;
            sOverflowed = 0;
        }
    }
    if (size > TEX3D_MAX_TEXELS) {
        sStats.refused++;
        return -1;
    }

    for (i = 0; i < sCount; i++) {
        struct entry *e = &sEntry[i];

        if (!e->used || e->key != key || e->pal != texpal) {
            continue;
        }
        if (e->texbank == (int8_t)texbank && e->palbank == (int8_t)palbank
            && e->texgen == texgen && e->palgen == palgen) {
            sStats.hits++;
            return i;
        }
        /*
         * The image is where it was and its memory has changed under it. The
         * entry is re-converted IN PLACE rather than appended: the pool would
         * otherwise grow by one image every time the game uploaded a texture,
         * and its size has not changed; it is the same TexParam.
         */
        sStats.stale++;
        victim = i;
        break;
    }

    /*
     * A linear scan, and the survey is why. 28 distinct images at the peak
     * against a table of 256, asked once per polygon batch and not once per
     * polygon, open addressing would be the right answer at the tile cache's four
     * thousand tiles a frame and is a hash function nobody needs at this one.
     */
    if (victim < 0) {
        sStats.misses++;
        if (sCount < TEX3D_ENTRIES && sPoolUsed + size <= sPoolWords) {
            victim = sCount++;
            sEntry[victim].off = (uint32_t)sPoolUsed;
            sPoolUsed += size;
        } else {
            /*
             * Full, and the answer is a refusal for the rest of the frame and
             * not an eviction. The pool is a bump allocator over images of
             * different sizes, so emptying it mid-frame would move every image
             * the caller has already been handed an entry for; it would draw
             * this frame's polygons out of another frame's texels, which is a
             * wrong picture rather than a missing one. The emptying happens at
             * the top of the next frame instead; this frame is simply not the
             * PICA's, which is a decision the seam already knows how to make.
             */
            sOverflowed = 1;
            sStats.full++;
            return -1;
        }
    }

    {
        struct entry *e = &sEntry[victim];

        if (tex3d_convert(sPool + e->off, texparam, texpal) != 0) {
            e->used = 0;
            sStats.refused++;
            return -1;
        }
        e->key = key;
        e->pal = texpal;
        e->size = size;
        e->texbank = (int8_t)texbank;
        e->palbank = (int8_t)palbank;
        e->texgen = texgen;
        e->palgen = palgen;
        e->used = 1;
        sStats.words += size;
    }
    if (converted != NULL) {
        *converted = 1;
    }
    return victim;
}

const uint32_t *tex3d_cache_texels(int entry)
{
    if (sPool == NULL || entry < 0 || entry >= sCount || !sEntry[entry].used) {
        return NULL;
    }
    return sPool + sEntry[entry].off;
}

unsigned tex3d_cache_size(int entry)
{
    if (entry < 0 || entry >= sCount || !sEntry[entry].used) {
        return 0;
    }
    return sEntry[entry].size;
}

void tex3d_cache_stats(struct tex3d_cache_stats *out)
{
    if (out != NULL) {
        *out = sStats;
    }
}

int tex3d_cache_entries(void)
{
    return sCount;
}

void tex3d_report(FILE *f)
{
    fprintf(f, "tex3d-entries %d\n", sCount);
    fprintf(f, "tex3d-pool-words %lu of %lu\n",
            (unsigned long)sPoolUsed, (unsigned long)sPoolWords);
    fprintf(f, "tex3d-hits %lu\n", sStats.hits);
    fprintf(f, "tex3d-misses %lu\n", sStats.misses);
    /* An image that was where it was and had changed underneath: the number
     * that says whether the per-slot generations are doing anything. */
    fprintf(f, "tex3d-stale %lu\n", sStats.stale);
    fprintf(f, "tex3d-evictions %lu\n", sStats.evictions);
    /* ...and the two refusals, which are frames the PICA did not draw. */
    fprintf(f, "tex3d-dirty reports %lu bumps %lu\n",
            sStats.reports, sStats.bumps);
    fprintf(f, "tex3d-refused %lu\n", sStats.refused);
    fprintf(f, "tex3d-full %lu\n", sStats.full);
    fprintf(f, "tex3d-texels %lu\n", sStats.words);
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#define CHECK(cond)                                                          \
    do {                                                                     \
        ran++;                                                               \
        if (!(cond)) {                                                       \
            fprintf(stderr, "  3ds_tex3d.c:%d failed\n", __LINE__);           \
            bad++;                                                           \
        }                                                                    \
    } while (0)

int tex3d_selftest(int *ranOut)
{
    int ran = 0, bad = 0;
    uint32_t p;

    /*
     * The expansion, against the two whites this port now has. A texture's
     * palette entry reaches 0x1F per channel, which is 0x3F in six bits and
     * 0xFF in eight; a BACKGROUND's white is 0xFB, because the tile cache carries bit 15
     * as green's sixth bit and a five-bit 0x1F only reaches 0x3E there. The
     * two files disagreeing about white is correct and is the finding this
     * check exists to hold still.
     */
    CHECK(expand555(0x7FFFu, 31u) == 0xFFFFFFFFu);
    CHECK(expand555(0x0000u, 31u) == 0x000000FFu);
    CHECK(expand555(0x0000u, 0u) == 0x00000000u);
    CHECK(expand555(0x001Fu, 31u) == 0xFF0000FFu);   /* red is the low bits */
    CHECK(expand555(0x03E0u, 31u) == 0x00FF00FFu);
    CHECK(expand555(0x7C00u, 31u) == 0x0000FFFFu);
    /* Bit 15 is not green's sixth bit here, unlike a 2D palette entry. */
    CHECK(expand555(0x8000u, 31u) == expand555(0x0000u, 31u));
    /* One step of five bits is two steps of six, and its eight-bit form is
     * the same replication tile_colour() uses. */
    CHECK(expand555(0x0001u, 31u) == 0x0C0000FFu);
    /* Alpha reaches both ends and nothing in between is off by more than a
     * count from a/31 of full scale. */
    CHECK((expand555(0x7FFFu, 0u) & 0xFFu) == 0x00u);
    CHECK((expand555(0x7FFFu, 16u) & 0xFFu) == 0x84u);

    /* Shape, and the fields it comes out of. */
    p = (3u << 20) | (0u << 23);
    CHECK(tex3d_width(p) == 64u && tex3d_height(p) == 8u);
    p = (7u << 20) | (7u << 23);
    CHECK(tex3d_width(p) == 1024u && tex3d_height(p) == 1024u);
    CHECK(tex3d_texels(p) == 1024u * 1024u);
    CHECK(tex3d_convert(NULL, p, 0) == -1);

    /* Wrapping is two independent pairs of bits, and the flip bit only means
     * anything with the repeat bit set. */
    CHECK(tex3d_wrap_s(0) == TEX3D_WRAP_CLAMP);
    CHECK(tex3d_wrap_t(0) == TEX3D_WRAP_CLAMP);
    CHECK(tex3d_wrap_s(1u << 16) == TEX3D_WRAP_REPEAT);
    CHECK(tex3d_wrap_s((1u << 16) | (1u << 18)) == TEX3D_WRAP_MIRROR);
    CHECK(tex3d_wrap_s(1u << 18) == TEX3D_WRAP_CLAMP);
    CHECK(tex3d_wrap_t(1u << 17) == TEX3D_WRAP_REPEAT);
    CHECK(tex3d_wrap_t((1u << 17) | (1u << 19)) == TEX3D_WRAP_MIRROR);
    CHECK(tex3d_wrap_t(1u << 19) == TEX3D_WRAP_CLAMP);

    /* The cache key drops the sampler's bits and keeps the image's. */
    CHECK((TEX3D_KEY_MASK & (0xFu << 16)) == 0);
    CHECK((TEX3D_KEY_MASK & (3u << 30)) == 0);
    CHECK((TEX3D_KEY_MASK & 0xFFFFu) == 0xFFFFu);
    CHECK((TEX3D_KEY_MASK & (0x3FFu << 20)) == (0x3FFu << 20));

    /* Which slot a texture and a palette live in, which is what a dirty
     * report is matched against. A 4-colour palette is addressed `<< 3` and
     * every other format `<< 4`, so the same TexPalette is a different slot
     * in the two, the one arithmetic in this file with no second reader. */
    CHECK(tex_slot_of(0x0000u) == 0);
    CHECK(tex_slot_of(0x4000u) == 1);            /* 0x4000 << 3 = 0x20000 */
    CHECK(tex_slot_of(0xC000u) == 3);
    CHECK(pal_slot_of(3u << 26, 0x0800u) == 2);  /* 0x800 << 4 = 0x8000  */
    CHECK(pal_slot_of(2u << 26, 0x0800u) == 1);  /* ...and << 3 = 0x4000 */

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return bad;
}
