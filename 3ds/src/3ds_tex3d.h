/*
 * 3ds/src/3ds_tex3d.h: the 3D engine's textures, in the form the PICA can
 * sample.
 *
 * This is the tile cache again for the other engine, for the same reason: the
 * PICA has no paletted texture format and no programmable fragment shader to
 * fake one with, so a DS texture is expanded to real colours on the CPU, once,
 * and cached. What is different is the source: eight formats instead of two,
 * sizes from 8x8 to 1024x1024, two address spaces of their own, and an alpha
 * channel that is part of the format rather than a property of index zero.
 *
 * It is split out from the rasterizer on purpose. The converter is pure
 * arithmetic over guest memory with an exact oracle, pc_gpu3d_soft.c's own
 * texture_lookup(), so every texel of every format can be checked on a build
 * machine, which is where a colour argument gets settled. The rest is a render
 * target and a vertex program, and neither can be checked anywhere but on a
 * PICA.
 *
 * The colours are not tile_texel()'s, and that is the trap in this file. The
 * tile cache carries a 2D palette entry's bit 15 as green's sixth bit, because
 * the 2D engine composites in six bits per channel. A texture palette entry
 * has no such bit: texture_lookup() returns plain RGB555 and render_pixel()
 * expands five bits to six. So a texture's white is 0xFF and a background's is
 * 0xFB, out of the same halfword value. Calling tile_texel() here would be
 * wrong on half of all colours and wrong by four counts, which is exactly the
 * size of mistake a screenshot cannot show.
 *
 * What is stored is the unmodulated texel. The DS multiplies the texel by the
 * vertex colour in six-bit space and with a white vertex that is the identity,
 * so the eight-bit expansion of the texel is exactly what the hardware would
 * put on screen for an unlit white polygon. Doing the multiply on the PICA in
 * eight bits is a different rounding, and that belongs with the shading.
 *
 * Wrapping is sampler state and not part of the image: the DS's clamp, repeat
 * and repeat-with-flip are GPU_CLAMP_TO_EDGE, GPU_REPEAT and
 * GPU_MIRRORED_REPEAT, and every DS texture side is a power of two, so the
 * PICA's sampler reproduces all three exactly. That decides the cache key.
 */

#ifndef POKEPLATINUM_3DS_TEX3D_H
#define POKEPLATINUM_3DS_TEX3D_H

#include <stdint.h>
#include <stdio.h>

/*
 * The largest image this converter will produce, in texels. A DS texParam can
 * name 1024x1024, which is four megabytes of RGBA8, more than the whole
 * working set this game ever asks for, on one polygon.
 *
 * 256 KB is the ceiling because the survey measured the real shape of the
 * demand: over the whole new-game replay the largest single image was 32,768
 * texels (`gx-texture-max-texels`), and one frame's whole working set peaked
 * at 28 distinct images and 815,104 bytes expanded. So this is twice the
 * largest thing the game has been seen to ask for, and about a quarter of what
 * serves a frame. An image past it is refused rather than expanded, and a
 * refusal is a frame the PICA does not draw, countable, and a finding if it
 * ever happens, rather than a pool that will not fit.
 */
#define TEX3D_MAX_TEXELS 65536u

/* The image the cache key names, which is not all of TexParam: the wrap and
 * flip bits (16-19) and the coordinate-transform mode (30-31) change how an
 * image is sampled and not what is in it. */
#define TEX3D_KEY_MASK 0x3FF0FFFFu

unsigned tex3d_width(uint32_t texparam);
unsigned tex3d_height(uint32_t texparam);
unsigned tex3d_texels(uint32_t texparam);

/*
 * One texel, as the PICA holds it: 0xRRGGBBAA, the software renderer's own
 * colour expanded five bits to six to eight, and its 0-31 alpha expanded to
 * 0-255. `si` and `ti` are inside the image; wrapping is the sampler's.
 */
uint32_t tex3d_texel(uint32_t texparam, uint32_t texpal, int si, int ti);

/*
 * The whole image into `out`, which must hold tex3d_texels() words, in PICA
 * swizzle order, 8x8 blocks in raster order, Morton inside a block, the
 * order the tile cache already writes tiles in and a texture upload is therefore a
 * memcpy. Row 0 of the output is row 0 of the DS texture; the PICA's v axis
 * runs the other way, and that flip lives in the texture coordinates the way
 * it already does for the 3D layer's own quad.
 *
 * Returns 0, or -1 for an image past TEX3D_MAX_TEXELS.
 */
