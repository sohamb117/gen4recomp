/*
 * The port's guest-addressed window.
 *
 * A host object that a DS *engine* has to reach cannot stay a host object.
 * Recompiled and decompiled code hand each other pointers freely because
 * makes a guest pointer and a host pointer the same value,
 * but that identity stops at the first piece of hardware that carries an
 * address in a field narrower than 32 bits, the FIFO's `data:26` and
 * SOUNDxSAD's 27, or at the first engine that reads memory by address
 * rather than by dereference, which is the SPU and the DMA unit.
 *
 * So the object moves instead of the field widening. This hands out blocks
 * inside ARM_PORT_WINDOW_BASE, which armrec_rt.c maps as a region, so a
 * pointer into it survives every DS-width address field and is readable by
 * everything that walks armrec_region_at().
 */
#ifndef PC_GUEST_WINDOW_H
#define PC_GUEST_WINDOW_H

#include <stdint.h>

/*
 * `bytes` of zeroed guest memory, 32-byte aligned, never freed. `what` names
 * the caller in the message if the window runs out, which is the only failure
 * mode: nothing here can return NULL.
 */
void *pc_guest_window_alloc(uint32_t bytes, const char *what);

/* How much of the window is spoken for, the linker-placed .bss plus every
 * run-time block, and how many run-time blocks there are. For the test,
 * which checks that the window is sized to what is in it rather than by eye. */
uint32_t pc_guest_window_used(void);
int      pc_guest_window_blocks(void);

/* Where the linker-placed half ends, i.e. the first address the allocator
 * will ever return. Zero before armrec_mem_init(). */
uint32_t pc_guest_window_placed_end(void);

/* The region's own base and size, as armrec reports them. Zero if
 * armrec_mem_init() has not run. */
uint32_t pc_guest_window_base(void);
uint32_t pc_guest_window_size(void);

#endif /* PC_GUEST_WINDOW_H */
