/* C reimplementations of the fx library's matrix kernels, which the SDK ships
 * only as mwcc `asm void` bodies inside
 * subprojects/NitroSDK-4.2.30001/libraries/fx/src/fx_mtx{22,33,43,44}.c.
 * The public entry points (MTX_Identity44, MTX_RotX43, ...) are SDK_INLINEs in
 * include/nitro/fx/fx_mtx*.h that assert and then tail into these underscore
 * twins, so the link needs real definitions for them.
 *
 * Matrices are row-major fx32 (1.19.12, FX32_ONE == 0x1000) and the SDK's
 * convention is row vectors: v' = v * M. That puts the rotations at
 *
 *   RotX: _11 =  cos  _12 =  sin        RotY: _00 =  cos  _02 = -sin
 *         _21 = -sin  _22 =  cos              _20 =  sin  _22 =  cos
 *
 *   RotZ / Rot22: _00 =  cos  _01 =  sin
 *                 _10 = -sin  _11 =  cos
 *
 * i.e. +sin sits above the diagonal for X and Z but below it for Y. Every
 * assignment below was transcribed store-by-store from the corresponding asm
 * body (the str/stmia offsets name the element uniquely), so the sign
 * placement is the SDK's own, not a rederivation. */

#include <nitro/fx/fx.h>
#include <nitro/fx/fx_mtx22.h>
#include <nitro/fx/fx_mtx33.h>
#include <nitro/fx/fx_mtx43.h>
#include <nitro/fx/fx_mtx44.h>

/* ------------------------------------------------------------------ */
/* 2x2 (fx_mtx22.c)                                                   */
/* ------------------------------------------------------------------ */

void MTX_Identity22_(MtxFx22 *pDst)
{
    pDst->_00 = FX32_ONE;
    pDst->_01 = 0;
    pDst->_10 = 0;
    pDst->_11 = FX32_ONE;
}

void MTX_Rot22_(MtxFx22 *pDst, fx32 sinVal, fx32 cosVal)
{
    pDst->_00 = cosVal;
    pDst->_01 = sinVal;
    pDst->_10 = -sinVal;
    pDst->_11 = cosVal;
}

/* ------------------------------------------------------------------ */
/* 3x3 (fx_mtx33.c)                                                   */
/* ------------------------------------------------------------------ */

void MTX_Identity33_(MtxFx33 *pDst)
{
    pDst->_00 = FX32_ONE;
    pDst->_01 = 0;
    pDst->_02 = 0;
    pDst->_10 = 0;
    pDst->_11 = FX32_ONE;
    pDst->_12 = 0;
    pDst->_20 = 0;
    pDst->_21 = 0;
    pDst->_22 = FX32_ONE;
}

void MTX_RotX33_(MtxFx33 *pDst, fx32 sinVal, fx32 cosVal)
{
    pDst->_00 = FX32_ONE;
    pDst->_01 = 0;
    pDst->_02 = 0;
    pDst->_10 = 0;
    pDst->_11 = cosVal;
    pDst->_12 = sinVal;
    pDst->_20 = 0;
    pDst->_21 = -sinVal;
    pDst->_22 = cosVal;
}

void MTX_RotY33_(MtxFx33 *pDst, fx32 sinVal, fx32 cosVal)
{
    pDst->_00 = cosVal;
    pDst->_01 = 0;
    pDst->_02 = -sinVal;
    pDst->_10 = 0;
    pDst->_11 = FX32_ONE;
    pDst->_12 = 0;
    pDst->_20 = sinVal;
    pDst->_21 = 0;
    pDst->_22 = cosVal;
}

void MTX_RotZ33_(MtxFx33 *pDst, fx32 sinVal, fx32 cosVal)
{
    pDst->_00 = cosVal;
    pDst->_01 = sinVal;
    pDst->_02 = 0;
    pDst->_10 = -sinVal;
    pDst->_11 = cosVal;
    pDst->_12 = 0;
    pDst->_20 = 0;
    pDst->_21 = 0;
    pDst->_22 = FX32_ONE;
}

/* ------------------------------------------------------------------ */
/* 4x3: three basis rows plus a translation row (fx_mtx43.c)          */
/* ------------------------------------------------------------------ */

void MTX_Identity43_(MtxFx43 *pDst)
{
    pDst->_00 = FX32_ONE;
    pDst->_01 = 0;
    pDst->_02 = 0;
    pDst->_10 = 0;
    pDst->_11 = FX32_ONE;
    pDst->_12 = 0;
    pDst->_20 = 0;
    pDst->_21 = 0;
    pDst->_22 = FX32_ONE;
    pDst->_30 = 0;
    pDst->_31 = 0;
    pDst->_32 = 0;
}

