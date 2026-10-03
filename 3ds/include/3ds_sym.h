/*
 * 3ds/src/3ds_sym.c answers pc_sym.h on this console; the rest of that
 * interface is declared there. This is the one entry point that is not part
 * of it; the self-test, which has to run after the loader has relocated the
 * image, because the addresses the table resolves to are differences and a
 * build machine has no relocation to apply.
 */

#ifndef POKEPLATINUM_3DS_SYM_H
#define POKEPLATINUM_3DS_SYM_H

/* Checks run in *ran; the return is how many failed. */
int ov_addr_selftest(int *ran);

#endif /* POKEPLATINUM_3DS_SYM_H */
