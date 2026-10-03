/*
 * 3ds/src/3ds_cp.h: the ARM9's divider and square-root unit.
 *
 * 0x04000280-0x040002BF is the one part of the I/O row that is not storage.
 * On hardware, writing the operands starts a unit that fills DIV_RESULT,
 * DIVREM_RESULT and SQRT_RESULT a few cycles later; in a slab those registers
 * hold whatever was last stored in them, so every FX_Div, FX_Inv, FX_Sqrt and
 * VEC_Normalize in the game would read zeros.
 *
 * What makes it work is a completion point, not a clock. Touching a register
 * the unit owns (either CNT, or a result register) computes what the unit
 * would have produced from the operands as they stand, stores it into the real
 * register, and hands back a pointer to it. The operands (numer, denom,
 * SQRT_PARAM) are left alone: they are storage on hardware too. That is
 * armrec_rt.c's model, and pc/include/nitro/hw/ARM9/ioreg_CP.h is the shadow
 * that routes the game's nine `reg_CP_*` macros into it.
 *
 * Why this file exists at all. Armrec's version reaches the registers with
 * `*(volatile uint32_t *)(uintptr_t)0x04000280`, which is a real address there
 * and is nothing here. Everything else about the model is the same, so this is
 * the same code over the translator, the arrangement armrec_mem_3ds.c and
 * 3ds_vram.c already use, and for the same reason: tools/armrec/armrec_rt.c
 * stays byte-identical for pokediamond.
 *
 * Not pinned against armrec_rt.c, unlike the VRAM tables, and that is a
 * measurement rather than an omission. The placement tables came out of a
 * console sweep and have no second oracle, so a drifted copy is undetectable.
 * A divider does: floor(sqrt(v)), truncating division, and the three states
 * hardware answers oddly (a zero divisor, and 32-bit INT_MIN / -1) are
 * literals in cp_selftest() below. A copy that drifts fails those.
 *
 * No libctru here, so 3ds/tests/cp_div.c runs the whole model on a build
 * machine over a malloc'd slab.
 */

#ifndef POKEPLATINUM_3DS_CP_H
#define POKEPLATINUM_3DS_CP_H

#include <stdint.h>

/* The registers the unit owns. SQRT_PARAM at 0x040002B8 is storage and is
 * deliberately outside the window. */
#define CP_DIVCNT_ADDR  0x04000280u
#define CP_NUMER_ADDR   0x04000290u
#define CP_DENOM_ADDR   0x04000298u
#define CP_RESULT_ADDR  0x040002A0u
#define CP_REM_ADDR     0x040002A8u
#define CP_SQRTCNT_ADDR 0x040002B0u
#define CP_SQRTRES_ADDR 0x040002B4u
#define CP_SQRTPARAM_ADDR 0x040002B8u

/*
 * The completion point: finish whatever `addr` names, then hand back the host
 * pointer to it. NULL only when the slab is not bound. An address the unit
 * does not own is plain memory and comes straight back through the translator.
 */
void *cp_ptr(uint32_t guest);

/*
 * The divider, the square root, and the three states that are neither: a zero
 * divisor in each of the three modes, 32-bit INT_MIN / -1, and the operand
 * registers staying storage. Returns failures, fills `*ran`. Writes the CP
 * registers and leaves them zeroed.
 */
int cp_selftest(int *ran);

#endif /* POKEPLATINUM_3DS_CP_H */
