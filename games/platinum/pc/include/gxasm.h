/* pc shadow of NitroSDK libraries/gx/include/gxasm.h.
 *
 * The SDK's private header declares GX_SendFifo36B and then re-defines it as
 * a mwcc `static inline` wrapper over MI_Copy36B, which gcc rejects
 * ("static declaration follows non-static declaration").  The five gx TUs
 * only need the declarations; leaving GX_SendFifo*B undefined at link time is
 * intentional, the PC runtime supplies them.
 *
 * Found via both include spellings: `"gxasm.h"` hits -Ipc/include directly,
 * and `"../include/gxasm.h"` resolves to pc/include/../include/gxasm.h ==
 * pc/include/gxasm.h because this directory is itself named `include`.
 */
#ifndef NITRO_GXASM_H_
#define NITRO_GXASM_H_

#include <nitro/mi/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

void GX_SendFifo36B(register const void * pSrc, register void * pDest);
void GX_SendFifo48B(register const void * pSrc, register void * pDest);
void GX_SendFifo64B(register const void * pSrc, register void * pDest);
void GX_SendFifo128B(register const void * pSrc, register void * pDest);

#ifdef __cplusplus
}
#endif

#endif
