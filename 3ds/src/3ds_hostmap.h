/*
 * 3ds/src/3ds_hostmap.h: the translator, for a caller that asks per pixel.
 *
 * `armrec_host_ptr()` is the honest translation and it is not cheap: a scan
 * over the eleven guest rows, and for VRAM a second scan over the five windows
 * with a modulo in it, which on ARM11 is a call into libgcc because this CPU
 * has no divide instruction. Most of the port asks it once and keeps the
 * answer. A software rasterizer cannot; it reads a tile map, then a tile
 * row, then the next tile map, tens of thousands of times a frame.
 *
 * This is the same answer with the same rules, memoised by 16 KB block. See
 * 3ds_hostmap.c for why 16 KB is the right grain, when a block is refused, and
 * what an unmapped VRAM address gets instead of NULL.
 */

#ifndef POKEPLATINUM_3DS_HOSTMAP_H
#define POKEPLATINUM_3DS_HOSTMAP_H

#include <stdint.h>

/*
 * Host pointer for a guest address. Never NULL: an address with no memory
 * behind it answers into a shared read-only zero block, which is what the PC
 * port's window floor gives its renderer and what hardware gives a read from
 * an unmapped VRAM window. Do not write through a pointer you did not first
 * know was mapped.
 */
void *hostmap_ptr(uint32_t guest);

/*
 * Forget everything. 3ds_vram.c calls this whenever it rebuilds the bank map,
 * through a weak reference, so nothing has to remember to; it is here as well
 * because a test that rebinds the slab needs it.
 */
void hostmap_flush(void);

/* Lookups, misses, and blocks the cache refused because they are not one
 * contiguous mapping. Zeroed by hostmap_flush(). Diagnostics for the cache, which
 * is where a frame time says whether any of this was worth it. */
unsigned long hostmap_hits(void);
unsigned long hostmap_misses(void);
unsigned long hostmap_uncacheable(void);

/* Host self-test: agreement with armrec_host_ptr() over every region, the
 * zero block, the refusal, and staleness after a remap. Returns failures,
 * fills *ran. */
int hostmap_selftest(int *ran);

#endif /* POKEPLATINUM_3DS_HOSTMAP_H */
