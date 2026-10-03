/*
 * 3ds/src/3ds_guest.h: guest address to host pointer, and back.
 *
 * The two functions this port turns on. On PC a guest address is a host
 * address and neither of these exists: armrec_mem_init() mmaps 0x02000000 at
 * 0x02000000 and a cast is the whole translation. Here the slab is wherever
 * the heap put it, so `slab + (guest - base) + offset` is the translation and
 * these are the only two functions that know it. Everything else, the
 * hardware models, the shadows, the SPU glue, keeps speaking guest
 * addresses, which is what makes the rest of the tree portable.
 *
 * The names are armrec's because the backend registers these rows with armrec and the
 * hooks in pc/hw will call them; whether armrec_rt.h grows identity inlines
 * for the PC build is the decision, not this file's. Nothing in `pc/`
 * compiles this file.
 *
 * No libctru here, deliberately. The arithmetic is pure C over a slab pointer
 * the caller binds, so 3ds/tests/guest_xlat.c links it on the host with a
 * malloc'd slab and checks every region, every hole and every boundary,
 * hundreds of cases, on a machine with no 3DS. The same code runs on the
 * console; only the pointer differs.
 *
 * VRAM has no answer yet. A guest address in 0x06000000-0x07000000 is not a
 * fixed offset into anything: VRAMCNT decides which of the nine banks is
 * reachable through which window, and the same 16 KB moves between them. That
 * model is 2.7. Until then a VRAM address translates to NULL, a loud
 * failure, rather than a plausible pointer into the bank store that would be
 * right only while every bank happened to be where the guess put it.
 */

#ifndef POKEPLATINUM_3DS_GUEST_H
#define POKEPLATINUM_3DS_GUEST_H

#include <stdint.h>

/*
 * Point the translator at the slab. mem_init() calls this; a test calls it
 * with its own block. Binding NULL unbinds, after which every translation is
 * NULL / 0.
 */
void guest_bind(void *slab_base);

/*
 * Host pointer for a guest address, or NULL if there is no memory there,
 * a hole in the DS map, VRAM (see above), the AGB slot above its 64 KB probe
 * buffer, or an address outside the map entirely. Never a wrapped or clamped
 * pointer: a wrong pointer is a corruption, a NULL is a crash with a cause.
 */
void *armrec_host_ptr(uint32_t guest);

/*
 * The guest address of a host pointer into the slab, or 0 if it is not in the
 * slab. 0 is not a valid guest address in this map; the lowest region is
 * ITCM at 0x01FF8000, so it doubles as the failure value.
 */
uint32_t armrec_guest_addr(const void *host);

/*
 * The host pointer to one GUEST_R_* row's backing, which is bookkeeping rather
 * than translation: it answers "where did region N land", not "what host
 * address is guest 0x04000130". 3ds_mem.c has the same function over its own
 * slab pointer; this one is over the bound one, so a file that must stay free
 * of libctru (3ds_vram.c, which needs the nine banks) can still find its
 * storage. NULL before guest_bind() or for an index out of range.
 */
void *guest_region_base(int index);

/*
 * Split a guest range where the host mapping stops being one pointer.
 *
 * armrec_host_ptr() answers for one address. A bulk fill or copy needs a
 * pointer AND a length, and in this map those two are not the same question:
 * A VRAM range is nine banks appearing in five windows at a 16 KB grain, so
 * `p + n` is not `armrec_host_ptr(guest + n)` past the first block boundary,
 * and a memcpy over the whole range walks off the end of a bank into whatever
 * the slab put next to it.
 *
 * So this is an iterator, not a translation. It returns the length of the
 * first run of [guest, guest + want) that is one thing, and sets *hostOut to
 * that run's host pointer, or to NULL when the run is a run of nothing.
 * Both cases are useful to a caller: a mapped run is copied, an unmapped one
 * is skipped the way hardware drops a write to a bank that is not there.
 *
 * The return is never 0 for want > 0, so `guest += n` always advances and a
 * walk over it always terminates.
 */
uint32_t guest_span(uint32_t guest, uint32_t want, void **hostOut);

/*
 * Which GUEST_R_* row an address falls in, by its ADDRESSES rather than by
 * its backing, VRAM's window is 16 MB over 0xA4000 of memory and every
 * address in it is still VRAM. -1 when the address is in no row at all, which
 * is a hole in the DS map and a fault on hardware.
 */
int guest_region_index(uint32_t guest);

/*
 * Write through the translator, read back through plain slab arithmetic, and
 * the other way round, over every region and every edge case. Returns the
 * number of checks that failed, 0 for a pass, and fills `*ran` with how many
 * it made if `ran` is non-NULL. Runs on the host and on the console: this is
 * the check the self-test binary reports, not a paper model.
 *
 * Requires a bound slab. Scribbles on the regions it tests, so call it before
 * guest memory means anything.
 */
int guest_selftest(int *ran);

#endif /* POKEPLATINUM_3DS_GUEST_H */
