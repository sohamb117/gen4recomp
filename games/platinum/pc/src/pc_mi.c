/* C reimplementation of the NitroSDK's mwcc-asm MI/DC/IC surface for the PC
 * build. The authoritative sources are the SDK's own asm-in-C files:
 *   subprojects/NitroSDK-4.2.30001/libraries/mi/src/mi_memory.c
 *   subprojects/NitroSDK-4.2.30001/libraries/mi/src/mi_uncompress.c
 *   subprojects/NitroSDK-4.2.30001/libraries/mi/src/mi_swap.c
 * Each function below was derived line-by-line from that asm, preserving the
 * exact byte counts the hardware loops touch (including the deliberate
 * halfword/word overrun on ragged sizes). The LZ decoders were additionally
 * cross-checked against tools/nitrogfx/lz.c (known-good decoder for the
 * normal-format subset).
 *
 * House rule: nothing silent. Anything this file cannot reproduce faithfully
 * traps loudly instead of approximating. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nitro/types.h>

#include <nitro/mi/memory.h>
#include <nitro/mi/swap.h>
#include <nitro/mi/uncompress.h>
#include <nitro/os/ARM9/cache.h>

#include "pc_gpu3d.h"

static void __attribute__((noreturn)) pc_mi_trap(const char *name, const char *reason)
{
    fprintf(stderr, "pc: %s: not implemented (%s)\n", name, reason);
    abort();
}

/* ================================================================== */
/* CPU fill / copy (mi_memory.c)                                      */
/* ================================================================== */

/* asm: word-optimized memset with read-modify-write halfword edges; the net
 * memory effect on normal RAM is exactly memset for every size/alignment. */
void MI_CpuFill8(void *dest, u8 data, u32 size)
{
    memset(dest, data, size);
}

/* asm: forward copy whose unit size (byte/halfword/word) depends on the
 * mutual alignment of src and dest. For the SDK contract (non-overlapping
 * buffers) every path is byte-exact memcpy. Under overlap the asm's result
 * depends on which alignment path it took, so we refuse to guess. */
void MI_CpuCopy8(const void *src, void *dest, u32 size)
{
    const u8 *s = src;
    u8 *d = dest;

    /* src == dest is the one overlap every path agrees on: each unit is
     * loaded and stored back unchanged. The union room's comm setup does
     * exactly that (a 32-byte self-copy right after a child joins). */
    if (d == s) {
        return;
    }
    if (size != 0 && d < s + size && s < d + size) {
        pc_mi_trap("MI_CpuCopy8", "overlapping copy; asm result depends on alignment path");
    }
    memcpy(dest, src, size);
}

/* asm: strh at dest+i for i = 0,2,... while i < size. An odd size makes the
 * final halfword store overrun by one byte, exactly as on hardware (the SDK
 * wrappers assert size is even). The loops below keep that shape. */
void MIi_CpuClear16(u16 data, void *destp, u32 size)
{
    u8 *dest = destp;
    u32 i;

    for (i = 0; i < size; i += 2) {
        memcpy(dest + i, &data, 2);
    }
}

/* PC_TRACE_LCDC: pc_probe2d.c's gate; prints copies aimed at the LCDC
 * texture-load window. Free when the env var is unset. */
extern void pc_trace_lcdc(const char *who, const void *src, uint32_t dst,
                          uint32_t size);

/* asm: ldrh/strh pairs, forward, same ragged-tail behavior as Clear16. The
 * per-halfword forward copy is also what the asm does under overlap. */
void MIi_CpuCopy16(const void *srcp, void *destp, u32 size)
{
    const u8 *src = srcp;
    u8 *dest = destp;
    u32 i;

    pc_trace_lcdc("cpu16", srcp, (uint32_t)(uintptr_t)destp, size);
    for (i = 0; i < size; i += 2) {
        memcpy(dest + i, src + i, 2);
    }
}

/* asm: stmia one word at a time while dest < dest+size; a non-multiple-of-4
 * size overruns to the next word boundary, as on hardware. */
void MIi_CpuClear32(u32 data, void *destp, u32 size)
{
    u8 *dest = destp;
    u8 *end = dest + size;

    while (dest < end) {
        memcpy(dest, &data, 4);
        dest += 4;
    }
}

/* asm: ldmia/stmia one word at a time, forward; same ragged tail. */
void MIi_CpuCopy32(const void *srcp, void *destp, u32 size)
{
    const u8 *src = srcp;
    u8 *dest = destp;
    u8 *end = dest + size;

    pc_trace_lcdc("cpu32", srcp, (uint32_t)(uintptr_t)destp, size);
    while (dest < end) {
        memcpy(dest, src, 4);
        src += 4;
        dest += 4;
    }
}

/* asm: 8-register stmia for floor(size/32) blocks, then single-word stores
 * while dest < dest+size (so the tail still rounds up to a word boundary). */
