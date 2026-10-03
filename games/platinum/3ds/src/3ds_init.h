/*
 * 3ds/src/3ds_init.h: the host layer's start-up order, as one call.
 *
 * pc/src/pc_main.c brings the port's host models up in a fixed sequence
 * before it enters NitroMain. That sequence is not a list of independent
 * initialisers; it is an order, and the order is the point. This
 * header is the one entry point the crt calls to run it.
 *
 * See 3ds_init.c for which models are in it, which two are deliberately
 * absent, and why PXI and PM have no step of their own.
 */

#ifndef POKEPLATINUM_3DS_INIT_H
#define POKEPLATINUM_3DS_INIT_H

/*
 * Run every host model in pc_main.c's order. Returns 0, or -1 at the first
 * step that failed, the sequence stops there, because every step after a
 * failed one would be initialising on top of a model that is not up.
 *
 * Must be called after armrec_mem_init(): four of the five steps write guest
 * memory or allocate out of the port window.
 */
int host_init(void);

/*
 * The failing step's name, or NULL when host_init() has not failed. This is
 * what the crt puts on the screen; a console has no stderr anyone reads.
 */
const char *host_init_failed(void);

/* Steps that ran, and steps this link does not have (the self-test .3dsx
 * links no pc/src, so four of the five are absent there and skipped). */
int host_init_ran(void);
int host_init_skipped(void);

#endif /* POKEPLATINUM_3DS_INIT_H */
