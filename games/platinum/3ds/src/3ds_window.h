/*
 * 3ds/src/3ds_window.h: the port's guest-addressed window, on the slab.
 *
 * What the window is for, in one paragraph, because the name does not say it.
 * A DS engine that carries an address in a field narrower than 32 bits cannot
 * be handed a host object: SOUNDxSAD is 27 bits and the SPU is a DMA engine,
 * so a wave anywhere else is both unreachable and untruncatable. On PC the
 * answer was a region in the 0x02800000-0x03000000 hole, ARM_PORT_WINDOW_BASE,
 * and pc/src/pc_guest_window.c hands it out. Here the same region is a row
 * of the slab (GUEST_R_WINDOW), so the allocator is the same bump allocator
 * over different storage, and this file is that allocator.
 *
 * The names are the pc's on purpose. Pc/patches/src/sound_system.c.patch
 * declares `pc_guest_window_alloc` itself, inside SoundSystem_Get(), and that
 * patch has to apply to one decompiled tree for both ports. Matching the
 * spelling is what lets the 3DS build link the same patched game code with no
 * 3DS-only patch behind it. The prototypes below are pc/include/
 * pc_guest_window.h's, unchanged, so a translation unit that includes that
 * header from the header shadows onwards still gets the right declarations.
 *
 * A returned pointer is a host pointer, and that is the whole difference from
 * PC. There a block's address *is* its guest address; here the caller
 * dereferences what it gets back and anything that needs the DS-visible
 * address asks armrec_guest_addr() for it. This window is what makes the SPU
 * do exactly that.
 *
 * No libctru here, for the reason 3ds_guest.c has none: the host test links
 * this file against a malloc'd slab and runs the same window_selftest() the
 * console runs.
 */

#ifndef POKEPLATINUM_3DS_WINDOW_H
#define POKEPLATINUM_3DS_WINDOW_H

#include <stdint.h>

/*
 * `bytes` of zeroed guest memory, 32-byte aligned, never freed, NNS's sound
 * heap asserts that alignment on every block it is given, and nothing in the
 * window has a lifetime shorter than the process.
 *
 * `what` names the caller in the message if the window runs out. Unlike the PC
 * version this returns NULL rather than trapping: armrec_trap() comes with the backend and
 * there is nowhere for a message to go before it. Until then the failure is a
 * NULL, window_strerror() explains it, and the self-test binary is what
 * makes it visible. A caller that ignores the NULL gets a null dereference,
 * which is loud on this console.
 */
void *pc_guest_window_alloc(uint32_t bytes, const char *what);

/* How much of the window is spoken for, and how many blocks that is. */
uint32_t pc_guest_window_used(void);
int      pc_guest_window_blocks(void);

/*
 * Where the link-placed half ends, i.e. the first address the allocator will
 * ever return. On PC that is `__pc_guest_window_free`, a symbol a generated
 * linker fragment places after the objects the ELF puts in the window. This
 * port has no such half, and neither, as it turns out, does platinum's PC
 * build, so this is the region's base, and it exists to keep the API one
 * API. See 3ds_window.c for the grep that settled it.
 */
uint32_t pc_guest_window_placed_end(void);

/* The region's base and size, from the guest map. Constants, not state. */
uint32_t pc_guest_window_base(void);
uint32_t pc_guest_window_size(void);

/*
 * Forget every block. There is no free() and this is not one: it is how the
 * self-test hands back a window it filled, and how a fresh slab binding starts
 * clean. Calling it once real allocations exist would hand the next caller
 * memory the game is still using.
 */
void window_reset(void);

/* Why the last allocation failed. "" if none has. */
const char *window_strerror(void);

/*
 * Bump arithmetic, alignment, zeroing, exhaustion and the round trip through
 * the translator, over a bound slab. Returns the number of checks that failed,
 * 0 for a pass, and fills `*ran` with how many it made if `ran` is non-NULL.
 *
 * Allocates from the window and scribbles on it, then resets: call it before
 * anything real has been allocated, and zero the slab afterwards the way
 * 3ds_main.c does for guest_selftest().
 */
int window_selftest(int *ran);

#endif /* POKEPLATINUM_3DS_WINDOW_H */
