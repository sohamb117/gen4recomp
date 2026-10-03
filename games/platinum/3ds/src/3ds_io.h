/*
 * 3ds/src/3ds_io.h: the two processors' I/O pages.
 *
 * 0x04000000 is one row of the slab and two pages of registers: what a DS puts
 * at those addresses depends on which processor is asking, and 83 of the first
 * page's 2,048 halfwords are private and writable to one side or the other.
 * The running processor's page is the row itself; the other's is a save
 * buffer. See 3ds_io.c for why that is copies rather than a remap, why the
 * mirror list has exactly two entries, and why this game has never switched.
 *
 * The names are local rather than armrec's, because armrec_rt.c defines
 * `armrec_cpu_switch` and `armrec_io_page` for the PC build, and the backend is the
 * task that decides how much of that file this console compiles. No libctru
 * here, so the host runs the same self-test the 3dsx does.
 */

#ifndef POKEPLATINUM_3DS_IO_H
#define POKEPLATINUM_3DS_IO_H

#include <stdint.h>

/* ARMREC_CPU_ARM9 / ARMREC_CPU_ARM7, by their values. */
#define IO_CPU_ARM9 0
#define IO_CPU_ARM7 1

/* Zero both saved pages and make the ARM9 the running processor, the state a
 * reset console starts in. Call after the slab is bound. */
void io_pages_init(void);

/* Which processor's page is live. */
int io_cpu(void);

/*
 * The `cpu`'s page: the live row for the one that is running, its saved copy
 * for the one that is not, so a hardware model can reach either without
 * knowing which is which. NULL before io_pages_init() or for a bad index.
 */
void *io_page(int cpu);

/*
 * Make `cpu` the running processor: save the outgoing page, copy the mirrored
 * halfwords into the incoming one, install it. 0, or -1 for a bad index or an
 * unbound slab. Switching to the processor already running is a no-op.
 */
int io_switch(int cpu);

/* The mirrored registers; the halfwords both processors read the same value
 * for. Two, and adding one takes a measurement. */
int      io_mirror_count(void);
uint32_t io_mirror_at(int i, const char **name);

/*
 * Private registers staying private, mirrored ones crossing both ways, the
 * saved copies readable while suspended, and the two registers that are
 * visible across on hardware staying out of the mirror list. Returns failures,
 * fills `*ran`. Scribbles on the I/O page and puts it back to zeros.
 */
int io_selftest(int *ran);

#endif /* POKEPLATINUM_3DS_IO_H */
