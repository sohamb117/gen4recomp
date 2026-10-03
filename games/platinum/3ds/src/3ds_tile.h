/*
 * 3ds/src/3ds_tile.h: DS tiles, in the form the PICA can sample.
 *
 * A DS background tile is 8x8 pixels of palette indices, four or eight bits
 * each, and a PICA texture holds no palette at all: there is no paletted
 * format and no programmable fragment shader to fake one with. So a tile is
 * expanded to real colours on the CPU, once, and cached.
 *
 * One tile is exactly one PICA swizzle block. Textures on this hardware are
 * stored as 8x8 blocks in raster order with the pixels inside a block in
 * Morton order, so an expanded tile is 64 contiguous texels that can be
 * memcpy'd into an atlas without touching anything around it.
 *
 * RGBA8, not RGBA5551, for two measured reasons:
 *
 *   1. A DS palette entry's bit 15 is not alpha here. pc_gpu2d.c carries it as
 *      green's sixth bit, so half of all palette entries are one step greener
 *      than their five-bit value. A 5551 texture has nowhere to put that bit.
 *   2. The two hardwares expand differently. The DS composites in six bits per
 *      channel and expands to eight by replicating the top two bits, so its
 *      white is 0xFBFBFB; a PICA sampling RGBA5551 replicates from five bits
 *      and gets 0xFF.
 *
 * The cost of being right is two bytes a texel: 256 bytes a tile, and about
 * 1 MB for the 4,100-tile worst case a frame can ask for.
 *
 * Pure C over caller buffers. No libctru, no guest memory, no allocation,
 * which is what lets 3ds/tests/tile_expand.c check this against the real
 * software renderer on a build machine.
 */

#ifndef POKEPLATINUM_3DS_TILE_H
#define POKEPLATINUM_3DS_TILE_H

#include <stdint.h>

#define TILE_SIDE    8
#define TILE_TEXELS  (TILE_SIDE * TILE_SIDE)
#define TILE_BYTES4  32                 /* 4bpp: one nibble a pixel  */
#define TILE_BYTES8  64                 /* 8bpp: one byte a pixel    */
#define TILE_PAL4    16
#define TILE_PAL8    256

/*
 * A DS colour, in the form pc_video's surfaces already carry: 0x00RRGGBB,
 * expanded exactly the way pc_gpu2d.c's rgb15to18() and expand() expand it,
 * bit 15 included.
 */
uint32_t tile_colour(uint16_t c15);

/* ... and as a texel: 0xRRGGBBAA, opaque. Index 0 is transparent and never
 * goes through here. */
uint32_t tile_texel(uint16_t c15);

/*
 * A whole palette, converted once so a tile's expansion is 64 table lookups.
 * `entries` is TILE_PAL4 or TILE_PAL8; entry 0 comes out fully transparent,
 * which is what makes the alpha test enough to skip it.
 */
void tile_palette(uint32_t *out, const uint16_t *pal15, int entries);

/*
 * One tile into `out`, which is TILE_TEXELS words in PICA swizzle order.
 * `pal` is a table from tile_palette(). Neither function flips: a flipped tile
 * is the same texels with the quad's texture coordinates mirrored, which is
 * free, and making it a second cache entry would not be.
 */
void tile_expand4(uint32_t *out, const uint8_t *src, const uint32_t *pal);
void tile_expand8(uint32_t *out, const uint8_t *src, const uint32_t *pal);

/* Where (x, y) inside a tile lands in that order. Exposed for the test and
 * for the background path's atlas arithmetic. */
int tile_swizzle(int x, int y);

/* ------------------------------------------------------------------ */
/* The cache                                                           */
/* ------------------------------------------------------------------ */
/*
 * Keyed on the tile's guest address, its palette's guest address and its
 * depth. Even with no cache the worst case is about 4,100 tiles a frame, which
 * is under two milliseconds of expansion, so this is the optimisation and not
 * the design.
 *
 * Guest writes invalidate an entry, and this file cannot see them: the caller
 * reports them with tile_cache_dirty(), which 3ds_mi_host.c's bulk copy
 * wrappers already sit on. A generation counter per 16 KB block means a dirty
 * report costs one increment and a lookup two comparisons.
 *
 * The palette is keyed by its contents and not its address, which is measured
 * rather than tidy: this game fades by writing palette RAM a halfword at a
 * time, which no bulk copy reports. Tiles cached against a palette address
 * kept the shade the fade started from, and a whole-frame byte compare on the
 * console is what caught it.
 *
 * Blocks outside TILE_DIRTY_BASE to +TILE_DIRTY_SPAN are not tracked, because
 * no tile and no palette can live there. A dirty report for one is ignored
 * rather than treated as a flush; the game DMAs into main RAM constantly.
 */

#define TILE_DIRTY_BASE  0x05000000u
#define TILE_DIRTY_SPAN  0x02000000u    /* palettes, VRAM, OAM             */
#define TILE_DIRTY_SHIFT 14             /* 16 KB, the VRAM mapping's grain */
#define TILE_DIRTY_BLOCKS (TILE_DIRTY_SPAN >> TILE_DIRTY_SHIFT)

struct tile_cache_stats {
    unsigned long hits;
    unsigned long misses;
    unsigned long evictions;
    unsigned long stale;        /* found, but its memory had moved under it */
};

/*
 * `pool` is where expanded tiles go: `slots` * TILE_TEXELS words, and the background path
 * points it at the linear-memory copy of its atlas so an expansion lands where
 * the GPU already reads. Returns 0, or -1 if the pool is too small to hold
 * one tile.
 */
int tile_cache_init(uint32_t *pool, int slots);

/*
 * The slot holding this tile, expanded on a miss. `frame` is any counter that
 * increases once a frame; it is what eviction picks by. Returns -1 only if
 * every slot has already been used this frame, which means the pool is too
 * small for the scene rather than that anything is wrong.
 *
 * `expanded` is set to 1 when this call wrote the slot and 0 when it found it
 * already there, and it may be NULL. It exists because the pool is the linear
 * side of a texture: the caller has to push exactly the slots that changed out
 * of the data cache, and asking the statistics whether the miss count moved
 * would be a four-word copy per tile for the same answer.
 */
int tile_cache_get(uint32_t tileaddr, uint32_t palkey, int bpp,
                   const uint8_t *src, const uint32_t *pal, unsigned frame,
                   int *expanded);

/* The expanded texels of a slot tile_cache_get() returned. */
const uint32_t *tile_cache_slot(int slot);

/* A guest write. Bytes outside the tracked span are ignored. */
void tile_cache_dirty(uint32_t addr, uint32_t bytes);

/* Everything, unconditionally: a bank remap moves what an address means. */
void tile_cache_reset(void);

void tile_cache_stats(struct tile_cache_stats *out);

/* How many slots the cache is holding, and 0 when it has no pool at all;
 * which is the difference between "the scene did not fit" and "nobody ever
 * gave it an atlas", and the two look identical from a return of -1. */
int tile_cache_slots(void);

/* Self-test, the same shape every model in this port carries. */
int tile_selftest(int *ranOut);

#endif /* POKEPLATINUM_3DS_TILE_H */
