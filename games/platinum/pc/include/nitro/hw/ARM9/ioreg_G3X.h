/*
 * PC port shadow of the SDK's ioreg_G3X.h, the packed geometry FIFO port
 * and the engine's status and result registers.
 *
 * 0x04000400 is a FIFO: every store pushes, so reg_G3X_GXFIFO goes through
 * armrec_gx_port()'s staging word like the per-command ports in the
 * ioreg_G3.h shadow (see that file for the model and its one limit).
 *
 * 0x04000600-0x040006A3 IS COMPUTED. armrec_gx_reg() puts the engine's
 * current GXSTAT, RAM counts, test results and clip and direction matrices
 * where the pointer points *before* handing it over; which is what lets
 * G3X_GetClipMtx() take the address of the first word and block-copy sixteen
 * through MI_Copy64B. It refreshes the whole block for exactly that reason.
 * A guest *write* to the writable registers in it (GXSTAT and
 * DISP_1DOT_DEPTH) lands in memory and is noticed at the engine's next
 * access by comparison, which is how `reg_G3X_GXSTAT |= mask` reaches the
 * model.
 *
 * The rendering registers this header also defines (DISP3DCNT, the clear
 * colour and depth, fog, toon, edge, ALPHA_TEST_REF) are NOT redirected:
 * They are plain guest memory, latched whole by pc_gpu3d_vblank() at the
 * frame boundary, the same arrangement pokediamond settled (its decision
 * 36) after measuring where each register class actually lives.
 */

#ifndef PC_SHADOW_IOREG_G3X_H
#define PC_SHADOW_IOREG_G3X_H

#include_next <nitro/hw/ARM9/ioreg_G3X.h>

#ifndef SDK_ASM

#include <stdint.h>

uint32_t *armrec_gx_port(uint32_t addr);
void *armrec_gx_reg(uint32_t addr);

#undef reg_G3X_GXFIFO
#undef reg_G3X_GXSTAT
#undef reg_G3X_LISTRAM_COUNT
#undef reg_G3X_VTXRAM_COUNT
#undef reg_G3X_DISP_1DOT_DEPTH
#undef reg_G3X_POS_RESULT_X
#undef reg_G3X_POS_RESULT_Y
#undef reg_G3X_POS_RESULT_Z
#undef reg_G3X_POS_RESULT_W
#undef reg_G3X_VEC_RESULT_X
#undef reg_G3X_VEC_RESULT_Y
#undef reg_G3X_VEC_RESULT_Z
#undef reg_G3X_CLIPMTX_RESULT_0
#undef reg_G3X_CLIPMTX_RESULT_1
#undef reg_G3X_CLIPMTX_RESULT_2
#undef reg_G3X_CLIPMTX_RESULT_3
#undef reg_G3X_CLIPMTX_RESULT_4
#undef reg_G3X_CLIPMTX_RESULT_5
#undef reg_G3X_CLIPMTX_RESULT_6
#undef reg_G3X_CLIPMTX_RESULT_7
#undef reg_G3X_CLIPMTX_RESULT_8
#undef reg_G3X_CLIPMTX_RESULT_9
#undef reg_G3X_CLIPMTX_RESULT_10
#undef reg_G3X_CLIPMTX_RESULT_11
#undef reg_G3X_CLIPMTX_RESULT_12
#undef reg_G3X_CLIPMTX_RESULT_13
#undef reg_G3X_CLIPMTX_RESULT_14
#undef reg_G3X_CLIPMTX_RESULT_15
#undef reg_G3X_VECMTX_RESULT_0
#undef reg_G3X_VECMTX_RESULT_1
#undef reg_G3X_VECMTX_RESULT_2
#undef reg_G3X_VECMTX_RESULT_3
#undef reg_G3X_VECMTX_RESULT_4
#undef reg_G3X_VECMTX_RESULT_5
#undef reg_G3X_VECMTX_RESULT_6
#undef reg_G3X_VECMTX_RESULT_7
#undef reg_G3X_VECMTX_RESULT_8

/* The packed command port: a staging word, committed by the next access. */
#define reg_G3X_GXFIFO             (*(REGType32v *)armrec_gx_port(REG_GXFIFO_ADDR))

/* The status and result block: computed into guest memory before the
 * pointer comes back, so a block read of the clip matrix works. */
