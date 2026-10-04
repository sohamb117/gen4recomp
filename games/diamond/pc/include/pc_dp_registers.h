/*
 * D/P shadow of arm9/lib/NitroSDK/include/registers.h: the registers guest
 * memory cannot simply be memory, routed through the armrec runtime the way
 * Platinum's pc/include/nitro/hw/ARM9/ioreg_{CP,G3,G3X}.h route the 4.2 SDK's
 * (read those for the model):
 *
 *   0x04000280-0x040002B7  maths coprocessor control/results: armrec_cp_ptr()
 *                          computes the result before handing back a pointer
 *   0x04000400-0x040005CB  geometry command FIFO / ports: armrec_gx_port()
 *                          stages the word and commits it on the next access
 *   0x04000600-0x040006A3  geometry status and results: armrec_gx_reg()
 *                          refreshes the block before handing back a pointer
 *
 * Without this, D's decompiled C (FX_cp.c's divider, GX_g3_util.c's matrix
 * multiplies, GX_g3x.c's status polls) wrote plain memory nobody read: the
 * title screen's 3D died on "command 0x20 arrived while 0x1A still wanted 1
 * of its 9 parameters". The recompiled assembly needs none of this; armrec
 * hooks those files' loads and stores itself (ARMREC_*_HOOK).
 *
 * Every hooked name, generated once from the original by address window;
 * the original is included first (by path: its includers use a quote
 * include, which a -I shadow cannot win, so pc_prelude.h includes this and
 * its guard holds), then each name is redefined.
 */
#ifndef PC_DP_REGISTERS_SHADOW_H
#define PC_DP_REGISTERS_SHADOW_H

#include "../../arm9/lib/NitroSDK/include/registers.h"

#include <stdint.h>
void *armrec_cp_ptr(uint32_t addr);
uint32_t *armrec_gx_port(uint32_t addr);
void *armrec_gx_reg(uint32_t addr);

/* coprocessor */
#undef reg_CP_DIVCNT
#define reg_CP_DIVCNT (*(REGType16v *)armrec_cp_ptr(0x04000280u))
#undef reg_CP_DIV_RESULT
#define reg_CP_DIV_RESULT (*(REGType64v *)armrec_cp_ptr(0x040002A0u))
#undef reg_CP_DIV_RESULT_L
#define reg_CP_DIV_RESULT_L (*(REGType32v *)armrec_cp_ptr(0x040002A0u))
#undef reg_CP_DIV_RESULT_H
#define reg_CP_DIV_RESULT_H (*(REGType32v *)armrec_cp_ptr(0x040002A4u))
#undef reg_CP_DIVREM_RESULT
#define reg_CP_DIVREM_RESULT (*(REGType64v *)armrec_cp_ptr(0x040002A8u))
#undef reg_CP_DIVREM_RESULT_L
#define reg_CP_DIVREM_RESULT_L (*(REGType32v *)armrec_cp_ptr(0x040002A8u))
#undef reg_CP_DIVREM_RESULT_H
#define reg_CP_DIVREM_RESULT_H (*(REGType32v *)armrec_cp_ptr(0x040002ACu))
#undef reg_CP_SQRTCNT
#define reg_CP_SQRTCNT (*(REGType16v *)armrec_cp_ptr(0x040002B0u))
#undef reg_CP_SQRT_RESULT
#define reg_CP_SQRT_RESULT (*(REGType32v *)armrec_cp_ptr(0x040002B4u))

