/*
 * 3ds/src/3ds_ioreg.h: the one thing a port-chain file may ask of the
 * game chain.
 *
 * 3ds_ioreg.c is compiled with GAME_CFLAGS: it sees <nitro/...> and it must
 * never see <3ds.h>, because the two disagree about enum width. 3ds_main.c is
 * the opposite. This header is what lets the second call the first, so it
 * includes nothing at all, adding an include here would be adding it to one
 * side or the other.
 */

#ifndef POKEPLATINUM_3DS_IOREG_H
#define POKEPLATINUM_3DS_IOREG_H

/*
 * Does a register access from a game translation unit reach the slab?
 * Returns the number of checks that failed, 0 for a pass, and fills `*ran`
 * with how many it made. Writes to the I/O row and leaves it zeroed.
 *
 * Requires a bound slab. Everything it checks is a fact about addresses, so
 * the answer is the same on the console and under an emulator.
 */
int ioreg_selftest(int *ran);

/*
 * The 1-based index of the first check that failed, or -1 if none did. This
 * one runs only on the console; it is a statement about a compile line, and
 * the host has a different one, so the index is the whole debugging channel.
 */
int ioreg_first_failure(void);

#endif /* POKEPLATINUM_3DS_IOREG_H */
