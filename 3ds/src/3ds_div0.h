/*
 * 3ds/src/3ds_div0.h: what a caller may ask of 3ds_div0.c.
 */

#ifndef POKEPLATINUM_3DS_DIV0_H
#define POKEPLATINUM_3DS_DIV0_H

/*
 * Does a division by zero answer the way the cartridge answered? Returns
 * failures, 0 for a pass, fills `*ran`. Touches no guest memory. ARM only,
 * the same expressions kill an x86 process.
 */
int div0_selftest(int *ran);

/* 1-based index of the first check that failed, -1 if none. */
int div0_first_failure(void);

/* pc/src/pc_selftest.c's name for the same suite: 1 when it passed, which is
 * that table's convention rather than this file's failure count. */
int pc_div0_selftest(void);

#endif /* POKEPLATINUM_3DS_DIV0_H */
