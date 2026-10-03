/*
 * PC port shadow of the SDK's ioreg_G3.h, the geometry engine's per-command
 * ports, 0x04000440-0x040005CB.
 *
 * Each of these addresses is a FIFO on hardware: every store pushes a
 * command or a parameter, so the last one must not simply win, which is all
 * identity-mapped memory can offer. armrec_gx_port() (tools/armrec/
 * armrec_rt.c, shared verbatim with pokediamond) hands back a *staging* word
 * instead of the register, and the command is committed by the next access
 * of any kind; the value cannot be read before the store that produces it,
 * and nothing in the tree reads one of these back (they are write-only on
 * hardware too). pc/hw/pc_gpu3d.c is the engine behind it; armrec_rt.c
 * carries weak no-ops so a build without the engine still links, in which
 * case these macros degrade to plain memory.
 *
 * What this cannot do, and it is loud rather than silent: a caller that
 * takes a port's address once and stores through the pointer repeatedly
 * stages one word and loses the rest. pc/hw/pc_gpu3d.c aborts by name when
 * the next command arrives with the previous one still short of parameters.
 * The callers that do this in the SDK (G3_RotX/y/z, G3_LoadTexMtxTexCoord,
 * G3_LoadTexMtxEnv; all in libraries/gx/src/g3_util.c) are supplied as
 * strong overrides by pc/src/pc_gx_g3_util.c instead; the abort stands as
 * the backstop for a caller nobody has met yet.
 */

#ifndef PC_SHADOW_IOREG_G3_H
#define PC_SHADOW_IOREG_G3_H

#include_next <nitro/hw/ARM9/ioreg_G3.h>

#ifndef SDK_ASM

#include <stdint.h>

uint32_t *armrec_gx_port(uint32_t addr);

#undef reg_G3_MTX_MODE
#undef reg_G3_MTX_PUSH
#undef reg_G3_MTX_POP
#undef reg_G3_MTX_STORE
#undef reg_G3_MTX_RESTORE
#undef reg_G3_MTX_IDENTITY
#undef reg_G3_MTX_LOAD_4x4
#undef reg_G3_MTX_LOAD_4x3
#undef reg_G3_MTX_MULT_4x4
#undef reg_G3_MTX_MULT_4x3
#undef reg_G3_MTX_MULT_3x3
#undef reg_G3_MTX_SCALE
#undef reg_G3_MTX_TRANS
#undef reg_G3_COLOR
#undef reg_G3_NORMAL
#undef reg_G3_TEXCOORD
#undef reg_G3_VTX_16
#undef reg_G3_VTX_10
#undef reg_G3_VTX_XY
#undef reg_G3_VTX_XZ
#undef reg_G3_VTX_YZ
#undef reg_G3_VTX_DIFF
#undef reg_G3_POLYGON_ATTR
#undef reg_G3_TEXIMAGE_PARAM
#undef reg_G3_TEXPLTT_BASE
#undef reg_G3_DIF_AMB
#undef reg_G3_SPE_EMI
#undef reg_G3_LIGHT_VECTOR
#undef reg_G3_LIGHT_COLOR
#undef reg_G3_SHININESS
#undef reg_G3_BEGIN_VTXS
#undef reg_G3_END_VTXS
#undef reg_G3_SWAP_BUFFERS
#undef reg_G3_VIEWPORT
#undef reg_G3_BOX_TEST
#undef reg_G3_POS_TEST
#undef reg_G3_VEC_TEST

