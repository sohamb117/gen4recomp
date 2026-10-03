/*
 * 3ds/src/3ds_tile.c: see 3ds_tile.h.
 */

#include "3ds_tile.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Colour                                                              */
/* ------------------------------------------------------------------ */

/*
 * pc_gpu2d.c's rgb15to18() and expand(), in one step and with the same two
 * quirks: green picks up the halfword's top bit as its low bit, and six bits
 * become eight by replicating the top two rather than by scaling. White is
 * 0xFBFBFB here, not 0xFFFFFF, and 3ds/tests/tile_expand.c checks that against
 * the renderer itself rather than against this comment.
 */
uint32_t tile_colour(uint16_t c15)
{
    uint32_t r = (uint32_t)(c15 & 0x001Fu) << 1;
    uint32_t g = ((uint32_t)(c15 & 0x03E0u) >> 4)
                 | ((uint32_t)(c15 & 0x8000u) >> 15);
    uint32_t b = (uint32_t)(c15 & 0x7C00u) >> 9;

    r = (r << 2) | (r >> 4);
    g = (g << 2) | (g >> 4);
    b = (b << 2) | (b >> 4);
    return (r << 16) | (g << 8) | b;
}

uint32_t tile_texel(uint16_t c15)
{
    return (tile_colour(c15) << 8) | 0xFFu;
}