void MIi_CpuClearFast(u32 data, void *destp, u32 size)
{
    u8 *dest = destp;
    u8 *blockEnd = dest + (size & ~(u32)31);
    u8 *end = dest + size;
    u32 i;

    while (dest < blockEnd) {
        for (i = 0; i < 8; i++) {
            memcpy(dest + 4 * i, &data, 4);
        }
        dest += 32;
    }
    while (dest < end) {
        memcpy(dest, &data, 4);
        dest += 4;
    }
}

/* asm: ldmia/stmia of 8 words per block (whole 32-byte chunk is read before
 * written, kept via the bounce buffer), then word-at-a-time tail rounding
 * up to a word boundary. */
void MIi_CpuCopyFast(const void *srcp, void *destp, u32 size)
{
    const u8 *src = srcp;
    u8 *dest = destp;
    u8 *blockEnd = dest + (size & ~(u32)31);
    u8 *end = dest + size;
    u32 tmp[8];

    while (dest < blockEnd) {
        memcpy(tmp, src, 32);
        memcpy(dest, tmp, 32);
        src += 32;
        dest += 32;
    }
    while (dest < end) {
        memcpy(dest, src, 4);
        src += 4;
        dest += 4;
    }
}

/*
 * Fixed-size ldmia/stmia sequences: plain forward copies of N bytes, except
 * where the SDK points them at the geometry engine. G3_MultMtx33 stores the
 * MTX_MULT_3x3 command word to GXFIFO and copies the nine parameters after it
 * with MI_Copy36B(m, &reg_G3X_GXFIFO); G3X_GetClipMtx and G3X_GetVectorMtx
 * read the clip and direction matrices out of the result block with
 * MI_Copy64B and MI_Copy36B. Decompiled C hands these the staging word and a
 * block armrec_gx_reg() has just refreshed; recompiled SDK code (a ROM-only
 * core's GX_g3imm/GX_g3x, which NNS G3d's billboard handlers call) hands them
 * the register addresses themselves. So the copy does what the bus does: a
 * source in the status and result block is computed first, and each word
 * stored into the command window is its own store (consecutive addresses,
 * the stmia's, all FIFO pushes inside the GXFIFO mirror).
 */
static void mi_copy_fixed(const void *pSrc, void *pDest, u32 size)
{
    uintptr_t s = (uintptr_t)pSrc, d = (uintptr_t)pDest;
    u32 words[16], i;

    if (s < PC_GX_IO_END && s + size > 0x04000600u)
        pc_gpu3d_refresh_regs();
    if (d < PC_GX_CMD_END && d + size > PC_GX_FIFO_BASE) {
        memcpy(words, pSrc, size);
        for (i = 0; i < size / 4; i++)
            pc_gx_store((volatile uint32_t *)(d + i * 4), words[i]);
        return;
    }
    memcpy(pDest, pSrc, size);
}

void MI_Copy16B(register const void *pSrc, register void *pDest) { mi_copy_fixed(pSrc, pDest, 16); }
void MI_Copy32B(register const void *pSrc, register void *pDest) { mi_copy_fixed(pSrc, pDest, 32); }
void MI_Copy36B(register const void *pSrc, register void *pDest) { mi_copy_fixed(pSrc, pDest, 36); }
void MI_Copy48B(register const void *pSrc, register void *pDest) { mi_copy_fixed(pSrc, pDest, 48); }
void MI_Copy64B(register const void *pSrc, register void *pDest) { mi_copy_fixed(pSrc, pDest, 64); }

void MI_Zero36B(register void *pDest) { memset(pDest, 0, 36); }

/* asm: single SWP instruction. The guest is single-threaded on this port, so
 * a plain read+write is a faithful atomic swap. */
u32 MI_SwapWord(u32 setData, volatile u32 *destp)
{
    u32 old = *destp;
    *destp = setData;
    return old;
}

/* ================================================================== */
/* BIOS-format LZ77 decompression (mi_uncompress.c)                   */
/* ================================================================== */

/* Stream layout (derived from the SDK asm; the normal-format subset is
 * cross-checked against tools/nitrogfx/lz.c):
 *   u32 header: bits 4-7 = 0x1 (LZ), bits 0-3 = compParam, bits 8-31 =
 *   uncompressed size. Then tag bytes, each followed by 8 tokens, MSB first:
 *   bit clear = 1 literal byte; bit set = back-reference.
 *
 *   Normal reference (compParam == 0), 2 bytes b0 b1:
 *     length = (b0 >> 4) + 3, distance = (((b0 & 0xF) << 8) | b1) + 1
 *   Extended lengths (compParam != 0, "LZ ex"):
 *     b0 & 0xE0 != 0             -> length = (b0 >> 4) + 1        (1 hdr byte)
 *     b0 >> 4 == 0               -> length = ((b0 & 0xF) << 4 | b1 >> 4) + 0x11
 *     b0 >> 4 == 1               -> length = ((b0 & 0xF) << 12 | b1 << 4 | b2 >> 4) + 0x111
 *     ...with the distance packed in the low nibble of the last length byte
 *     and one following byte, same as the normal form.
 *
 * The asm subtracts the whole run length from the remaining count before
 * copying, so a malformed stream overruns dest rather than clamping; we
 * reproduce that. Decoding stops as soon as the remaining count reaches
 * zero, even mid-tag. */

