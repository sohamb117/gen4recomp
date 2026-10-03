/*
 * Host implementations of the NitroSDK graphics routines that ship as mwcc
 * assembly in this tree:
 *
 *   libraries/gx/src/gxasm.c    GX_SendFifo48B / 64B / 128B
 *   libraries/gx/src/g3x.c      GXi_NopClearFifo128_ (static asm; strip_asm
 *                               leaves the call, this global satisfies it)
 *   libraries/gx/include/gxasm.h  GX_SendFifo36B (an inline MI_Copy36B in the
 *                               SDK header; pc/include/gxasm.h keeps only the
 *                               declaration and this file supplies it)
 *
 * All of them push words at the geometry command FIFO, reg_G3X_GXFIFO at
 * 0x04000400. They are assembly on hardware because what matters is the
 * *sequence of stores*, `stmia rD, {...}` without writeback writes the same
 * window every time, and on hardware each store pushes another FIFO entry.
 * pc_gx_store() (pc/include/pc_gpu3d.h) reproduces that: a store whose
 * destination resolves to the geometry engine's staging word or command
 * window becomes a FIFO push; any other destination is a plain store, which
 * is what a test driving these with ordinary buffers observes.
 *
 * The multi-word bursts (stmia {r2-r8,r12}) store to CONSECUTIVE addresses
 * from the base, all inside the FIFO mirror 0x04000400-0x0400043F, where
 * every word is a push regardless of offset, so dst[0..7] here is exact,
 * not an approximation.
 *
 * These are strong definitions; the SDK objects are --weaken'ed, and the
 * five names were removed from pc/stubs.list when this file landed.
 */

#include "armrec_rt.h"
#include "pc_gpu3d.h"

#include <stdint.h>

typedef volatile uint32_t pc_vu32;

/*
 * ldmia r0!, {r2,r3,r12} / stmia r1, {r2,r3,r12}, four times: 12 words from
 * an advancing source into a fixed 3-word window. Callers: G3_LoadMtx43,
 * G3_MultMtx43 (4x3 matrices).
 */
void GX_SendFifo48B(const void *pSrc, void *pDest) {
    const uint32_t *src = (const uint32_t *)pSrc;
    pc_vu32 *dst = (pc_vu32 *)pDest;
    int i;

    for (i = 0; i < 4; i++) {
        pc_gx_store(&dst[0], src[0]);
        pc_gx_store(&dst[1], src[1]);
        pc_gx_store(&dst[2], src[2]);
        src += 3;
    }
}

/*
 * ldmia r0!, {r2-r8,r12} / stmia r1, {...}, twice: 16 words in 8-word
 * bursts. Callers: G3_LoadMtx44, G3_MultMtx44 (4x4 matrices).
 */
void GX_SendFifo64B(const void *pSrc, void *pDest) {
    const uint32_t *src = (const uint32_t *)pSrc;
    pc_vu32 *dst = (pc_vu32 *)pDest;
    int i, j;

    for (i = 0; i < 2; i++) {
        for (j = 0; j < 8; j++) {
            pc_gx_store(&dst[j], src[j]);
        }
        src += 8;
    }
}

/*
 * 32 words in four 8-word bursts. Caller: G3_LoadShininessTable.
 */
void GX_SendFifo128B(const void *pSrc, void *pDest) {
    const uint32_t *src = (const uint32_t *)pSrc;
    pc_vu32 *dst = (pc_vu32 *)pDest;
    int i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 8; j++) {
            pc_gx_store(&dst[j], src[j]);
        }
        src += 8;
    }
}

/*
 * The SDK header's inline is MI_Copy36B(pSrc, pDest): 9 words to consecutive
 * addresses inside the FIFO mirror. Callers: G3_MultMtx33, G3_LoadMtx33.
 */
void GX_SendFifo36B(const void *pSrc, void *pDest) {
    const uint32_t *src = (const uint32_t *)pSrc;
    pc_vu32 *dst = (pc_vu32 *)pDest;
    int i;

    for (i = 0; i < 9; i++) {
        pc_gx_store(&dst[i], src[i]);
    }
}

/*
 * mov r1-r3,r12 = 0 then stmia r0, {r1-r3,r12} thirty-two times: 128 zero
 * words. Sending 128 NOP commands (opcode 0) is how the SDK drains the
 * geometry FIFO, which G3X_ClearFifo() does at init before waiting on the
 * busy bit.
 */
void GXi_NopClearFifo128_(void *reg) {
    pc_vu32 *dst = (pc_vu32 *)reg;
    int i;

    for (i = 0; i < 32; i++) {
        pc_gx_store(&dst[0], 0);
        pc_gx_store(&dst[1], 0);
        pc_gx_store(&dst[2], 0);
        pc_gx_store(&dst[3], 0);
    }
}