int tex3d_convert(uint32_t *out, uint32_t texparam, uint32_t texpal);

/* The sampler state the image does not carry. */
enum {
    TEX3D_WRAP_CLAMP = 0,
    TEX3D_WRAP_REPEAT,
    TEX3D_WRAP_MIRROR
};

int tex3d_wrap_s(uint32_t texparam);
int tex3d_wrap_t(uint32_t texparam);

/* ------------------------------------------------------------------ */
/* The cache                                                           */
/* ------------------------------------------------------------------ */
/*
 * What invalidates an entry, and why it is not the tile cache's ANSWER. The tile cache
 * keys its generations on a guest address, because a tile is fetched from one.
 * A texture is not: texture_lookup() works in a 512 KB texture space and a
 * 128 KB palette space that the VRAM banks are mapped *into*, so the same
 * texture address means different memory after a remap and a guest address has
 * no fixed place in it.
 *
 * The generations are per bank (nine counters) and an entry remembers
 * which bank its texture slot and its palette slot were pointing into when it
 * was converted. A lookup re-derives both and compares bank and generation, so
 * an image is stale either because its memory changed or because its slot now
 * names different memory. That means a remap needs no reset of its own, and a
 * background upload into one bank does not throw away textures in another.
 *
 * Per slot would have been the obvious choice and it is wrong. The SDK loads a
 * texture by putting the bank in LCDC, writing it, and giving it back to the
 * texture engine afterwards, so while the write happens the bank is not a
 * texture slot at all, and a slot-keyed report has nothing to bump. Every
 * cached image would survive its own replacement. The test in
 * 3ds/tests/tex3d_convert.c performs that sequence, which is how this was
 * found rather than shipped.
 */

struct tex3d_cache_stats {
    unsigned long hits;
    unsigned long misses;
    unsigned long evictions;
    unsigned long stale;        /* found, but its slot had moved under it  */
    unsigned long refused;      /* an image past TEX3D_MAX_TEXELS          */
    unsigned long full;         /* no room in the pool this frame          */
    unsigned long words;        /* texels converted, over the whole run    */
    /*
     * ...and whether anything is telling this cache about writes at all.
     * `reports` is every bulk guest write the port saw, `bumps` the ones that
     * landed in a VRAM bank. A run with bumps but no `stale` means the game
     * never rewrote an image this cache was holding; a run with no bumps means
     * the cache CANNOT go stale and every number above it is worthless, and
     * those two look identical from `stale` alone, which is why both are here.
     */
    unsigned long reports;
    unsigned long bumps;
};

/*
 * `pool` is where converted images go and `words` is how many texels it holds.
 * Unlike the tile cache's, entries here are of different sizes, so the pool is a bump
 * allocator reset when it fills rather than a table of equal slots, an image
 * is contiguous and a slot number is an offset into it.
 *
 * Returns 0, or -1 if the pool cannot hold one maximum-sized image.
 */
int tex3d_cache_init(uint32_t *pool, size_t words);

/*
 * The entry holding this image, converted on a miss. `frame` is any counter
 * that increases once a frame. Returns the entry, or -1 when the image is
 * refused or the pool is full for this frame; both of which the caller
 * treats as "this frame is not the PICA's" rather than as a wrong picture.
 *
 * `converted` is set to 1 when this call wrote the pool and 0 when it found
 * the image already there, and may be NULL. It is what tells the caller which
 * ranges have to leave the data cache, the same way the tile cache's `expanded` does.
 */
int tex3d_cache_get(uint32_t texparam, uint32_t texpal, unsigned frame,
                    int *converted);

/* Where an entry's texels are, and how many. */
const uint32_t *tex3d_cache_texels(int entry);
unsigned tex3d_cache_size(int entry);

/* A guest write, in guest addresses. Anything outside VRAM is ignored. */
void tex3d_cache_dirty(uint32_t addr, uint32_t bytes);

/* Everything, unconditionally: a bank remap moves what a slot means. */
void tex3d_cache_reset(void);

void tex3d_cache_stats(struct tex3d_cache_stats *out);
int tex3d_cache_entries(void);

void tex3d_report(FILE *f);

/* Self-test, the same shape every model in this port carries. */
int tex3d_selftest(int *ranOut);

#endif /* POKEPLATINUM_3DS_TEX3D_H */