/* Parse one back-reference token; returns the advanced src pointer. */
static const u8 *pc_mi_lz_token(const u8 *src, int exFormat, u32 *lenOut, u32 *distOut)
{
    u32 base;
    u32 b = *src;

    if (!exFormat) {
        base = 3;
    } else if (b & 0xE0) {
        base = 1;
    } else if (!(b & 0x10)) {
        base = ((b & 0xF) << 4) + 0x11;
        src++;
    } else {
        src++;
        base = ((b & 0xF) << 12) + ((u32)*src << 4) + 0x111;
        src++;
    }
    b = *src++;
    *lenOut = base + (b >> 4);
    *distOut = (((b & 0xF) << 8) | *src) + 1;
    src++;
    return src;
}

void pc_mi_selftest(void);

/* Env-gated once-guard; pc_mi_selftest() is also exported for a test runner. */
static void pc_mi_lz_maybe_selftest(void)
{
    static int done = 0;

    if (done) {
        return;
    }
    done = 1; /* set before running: the selftest calls the decoders */
    if (getenv("PC_MI_SELFTEST") != NULL) {
        pc_mi_selftest();
    }
}

void MI_UncompressLZ8(const void *srcp, void *destp)
{
    const u8 *src = srcp;
    u8 *dest = destp;
    u32 header;
    s32 remaining;
    int exFormat;

    pc_mi_lz_maybe_selftest();

    memcpy(&header, src, 4);
    src += 4;
    remaining = (s32)(header >> 8);
    exFormat = (header & 0x0F) != 0;

    while (remaining > 0) {
        u32 flags = *src++;
        int i;

        for (i = 0; i < 8; i++) {
            if (flags & 0x80) {
                u32 len, dist;

                src = pc_mi_lz_token(src, exFormat, &len, &dist);
                remaining -= (s32)len;
                while (len--) {
                    *dest = *(dest - dist);
                    dest++;
                }
            } else {
                *dest++ = *src++;
                remaining--;
            }
            if (remaining <= 0) {
                break;
            }
            flags <<= 1;
        }
    }
}

/* The 16-bit variant assembles output bytes in a one-halfword latch and only
 * ever issues strh to dest (VRAM-safe writes); back-references are read back
 * from dest memory. Two hardware quirks are reproduced exactly:
 *   - a trailing byte of an odd-sized stream stays in the latch and is never
 *     written (the asm exits without flushing);
 *   - a distance-1 reference to the still-latched byte reads memory that has
 *     not been written yet. On hardware that returns stale VRAM; such streams
 *     are illegal for this decoder, so we trap loudly instead of inventing a
 *     byte. */
void MI_UncompressLZ16(const void *srcp, void *destp)
{
    const u8 *src = srcp;
    u8 *dest = destp;
    u32 header;
    s32 remaining;
    int exFormat;
    u32 pos = 0;   /* logical byte position in dest */
    u32 latch = 0; /* pending low byte while pos is odd */

    pc_mi_lz_maybe_selftest();

    memcpy(&header, src, 4);
    src += 4;
    remaining = (s32)(header >> 8);
    exFormat = (header & 0x0F) != 0;

#define PC_MI_LZ16_PUT(b)                            \
    do {                                             \
        if ((pos & 1) == 0) {                        \
            latch = (b);                             \
        } else {                                     \
            u16 h = (u16)(latch | ((u32)(b) << 8));  \
            memcpy(dest + pos - 1, &h, 2);           \
        }                                            \
        pos++;                                       \
    } while (0)

    while (remaining > 0) {
        u32 flags = *src++;
        int i;

        for (i = 0; i < 8; i++) {
            if (flags & 0x80) {
                u32 len, dist;

                src = pc_mi_lz_token(src, exFormat, &len, &dist);
                remaining -= (s32)len;
                while (len--) {
                    if (dist == 1 && (pos & 1)) {
                        fprintf(stderr, "pc: MI_UncompressLZ16: distance-1 reference to the "
                                        "unflushed halfword latch (stream illegal for the "
                                        "VRAM-safe decoder)\n");
                        abort();
                    }
                    PC_MI_LZ16_PUT(dest[pos - dist]);
                }
            } else {
                PC_MI_LZ16_PUT(*src++);
                remaining--;
            }
            if (remaining <= 0) {
                break;
            }
            flags <<= 1;
        }
    }
    /* Odd total size: the latched final byte is dropped, matching the asm. */
#undef PC_MI_LZ16_PUT
}

