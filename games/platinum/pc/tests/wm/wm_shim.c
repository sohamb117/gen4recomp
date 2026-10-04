/*
 * The handful of OS/MI/DC services the NitroSDK ARM9 WM library needs, for
 * the WM model's unit test (test_wm.c). The test links the real ARM9 WM
 * sources and the real pc_pxi.c with pc_wm.c into a wasm32 module, so the
 * structures, pointers and command encodings are exactly the guest's; these
 * stand-ins are the port's own semantics in miniature (one thread, cache
 * maintenance is a no-op, interrupts are a flag).
 */
#include <nitro/os.h>
#include <nitro/rtc.h>
#include "pc_np_options.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

pc_np_status pc_np_stat;

void DC_InvalidateRange(void *start, u32 n) { (void)start; (void)n; }
void DC_StoreRange(const void *start, u32 n) { (void)start; (void)n; }
void DC_FlushRange(const void *start, u32 n) { (void)start; (void)n; }

void MI_CpuCopy8(const void *src, void *dst, u32 size) { memmove(dst, src, size); }
void MI_CpuFill8(void *dst, u8 data, u32 size) { memset(dst, data, size); }
void MIi_CpuCopy16(const void *src, void *dst, u32 size) { memmove(dst, src, size); }
void MIi_CpuCopy32(const void *src, void *dst, u32 size) { memmove(dst, src, size); }
void MIi_CpuCopyFast(const void *src, void *dst, u32 size) { memmove(dst, src, size); }
void MIi_CpuClear16(u16 data, void *dst, u32 size)
{
    u16 *p = dst;
    u32 i;

    for (i = 0; i < size / 2; i++) {
        p[i] = data;
    }
}
void MIi_CpuClear32(u32 data, void *dst, u32 size)
{
    u32 *p = dst;
    u32 i;

    for (i = 0; i < size / 4; i++) {
        p[i] = data;
    }
}
void MI_DmaFill32(u32 dmaNo, void *dst, u32 data, u32 size)
{
    (void)dmaNo;
    MIi_CpuClear32(data, dst, size);
}

static OSIntrMode sIntr = 1;
OSIntrMode OS_DisableInterrupts(void)
{
    OSIntrMode old = sIntr;

    sIntr = 0;
    return old;
}
OSIntrMode OS_RestoreInterrupts(OSIntrMode state)
{
    OSIntrMode old = sIntr;

    sIntr = state;
    return old;
}

void OS_InitMessageQueue(OSMessageQueue *mq, OSMessage *msgArray, s32 msgCount)
{
    memset(mq, 0, sizeof *mq);
    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
}
BOOL OS_SendMessage(OSMessageQueue *mq, OSMessage msg, s32 flags)
{
    (void)flags;
    if (mq->usedCount >= mq->msgCount) {
        return FALSE;
    }
    mq->msgArray[(mq->firstIndex + mq->usedCount) % mq->msgCount] = msg;
    mq->usedCount++;
    return TRUE;
}
BOOL OS_JamMessage(OSMessageQueue *mq, OSMessage msg, s32 flags)
{
    (void)flags;
    if (mq->usedCount >= mq->msgCount) {
        return FALSE;
    }
    mq->firstIndex = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
    mq->msgArray[mq->firstIndex] = msg;
    mq->usedCount++;
    return TRUE;
}
BOOL OS_ReceiveMessage(OSMessageQueue *mq, OSMessage *msg, s32 flags)
{
    (void)flags;
    if (mq->usedCount == 0) {
        return FALSE;
    }
    if (msg != NULL) {
        *msg = mq->msgArray[mq->firstIndex];
    }
    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    mq->usedCount--;
    return TRUE;
}

void OS_Terminate(void)
{
    fprintf(stderr, "test_wm: OS_Terminate\n");
    abort();
}

void RTC_Init(void) {}
RTCResult RTC_GetTime(RTCTime *time)
{
    memset(time, 0, sizeof *time);
    return RTC_RESULT_SUCCESS;
}
