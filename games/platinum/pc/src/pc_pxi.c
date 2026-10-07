/*
 * PXI (the ARM9<->ARM7 FIFO) modeled without an ARM7.
 *
 * These are strong definitions; the SDK's compiled pxi objects are
 * weakened at build time, so this file replaces them wholesale. The
 * hardware handshake (PXI_InitFifo stepping IPCSYNC nibbles against the
 * subprocessor) can never converge here: there is no other processor and
 * the IO window is plain memory, so the remote nibble would spin forever.
 * Replacing the mechanism, not patching the loop, is the honest shape;
 * the SDK's own state machine is meaningless without the other side.
 *
 * The model: callbacks register normally and the "subprocessor" reports
 * ready for every tag, because the components an ARM7 would run (touch,
 * RTC, sound, wifi) will be modeled on this side as their walls appear.
 * A send with no responder is reported ONCE per tag on stderr and
 * otherwise dropped, visibly incomplete rather than silently wrong, and
 * a caller that genuinely needs the reply becomes the next named wall
 * instead of a mystery hang.
 */
#include <nitro/pxi.h>
#include <stdio.h>

static PXIFifoCallback sRecvCallback[PXI_MAX_FIFO_TAG];
static u32 sWarnedDrop[(PXI_MAX_FIFO_TAG + 31) / 32];

/* The modeled "ARM7" components, one per tag (pc_rtc.c is tag 5's). A
 * responder consumes the sent word and answers through pc_pxi_reply,
 * which delivers to the tag's registered ARM9 receive callback, the
 * same synchronous-completion shape the DMA and card models use. */
static void (*sResponder[PXI_MAX_FIFO_TAG])(u32 data);

/* A receive callback runs as the PXI receive interrupt's handler, which the
 * hardware does not re-enter: a reply to a word the callback itself sends
 * is taken only after the callback returns. Replies raised while a callback
 * runs therefore wait here and are delivered, in order, when the outermost
 * one returns. TWL-SDK's NVRAM state machine (Black/White's DWC user-ID
 * write at boot) sends the next command from the callback and only then
 * advances its state; delivered inline, the reply found the old state, sent
 * the same command again and recursed until the host stack overflowed. */
#define PC_PXI_PENDING 64
static struct { int tag; u32 data; BOOL err; } sPending[PC_PXI_PENDING];
static int sPendHead, sPendCount, sInCallback;

static void deliver(int tag, u32 data, BOOL err)
{
    if (sRecvCallback[tag] == NULL) {
        return;
    }
    if (sInCallback) {
        if (sPendCount == PC_PXI_PENDING) {
            fprintf(stderr, "pc_pxi: %d replies pending inside a receive "
                    "callback (tag %d)\n", PC_PXI_PENDING, tag);
            __builtin_trap();
        }
        int i = (sPendHead + sPendCount++) % PC_PXI_PENDING;
        sPending[i].tag = tag;
        sPending[i].data = data;
        sPending[i].err = err;
        return;
    }
    sInCallback = 1;
    sRecvCallback[tag]((PXIFifoTag)tag, data, err);
    while (sPendCount > 0) {
        int i = sPendHead;
        sPendHead = (sPendHead + 1) % PC_PXI_PENDING;
        sPendCount--;
        if (sRecvCallback[sPending[i].tag] != NULL) {
            sRecvCallback[sPending[i].tag]((PXIFifoTag)sPending[i].tag,
                                           sPending[i].data, sPending[i].err);
        }
    }
    sInCallback = 0;
}

void pc_pxi_set_responder(int tag, void (*fn)(u32 data))
{
    sResponder[tag] = fn;
}

void pc_pxi_reply(int tag, u32 data)
{
    deliver(tag, data, FALSE);
}

/* The same with the FIFO word's error bit as the sender set it: the ARM7's
 * card library sends every reply on the FS tag with it set
 * (CARDi_SendPxi), and TWL-SDK's ARM9 receiver acts only on such words. */