void tile_palette(uint32_t *out, const uint16_t *pal15, int entries)
{
    int i;

    /* Index 0 is the DS's transparent index in every tiled mode. Zero rather
     * than "the colour with alpha 0", so a slot that leaks into a picture is
     * black-and-invisible rather than a plausible colour. */
    out[0] = 0;
    for (i = 1; i < entries; i++) {
        out[i] = tile_texel(pal15[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Swizzle                                                             */
/* ------------------------------------------------------------------ */

/*
 * Morton order inside the block: x and y bits interleaved, x first. Built once
 * into a table because the expansion walks it 64 times a tile and the shifts
 * are worth more as one load.
 */
static uint8_t sSwizzle[TILE_TEXELS];
static int sSwizzleReady;

static void swizzle_init(void)
{
    int x, y;

    for (y = 0; y < TILE_SIDE; y++) {
        for (x = 0; x < TILE_SIDE; x++) {
            sSwizzle[y * TILE_SIDE + x] =
                (uint8_t)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1)
                          | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
        }
    }
    sSwizzleReady = 1;
}

int tile_swizzle(int x, int y)
{
    if (!sSwizzleReady) {
        swizzle_init();
    }
    return sSwizzle[y * TILE_SIDE + x];
}

/* ------------------------------------------------------------------ */
/* Expansion                                                           */
/* ------------------------------------------------------------------ */

void tile_expand4(uint32_t *out, const uint8_t *src, const uint32_t *pal)
{
    int y;

    if (!sSwizzleReady) {
        swizzle_init();
    }

    for (y = 0; y < TILE_SIDE; y++) {
        const uint8_t *row = src + y * (TILE_SIDE / 2);
        const uint8_t *sw = sSwizzle + y * TILE_SIDE;
        int i;

        /* One byte is two pixels, low nibble first, the same order
         * pc_gpu2d.c reads them in, where an even x takes `b & 15`. */
        for (i = 0; i < TILE_SIDE / 2; i++) {
            uint8_t b = row[i];

            out[sw[i * 2]] = pal[b & 15u];
            out[sw[i * 2 + 1]] = pal[b >> 4];
        }
    }
}

void tile_expand8(uint32_t *out, const uint8_t *src, const uint32_t *pal)
{
    int y;

    if (!sSwizzleReady) {
        swizzle_init();
    }

    for (y = 0; y < TILE_SIDE; y++) {
        const uint8_t *row = src + y * TILE_SIDE;
        const uint8_t *sw = sSwizzle + y * TILE_SIDE;
        int x;

        for (x = 0; x < TILE_SIDE; x++) {
            out[sw[x]] = pal[row[x]];
        }
    }
}

/* ------------------------------------------------------------------ */
/* The cache                                                           */
/* ------------------------------------------------------------------ */

#define TILE_SLOTS_MAX 8192

struct slot {
    uint32_t tileaddr;
    uint32_t palkey;            /* the palette's contents, not its address */
    uint16_t tilegen;           /* the generation the tile's block had     */
    uint8_t bpp;
    uint8_t used;
    unsigned frame;             /* last frame it was asked for */
};

static struct slot sSlot[TILE_SLOTS_MAX];
static uint32_t *sPool;
static int sSlots;
static uint16_t sGen[TILE_DIRTY_BLOCKS];
static struct tile_cache_stats sStats;

static uint16_t gen_of(uint32_t addr)
{
    uint32_t off = addr - TILE_DIRTY_BASE;

    if (addr < TILE_DIRTY_BASE || off >= TILE_DIRTY_SPAN) {
        return 0;
    }
    return sGen[off >> TILE_DIRTY_SHIFT];
}

int tile_cache_init(uint32_t *pool, int slots)
{
    if (pool == NULL || slots < 1) {
        return -1;
    }
    if (slots > TILE_SLOTS_MAX) {
        slots = TILE_SLOTS_MAX;
    }
    sPool = pool;
    sSlots = slots;
    tile_cache_reset();
    return 0;
}

void tile_cache_reset(void)
{
    memset(sSlot, 0, sizeof sSlot);
    memset(&sStats, 0, sizeof sStats);
    /* The generations are not cleared: they are a monotone counter per block
     * and clearing them could make a stale slot look current. Every slot is
     * empty now anyway. */
}

void tile_cache_dirty(uint32_t addr, uint32_t bytes)
{
    uint32_t first, last, i;

    /* Nothing cached yet means nothing to invalidate, and this is called from
     * every bulk copy the game makes, so it is the first test, not a
     * consequence of the range walk below. */
    if (sPool == NULL || bytes == 0) {
        return;
    }
    if (addr < TILE_DIRTY_BASE) {
        if (addr + bytes <= TILE_DIRTY_BASE) {
            return;
        }
        bytes -= TILE_DIRTY_BASE - addr;
        addr = TILE_DIRTY_BASE;
    }
    if (addr - TILE_DIRTY_BASE >= TILE_DIRTY_SPAN) {
        return;
    }
    if (addr - TILE_DIRTY_BASE + bytes > TILE_DIRTY_SPAN) {
        bytes = TILE_DIRTY_SPAN - (addr - TILE_DIRTY_BASE);
    }

    first = (addr - TILE_DIRTY_BASE) >> TILE_DIRTY_SHIFT;
    last = (addr - TILE_DIRTY_BASE + bytes - 1) >> TILE_DIRTY_SHIFT;
    for (i = first; i <= last; i++) {
        sGen[i]++;
    }
}

/*
 * Open addressing, because the alternative is a chain of pointers into a pool
 * that the background path wants to be a plain array. The probe is linear from a hash of the
 * two addresses and it stops at the first empty slot, which is what makes a
 * miss cheap; a full table is answered by evicting the oldest of the eight
 * slots the probe walked rather than by walking the whole table.
 */
#define TILE_PROBE 8

static unsigned hash_key(uint32_t tileaddr, uint32_t palkey, int bpp)
{
    unsigned h = (unsigned)(tileaddr >> 5) * 2654435761u;

    h ^= (unsigned)palkey * 2246822519u;
    h ^= (unsigned)bpp * 668265263u;
    /*
     * ...and the high bits folded down, which is not tidiness. The slot count
     * is a power of two, so the index is the low bits of this, and a
     * multiplicative hash only mixes bits upward, so the low bits of the
     * result are a function of the low bits of the inputs alone. Sixteen
     * palettes of one tile whose keys agree low down therefore land in one
     * bucket, the eight-slot probe fills with them, and the ninth is refused:
     * A whole engine-frame dropped because a palette was too similar to
     * another one.
     *
     * The effect pass is what made that likely rather than possible. A palette with a
     * fade folded into it is a different key on every frame of the fade, so a
     * scene that used to ask for one key per tile now asks for a new one
     * sixteen times a second, and the check that found this asked for
     * sixteen sub-palettes of the same tile in one frame and lost the frame.
     */
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return h;
}

int tile_cache_get(uint32_t tileaddr, uint32_t palkey, int bpp,
                   const uint8_t *src, const uint32_t *pal, unsigned frame,
                   int *expanded)
{
    unsigned h;
    int i;
    int victim = -1;
    unsigned victimFrame = 0;

    if (expanded != NULL) {
        *expanded = 0;
    }
    if (sPool == NULL) {
        return -1;
    }

    h = hash_key(tileaddr, palkey, bpp) % (unsigned)sSlots;
    for (i = 0; i < TILE_PROBE; i++) {
        int idx = (int)((h + (unsigned)i) % (unsigned)sSlots);
        struct slot *s = &sSlot[idx];

        if (!s->used) {
            victim = idx;
            break;
        }
        if (s->tileaddr == tileaddr && s->palkey == palkey
            && s->bpp == (uint8_t)bpp) {
            if (s->tilegen == gen_of(tileaddr)) {
                s->frame = frame;
                sStats.hits++;
                return idx;
            }
            /* The key still matches but the memory under it has been written,
             * so this is the same tile with different pixels. */
            sStats.stale++;
            victim = idx;
            break;
        }
        /* Never evict something already drawn this frame: that is a loop
         * between two tiles that both get expanded every time. */
        if (s->frame != frame && (victim < 0 || s->frame < victimFrame)) {
            victim = idx;
            victimFrame = s->frame;
        }
    }

    if (victim < 0) {
        return -1;
    }
    if (sSlot[victim].used && !(sSlot[victim].tileaddr == tileaddr
                                && sSlot[victim].palkey == palkey
                                && sSlot[victim].bpp == (uint8_t)bpp)) {
        sStats.evictions++;
    }

    if (bpp == 4) {
        tile_expand4(sPool + (size_t)victim * TILE_TEXELS, src, pal);
    } else {
        tile_expand8(sPool + (size_t)victim * TILE_TEXELS, src, pal);
    }

    sSlot[victim].tileaddr = tileaddr;
    sSlot[victim].palkey = palkey;
    sSlot[victim].bpp = (uint8_t)bpp;
    sSlot[victim].tilegen = gen_of(tileaddr);
    sSlot[victim].used = 1;
    sSlot[victim].frame = frame;
    sStats.misses++;
    if (expanded != NULL) {
        *expanded = 1;
    }
    return victim;
}

const uint32_t *tile_cache_slot(int slot)
{
    if (sPool == NULL || slot < 0 || slot >= sSlots) {
        return NULL;
    }
    return sPool + (size_t)slot * TILE_TEXELS;
}

int tile_cache_slots(void)
{
    return sPool != NULL ? sSlots : 0;
}

void tile_cache_stats(struct tile_cache_stats *out)
{
    *out = sStats;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */
/*
 * The console runs this beside every other model. What it cannot check
 * here is the one thing that matters most; that these colours are the
 * software renderer's colours, because pc_gpu2d.c is not linked into the
 * self-test path. 3ds/tests/tile_expand.c is where that is asked, against the
 * renderer itself.
 */

#ifdef TILE_SELFTEST_VERBOSE
#include <stdio.h>
#define FAILNOTE() fprintf(stderr, "  3ds_tile.c:%d failed\n", __LINE__)
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

int tile_selftest(int *ranOut)
{
    int ran = 0;
    int failed = 0;
    static uint32_t pool[4 * TILE_TEXELS];
    static uint32_t out[TILE_TEXELS];
    static uint32_t pal[TILE_PAL8];
    static uint16_t pal15[TILE_PAL8];
    static uint8_t tile[TILE_BYTES8];
    static uint8_t seen[TILE_TEXELS];
    int i, x, y, slot, other;
    uint32_t *savedPool;
    int savedSlots;
    struct tile_cache_stats st;

    /* --- colour ------------------------------------------------------ */

    /* Six bits per channel out to eight by replicating the top two: the
     * renderer's white, which is not 0xFFFFFF. */
    CHECK(tile_colour(0x7FFFu) == 0x00FBFBFBu);
    CHECK(tile_colour(0x0000u) == 0x00000000u);
    /* Bit 15 is green's low bit and nothing else: setting it moves green one
     * six-bit step and leaves red and blue alone. */
    CHECK(tile_colour(0x8000u) == 0x00000400u);
    CHECK(tile_colour(0x001Fu) == 0x00FB0000u);
    CHECK(tile_colour(0x7C00u) == 0x000000FBu);
    CHECK(tile_texel(0x7FFFu) == 0xFBFBFBFFu);

    /* --- swizzle ----------------------------------------------------- */

    memset(seen, 0, sizeof seen);
    for (y = 0; y < TILE_SIDE; y++) {
        for (x = 0; x < TILE_SIDE; x++) {
            int s = tile_swizzle(x, y);

            if (s >= 0 && s < TILE_TEXELS) {
                seen[s]++;
            }
        }
    }
    for (i = 0; i < TILE_TEXELS; i++) {
        CHECK(seen[i] == 1);        /* a permutation, not a mapping */
    }
    CHECK(tile_swizzle(0, 0) == 0);
    CHECK(tile_swizzle(1, 0) == 1);
    CHECK(tile_swizzle(0, 1) == 2);
    CHECK(tile_swizzle(1, 1) == 3);
    CHECK(tile_swizzle(2, 0) == 4);
    CHECK(tile_swizzle(7, 7) == 63);

    /* --- expansion --------------------------------------------------- */

    for (i = 0; i < TILE_PAL8; i++) {
        pal15[i] = (uint16_t)(i * 3 + 1);
    }
    tile_palette(pal, pal15, TILE_PAL8);
    CHECK(pal[0] == 0);
    CHECK(pal[5] == tile_texel(pal15[5]));

    /* 8bpp: pixel (x, y) is index x + y, so every texel is checkable. */
    for (y = 0; y < TILE_SIDE; y++) {
        for (x = 0; x < TILE_SIDE; x++) {
            tile[y * TILE_SIDE + x] = (uint8_t)(x + y);
        }
    }
    tile_expand8(out, tile, pal);
    for (y = 0; y < TILE_SIDE; y++) {
        for (x = 0; x < TILE_SIDE; x++) {
            CHECK(out[tile_swizzle(x, y)] == pal[x + y]);
        }
    }
    /* Index 0 stays transparent wherever it lands. */
    CHECK(out[tile_swizzle(0, 0)] == 0);

    /* 4bpp: the low nibble is the even pixel, which is the order the software
     * renderer reads a byte in. */
    for (i = 0; i < TILE_BYTES4; i++) {
        tile[i] = (uint8_t)(((i + 1) & 15) | (((i + 2) & 15) << 4));
    }
    tile_palette(pal, pal15, TILE_PAL4);
    tile_expand4(out, tile, pal);
    for (y = 0; y < TILE_SIDE; y++) {
        for (x = 0; x < TILE_SIDE; x++) {
            uint8_t b = tile[y * (TILE_SIDE / 2) + x / 2];
            unsigned idx = (x & 1) ? (unsigned)(b >> 4) : (unsigned)(b & 15u);

            CHECK(out[tile_swizzle(x, y)] == pal[idx]);
        }
    }

    /* --- the cache --------------------------------------------------- */
    /*
     * The pool this test installs is a local one, and by the time it runs the
     * real atlas has already been handed over: gpu_init() is called
     * from the crt long before the self-tests. So whatever is there is put
     * back at the end of this section rather than left as this test found it
     * convenient, a four-slot cache in a stack buffer looks exactly like a
     * pool too small for the scene, and the renderer would answer "not
     * drawable" on every frame of the game.
     */
    savedPool = sPool;
    savedSlots = sSlots;

    CHECK(tile_cache_init(pool, 4) == 0);
    slot = tile_cache_get(0x06000000u, 0x05000000u, 8, tile, pal, 1u, NULL);
    CHECK(slot >= 0);
    CHECK(tile_cache_slot(slot) != NULL);
    CHECK(tile_cache_get(0x06000000u, 0x05000000u, 8, tile, pal, 1u, NULL) == slot);
    tile_cache_stats(&st);
    CHECK(st.misses == 1 && st.hits == 1);

    /* A write to the tile's own memory is a different tile at the same
     * address, and a write somewhere else is not. */
    tile_cache_dirty(0x06000000u, 64u);
    CHECK(tile_cache_get(0x06000000u, 0x05000000u, 8, tile, pal, 2u, NULL) >= 0);
    tile_cache_stats(&st);
    CHECK(st.stale == 1 && st.misses == 2);
    tile_cache_dirty(0x02000000u, 0x10000u);    /* main RAM: not tracked */
    CHECK(tile_cache_get(0x06000000u, 0x05000000u, 8, tile, pal, 3u, NULL) >= 0);
    tile_cache_stats(&st);
    CHECK(st.hits == 2);

    /*
     * The palette is the other half of the key, and it is the palette's
     * CONTENTS: a caller whose colours changed passes a different key and gets
     * a different slot, with no dirty report anywhere; which is the whole
     * point, because this game fades by storing halfwords into palette RAM and
     * nothing reports those.
     */
    other = tile_cache_get(0x06000000u, 0x1234u, 8, tile, pal, 3u, NULL);
    CHECK(other >= 0 && other != slot);
    CHECK(tile_cache_get(0x06000000u, 0x05000000u, 8, tile, pal, 3u, NULL) == slot);

    /* A reset empties it; a bank remap is why that has to exist. */
    tile_cache_reset();
    tile_cache_stats(&st);
    CHECK(st.hits == 0 && st.misses == 0);
    CHECK(tile_cache_get(0x06000000u, 0x05000000u, 8, tile, pal, 4u, NULL) >= 0);
    tile_cache_stats(&st);
    CHECK(st.misses == 1);

    /* Asking for more distinct tiles than the pool holds is answered with -1
     * rather than by throwing away a tile this frame has already drawn. */
    tile_cache_init(pool, 4);
    for (i = 0; i < 4; i++) {
        CHECK(tile_cache_get(0x06010000u + (uint32_t)i * 64u, 0x05000000u, 8,
                             tile, pal, 9u, NULL) >= 0);
    }
    CHECK(tile_cache_get(0x06020000u, 0x05000000u, 8, tile, pal, 9u, NULL)
          == -1);
    /* ... and the next frame it fits again. */
    CHECK(tile_cache_get(0x06020000u, 0x05000000u, 8, tile, pal, 10u, NULL)
          >= 0);

    /* ...and the atlas back, with nothing of this test's left in it. */
    if (savedPool != NULL) {
        tile_cache_init(savedPool, savedSlots);
    } else {
        sPool = NULL;
        sSlots = 0;
        tile_cache_reset();
    }
    memset(&sStats, 0, sizeof sStats);

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
