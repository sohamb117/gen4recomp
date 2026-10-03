/*
 * 3ds/src/3ds_sdk_statics.h: what a port-chain file may ask of
 * 3ds_sdk_statics.c.
 *
 * Includes nothing, for the reason 3ds_hwmem.h includes nothing: the file it
 * declares is compiled against the DS SDK and its caller against libctru, and
 * no header may be on both sides of that line.
 */

#ifndef POKEPLATINUM_3DS_SDK_STATICS_H
#define POKEPLATINUM_3DS_SDK_STATICS_H

/*
 * Write the two SDK statics whose initialisers stopped being constant when
 * the memory shadow moved their addresses onto the slab. Requires a bound
 * slab: call it straight after armrec_mem_init(), and before any SDK code
 * runs.
 */
void sdk_statics_publish(void);

/*
 * Are the two objects in this link at all? The port's own 3dsx has no game
 * code, so both are weak-undefined there and there is nothing to publish.
 */
int sdk_statics_present(void);

/* Non-zero if either is still unwritten. Nine checks, or none if the game is
 * not in this link. */
int sdk_statics_check(void);

#endif /* POKEPLATINUM_3DS_SDK_STATICS_H */