#define reg_G3_MTX_MODE        (*(REGType32v *)armrec_gx_port(REG_MTX_MODE_ADDR))
#define reg_G3_MTX_PUSH        (*(REGType32v *)armrec_gx_port(REG_MTX_PUSH_ADDR))
#define reg_G3_MTX_POP         (*(REGType32v *)armrec_gx_port(REG_MTX_POP_ADDR))
#define reg_G3_MTX_STORE       (*(REGType32v *)armrec_gx_port(REG_MTX_STORE_ADDR))
#define reg_G3_MTX_RESTORE     (*(REGType32v *)armrec_gx_port(REG_MTX_RESTORE_ADDR))
#define reg_G3_MTX_IDENTITY    (*(REGType32v *)armrec_gx_port(REG_MTX_IDENTITY_ADDR))
#define reg_G3_MTX_LOAD_4x4    (*(REGType32v *)armrec_gx_port(REG_MTX_LOAD_4x4_ADDR))
#define reg_G3_MTX_LOAD_4x3    (*(REGType32v *)armrec_gx_port(REG_MTX_LOAD_4x3_ADDR))
#define reg_G3_MTX_MULT_4x4    (*(REGType32v *)armrec_gx_port(REG_MTX_MULT_4x4_ADDR))
#define reg_G3_MTX_MULT_4x3    (*(REGType32v *)armrec_gx_port(REG_MTX_MULT_4x3_ADDR))
#define reg_G3_MTX_MULT_3x3    (*(REGType32v *)armrec_gx_port(REG_MTX_MULT_3x3_ADDR))
#define reg_G3_MTX_SCALE       (*(REGType32v *)armrec_gx_port(REG_MTX_SCALE_ADDR))
#define reg_G3_MTX_TRANS       (*(REGType32v *)armrec_gx_port(REG_MTX_TRANS_ADDR))
#define reg_G3_COLOR           (*(REGType32v *)armrec_gx_port(REG_COLOR_ADDR))
#define reg_G3_NORMAL          (*(REGType32v *)armrec_gx_port(REG_NORMAL_ADDR))
#define reg_G3_TEXCOORD        (*(REGType32v *)armrec_gx_port(REG_TEXCOORD_ADDR))
#define reg_G3_VTX_16          (*(REGType32v *)armrec_gx_port(REG_VTX_16_ADDR))
#define reg_G3_VTX_10          (*(REGType32v *)armrec_gx_port(REG_VTX_10_ADDR))
#define reg_G3_VTX_XY          (*(REGType32v *)armrec_gx_port(REG_VTX_XY_ADDR))
#define reg_G3_VTX_XZ          (*(REGType32v *)armrec_gx_port(REG_VTX_XZ_ADDR))
#define reg_G3_VTX_YZ          (*(REGType32v *)armrec_gx_port(REG_VTX_YZ_ADDR))
#define reg_G3_VTX_DIFF        (*(REGType32v *)armrec_gx_port(REG_VTX_DIFF_ADDR))
#define reg_G3_POLYGON_ATTR    (*(REGType32v *)armrec_gx_port(REG_POLYGON_ATTR_ADDR))
#define reg_G3_TEXIMAGE_PARAM  (*(REGType32v *)armrec_gx_port(REG_TEXIMAGE_PARAM_ADDR))
#define reg_G3_TEXPLTT_BASE    (*(REGType32v *)armrec_gx_port(REG_TEXPLTT_BASE_ADDR))
#define reg_G3_DIF_AMB         (*(REGType32v *)armrec_gx_port(REG_DIF_AMB_ADDR))
#define reg_G3_SPE_EMI         (*(REGType32v *)armrec_gx_port(REG_SPE_EMI_ADDR))
#define reg_G3_LIGHT_VECTOR    (*(REGType32v *)armrec_gx_port(REG_LIGHT_VECTOR_ADDR))
#define reg_G3_LIGHT_COLOR     (*(REGType32v *)armrec_gx_port(REG_LIGHT_COLOR_ADDR))
#define reg_G3_SHININESS       (*(REGType32v *)armrec_gx_port(REG_SHININESS_ADDR))
#define reg_G3_BEGIN_VTXS      (*(REGType32v *)armrec_gx_port(REG_BEGIN_VTXS_ADDR))
#define reg_G3_END_VTXS        (*(REGType32v *)armrec_gx_port(REG_END_VTXS_ADDR))
#define reg_G3_SWAP_BUFFERS    (*(REGType32v *)armrec_gx_port(REG_SWAP_BUFFERS_ADDR))
#define reg_G3_VIEWPORT        (*(REGType32v *)armrec_gx_port(REG_VIEWPORT_ADDR))
#define reg_G3_BOX_TEST        (*(REGType32v *)armrec_gx_port(REG_BOX_TEST_ADDR))
#define reg_G3_POS_TEST        (*(REGType32v *)armrec_gx_port(REG_POS_TEST_ADDR))
#define reg_G3_VEC_TEST        (*(REGType32v *)armrec_gx_port(REG_VEC_TEST_ADDR))

#endif /* SDK_ASM */

#endif /* PC_SHADOW_IOREG_G3_H */
