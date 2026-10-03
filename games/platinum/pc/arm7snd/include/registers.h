#ifndef NITRO_REGISTERS_H
#define NITRO_REGISTERS_H

#include "nitro/registers_shared.h"

#define reg_EXTKEYIN (*(REGType16v *)0x4000136)

#define reg_POWCNT2 (*(REGType16v *)0x4000304)

#define reg_SOUNDCNT_VOL (*(REGType8v *)0x4000500)
#define reg_SOUNDCNT_MIX (*(REGType8v *)0x4000501)

#define reg_SOUNDxCNT_VOL(x)  (*(REGType8v *)(0x4000400 + ((int)(x) * 0x10)))
#define reg_SOUNDxCNT_VOLS(x) (*(REGType16v *)(0x4000400 + ((int)(x) * 0x10)))
#define reg_SOUNDxCNT_PAN(x)  (*(REGType8v *)(0x4000402 + ((int)(x) * 0x10)))
#define reg_SOUNDxCNT_STAT(x) (*(REGType8v *)(0x4000403 + ((int)(x) * 0x10)))

// Starting a channel is this one byte store, and the driver's two start
// sites spell it through this macro rather than writing the bit directly.
// On hardware the two spellings are the same store. The PC port's SPU model
// latches register *state*, and a start bit that was cleared and set again
// inside one sequencer tick reads unchanged at both ends; a retrigger is
// invisible to it, so under PLATFORM_PC the macro also names the event to
// the model. The ROM build's expansion is the identical store.
#ifdef PLATFORM_PC
void pc_spu_keyon_note(int idx);
#define reg_SOUNDxCNT_KEYON(x) (pc_spu_keyon_note((int)(x)), reg_SOUNDxCNT_STAT(x) |= 0x80)
#else
#define reg_SOUNDxCNT_KEYON(x) (reg_SOUNDxCNT_STAT(x) |= 0x80)
#endif

/*
 * The value a channel's source address register, or a capture unit's
 * destination, is given. The SPU is a DMA engine, so what these registers
 * carry is an address in the DS's own memory map, and on hardware (and on a
 * port whose guest memory is identity-mapped) the pointer the driver holds
 * already is one. A port where it is not has to convert here; there is
 * nowhere later, because SOUNDxSAD is 27 bits wide and a host pointer that
 * crosses it arrives as a different number with nothing to say so.
 */
#ifndef SND_DMA_ADDR
#define SND_DMA_ADDR(p) ((u32)(p))
#endif

#define reg_SOUNDxCNT(x)      (*(REGType32v *)(0x4000400 + ((int)x) * 0x10))
#define reg_SOUNDoffCNT(off)  (*(REGType32v *)(0x4000400 + (int)(off)))
#define reg_SOUNDxSAD(x)      (*(REGType32v *)(0x4000404 + ((int)x) * 0x10))
#define reg_SOUNDoffSAD(off)  (*(REGType32v *)(0x4000404 + (int)(off)))
#define reg_SOUNDxTMR(x)      (*(REGType16v *)(0x4000408 + ((int)x) * 0x10))
#define reg_SOUNDoffTMR(off)  (*(REGType16v *)(0x4000408 + (int)(off)))
#define reg_SOUNDxPNT(x)      (*(REGType16v *)(0x400040A + ((int)x) * 0x10))
#define reg_SOUNDoffPNT(off)  (*(REGType16v *)(0x400040A + (int)(off)))
#define reg_SOUNDxLEN(x)      (*(REGType32v *)(0x400040C + ((int)x) * 0x10))
#define reg_SOUNDoffLEN(off)  (*(REGType32v *)(0x400040C + (int)(off)))

#define reg_SNDCAPxCNT(x) (*(REGType8v *)(0x4000508 + ((int)(x))))

// The capture unit's destination and length. The decompiled SND_capture.c
// wrote both as bare pointer casts; they are macros here so that every
// register in this driver is reached one way. The expansion is the identical
// store.
#define reg_SNDCAPxDAD(x) (*(REGType32v *)(0x4000510 + ((int)(x) * 8)))
#define reg_SNDCAPxLEN(x) (*(REGType16v *)(0x4000514 + ((int)(x) * 8)))

#define EXTKEYIN_X     (1 << 0)
#define EXTKEYIN_Y     (1 << 1)
#define EXTKEYIN_DEBUG (1 << 3)
#define EXTKEYIN_PEN   (1 << 6)
#define EXTKEYIN_HINGE (1 << 7)

#endif // NITRO_REGISTERS_H
