/*
 * The five SDK helpers that cache a geometry command port's address , 
 * pokediamond's pc_gx_g3_util.c arrangement, applied to this SDK's set.
 *
 * libraries/gx/src/g3_util.c is ordinary C and compiles for the host
 * unmodified. Five of its functions, though, take the address of a command
 * port *once* and then store through the resulting pointer nine or sixteen
 * times:
 *
 *     vs32 *p = (vs32 *)&reg_G3_MTX_MULT_3x3;   // armrec_gx_port(0x4000468)
 *     *p = FX32_ONE;                            // a plain store to a word
 *     *p = 0;                                   // ...and again, to the same one
 *
 * A macro can run code when its address is *taken* and cannot run any when a
 * store through that pointer completes, so the pointer armrec_gx_port()
 * hands back is a staging word committed by the next access, nine stores to
 * it would be one parameter. pc/hw/pc_gpu3d.c aborts by name when a command
 * arrives while the previous one is still short of parameters, which is how
 * a sixth caller would announce itself; these five are supplied here with
 * each store routed through pc_gx_store(), which resolves the pointer back
 * to the port it was armed for and gives each store its own push.
 *
 * The bodies are transcribed from g3_util.c verbatim apart from that one
 * change. These are strong definitions; the SDK's own compiled versions are
 * objcopy --weaken'ed and lose the link tie automatically.
 *
 * The G3i_*W_ projection helpers and the G3BS/G3CS display-list writers in
 * the same SDK file are NOT here: the former send through G3_LoadMtx44
 * (GX_SendFifo64B, pc/src/pc_gx.c's problem, already solved), the latter
 * write ordinary display-list memory.
 */

#include <nitro/gx/g3_util.h>
#include <nitro/gx/g3imm.h>
#include <nitro/fx/fx.h>
#include <nitro/hw/ARM9/ioreg_G3.h>

#include "pc_gpu3d.h"

void G3_RotX(fx32 s, fx32 c)
{
    vs32 *p = (vs32 *)&reg_G3_MTX_MULT_3x3;

    SDK_MINMAX_ASSERT(s, -FX32_ONE, FX32_ONE);
    SDK_MINMAX_ASSERT(c, -FX32_ONE, FX32_ONE);

    pc_gx_store((volatile uint32_t *)p, (uint32_t)FX32_ONE);
    pc_gx_store((volatile uint32_t *)p, 0);
    pc_gx_store((volatile uint32_t *)p, 0);
    pc_gx_store((volatile uint32_t *)p, 0);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)c);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)s);
    pc_gx_store((volatile uint32_t *)p, 0);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)-s);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)c);
}

void G3_RotY(fx32 s, fx32 c)
{
    vs32 *p = (vs32 *)&reg_G3_MTX_MULT_3x3;

    SDK_MINMAX_ASSERT(s, -FX32_ONE, FX32_ONE);
    SDK_MINMAX_ASSERT(c, -FX32_ONE, FX32_ONE);

    pc_gx_store((volatile uint32_t *)p, (uint32_t)c);
    pc_gx_store((volatile uint32_t *)p, 0);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)-s);
    pc_gx_store((volatile uint32_t *)p, 0);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)FX32_ONE);
    pc_gx_store((volatile uint32_t *)p, 0);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)s);
    pc_gx_store((volatile uint32_t *)p, 0);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)c);
}

void G3_RotZ(fx32 s, fx32 c)
{
    vs32 *p = (vs32 *)&reg_G3_MTX_MULT_3x3;

    SDK_MINMAX_ASSERT(s, -FX32_ONE, FX32_ONE);
    SDK_MINMAX_ASSERT(c, -FX32_ONE, FX32_ONE);

    pc_gx_store((volatile uint32_t *)p, (uint32_t)c);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)s);
    pc_gx_store((volatile uint32_t *)p, 0);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)-s);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)c);
    pc_gx_store((volatile uint32_t *)p, 0);
    pc_gx_store((volatile uint32_t *)p, 0);
    pc_gx_store((volatile uint32_t *)p, 0);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)FX32_ONE);
}

void G3_LoadTexMtxTexCoord(const MtxFx44 *mtx)
{
    vs32 *p = (vs32 *)&reg_G3_MTX_LOAD_4x4;
    SDK_NULL_ASSERT(mtx);
    G3_MtxMode(GX_MTXMODE_TEXTURE);

    pc_gx_store((volatile uint32_t *)p, (uint32_t)mtx->_00);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)mtx->_01);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)mtx->_02);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)mtx->_03);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)mtx->_10);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)mtx->_11);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)mtx->_12);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)mtx->_13);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_20 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_21 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_22 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_23 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_30 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_31 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_32 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_33 << 4));
}

void G3_LoadTexMtxEnv(const MtxFx44 *mtx)
{
    vs32 *p = (vs32 *)&reg_G3_MTX_LOAD_4x4;
    SDK_NULL_ASSERT(mtx);
    G3_MtxMode(GX_MTXMODE_TEXTURE);

    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_00 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_01 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_02 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_03 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_10 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_11 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_12 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_13 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_20 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_21 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_22 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)(mtx->_23 << 4));
    pc_gx_store((volatile uint32_t *)p, (uint32_t)mtx->_30);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)mtx->_31);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)mtx->_32);
    pc_gx_store((volatile uint32_t *)p, (uint32_t)mtx->_33);
}
