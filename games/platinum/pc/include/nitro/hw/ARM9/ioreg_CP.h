/*
 * PC port shadow of the SDK's ioreg_CP.h, the hardware divider and
 * square-root unit at 0x04000280-0x040002BF.
 *
 * Identity mapping makes those addresses plain memory here, so nothing ever
 * computes and the result registers hold whatever was last stored there:
 * Every FX_Div, FX_Inv, FX_Sqrt and VEC_Normalize in the game would read
 * zeros. armrec_cp_ptr() (tools/armrec/armrec_rt.c, shared verbatim with
 * pokediamond, where the model was measured) is the completion point:
 * touching a register the unit owns; either CNT, or a result register , 
 * computes what the unit would have produced from the operand registers as
 * they stand, stores it into the real register, then hands back a pointer to
 * that register. The operand registers (numer, denom, SQRT_PARAM) are
 * untouched: they are storage, and storage is what identity-mapped memory
 * already gives.
 *
 * A pointer rather than a value because both CNTs are written through their
 * macro (`reg_CP_DIVCNT = 0;`) and a macro used as an lvalue cannot become a
 * call; and because code takes the address of a result register (FX_DivS32
 * spells the low word `*(REGType32v *)&reg_CP_DIV_RESULT`). Keeping the
 * lvalue form means no source changes at all. The CNTs are hooked too so a
 * pointer taken once and read after a busy-wait still finds the value
 * already in the register.
 *
 * #include_next picks up the SDK original; everything below is a change to
 * nine macros out of its set. This is pokediamond's pc/include/registers.h
 * arrangement, split per-header because the SDK splits the registers.
 */

#ifndef PC_SHADOW_IOREG_CP_H
#define PC_SHADOW_IOREG_CP_H

#include_next <nitro/hw/ARM9/ioreg_CP.h>

#ifndef SDK_ASM

#include <stdint.h>

void *armrec_cp_ptr(uint32_t addr);

#undef reg_CP_DIVCNT
#undef reg_CP_DIV_RESULT
#undef reg_CP_DIV_RESULT_L
#undef reg_CP_DIV_RESULT_H
#undef reg_CP_DIVREM_RESULT
#undef reg_CP_DIVREM_RESULT_L
#undef reg_CP_DIVREM_RESULT_H
#undef reg_CP_SQRTCNT
#undef reg_CP_SQRT_RESULT

#define reg_CP_DIVCNT           (*(REGType16v *)armrec_cp_ptr(REG_DIVCNT_ADDR))
#define reg_CP_DIV_RESULT       (*(REGType64v *)armrec_cp_ptr(REG_DIV_RESULT_ADDR))
#define reg_CP_DIV_RESULT_L     (*(REGType32v *)armrec_cp_ptr(REG_DIV_RESULT_L_ADDR))
#define reg_CP_DIV_RESULT_H     (*(REGType32v *)armrec_cp_ptr(REG_DIV_RESULT_H_ADDR))
#define reg_CP_DIVREM_RESULT    (*(REGType64v *)armrec_cp_ptr(REG_DIVREM_RESULT_ADDR))
#define reg_CP_DIVREM_RESULT_L  (*(REGType32v *)armrec_cp_ptr(REG_DIVREM_RESULT_L_ADDR))
#define reg_CP_DIVREM_RESULT_H  (*(REGType32v *)armrec_cp_ptr(REG_DIVREM_RESULT_H_ADDR))
#define reg_CP_SQRTCNT          (*(REGType16v *)armrec_cp_ptr(REG_SQRTCNT_ADDR))
#define reg_CP_SQRT_RESULT      (*(REGType32v *)armrec_cp_ptr(REG_SQRT_RESULT_ADDR))

#endif /* SDK_ASM */

#endif /* PC_SHADOW_IOREG_CP_H */
