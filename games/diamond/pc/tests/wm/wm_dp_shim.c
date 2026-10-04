/*
 * What D's recompiled ARM9 WM library needs around it, for the WM model's
 * D/P unit test (test_wm_dp.c).
 *
 * armrec's output (pc/mk/armrec.mk) calls through a small runtime: one
 * guest stack pointer, a table from guest code addresses to recompiled
 * functions, static data copied to its guest addresses, and a `c2u$NAME`
 * adapter for every call that leaves recompiled code. In the module those
 * come from tools/armrec/armrec_rt.c and the bridge (pc/mk/bridge.mk); here
 * they are the same contract in miniature, with the OS/MI/DC services the
 * library calls given the port's semantics (one thread, cache maintenance is
 * a no-op, interrupts are a flag) and PXI going to the real pc_pxi.c, whose
 * tag-10 responder is pc_wm.c.
 */
#include <nitro/os.h>
#include <nitro/pxi.h>
#include "armrec_rt.h"
#include "pc_np_options.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

pc_np_status pc_np_stat;

/* ---- the armrec runtime ---- */

uint32_t armrec_sp = 0x023E0000u; /* ARM_STACK_TOP: the top of main RAM */

static struct {
    uint32_t addr;
    armrec_fn fn;
} sFns[128];
static int sFnCount;

void armrec_register(uint32_t addr, armrec_fn fn, const char *name)
{
    (void)name;
    if (sFnCount >= (int)(sizeof sFns / sizeof sFns[0])) {
        fprintf(stderr, "wm_dp_shim: function table full at %s\n", name);
        abort();
    }
    sFns[sFnCount].addr = addr;
    sFns[sFnCount].fn = fn;
    sFnCount++;
}

void armrec_load_data(uint32_t addr, const void *src, uint32_t len)
{
    memcpy((void *)(uintptr_t)addr, src, len);
}

/* A guest code address is a recompiled function; a value below
 * ARMREC_WASM_FNPTR_END is a C function pointer (a table index), which is
 * what the test hands the library as callbacks. Every WM callback takes one
 * pointer. */
uint64_t armrec_dispatch(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    int i;

    if (addr != 0 && addr < ARMREC_WASM_FNPTR_END) {
        ((void (*)(void *))(uintptr_t)addr)((void *)(uintptr_t)a0);
        return 0;
    }
    for (i = 0; i < sFnCount; i++) {
        if (sFns[i].addr == (addr & ~1u)) {
            return sFns[i].fn(a0, a1, a2, a3);
        }
    }
    fprintf(stderr, "wm_dp_shim: call to unknown code address %#x\n", (unsigned)addr);
    abort();
}

#define P(x) ((void *)(uintptr_t)(x))
#define ADAPTER(name) uint64_t c2u$##name(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)

/* ---- DC / MI ---- */

ADAPTER(DC_InvalidateRange) { return 0; }
ADAPTER(DC_StoreRange) { return 0; }
ADAPTER(MI_CpuCopy8) { memmove(P(a1), P(a0), a2); return 0; }
ADAPTER(MI_CpuFill8) { memset(P(a0), (int)(a1 & 0xff), a2); return 0; }
ADAPTER(MIi_CpuCopy16) { memmove(P(a1), P(a0), a2 & ~1u); return 0; }
ADAPTER(MIi_CpuCopy32) { memmove(P(a1), P(a0), a2 & ~3u); return 0; }
ADAPTER(MIi_CpuCopyFast) { memmove(P(a1), P(a0), a2 & ~3u); return 0; }
ADAPTER(MIi_CpuClear16)
{
    u16 *p = P(a1);
    u32 i;

    for (i = 0; i < a2 / 2; i++) {
        p[i] = (u16)a0;
    }
    return 0;
}
ADAPTER(MIi_CpuClear32)
{
    u32 *p = P(a1);
    u32 i;

    for (i = 0; i < a2 / 4; i++) {
        p[i] = a0;
    }
    return 0;
}
/* MI_DmaFill32(dmaNo, dest, data, size) */
ADAPTER(MI_DmaFill32) { return c2u$MIi_CpuClear32(a2, a1, a3, 0); }

/* ---- OS ---- */

static u32 sIntr = 1;
ADAPTER(OS_DisableInterrupts)
{
    u32 old = sIntr;

    sIntr = 0;
    return old;
}
ADAPTER(OS_RestoreInterrupts)
{
    u32 old = sIntr;

    sIntr = a0;
    return old;
}

ADAPTER(OS_InitMessageQueue)
{
    OSMessageQueue *mq = P(a0);

    memset(mq, 0, sizeof *mq);
    mq->msgArray = P(a1);
    mq->msgCount = (s32)a2;
    return 0;
}
ADAPTER(OS_SendMessage)
{
    OSMessageQueue *mq = P(a0);

    if (mq->usedCount >= mq->msgCount) {
        return FALSE;
    }
    mq->msgArray[(mq->firstIndex + mq->usedCount) % mq->msgCount] = P(a1);
    mq->usedCount++;
    return TRUE;
}
ADAPTER(OS_JamMessage)
{
    OSMessageQueue *mq = P(a0);

    if (mq->usedCount >= mq->msgCount) {
        return FALSE;
    }
    mq->firstIndex = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
    mq->msgArray[mq->firstIndex] = P(a1);
    mq->usedCount++;
    return TRUE;
}
ADAPTER(OS_ReceiveMessage)
{
    OSMessageQueue *mq = P(a0);
    OSMessage *msg = P(a1);

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

ADAPTER(OS_GetMacAddress)
{
    memcpy(P(a0), (const u8 *)OS_GetSystemWork()->nvramUserInfo + ((sizeof(NVRAMConfig) + 3) & ~3u), 6);
    return 0;
}

ADAPTER(OS_Terminate)
{
    fprintf(stderr, "test_wm_dp: OS_Terminate\n");
    abort();
}

/* D's RTC is recompiled too (RTC_external.s); WM_GetNextTgid seeds from it. */
uint64_t RTC_Init(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) { return 0; }
uint64_t RTC_GetTime(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    memset(P(a0), 0, 12); /* RTCTime: hour, minute, second */
    return 0;
}

/* ---- PXI, into the real pc_pxi.c ---- */

/* The library registers WmReceiveFifo by its guest address; pc_pxi.c calls
 * a C function pointer, so tag 10 gets a trampoline back into dispatch. */
static uint32_t sFifoCb[PXI_MAX_FIFO_TAG];

static void fifo_tramp(PXIFifoTag tag, u32 data, BOOL err)
{
    armrec_dispatch(sFifoCb[tag], (uint32_t)tag, data, (uint32_t)err, 0);
}

ADAPTER(PXI_Init) { PXI_Init(); return 0; }
ADAPTER(PXI_IsCallbackReady) { return (uint32_t)PXI_IsCallbackReady((int)a0, (PXIProc)a1); }
ADAPTER(PXI_SendWordByFifo) { return (uint32_t)PXI_SendWordByFifo((int)a0, a1, (BOOL)a2); }
ADAPTER(PXI_SetFifoRecvCallback)
{
    sFifoCb[a0] = a1;
    PXI_SetFifoRecvCallback((int)a0, a1 != 0 ? fifo_tramp : NULL);
    return 0;
}