void pc_pxi_reply_err(int tag, u32 data, BOOL err)
{
    deliver(tag, data, err);
}

void PXI_Init(void)
{
    PXI_InitFifo();
}

void PXI_InitFifo(void)
{
    int i;
    /* Once, as the SDK's own body is (pxi_fifo.c: `if (!FifoCtrlInit)`).
     * Components call PXI_Init again from their own inits after others have
     * registered, so a clearing re-init drops the earlier callbacks and every
     * reply on those tags lands nowhere. Diamond/Pearl met it first (OS_Init's
     * PM_Init, CTRDG_Init, the game's RTC_Init); in Platinum it was the RTC:
     * InitRTC's first read completed, a later PXI_Init dropped
     * RtcCommonCallback, the next read's reply vanished with the RTC library
     * still locked, and the clock stood at boot time forever (no day/night,
     * Poketch watch frozen at 10:00, measured by tests/gameplay). */
    static int sFifoInit;

    if (sFifoInit) {
        return;
    }
    sFifoInit = 1;
    for (i = 0; i < PXI_MAX_FIFO_TAG; i++) {
        sRecvCallback[i] = NULL;
    }
}

void PXI_SetFifoRecvCallback(int fifotag, PXIFifoCallback callback)
{
    sRecvCallback[fifotag] = callback;
}

void PXI_SetFifoSendCallback(PXIFifoEmtpyCallback callback)
{
    /* Send-empty notification: the modeled FIFO is never full, so the
     * callback would fire immediately and forever; nothing here needs it. */
    (void)callback;
}

BOOL PXI_IsCallbackReady(int fifotag, PXIProc proc)
{
    /*
     * Honest per processor, and the distinction is load-bearing.
     *
     * PXI_PROC_ARM7 asks "is the OTHER processor's receiver up for this
     * tag", callers spin on it waiting for the ARM7 to boot (os_reset,
     * card, rtc, ctrdg) or gate a feature on the radio existing (wvr line
     * 21). The remote side is modeled here by a responder, so it is up:
     * TRUE lets initialization proceed to where a component is exercised.
     *
     * PXI_PROC_ARM9 asks "have I, the ARM9, already registered my own
     * receive callback for this tag", and the callers use it to decide
     * whether to register: `if (!ready) PXI_SetFifoRecvCallback(...)`.
     * Answering TRUE there means the ARM9 never registers, so a responder's
     * reply lands on a NULL slot and vanishes. That is exactly what kept the
     * WVR radio from ever completing: WVR_StartUpAsync skipped registering
     * WvrReceiveCallback, the reply went nowhere, WirelessDriver stayed
     * CONNECTING forever, and every battle drew the network icon over
     * unloaded tiles. So for the ARM9 the honest answer is whether the slot
     * is set; which is what the DS's own pxiHandleChecker[ARM9] tracks.
     */
    if (proc == PXI_PROC_ARM9) {
        return (fifotag >= 0 && fifotag < PXI_MAX_FIFO_TAG &&
                sRecvCallback[fifotag] != NULL) ? TRUE : FALSE;
    }
    (void)fifotag;
    return TRUE;
}

int PXI_SendWordByFifo(int fifotag, u32 data, BOOL err)
{
    (void)err;
    if (sResponder[fifotag] != NULL) {
        sResponder[fifotag](data);
        return PXI_FIFO_SUCCESS;
    }
    if (!(sWarnedDrop[fifotag / 32] & (1u << (fifotag % 32)))) {
        sWarnedDrop[fifotag / 32] |= 1u << (fifotag % 32);
        fprintf(stderr,
                "pc_pxi: send on fifo tag %d dropped (first word %#x), "
                "no responder modeled for this tag yet\n",
                fifotag, (unsigned)data);
    }
    return PXI_FIFO_SUCCESS;
}
