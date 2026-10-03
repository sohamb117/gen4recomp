/*
 * DMA as what it is when the CPU is spinning on it: a memory operation.
 *
 * The SDK's mi_dma.c (weakened at build time, replaced here) programs the
 * channel registers and waits for the enable bit to clear. The IO window
 * on this port is plain memory (nothing clears that bit) so the
 * mechanism is replaced rather than the wait patched: on hardware, a
 * plain-priority DMA with the CPU parked on the busy flag completes
 * before the wait ends, and the observable result is exactly "the copy
 * happened, then the callback ran". That is what these do, immediately.
 *
 * Async variants complete synchronously and then invoke the callback:
 * "finished before you looked" is a legal DMA schedule, and every SDK
 * caller must already tolerate it. HBlank/VBlank-timed DMA
 * (mi_dma_hblank.c, mi_dma_vblank.c) is NOT this file's claim, timed
 * channels spread work across a frame, their observable interleaving is
 * the point, and nothing here models frames yet. Those walls keep their
 * own names.
 *
 * The u64 fill argument mirrors the SDK's MIi_DmaSetParams-derived
 * signatures: fills replicate a 16- or 32-bit unit; sizes are bytes.
 */
#include <nitro/mi/dma.h>
#include <nitro/os.h>
#include <stdint.h>
#include <string.h>

static void fill16(void *dest, u16 unit, u32 size)
{
    u16 *d = dest;
    u32 n = size / 2;
    while (n--) {
        *d++ = unit;
    }
}

static void fill32(void *dest, u32 unit, u32 size)
{
    u32 *d = dest;
    u32 n = size / 4;
    while (n--) {
        *d++ = unit;
    }
}

void MI_DmaFill16(u32 dmaNo, void *dest, u16 data, u32 size)
{
    (void)dmaNo;
    fill16(dest, data, size);
}

void MI_DmaFill32(u32 dmaNo, void *dest, u32 data, u32 size)
{
    (void)dmaNo;
    fill32(dest, data, size);
}

/* PC_TRACE_LCDC: pc_probe2d.c's gate; prints copies aimed at the LCDC
 * texture-load window. Free when the env var is unset. */
extern void pc_trace_lcdc(const char *who, const void *src, uint32_t dst,
                          uint32_t size);

void MI_DmaCopy16(u32 dmaNo, const void *src, void *dest, u32 size)
{
    (void)dmaNo;
    pc_trace_lcdc("dma16", src, (uint32_t)(uintptr_t)dest, size);
    if (src == NULL || dest == NULL || (size & ~1u) == 0) {
        return;
    }
    memmove(dest, src, size & ~1u);
}

void MI_DmaCopy32(u32 dmaNo, const void *src, void *dest, u32 size)
{
    (void)dmaNo;
    pc_trace_lcdc("dma32", src, (uint32_t)(uintptr_t)dest, size);
    /* memmove is marked nonnull. A 0-byte or NULL DMA is a hardware
     * no-op; C still treats memmove(NULL, NULL, 0) as UB. */
    if (src == NULL || dest == NULL || (size & ~3u) == 0) {
        return;
    }
    memmove(dest, src, size & ~3u);
}

void MI_DmaFill16Async(u32 dmaNo, void *dest, u16 data, u32 size,
                       MIDmaCallback callback, void *arg)
{
    MI_DmaFill16(dmaNo, dest, data, size);
    if (callback) {
        callback(arg);
    }
}

void MI_DmaFill32Async(u32 dmaNo, void *dest, u32 data, u32 size,
                       MIDmaCallback callback, void *arg)
{
    MI_DmaFill32(dmaNo, dest, data, size);
    if (callback) {
        callback(arg);
    }
}

void MI_DmaCopy16Async(u32 dmaNo, const void *src, void *dest, u32 size,
                       MIDmaCallback callback, void *arg)
{
    MI_DmaCopy16(dmaNo, src, dest, size);
    if (callback) {
        callback(arg);
    }
}

void MI_DmaCopy32Async(u32 dmaNo, const void *src, void *dest, u32 size,
                       MIDmaCallback callback, void *arg)
{
    MI_DmaCopy32(dmaNo, src, dest, size);
    if (callback) {
        callback(arg);
    }
}

/* "Send" writes a stream to one fixed destination address, a hardware
 * FIFO. The only fixed destinations on a DS are IO registers, and no
 * device behind the IO window is modeled here yet; completing the write
 * into plain memory would store one garbage word where a device should
 * have consumed a stream. Loud, like MIi_CpuSend32. */
