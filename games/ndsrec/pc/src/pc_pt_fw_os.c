/*
 * The few OS services the Download Play client's SDK code needs
 * (pc/src/pc_pt_dlplay.c: NitroSDK 4.2's ARM9 WM library and multiboot
 * child, compiled for the Poké Transfer child's station). It runs before
 * the child program, the firmware's place, so none of the child's own OS
 * is up yet, and none of its functions has a name to link against.
 *
 *   message queues    wm_system.c keeps its request buffers in one. The
 *                     firmware phase is a single thread, so a send or
 *                     receive that would have to sleep can never be woken;
 *                     it stops the run instead (NitroSDK os_message.c is
 *                     what this follows otherwise).
 *   RTC_GetTime       WM_GetNextTgid seeds the tgid from the clock
 *                     (pc_rtc.c's time, the one the guest's RTC reads).
 *   MATH_CountPopulation  for WM_SetMPDataToPort's destination count.
 *   OS_Terminate      wm_system.c's answer to an ARM7 it cannot talk to.
 */
#include <nitro.h>
#include <nitro/rtc.h>
#include "mb_private.h"

extern void pc_trap_unreached(const char *sym, const char *why) __attribute__((noreturn));
extern void pc_rtc_now_fields(int *y, int *mo, int *d, int *h, int *mi, int *s);

void OS_InitMessageQueue(OSMessageQueue *mq, OSMessage *msgArray, s32 msgCount)
{
    MI_CpuClear8(mq, sizeof *mq);
    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
}

BOOL OS_SendMessage(OSMessageQueue *mq, OSMessage msg, s32 flags)
{
    OSIntrMode enabled = OS_DisableInterrupts();

    if (mq->usedCount >= mq->msgCount) {
        (void)OS_RestoreInterrupts(enabled);
        if (flags & OS_MESSAGE_BLOCK)
            pc_trap_unreached("OS_SendMessage", "the Download Play client's queue is full and nothing can drain it");
        return FALSE;
    }
    mq->msgArray[(mq->firstIndex + mq->usedCount) % mq->msgCount] = msg;
    mq->usedCount++;
    (void)OS_RestoreInterrupts(enabled);
    return TRUE;
}

BOOL OS_JamMessage(OSMessageQueue *mq, OSMessage msg, s32 flags)
{
    OSIntrMode enabled = OS_DisableInterrupts();

    if (mq->usedCount >= mq->msgCount) {
        (void)OS_RestoreInterrupts(enabled);
        if (flags & OS_MESSAGE_BLOCK)
            pc_trap_unreached("OS_JamMessage", "the Download Play client's queue is full and nothing can drain it");
        return FALSE;
    }
    mq->firstIndex = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
    mq->msgArray[mq->firstIndex] = msg;
    mq->usedCount++;
    (void)OS_RestoreInterrupts(enabled);
    return TRUE;
}

BOOL OS_ReceiveMessage(OSMessageQueue *mq, OSMessage *msg, s32 flags)
{
    OSIntrMode enabled = OS_DisableInterrupts();

    if (mq->usedCount == 0) {
        (void)OS_RestoreInterrupts(enabled);
        if (flags & OS_MESSAGE_BLOCK)
            pc_trap_unreached("OS_ReceiveMessage", "the Download Play client's queue is empty and nothing can fill it");
        return FALSE;
    }
    if (msg != NULL)
        *msg = mq->msgArray[mq->firstIndex];
    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    mq->usedCount--;
    (void)OS_RestoreInterrupts(enabled);
    return TRUE;
}

void RTC_Init(void)
{
}

RTCResult RTC_GetTime(RTCTime *time)
{
    int y, mo, d, h, mi, s;

    pc_rtc_now_fields(&y, &mo, &d, &h, &mi, &s);
    time->hour = (u32)h;
    time->minute = (u32)mi;
    time->second = (u32)s;
    return RTC_RESULT_SUCCESS;
}

u8 MATH_CountPopulation(u32 x)
{
    return (u8)__builtin_popcount(x);
}

void OS_Terminate(void)
{
    pc_trap_unreached("OS_Terminate", "the Download Play client's WM library gave up on the ARM7");
}

/* The multiboot library's task thread (libraries/mb/src/mb_task.c) is the
 * parent's: only MB_RegisterFile's file-information loader starts it, so on a
 * child it never exists and MBi_CommEnd always takes its MBi_CallReset branch.
 * These two answer for mb_task.c, which would need the OS thread library this
 * phase does not have, as it would answer on a child. */
BOOL MBi_IsTaskAvailable(void)
{
    return FALSE;
}

void MBi_EndTaskThread(MB_TASK_FUNC callback)
{
    (void)callback;
    pc_trap_unreached("MBi_EndTaskThread", "a multiboot child has no task thread to end");
}