/* Widen: each row gains a fourth column of 0, the translation row gains 1. */
void MTX_Copy43To44_(const MtxFx43 *pSrc, MtxFx44 *pDst)
{
    int r;

    for (r = 0; r < 4; r++) {
        pDst->m[r][0] = pSrc->m[r][0];
        pDst->m[r][1] = pSrc->m[r][1];
        pDst->m[r][2] = pSrc->m[r][2];
        pDst->m[r][3] = (r == 3) ? FX32_ONE : 0;
    }
}

void MTX_Scale43_(MtxFx43 *pDst, fx32 x, fx32 y, fx32 z)
{
    pDst->_00 = x;
    pDst->_01 = 0;
    pDst->_02 = 0;
    pDst->_10 = 0;
    pDst->_11 = y;
    pDst->_12 = 0;
    pDst->_20 = 0;
    pDst->_21 = 0;
    pDst->_22 = z;
    pDst->_30 = 0;
    pDst->_31 = 0;
    pDst->_32 = 0;
}

void MTX_RotX43_(MtxFx43 *pDst, fx32 sinVal, fx32 cosVal)
{
    pDst->_00 = FX32_ONE;
    pDst->_01 = 0;
    pDst->_02 = 0;
    pDst->_10 = 0;
    pDst->_11 = cosVal;
    pDst->_12 = sinVal;
    pDst->_20 = 0;
    pDst->_21 = -sinVal;
    pDst->_22 = cosVal;
    pDst->_30 = 0;
    pDst->_31 = 0;
    pDst->_32 = 0;
}

void MTX_RotY43_(MtxFx43 *pDst, fx32 sinVal, fx32 cosVal)
{
    pDst->_00 = cosVal;
    pDst->_01 = 0;
    pDst->_02 = -sinVal;
    pDst->_10 = 0;
    pDst->_11 = FX32_ONE;
    pDst->_12 = 0;
    pDst->_20 = sinVal;
    pDst->_21 = 0;
    pDst->_22 = cosVal;
    pDst->_30 = 0;
    pDst->_31 = 0;
    pDst->_32 = 0;
}

/* ------------------------------------------------------------------ */
/* 4x4 (fx_mtx44.c)                                                   */
/* ------------------------------------------------------------------ */

void MTX_Identity44_(MtxFx44 *pDst)
{
    int i;

    for (i = 0; i < 16; i++) {
        pDst->a[i] = 0;
    }

    pDst->_00 = FX32_ONE;
    pDst->_11 = FX32_ONE;
    pDst->_22 = FX32_ONE;
    pDst->_33 = FX32_ONE;
}

/* Narrow: drop the fourth column of every row. */
void MTX_Copy44To43_(const MtxFx44 *pSrc, MtxFx43 *pDst)
{
    int r;

    for (r = 0; r < 4; r++) {
        pDst->m[r][0] = pSrc->m[r][0];
        pDst->m[r][1] = pSrc->m[r][1];
        pDst->m[r][2] = pSrc->m[r][2];
    }
}

void MTX_RotX44_(MtxFx44 *pDst, fx32 sinVal, fx32 cosVal)
{
    pDst->_00 = FX32_ONE;
    pDst->_01 = 0;
    pDst->_02 = 0;
    pDst->_03 = 0;
    pDst->_10 = 0;
    pDst->_11 = cosVal;
    pDst->_12 = sinVal;
    pDst->_13 = 0;
    pDst->_20 = 0;
    pDst->_21 = -sinVal;
    pDst->_22 = cosVal;
    pDst->_23 = 0;
    pDst->_30 = 0;
    pDst->_31 = 0;
    pDst->_32 = 0;
    pDst->_33 = FX32_ONE;
}

void MTX_RotY44_(MtxFx44 *pDst, fx32 sinVal, fx32 cosVal)
{
    pDst->_00 = cosVal;
    pDst->_01 = 0;
    pDst->_02 = -sinVal;
    pDst->_03 = 0;
    pDst->_10 = 0;
    pDst->_11 = FX32_ONE;
    pDst->_12 = 0;
    pDst->_13 = 0;
    pDst->_20 = sinVal;
    pDst->_21 = 0;
    pDst->_22 = cosVal;
    pDst->_23 = 0;
    pDst->_30 = 0;
    pDst->_31 = 0;
    pDst->_32 = 0;
    pDst->_33 = FX32_ONE;
}

void MTX_RotZ44_(MtxFx44 *pDst, fx32 sinVal, fx32 cosVal)
{
    pDst->_00 = cosVal;
    pDst->_01 = sinVal;
    pDst->_02 = 0;
    pDst->_03 = 0;
    pDst->_10 = -sinVal;
    pDst->_11 = cosVal;
    pDst->_12 = 0;
    pDst->_13 = 0;
    pDst->_20 = 0;
    pDst->_21 = 0;
    pDst->_22 = FX32_ONE;
    pDst->_23 = 0;
    pDst->_30 = 0;
    pDst->_31 = 0;
    pDst->_32 = 0;
    pDst->_33 = FX32_ONE;
}