#define reg_G3X_GXSTAT             (*(REGType32v *)armrec_gx_reg(REG_GXSTAT_ADDR))
#define reg_G3X_LISTRAM_COUNT      (*(const REGType16v *)armrec_gx_reg(REG_LISTRAM_COUNT_ADDR))
#define reg_G3X_VTXRAM_COUNT       (*(const REGType16v *)armrec_gx_reg(REG_VTXRAM_COUNT_ADDR))
#define reg_G3X_DISP_1DOT_DEPTH    (*(REGType16v *)armrec_gx_reg(REG_DISP_1DOT_DEPTH_ADDR))
#define reg_G3X_POS_RESULT_X       (*(const REGType32v *)armrec_gx_reg(REG_POS_RESULT_X_ADDR))
#define reg_G3X_POS_RESULT_Y       (*(const REGType32v *)armrec_gx_reg(REG_POS_RESULT_Y_ADDR))
#define reg_G3X_POS_RESULT_Z       (*(const REGType32v *)armrec_gx_reg(REG_POS_RESULT_Z_ADDR))
#define reg_G3X_POS_RESULT_W       (*(const REGType32v *)armrec_gx_reg(REG_POS_RESULT_W_ADDR))
#define reg_G3X_VEC_RESULT_X       (*(const REGType16v *)armrec_gx_reg(REG_VEC_RESULT_X_ADDR))
#define reg_G3X_VEC_RESULT_Y       (*(const REGType16v *)armrec_gx_reg(REG_VEC_RESULT_Y_ADDR))
#define reg_G3X_VEC_RESULT_Z       (*(const REGType16v *)armrec_gx_reg(REG_VEC_RESULT_Z_ADDR))
#define reg_G3X_CLIPMTX_RESULT_0   (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_0_ADDR))
#define reg_G3X_CLIPMTX_RESULT_1   (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_1_ADDR))
#define reg_G3X_CLIPMTX_RESULT_2   (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_2_ADDR))
#define reg_G3X_CLIPMTX_RESULT_3   (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_3_ADDR))
#define reg_G3X_CLIPMTX_RESULT_4   (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_4_ADDR))
#define reg_G3X_CLIPMTX_RESULT_5   (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_5_ADDR))
#define reg_G3X_CLIPMTX_RESULT_6   (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_6_ADDR))
#define reg_G3X_CLIPMTX_RESULT_7   (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_7_ADDR))
#define reg_G3X_CLIPMTX_RESULT_8   (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_8_ADDR))
#define reg_G3X_CLIPMTX_RESULT_9   (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_9_ADDR))
#define reg_G3X_CLIPMTX_RESULT_10  (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_10_ADDR))
#define reg_G3X_CLIPMTX_RESULT_11  (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_11_ADDR))
#define reg_G3X_CLIPMTX_RESULT_12  (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_12_ADDR))
#define reg_G3X_CLIPMTX_RESULT_13  (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_13_ADDR))
#define reg_G3X_CLIPMTX_RESULT_14  (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_14_ADDR))
#define reg_G3X_CLIPMTX_RESULT_15  (*(const REGType32v *)armrec_gx_reg(REG_CLIPMTX_RESULT_15_ADDR))
#define reg_G3X_VECMTX_RESULT_0    (*(const REGType32v *)armrec_gx_reg(REG_VECMTX_RESULT_0_ADDR))
#define reg_G3X_VECMTX_RESULT_1    (*(const REGType32v *)armrec_gx_reg(REG_VECMTX_RESULT_1_ADDR))
#define reg_G3X_VECMTX_RESULT_2    (*(const REGType32v *)armrec_gx_reg(REG_VECMTX_RESULT_2_ADDR))
#define reg_G3X_VECMTX_RESULT_3    (*(const REGType32v *)armrec_gx_reg(REG_VECMTX_RESULT_3_ADDR))
#define reg_G3X_VECMTX_RESULT_4    (*(const REGType32v *)armrec_gx_reg(REG_VECMTX_RESULT_4_ADDR))
#define reg_G3X_VECMTX_RESULT_5    (*(const REGType32v *)armrec_gx_reg(REG_VECMTX_RESULT_5_ADDR))
#define reg_G3X_VECMTX_RESULT_6    (*(const REGType32v *)armrec_gx_reg(REG_VECMTX_RESULT_6_ADDR))
#define reg_G3X_VECMTX_RESULT_7    (*(const REGType32v *)armrec_gx_reg(REG_VECMTX_RESULT_7_ADDR))
#define reg_G3X_VECMTX_RESULT_8    (*(const REGType32v *)armrec_gx_reg(REG_VECMTX_RESULT_8_ADDR))

#endif /* SDK_ASM */

#endif /* PC_SHADOW_IOREG_G3X_H */