static void dma_send_trap(const char *who)
{
    extern void pc_trap_unreached(const char *, const char *)
        __attribute__((noreturn));
    pc_trap_unreached(who,
                      "DMA to a fixed device address; no device model behind "
                      "the IO window yet");
}

void MI_DmaSend16(u32 dmaNo, const void *src, volatile void *dest, u32 size)
{
    (void)dmaNo; (void)src; (void)dest; (void)size;
    dma_send_trap("MI_DmaSend16");
}

/*
 * The 32-bit send is how the SDK DMAs a display list at the geometry FIFO
 * (MI_SendGXCommand / GX_SendDL). DMA-with-parked-CPU is a stream of word
 * stores performed now, and the GX layer consumes them the same way
 * MIi_CpuSend32's stream is consumed: one push per word, via
 * pc_gpu3d_store_through(). A destination the GX layer does not own still
 * traps; no other device FIFO is modeled.
 */
void MI_DmaSend32(u32 dmaNo, const void *src, volatile void *dest, u32 size)
{
    extern int pc_gpu3d_store_through(void *dst, u32 v);
    const u32 *s = (const u32 *)src;
    u32 n = size / 4;
    u32 i;

    (void)dmaNo;
    if (n == 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        if (!pc_gpu3d_store_through((void *)dest, s[i])) {
            dma_send_trap("MI_DmaSend32");
        }
    }
}

void MI_DmaSend16Async(u32 dmaNo, const void *src, volatile void *dest,
                       u32 size, MIDmaCallback callback, void *arg)
{
    (void)dmaNo; (void)src; (void)dest; (void)size; (void)callback; (void)arg;
    dma_send_trap("MI_DmaSend16Async");
}

void MI_DmaSend32Async(u32 dmaNo, const void *src, volatile void *dest,
                       u32 size, MIDmaCallback callback, void *arg)
{
    MI_DmaSend32(dmaNo, src, dest, size);
    if (callback) {
        callback(arg);
    }
}

/* Every transfer above completed before returning, so no channel is ever
 * busy and stopping or waiting is trivially satisfied. */
BOOL MI_IsDmaBusy(u32 dmaNo)
{
    (void)dmaNo;
    return FALSE;
}

/*
 * The GX-command senders (mi_dma_gxcommand.c). The SDK's compiled versions
 * are a DMA engine's worth of machinery: chunked MIi_DmaSetParams writes
 * driven by GXFIFO-underhalf interrupts, with a static isBusy latch cleared
 * from the DMA-end interrupt. None of those interrupts exist here, so the
 * compiled async path loses the display list into unmapped DMA registers
 * and leaves isBusy stuck TRUE, the second async caller spins forever on
 * it, which is exactly where the intro's opening cutscene froze
 * (ov77 -> NNS_G3dDraw -> MI_SendGXCommandAsync, measured under gdb).
 *
 * The port's DMA model (top of this file) already says what to do instead:
 * A DMA is a memory operation performed immediately, and an async
 * callback runs before return. The FIFO consumes through
 * pc_gpu3d_store_through, one push per word, same as MI_DmaSend32.
 */
static void pc_send_gx(const void *src, u32 commandLength, const char *who)
{
    extern int pc_gpu3d_store_through(void *dst, u32 v);
    const u32 *s = (const u32 *)src;
    u32 n = commandLength / 4;
    u32 i;

    for (i = 0; i < n; i++) {
        if (!pc_gpu3d_store_through((void *)0x04000400u, s[i])) {
            dma_send_trap(who);
        }
    }
}

void MI_SendGXCommand(u32 dmaNo, const void *src, u32 commandLength)
{
    (void)dmaNo;
    pc_send_gx(src, commandLength, "MI_SendGXCommand");
}

void MI_SendGXCommandAsync(u32 dmaNo, const void *src, u32 commandLength,
                           MIDmaCallback callback, void *arg)
{
    (void)dmaNo;
    pc_send_gx(src, commandLength, "MI_SendGXCommandAsync");
    if (callback) {
        callback(arg);
    }
}

void MI_SendGXCommandAsyncFast(u32 dmaNo, const void *src, u32 commandLength,
                               MIDmaCallback callback, void *arg)
{
    (void)dmaNo;
    pc_send_gx(src, commandLength, "MI_SendGXCommandAsyncFast");
    if (callback) {
        callback(arg);
    }
}

void MI_StopDma(u32 dmaNo)
{
    (void)dmaNo;
}

void MI_WaitDma(u32 dmaNo)
{
    (void)dmaNo;
}
