/*
 * 3ds/src/3ds_snd_addr.h: the addresses the SPU is allowed to see.
 *
 * The rule, and it is the one the PC port paid for. SoundxSAD carries bits
 * 26..2 of a source address, so the SPU can only be pointed at the first
 * 128 MB of the DS's map. On PC the sequencer ran for months with every one of
 * its eleven source addresses pointing inside a `static` in decompiled C: the
 * game was silent and nothing said so, because a host pointer that crosses a
 * 27-bit field simply arrives as a different number. The fix there was to move
 * the object into the port window; the rule here is the same one, stated so
 * that this port cannot re-earn it:
 *
 *   anything the SPU reads is allocated with pc_guest_window_alloc() and
 *   stored as a *guest* address. The glue translates when the DMA runs.
 *
 * WHY THE 3DS needs more than a counter. On PC a truncated host pointer was
 * loud: the image sits far above 0x08000000, the mask left an address in no
 * mapped region, and pc_spu counted it. Here the process heap begins at
 * 0x08000000 and the slab lands a few pages into it, so `host & 0x07FFFFFC`
 * can land *inside* a real guest region, 0x0A000000 masks to main RAM's
 * base, 0x14000000 to the I/O page. 3ds/tests/snd_addr.c measures how much of
 * the heap aliases that way. A pointer that survives truncation into plausible
 * memory would be read as a wave, so the classifier below also counts a source
 * address that translates but is *not* in the port window: in this port every
 * wave lives in the sound heap, and the sound heap is inside SoundSystem,
 * which is inside the window.
 *
 * Count, do not judge. That is pc_spu's arrangement for the same problem and
 * it is kept: nothing here refuses to play a sound because the address looks
 * unusual. It refuses only what cannot be read at all, and everything else is
 * a number a test can require to be zero over a real boot.
 *
 * No libctru here either, so 3ds/tests/snd_addr.c runs all of it on the host.
 */

#ifndef POKEPLATINUM_3DS_SND_ADDR_H
#define POKEPLATINUM_3DS_SND_ADDR_H

#include <stdint.h>

/*
 * What went wrong with an address, split by cause because the causes have
 * different fixes:
 *
 *   HOST   a host pointer reached the SPU. The object it points at has to move
 *          to pc_guest_window_alloc(); this is the class pc_spu.c calls
 *          `host` and the one that was worth a whole task on PC.
 *   WIDE   a guest address SOUNDxSAD cannot carry, the AGB slot and up.
 *          Nothing in this port should ever be there; a DS could not play it
 *          either.
 *   LOST   a source address that translates to nothing: a hole, VRAM, or the
 *          remains of a truncated pointer.
 *   STRAY  a source address that translates, but not into the port window.
 *          Legal on a console and not an error here; it is the fingerprint a
 *          truncated host pointer leaves when the mask happens to land in
 *          real guest memory, so it is counted and the bytes are still read.
 */
enum {
    SND_ADDR_HOST,
    SND_ADDR_WIDE,
    SND_ADDR_LOST,
    SND_ADDR_STRAY,
    SND_ADDR_KINDS
};

/* How many of each, and the first offender of each; a count says something
 * is wrong, the address says what. Read by the self-test today and by
 * whatever the sound work gives the port for --watch. */
extern uint32_t snd_addr_bad[SND_ADDR_KINDS];
extern uint32_t snd_addr_first[SND_ADDR_KINDS];

/*
 * The value to store in SOUNDxSAD for a buffer the port has allocated. Returns
 * the guest address, word-aligned the way the field is, or 0 with a count if
 * the buffer cannot be reached by the SPU at all. `what` names the caller in
 * the message.
 *
 * 0 is not a source address a DS can use either, so a caller that ignores the
 * failure gets silence rather than corruption.
 */
uint32_t snd_sad_from_host(const void *host, const char *what);

/*
 * What the sound DMA reads: the host pointer for a source address, or NULL if
 * there is nothing there. `bytes` is the span the caller intends to read, and
 * the whole span has to be inside one region; a wave that runs off the end
 * of its region is not a wave, and returning a pointer to its first byte would
 * hand the mixer a buffer overrun.
 *
 * The address is masked to the field's own bits first, so this can be given
 * either a register value or an address that has already been masked.
 */
void *snd_sad_ptr(uint32_t sad, uint32_t bytes);

/* Zero the counters. For the self-test, and for a run that wants a fresh
 * measurement. */
void snd_addr_reset(void);

/*
 * The rule, checked: a window block's guest address fits the field even though
 * its host pointer does not, a simulated SAD read finds the bytes that were
 * written through the host pointer, and each of the four classes is produced
 * on purpose and counted. Returns failures, fills `*ran`. Allocates from the
 * window and resets it, like window_selftest().
 */
int snd_addr_selftest(int *ran);

#endif /* POKEPLATINUM_3DS_SND_ADDR_H */
