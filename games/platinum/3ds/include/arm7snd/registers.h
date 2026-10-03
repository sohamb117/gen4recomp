/*
 * 3DS shadow of pc/arm7snd/include/registers.h.
 *
 * The same move as the ioreg shadows, on a second header tree. The SDK spells every ARM9
 * register as `HW_REG_BASE + offset`, so redefining one macro moved all 329
 * of them. The ARM7 sound driver was decompiled from a tree that had no such
 * base: each of its registers is a *literal*, `(*(REGType16v *)0x4000500)`,
 * and on PC that is correct because armrec maps 0x04000000 at 0x04000000. On
 * this console 0x04000500 is not addressable at all, so every one of them has
 * to be rewritten; there is no single name to move.
 *
 * The offsets and the widths are the originals'. What changes is only the
 * base: `armrec_io_base` is the host pointer to the slab's I/O row, the same
 * one 3ds/include/nitro/hw/ARM9/mmap_global.h gives the ARM9 side and the
 * same page `armrec_io_page(ARM9)` answers with. That last part is not a
 * coincidence to be relied on quietly, pc/hw/pc_spu.c does not intercept
 * stores, it diffs that page against a shadow once a frame and replays what
 * changed. A register the driver wrote somewhere else would be a channel the
 * mixer never hears.
 *
 * 3ds/tests/snd_reg_pin.py derives the list below from the two headers this
 * one shadows and fails if the sets differ, so a register added to the driver
 * cannot arrive here as a DS address by being forgotten.
 *
 * What is not here. reg_SOUNDxCNT_KEYON is built out of reg_SOUNDxCNT_STAT
 * and carries no address of its own, so redefining STAT moves it. And
 * REG_DMA0SAD_ADDR in the shared header is a bare constant that nothing in
 * the driver dereferences; the pin fails if that ever stops being true,
 * because converting it would need a measurement of what it is FOR, and
 * leaving a used one alone would hand a DMA model a host pointer.
 */

#ifndef POKEPLATINUM_3DS_ARM7SND_REGISTERS_H
#define POKEPLATINUM_3DS_ARM7SND_REGISTERS_H

#include <stdint.h>

#include_next <registers.h>

/*
 * Set by guest_bind() in 3ds/src/3ds_guest.c and cleared on unbind, so a
 * register access before the slab exists is a null dereference with an
 * obvious cause rather than a write into whatever the heap has at 0x04000500.
 */
extern unsigned char *armrec_io_base;

#define A7SND_IO(a) ((void *)(armrec_io_base + ((unsigned)(a) - 0x04000000u)))

/*
 * And the one register value that is not a number. Everything above moves
 * where a register lives; this moves what one of them carries. SOUNDxSAD and
 * SNDCAPxDAD hold addresses the SPU reads and writes as a DMA engine, in the
 * DS's map and in 27 bits, so the pointer the driver holds, which here is a
 * host pointer into the port window, has to become the window's guest address
 * before it is stored. 3ds/src/3ds_snd_addr.c is that conversion and has been
 * from the guest window; this is its caller. It answers 0, which is not a source address
 * a DS can use either, for anything it cannot serve, and counts why.
 */
uint32_t snd_sad_from_host(const void *host, const char *what);

#undef SND_DMA_ADDR
#define SND_DMA_ADDR(p) snd_sad_from_host((const void *)(p), "sound dma")

#undef reg_EXTKEYIN
#undef reg_POWCNT2
#undef reg_SOUNDCNT_VOL
#undef reg_SOUNDCNT_MIX
#undef reg_SOUNDxCNT_VOL
#undef reg_SOUNDxCNT_VOLS
#undef reg_SOUNDxCNT_PAN
#undef reg_SOUNDxCNT_STAT
#undef reg_SOUNDxCNT
#undef reg_SOUNDoffCNT
#undef reg_SOUNDxSAD
#undef reg_SOUNDoffSAD
#undef reg_SOUNDxTMR
#undef reg_SOUNDoffTMR
#undef reg_SOUNDxPNT
#undef reg_SOUNDoffPNT
#undef reg_SOUNDxLEN
#undef reg_SOUNDoffLEN
#undef reg_SNDCAPxCNT
#undef reg_SNDCAPxDAD
#undef reg_SNDCAPxLEN

#define reg_EXTKEYIN (*(REGType16v *)A7SND_IO(0x4000136))