/* geometry ports */
#undef reg_G3X_GXFIFO
#define reg_G3X_GXFIFO (*(REGType32v *)armrec_gx_port(0x04000400u))
#undef reg_G3_MTX_MODE
#define reg_G3_MTX_MODE (*(REGType32v *)armrec_gx_port(0x04000440u))
#undef reg_G3_MTX_PUSH
#define reg_G3_MTX_PUSH (*(REGType32v *)armrec_gx_port(0x04000444u))
#undef reg_G3_MTX_POP
#define reg_G3_MTX_POP (*(REGType32v *)armrec_gx_port(0x04000448u))
#undef reg_G3_MTX_STORE
#define reg_G3_MTX_STORE (*(REGType32v *)armrec_gx_port(0x0400044Cu))
#undef reg_G3_MTX_RESTORE
#define reg_G3_MTX_RESTORE (*(REGType32v *)armrec_gx_port(0x04000450u))
#undef reg_G3_MTX_IDENTITY
#define reg_G3_MTX_IDENTITY (*(REGType32v *)armrec_gx_port(0x04000454u))
#undef reg_G3_MTX_LOAD_4x4
#define reg_G3_MTX_LOAD_4x4 (*(REGType32v *)armrec_gx_port(0x04000458u))
#undef reg_G3_MTX_LOAD_4x3
#define reg_G3_MTX_LOAD_4x3 (*(REGType32v *)armrec_gx_port(0x0400045Cu))
#undef reg_G3_MTX_MULT_4x4
#define reg_G3_MTX_MULT_4x4 (*(REGType32v *)armrec_gx_port(0x04000460u))
#undef reg_G3_MTX_MULT_4x3
#define reg_G3_MTX_MULT_4x3 (*(REGType32v *)armrec_gx_port(0x04000464u))
#undef reg_G3_MTX_MULT_3x3
#define reg_G3_MTX_MULT_3x3 (*(REGType32v *)armrec_gx_port(0x04000468u))
#undef reg_G3_MTX_SCALE
#define reg_G3_MTX_SCALE (*(REGType32v *)armrec_gx_port(0x0400046Cu))
#undef reg_G3_MTX_TRANS
#define reg_G3_MTX_TRANS (*(REGType32v *)armrec_gx_port(0x04000470u))
#undef reg_G3_COLOR
#define reg_G3_COLOR (*(REGType32v *)armrec_gx_port(0x04000480u))
#undef reg_G3_NORMAL
#define reg_G3_NORMAL (*(REGType32v *)armrec_gx_port(0x04000484u))
#undef reg_G3_TEXCOORD
#define reg_G3_TEXCOORD (*(REGType32v *)armrec_gx_port(0x04000488u))
#undef reg_G3_VTX_16
#define reg_G3_VTX_16 (*(REGType32v *)armrec_gx_port(0x0400048Cu))
#undef reg_G3_VTX_10
#define reg_G3_VTX_10 (*(REGType32v *)armrec_gx_port(0x04000490u))
#undef reg_G3_VTX_XY
#define reg_G3_VTX_XY (*(REGType32v *)armrec_gx_port(0x04000494u))
#undef reg_G3_VTX_XZ
#define reg_G3_VTX_XZ (*(REGType32v *)armrec_gx_port(0x04000498u))
#undef reg_G3_VTX_YZ
#define reg_G3_VTX_YZ (*(REGType32v *)armrec_gx_port(0x0400049Cu))
#undef reg_G3_VTX_DIFF
#define reg_G3_VTX_DIFF (*(REGType32v *)armrec_gx_port(0x040004A0u))
#undef reg_G3_POLYGON_ATTR
#define reg_G3_POLYGON_ATTR (*(REGType32v *)armrec_gx_port(0x040004A4u))
#undef reg_G3_TEXIMAGE_PARAM
#define reg_G3_TEXIMAGE_PARAM (*(REGType32v *)armrec_gx_port(0x040004A8u))
#undef reg_G3_TEXPLTT_BASE
#define reg_G3_TEXPLTT_BASE (*(REGType32v *)armrec_gx_port(0x040004ACu))
#undef reg_G3_DIF_AMB
#define reg_G3_DIF_AMB (*(REGType32v *)armrec_gx_port(0x040004C0u))
#undef reg_G3_SPE_EMI
#define reg_G3_SPE_EMI (*(REGType32v *)armrec_gx_port(0x040004C4u))
#undef reg_G3_LIGHT_VECTOR
#define reg_G3_LIGHT_VECTOR (*(REGType32v *)armrec_gx_port(0x040004C8u))
#undef reg_G3_LIGHT_COLOR
#define reg_G3_LIGHT_COLOR (*(REGType32v *)armrec_gx_port(0x040004CCu))
#undef reg_G3_SHININESS
#define reg_G3_SHININESS (*(REGType32v *)armrec_gx_port(0x040004D0u))
#undef reg_G3_BEGIN_VTXS
#define reg_G3_BEGIN_VTXS (*(REGType32v *)armrec_gx_port(0x04000500u))
#undef reg_G3_END_VTXS
#define reg_G3_END_VTXS (*(REGType32v *)armrec_gx_port(0x04000504u))
#undef reg_G3_SWAP_BUFFERS
#define reg_G3_SWAP_BUFFERS (*(REGType32v *)armrec_gx_port(0x04000540u))
#undef reg_G3_VIEWPORT
#define reg_G3_VIEWPORT (*(REGType32v *)armrec_gx_port(0x04000580u))
#undef reg_G3_BOX_TEST
#define reg_G3_BOX_TEST (*(REGType32v *)armrec_gx_port(0x040005C0u))
#undef reg_G3_POS_TEST
#define reg_G3_POS_TEST (*(REGType32v *)armrec_gx_port(0x040005C4u))
#undef reg_G3_VEC_TEST
#define reg_G3_VEC_TEST (*(REGType32v *)armrec_gx_port(0x040005C8u))

