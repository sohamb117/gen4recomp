/*
 * 3ds/src/3ds_state.h: the guest state digest on this console.
 *
 * The three functions are pc/src/pc_state.h's, unchanged in name, arithmetic
 * and meaning: FNV-1a over guest memory, one digest per region armrec reports
 * and one combined value. What differs is that the bytes are reached through
 * the translator. See 3ds_state.c for what a hole in VRAM hashes as and for
 * why host-pointer marking is not here.
 */

#ifndef POKEPLATINUM_3DS_STATE_H
#define POKEPLATINUM_3DS_STATE_H

#include <stdint.h>

/*
 * The digest of a zeroed guest map with no bank enabled, what
 * armrec_mem_init() leaves behind, before the keypad reset words go in.
 *
 * It is a cross-host pin and almost the only one there can be. It depends on
 * the region table, the region sizes, the fold and nothing else: no host
 * pointer, no allocation address, no VRAMCNT. A PC run digested at the same
 * point owes this number. Anything later does not travel, guest memory holds
 * host pointers by then, and those are a fact about a link rather than about
 * the game.
 *
 * Recomputed from the region sizes by 3ds/tests/state_digest.c with a second
 * implementation, so this constant is not the arithmetic certifying itself.
 */
#define STATE_ZERO_DIGEST 0x5EB738D8E177BE8FULL

/* FNV-1a over any buffer, seeded. One implementation, so a digest in a report
 * and a digest of a host-side buffer mean the same thing. */
uint64_t pc_state_fnv1a(uint64_t seed, const void *p, uint32_t n);

/*
 * Digest [base, base+size) of guest memory. Reads nothing outside it. A VRAM
 * block with no bank behind it, and any address the map does not cover, hash
 * as the zeros the console reads there. 0 before armrec_mem_init().
 */
uint64_t pc_state_digest_span(uint32_t base, uint32_t size);

/* Digest every region armrec_region_at() reports, in table order, and fold
 * them big-endian into one value. 0 before armrec_mem_init(). */
uint64_t pc_state_digest(void);

/*
 * Host pointers in guest memory, 7.12. The names and the meaning are
 * pc/src/pc_state.h's: a marked word is skipped by every digest, because a
 * relink moves it and nothing else. What differs is that a caller here holds
 * host pointers and the translator says which guest word each one is.
 *
 * pc_state_mark_host_word() takes a GUEST address and ignores one the map
 * does not cover. pc_state_mark_host_field() takes the host pointer to the
 * field. pc_state_scan_mark_host_words() takes a filled object and marks
 * every aligned word in it whose value is a host pointer, into the slab or
 * into this image; which is what the patched constructors call.
 */
void pc_state_mark_host_word(uint32_t addr);
void pc_state_mark_host_field(const void *field);
void pc_state_scan_mark_host_words(const void *obj, uint32_t n);
uint32_t pc_state_host_word_count(void);

/*
 * The image's span, for the "is this a function pointer" half of the scan.
 * On the console it defaults to 3dsx.ld's __start__ and __end__ and nothing
 * has to call this; a host harness has no such image and sets its own.
 */
void state_set_image_bounds(uint32_t lo, uint32_t hi);

/* Forget every mark. For the self-test, which has to leave the digest the
 * way it found it; nothing in the port calls it. */
void state_clear_host_marks(void);

/*
 * The zero-state pin, a byte that moves a region and the total, an empty span,
 * and the VRAM case the flat walk cannot do: a marker written through the LCDC
 * window, the bank moved to main BG, and each window's digest before and after.
 * Leaves guest memory and VRAMCNT the way it found them, so it may run before
 * anything else. Returns failures, fills `*ran`.
 */
int state_selftest(int *ran);

#endif /* POKEPLATINUM_3DS_STATE_H */