#define reg_POWCNT2 (*(REGType16v *)A7SND_IO(0x4000304))

#define reg_SOUNDCNT_VOL (*(REGType8v *)A7SND_IO(0x4000500))
#define reg_SOUNDCNT_MIX (*(REGType8v *)A7SND_IO(0x4000501))

#define reg_SOUNDxCNT_VOL(x)  (*(REGType8v *)A7SND_IO(0x4000400 + ((int)(x) * 0x10)))
#define reg_SOUNDxCNT_VOLS(x) (*(REGType16v *)A7SND_IO(0x4000400 + ((int)(x) * 0x10)))
#define reg_SOUNDxCNT_PAN(x)  (*(REGType8v *)A7SND_IO(0x4000402 + ((int)(x) * 0x10)))
#define reg_SOUNDxCNT_STAT(x) (*(REGType8v *)A7SND_IO(0x4000403 + ((int)(x) * 0x10)))

#define reg_SOUNDxCNT(x)      (*(REGType32v *)A7SND_IO(0x4000400 + ((int)x) * 0x10))
#define reg_SOUNDoffCNT(off)  (*(REGType32v *)A7SND_IO(0x4000400 + (int)(off)))
#define reg_SOUNDxSAD(x)      (*(REGType32v *)A7SND_IO(0x4000404 + ((int)x) * 0x10))
#define reg_SOUNDoffSAD(off)  (*(REGType32v *)A7SND_IO(0x4000404 + (int)(off)))
#define reg_SOUNDxTMR(x)      (*(REGType16v *)A7SND_IO(0x4000408 + ((int)x) * 0x10))
#define reg_SOUNDoffTMR(off)  (*(REGType16v *)A7SND_IO(0x4000408 + (int)(off)))
#define reg_SOUNDxPNT(x)      (*(REGType16v *)A7SND_IO(0x400040A + ((int)x) * 0x10))
#define reg_SOUNDoffPNT(off)  (*(REGType16v *)A7SND_IO(0x400040A + (int)(off)))
#define reg_SOUNDxLEN(x)      (*(REGType32v *)A7SND_IO(0x400040C + ((int)x) * 0x10))
#define reg_SOUNDoffLEN(off)  (*(REGType32v *)A7SND_IO(0x400040C + (int)(off)))

#define reg_SNDCAPxCNT(x) (*(REGType8v *)A7SND_IO(0x4000508 + ((int)(x))))
#define reg_SNDCAPxDAD(x) (*(REGType32v *)A7SND_IO(0x4000510 + ((int)(x) * 8)))
#define reg_SNDCAPxLEN(x) (*(REGType16v *)A7SND_IO(0x4000514 + ((int)(x) * 8)))

/* The shared header's, reached through this file because the driver's own
 * registers.h includes it by quoted path and so never meets a shadow. */
#undef reg_OS_TM0CNT_L
#undef reg_OS_TM0CNT_H
#undef reg_OS_TM1CNT_L
#undef reg_OS_TM1CNT_H
#undef reg_OS_TM2CNT_L
#undef reg_OS_TM2CNT_H
#undef reg_OS_TM3CNT_L
#undef reg_OS_TM3CNT_H
#undef reg_OS_IME
#undef reg_OS_IE
#undef reg_OS_IF

#define reg_OS_TM0CNT_L (*(REGType16v *)A7SND_IO(0x4000100))
#define reg_OS_TM0CNT_H (*(REGType16v *)A7SND_IO(0x4000102))
#define reg_OS_TM1CNT_L (*(REGType16v *)A7SND_IO(0x4000104))
#define reg_OS_TM1CNT_H (*(REGType16v *)A7SND_IO(0x4000106))
#define reg_OS_TM2CNT_L (*(REGType16v *)A7SND_IO(0x4000108))
#define reg_OS_TM2CNT_H (*(REGType16v *)A7SND_IO(0x400010a))
#define reg_OS_TM3CNT_L (*(REGType16v *)A7SND_IO(0x400010c))
#define reg_OS_TM3CNT_H (*(REGType16v *)A7SND_IO(0x400010e))

#define reg_OS_IME (*(REGType16v *)A7SND_IO(0x4000208))
#define reg_OS_IE  (*(REGType32v *)A7SND_IO(0x4000210))
#define reg_OS_IF  (*(REGType32v *)A7SND_IO(0x4000214))

#endif /* POKEPLATINUM_3DS_ARM7SND_REGISTERS_H */