/* geometry status/results */
#undef reg_G3X_GXSTAT
#define reg_G3X_GXSTAT (*(REGType32v *)armrec_gx_reg(0x04000600u))
#undef reg_G3X_LISTRAM_COUNT
#define reg_G3X_LISTRAM_COUNT (*(REGType16v *)armrec_gx_reg(0x04000604u))
#undef reg_G3X_VTXRAM_COUNT
#define reg_G3X_VTXRAM_COUNT (*(REGType16v *)armrec_gx_reg(0x04000606u))
#undef reg_G3X_DISP_1DOT_DEPTH
#define reg_G3X_DISP_1DOT_DEPTH (*(REGType16v *)armrec_gx_reg(0x04000610u))
#undef reg_G3X_POS_RESULT_X
#define reg_G3X_POS_RESULT_X (*(const REGType32v *)armrec_gx_reg(0x04000620u))
#undef reg_G3X_POS_RESULT_Y
#define reg_G3X_POS_RESULT_Y (*(const REGType32v *)armrec_gx_reg(0x04000624u))
#undef reg_G3X_POS_RESULT_Z
#define reg_G3X_POS_RESULT_Z (*(const REGType32v *)armrec_gx_reg(0x04000628u))
#undef reg_G3X_POS_RESULT_W
#define reg_G3X_POS_RESULT_W (*(const REGType32v *)armrec_gx_reg(0x0400062Cu))
#undef reg_G3X_VEC_RESULT_X
#define reg_G3X_VEC_RESULT_X (*(const REGType16v *)armrec_gx_reg(0x04000630u))
#undef reg_G3X_VEC_RESULT_Y
#define reg_G3X_VEC_RESULT_Y (*(const REGType16v *)armrec_gx_reg(0x04000632u))
#undef reg_G3X_VEC_RESULT_Z
#define reg_G3X_VEC_RESULT_Z (*(const REGType16v *)armrec_gx_reg(0x04000634u))
#undef reg_G3X_CLIPMTX_RESULT_0
#define reg_G3X_CLIPMTX_RESULT_0 (*(const REGType32v *)armrec_gx_reg(0x04000640u))
#undef reg_G3X_CLIPMTX_RESULT_1
#define reg_G3X_CLIPMTX_RESULT_1 (*(const REGType32v *)armrec_gx_reg(0x04000644u))
#undef reg_G3X_CLIPMTX_RESULT_2
#define reg_G3X_CLIPMTX_RESULT_2 (*(const REGType32v *)armrec_gx_reg(0x04000648u))
#undef reg_G3X_CLIPMTX_RESULT_3
#define reg_G3X_CLIPMTX_RESULT_3 (*(const REGType32v *)armrec_gx_reg(0x0400064Cu))
#undef reg_G3X_CLIPMTX_RESULT_4
#define reg_G3X_CLIPMTX_RESULT_4 (*(const REGType32v *)armrec_gx_reg(0x04000650u))
#undef reg_G3X_CLIPMTX_RESULT_5
#define reg_G3X_CLIPMTX_RESULT_5 (*(const REGType32v *)armrec_gx_reg(0x04000654u))
#undef reg_G3X_CLIPMTX_RESULT_6
#define reg_G3X_CLIPMTX_RESULT_6 (*(const REGType32v *)armrec_gx_reg(0x04000658u))
#undef reg_G3X_CLIPMTX_RESULT_7
#define reg_G3X_CLIPMTX_RESULT_7 (*(const REGType32v *)armrec_gx_reg(0x0400065Cu))
#undef reg_G3X_CLIPMTX_RESULT_8
#define reg_G3X_CLIPMTX_RESULT_8 (*(const REGType32v *)armrec_gx_reg(0x04000660u))
#undef reg_G3X_CLIPMTX_RESULT_9
#define reg_G3X_CLIPMTX_RESULT_9 (*(const REGType32v *)armrec_gx_reg(0x04000664u))
#undef reg_G3X_CLIPMTX_RESULT_10
#define reg_G3X_CLIPMTX_RESULT_10 (*(const REGType32v *)armrec_gx_reg(0x04000668u))
#undef reg_G3X_CLIPMTX_RESULT_11
#define reg_G3X_CLIPMTX_RESULT_11 (*(const REGType32v *)armrec_gx_reg(0x0400066Cu))
#undef reg_G3X_CLIPMTX_RESULT_12
#define reg_G3X_CLIPMTX_RESULT_12 (*(const REGType32v *)armrec_gx_reg(0x04000670u))
#undef reg_G3X_CLIPMTX_RESULT_13
#define reg_G3X_CLIPMTX_RESULT_13 (*(const REGType32v *)armrec_gx_reg(0x04000674u))
#undef reg_G3X_CLIPMTX_RESULT_14
#define reg_G3X_CLIPMTX_RESULT_14 (*(const REGType32v *)armrec_gx_reg(0x04000678u))
#undef reg_G3X_CLIPMTX_RESULT_15
#define reg_G3X_CLIPMTX_RESULT_15 (*(const REGType32v *)armrec_gx_reg(0x0400067Cu))
#undef reg_G3X_VECMTX_RESULT_0
#define reg_G3X_VECMTX_RESULT_0 (*(const REGType32v *)armrec_gx_reg(0x04000680u))
#undef reg_G3X_VECMTX_RESULT_1
#define reg_G3X_VECMTX_RESULT_1 (*(const REGType32v *)armrec_gx_reg(0x04000684u))
#undef reg_G3X_VECMTX_RESULT_2
#define reg_G3X_VECMTX_RESULT_2 (*(const REGType32v *)armrec_gx_reg(0x04000688u))
#undef reg_G3X_VECMTX_RESULT_3
#define reg_G3X_VECMTX_RESULT_3 (*(const REGType32v *)armrec_gx_reg(0x0400068Cu))
#undef reg_G3X_VECMTX_RESULT_4
#define reg_G3X_VECMTX_RESULT_4 (*(const REGType32v *)armrec_gx_reg(0x04000690u))
#undef reg_G3X_VECMTX_RESULT_5
#define reg_G3X_VECMTX_RESULT_5 (*(const REGType32v *)armrec_gx_reg(0x04000694u))
#undef reg_G3X_VECMTX_RESULT_6
#define reg_G3X_VECMTX_RESULT_6 (*(const REGType32v *)armrec_gx_reg(0x04000698u))
#undef reg_G3X_VECMTX_RESULT_7
#define reg_G3X_VECMTX_RESULT_7 (*(const REGType32v *)armrec_gx_reg(0x0400069Cu))
#undef reg_G3X_VECMTX_RESULT_8
#define reg_G3X_VECMTX_RESULT_8 (*(const REGType32v *)armrec_gx_reg(0x040006A0u))

#endif