/* Hand-assembled vectors, documented byte by byte.
 *
 * vec_normal decodes to "HELLO_HELLO_HELLO_" (18 bytes):
 *   10 12 00 00  header: type 0x10 (LZ, param 0), size 0x000012 = 18
 *   02           tag 0b00000010: six literals, then one reference
 *   H e l l o _  literals
 *   90 05        reference: length (9)+3 = 12, distance 0x005+1 = 6
 *
 * vec_ex decodes to 36 x 'X' via the extended two-byte length form:
 *   11 24 00 00  header: type 0x10, compParam 1 (ex), size 0x000024 = 36
 *   40           tag 0b01000000: one literal, then one reference
 *   X            literal
 *   01 20 00     b0=0x01 (top nibble 0 -> two-byte length):
 *                length = ((1)<<4 | (0x20)>>4) + 0x11 = 0x23 = 35
 *                distance = ((0x20 & 0xF) << 8 | 0x00) + 1 = 1
 */
void pc_mi_selftest(void)
{
    static const u8 vec_normal[] = {
        0x10, 0x12, 0x00, 0x00,
        0x02,
        'H', 'E', 'L', 'L', 'O', '_',
        0x90, 0x05
    };
    static const char exp_normal[18] = "HELLO_HELLO_HELLO_";
    static const u8 vec_ex[] = {
        0x11, 0x24, 0x00, 0x00,
        0x40,
        'X',
        0x01, 0x20, 0x00
    };
    u8 out[64];
    int i;

    memset(out, 0xAA, sizeof(out));
    MI_UncompressLZ8(vec_normal, out);
    if (memcmp(out, exp_normal, 18) != 0) {
        fprintf(stderr, "pc: pc_mi_selftest: MI_UncompressLZ8 normal-format vector FAILED\n");
        abort();
    }

    memset(out, 0xAA, sizeof(out));
    MI_UncompressLZ16(vec_normal, out);
    if (memcmp(out, exp_normal, 18) != 0) {
        fprintf(stderr, "pc: pc_mi_selftest: MI_UncompressLZ16 normal-format vector FAILED\n");
        abort();
    }

    memset(out, 0xAA, sizeof(out));
    MI_UncompressLZ8(vec_ex, out);
    for (i = 0; i < 36; i++) {
        if (out[i] != 'X') {
            fprintf(stderr, "pc: pc_mi_selftest: MI_UncompressLZ8 extended-format vector FAILED\n");
            abort();
        }
    }

    fprintf(stderr, "pc: pc_mi_selftest: LZ decoders OK\n");
}

/* ================================================================== */
/* Cache maintenance: no-ops, host cache coherency is the host's job. */
/* ================================================================== */

void DC_FlushAll(void) {}
void DC_FlushRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_InvalidateRange(void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_StoreRange(const void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }
void DC_WaitWriteBufferEmpty(void) {}
void IC_InvalidateAll(void) {}
void IC_InvalidateRange(void *startAddr, u32 nBytes) { (void)startAddr; (void)nBytes; }

/* ================================================================== */
/* Loud traps                                                          */
/* ================================================================== */

void MIi_UncompressBackward(void *bottom)
{
    (void)bottom;
    pc_mi_trap("MIi_UncompressBackward", "in-place backward BIOS decompression is dead code in this game and stays unimplemented by design");
}

/*
 * Stream `size` bytes of words to one fixed destination, a hardware FIFO.
 * The one FIFO this game streams to is the geometry command port (display
 * lists: GX_SendDLNDma and the NNS model renderer come through here), and
 * the GX layer models it: pc_gpu3d_store_through() resolves a destination
 * that is the staging word or anywhere in the command window and gives each
 * word its own push. Any other fixed destination still traps, completing
 * the write into plain memory would store one garbage word where a device
 * should have consumed a stream.
 */
void MIi_CpuSend32(const void *srcp, volatile void *destp, u32 size)
{
    extern int pc_gpu3d_store_through(void *dst, u32 v);
    const u32 *src = (const u32 *)srcp;
    u32 n = size / 4;
    u32 i;

    if (n == 0) {
        return;
    }
    if (!pc_gpu3d_store_through((void *)destp, src[0])) {
        pc_mi_trap("MIi_CpuSend32",
                   "streams words to a fixed device address the GX layer "
                   "does not own; no other device FIFO is modeled");
    }
    for (i = 1; i < n; i++) {
        if (!pc_gpu3d_store_through((void *)destp, src[i])) {
            pc_mi_trap("MIi_CpuSend32",
                       "the GX layer disowned the destination mid-stream");
        }
    }
}
