/*
 * 3DS shadow of pc/arm7snd/include/mmap.h.
 *
 * One macro, and it is the reason this port had no sound. SND_command.c's
 * PXI callback decides whether the word the ARM9 just sent is a command list
 * or the zero poke by comparing it against main RAM's base, because on a DS
 * the list is in main RAM. This port hands the driver a host pointer, and
 * where the host puts the ARM9's objects is not something a DS constant can
 * describe: on PC the image happens to sit above 0x02000000 and every list
 * passed, and on this console homebrew is loaded at 0x00100000, so every list
 * was dropped in silence. Measured over a boot: three lists sent, three lists
 * refused, the driver's shared work pointer still NULL after a thousand
 * frames, and three sound registers written in the whole run, the three
 * that pc_arm7snd_init() writes before any of this.
 *
 * So the floor here is the image's own base, which is the lowest address any
 * pointer the driver can be handed will have: the 3dsx is loaded at
 * 0x00100000 and the slab comes out of the application heap far above it.
 * `__start__` is 3dsx.ld's, the same symbol 3ds/src/3ds_state.c takes the
 * image bounds from, so this is not a literal that has to be kept in step
 * with the loader.
 *
 * Why the other use of HW_MAIN_MEM is left alone. SND_bank.c asks the same
 * question of a wave offset, absolute pointer, or an offset into the
 * archive that has to be added to its base, and there the numbers below the
 * floor are real: the largest wave archive in this game is 458,564 bytes, so
 * main RAM's base leaves a factor of nine and the image's base would leave
 * two. Nothing this port loads a wave into is in the image, so that test does
 * not need moving and it keeps its margin.
 */

#ifndef POKEPLATINUM_3DS_ARM7SND_MMAP_H
#define POKEPLATINUM_3DS_ARM7SND_MMAP_H

#include_next <mmap.h>

/* 3dsx.ld. An array rather than a char so the address is the symbol's own
 * value and not something read out of the image. */
extern char __start__[];

#undef SND_CMD_ADDR_FLOOR
#define SND_CMD_ADDR_FLOOR ((unsigned)(unsigned long)(__start__))

#endif /* POKEPLATINUM_3DS_ARM7SND_MMAP_H */
