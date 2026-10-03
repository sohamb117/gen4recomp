/*
 * 3ds/src/3ds_mi_host.h: what the MI/DMA wrappers expose.
 *
 * The wrappers themselves have no declarations to make: the linker introduces
 * them, and nothing in the port calls a `__wrap_` name on purpose. What is
 * here is the predicate the walk is built on, so a test can ask it directly,
 * and two counters so a frame can say how much of its memory traffic needed
 * translating and how much of it hardware would have dropped.
 */

#ifndef POKEPLATINUM_3DS_MI_HOST_H
#define POKEPLATINUM_3DS_MI_HOST_H

#include <stdint.h>

/*
 * Non-zero when `p` is a DS address and not a host pointer. `size` is the
 * length of the access, so a range that starts inside the memory map and ends
 * outside it stops the console here instead of writing half of itself. Pass 0
 * for a length that is not known yet.
 *
 * Stops the console for a cartridge-slot address, which nothing should ever
 * hand a memory primitive on this port; see the top of 3ds_mi_host.c.
 */
int mi_host_is_guest(const void *p, uint32_t size);

/* Calls that needed a walk, and runs inside them with no memory behind them
 * (an unmapped VRAM window: hardware drops the write). Both since the last
 * reset. */
unsigned long mi_host_translated(void);
unsigned long mi_host_dropped(void);
void mi_host_reset_counts(void);

/*
 * Fills and copies over every shape the walk has: a host destination, a flat
 * guest one, a guest one spanning a discontinuity, and one with nothing behind
 * it. Returns failures, fills *ran. Needs a bound slab.
 */
int mi_host_selftest(int *ran);

#endif /* POKEPLATINUM_3DS_MI_HOST_H */
