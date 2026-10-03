/*
 * 3ds/src/3ds_ctrdg.h: what a port-chain file may ask of 3ds_ctrdg.c.
 *
 * Includes nothing, for the reason 3ds_hwmem.h includes nothing: the file it
 * declares is compiled against the DS SDK and its caller is compiled against
 * libctru, and no header may be on both sides of that line.
 */

#ifndef POKEPLATINUM_3DS_CTRDG_H
#define POKEPLATINUM_3DS_CTRDG_H

/*
 * Does the cartridge slot read as an empty slot? Every address the SDK and the
 * game reach it through, against the translator; the header and the module-ID
 * image word as zeros; the verdict those zeros give; and nothing translating
 * above the probe buffer. Returns failures, 0 for a pass, fills `*ran`.
 * Reads only. Requires a bound slab.
 */
int ctrdg_selftest(int *ran);

/* 1-based index of the first check that failed, -1 if none. Console-only, so
 * the index is the whole debugging channel. */
int ctrdg_first_failure(void);

#endif /* POKEPLATINUM_3DS_CTRDG_H */
