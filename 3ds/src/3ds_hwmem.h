/*
 * 3ds/src/3ds_hwmem.h: what a port-chain file may ask of 3ds_hwmem.c.
 *
 * Includes nothing, for the reason 3ds_ioreg.h includes nothing: the file it
 * declares is compiled against the DS SDK and its caller is compiled against
 * libctru, and no header may be on both sides of that line.
 */

#ifndef POKEPLATINUM_3DS_HWMEM_H
#define POKEPLATINUM_3DS_HWMEM_H

/*
 * Does every DS memory-map constant resolve to the slab row that holds it?
 * Returns failures, 0 for a pass, fills `*ran`. Writes to two words of the
 * shared work area and zeroes them again. Requires a bound slab.
 */
int hwmem_selftest(int *ran);

/* 1-based index of the first check that failed, -1 if none. Console-only, so
 * the index is the whole debugging channel. */
int hwmem_first_failure(void);

#endif /* POKEPLATINUM_3DS_HWMEM_H */
