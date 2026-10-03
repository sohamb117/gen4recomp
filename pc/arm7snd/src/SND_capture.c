#include "SND_capture.h"

#include "registers.h"
#include "code32.h"

enum SNDLoop
{
    SND_CAP_LOOP = 0,
    SND_CAP_ONESHOT = 1
};

void SND_SetupCapture(
    int idx, int format, void *captureData, int size, BOOL loop, int capCtrlSrc, int capCtrlDst)
{
    reg_SNDCAPxCNT(idx) = (u8)((format << 3) | ((loop ? SND_CAP_LOOP : SND_CAP_ONESHOT) << 2) |
                               (capCtrlSrc << 1) | capCtrlDst);
    reg_SNDCAPxDAD(idx) = SND_DMA_ADDR(captureData);
    reg_SNDCAPxLEN(idx) = (u16)size;
}

BOOL SND_IsCaptureActive(int idx)
{
    return (reg_SNDCAPxCNT(idx) & 0x80) != 0;
}
