/*
 * 3ds/src/armrec_mem_3ds.h: the one thing armrec_mem_3ds.c adds to armrec.
 *
 * The functions that file exists to provide, armrec_mem_init(),
 * armrec_region_at() and the rest, are declared in tools/armrec/armrec_rt.h,
 * because the whole point is that a caller cannot tell which host it is on.
 * The self-test is this port's own, so it is declared here.
 */

#ifndef POKEPLATINUM_3DS_ARMREC_MEM_H
#define POKEPLATINUM_3DS_ARMREC_MEM_H

/*
 * Check the region table against the slab: every row translates, round-trips
 * and answers armrec_is_guest_addr(); the five VRAM windows report content
 * rather than span; the port window is findable by name; and the holes, the
 * AGB slot and the ends of the map answer the way they answer on PC.
 *
 * Requires a successful armrec_mem_init(). Writes nothing to guest memory, so
 * it may run before anything else and needs no cleanup. Returns the number of
 * checks that failed, 0 for a pass, and fills `*ran` if `ran` is non-NULL.
 */
int armrec_mem_selftest(int *ran);

#endif /* POKEPLATINUM_3DS_ARMREC_MEM_H */
