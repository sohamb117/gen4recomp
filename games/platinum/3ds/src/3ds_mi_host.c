/*
 * 3ds/src/3ds_mi_host.c: the MI and DMA surface, over a map that is not the
 * identity.
 *
 * 3ds/include/3ds_hw_host.h moves every DS memory-map constant onto the slab,
 * so HW_MAIN_MEM and the shared-work names are host pointers the game
 * dereferences directly. It deliberately does not move the VRAM windows,
 * because 16 MB of addresses over 0xA4000 of memory, placed by nine registers,
 * is not something a base plus an offset can express. Those constants keep the
 * address a DS gives them and are translated per access.
 *
 * Per access is the part the memory primitives were not doing. MI_CpuFillFast,
 * MI_DmaCopy32 and the rest take a `void *` and write straight through it,
 * which is right on the PC port and here writes to an address this console has
 * nothing at. Measured on the first boot that reached the game: 188,929 word
 * writes and 94 block writes to unmapped addresses, all but 6,417 of them in a
 * VRAM window. Nothing the game cleared or uploaded in its first frames landed
 * anywhere.
 *
 * Why wrappers and not an edit to pc/src. The bodies in pc/src/pc_mi.c are a
 * line-by-line transcription of the SDK's mwcc assembly, ragged tails and
 * deliberate overruns included; splitting each into a block walk would put
 * this console's problem inside code whose whole value is that it still reads
 * like the assembly. And the split does not change the bytes: each contiguous
 * run is handed to the real function.
 *
 * Which pointers are guest ones, and the one place it is not decidable. Every
 * row of the guest map is below 0x08000000 except the AGB slot, and every host
 * address this console hands a process is at or above it. So the test is a
 * compare, exact for ten of the eleven rows.
 *
 * The AGB row is 0x08000000 and the slab is carved out of the application
 * heap, which is also 0x08000000, so the two overlap and no test can separate
 * them. The AGB row is therefore not translated here at all, and what makes
 * that safe is that nothing hands MI an AGB address: HW_CTRDG_ROM is redefined
 * to a host pointer, so the cartridge probe dereferences host memory directly.
 *
 * Trapping on the part of that window the slab does not cover was tried and is
 * wrong: the heap below the slab is live, ordinary host memory, and the first
 * frame put a buffer from it through a fill. There is no sub-range that is
 * guest-only. If a raw AGB address ever does reach a memory primitive it will
 * be written to the heap quietly, which is the one blind spot in this file.
 *
 * Unmapped is not an error here. A VRAM address with no bank behind it is a
 * write hardware drops and a read that gives zeros, so a run of nothing is
 * skipped rather than trapped, and counted. An address in no row is a hole in
 * the DS map, which hardware faults on, and so does this.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "3ds_fault.h"
#include "3ds_guest.h"
#include "3ds_guest_map.h"
#include "3ds_mi_host.h"

/* Sides of a copy are walked in chunks, and an unmapped source has to read as
 * zeros from somewhere. One block of them, sized to the placement grain so a
 * chunk is never cut short by this buffer before it is cut short by the map. */
#define MI_HOST_ZERO_BYTES 0x4000u
static const uint8_t sZero[MI_HOST_ZERO_BYTES];

static unsigned long sTranslated;
static unsigned long sDropped;

unsigned long mi_host_translated(void) { return sTranslated; }
unsigned long mi_host_dropped(void) { return sDropped; }

void mi_host_reset_counts(void)
{
    sTranslated = 0;
    sDropped = 0;
}

/*
 * Non-zero when `p` is a DS address rather than a host pointer. `size` is the
 * length of the access it heads, so a range that starts in the map and ends
 * past the end of it is caught here rather than half-performed.
 */
int mi_host_is_guest(const void *p, uint32_t size)
{
    /*
     * uintptr_t and not uint32_t, and the reason is the build machine rather
     * than the console. A 3DS pointer is 32 bits and the two are the same
     * there; the host that runs this file's self-test is LP64, and truncating
     * a stack address to 32 bits lands it anywhere, including under the
     * cartridge slot, where it would be taken for a guest address and
     * translated. Same class as the bounds-first rule in armrec_guest_addr().
     */
    uintptr_t a = (uintptr_t)p;
    uintptr_t agb = guest_map[GUEST_R_AGB].base;

    if (a >= agb) {
        return 0;
    }

    if (guest_region_index((uint32_t)a) < 0) {
        return 0; /* below the map entirely: a host pointer, or a null one. */
    }

    /* In the map at the front and out of it at the back is a range walking off
     * a region, which hardware faults on partway through. Say so now. */
    if (size != 0 && guest_region_index((uint32_t)a + size - 1u) < 0) {
        fault_stop("MI: range leaves the memory map",
                   "a fill or copy runs past the end of its region");
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* The two walks                                                       */
/* ------------------------------------------------------------------ */

typedef void (*mi_fill_fn)(uint32_t data, void *dest, uint32_t size, void *ctx);
typedef void (*mi_copy_fn)(const void *src, void *dest, uint32_t size, void *ctx);

/*
 * The tile cache's tile cache, weak for the reason hostmap_flush() is weak in
 * 3ds_vram.c: the self-test binary and the host harnesses link this file
 * without that one. Every bulk write to guest memory reports itself here,
 * which is how a cached tile learns its pixels have been replaced, and this
 * is the *only* way it can learn, so a tile whose pixels are written by a
 * plain guest store rather than by one of these functions would go stale.
 * This game loads its tiles and palettes through the SDK's GX_Load* calls,
 * which are these; the GPU present's PRESENT_VERIFY is what would catch it if some
 * screen did not.
 */
extern void tile_cache_dirty(uint32_t addr, uint32_t bytes) __attribute__((weak));
/*
 * ...AND the 3D rasterizer's TEXTURE CACHE, for exactly the same reason and through exactly
 * the same door. The texture converter built this entry point and worked out what it has to
 * key on, the bank rather than the texture slot, because the SDK loads a
 * texture with the bank mapped into LCDC, and then nothing called it. A
 * texture cache that is never told anything was written never goes stale: it
 * converted each address once and kept that image for the rest of the run, so
 * a room drew with the textures of whatever had been at those addresses
 * before. It looked like right geometry with the wrong pictures on it, which
 * is what the 3D rasterizer's pixel diff was built to see and what it saw first.
 */
extern void tex3d_cache_dirty(uint32_t addr, uint32_t bytes) __attribute__((weak));

static void mi_dirty(uint32_t guest, uint32_t size)
{
    if (tile_cache_dirty != NULL) {
        tile_cache_dirty(guest, size);
    }
    if (tex3d_cache_dirty != NULL) {
        tex3d_cache_dirty(guest, size);
    }
}

static void fill_walk(void *dest, uint32_t size, uint32_t data, mi_fill_fn op,
                      void *ctx)
{
    uint32_t guest = (uint32_t)(uintptr_t)dest;
    uint32_t left = size;

    sTranslated++;
    mi_dirty(guest, size);
    while (left != 0) {
        void *host;
        uint32_t n = guest_span(guest, left, &host);

        if (host != NULL) {
            op(data, host, n, ctx);
        } else {
            sDropped++;
        }
        guest += n;
        left -= n;
    }
}

static void copy_walk(const void *src, void *dest, uint32_t size, int srcGuest,
                      int destGuest, mi_copy_fn op, void *ctx)
{
    uint32_t sg = (uint32_t)(uintptr_t)src;
    uint32_t dg = (uint32_t)(uintptr_t)dest;
    const uint8_t *sh = (const uint8_t *)src;
    uint8_t *dh = (uint8_t *)dest;
    uint32_t left = size;

    sTranslated++;
    if (destGuest) {
        mi_dirty(dg, size);
    }
    while (left != 0) {
        const uint8_t *sp = sh;
        uint8_t *dp = dh;
        uint32_t n = left;

        if (srcGuest) {
            void *h;
            uint32_t run = guest_span(sg, n, &h);

            n = run;
            sp = (const uint8_t *)h;
            if (sp == NULL) {
                /* Nothing behind it: hardware reads zeros. */
                sp = sZero;
                if (n > MI_HOST_ZERO_BYTES) {
                    n = MI_HOST_ZERO_BYTES;
                }
            }
        }
        if (destGuest) {
            void *h;
            uint32_t run = guest_span(dg, n, &h);

            if (run < n) {
                n = run;
            }
            dp = (uint8_t *)h;
        }

        if (dp != NULL) {
            op(sp, dp, n, ctx);
        } else {
            sDropped++;
        }

        left -= n;
        sg += n;
        dg += n;
        if (!srcGuest) {
            sh += n;
        }
        if (!destGuest) {
            dh += n;
        }
    }
}

/* ------------------------------------------------------------------ */
/* The wrappers                                                        */
/* ------------------------------------------------------------------ */

/*
 * One shape each. The real function does the bytes, the wrapper only decides
 * where they go, so a call with no guest address in it costs one compare per
 * pointer and then the call it would have made anyway.
 */

#define MI_WRAP_FILL(name)                                                    \
    extern void __real_##name(uint32_t data, void *destp, uint32_t size);     \
    static void name##_run(uint32_t d, void *h, uint32_t n, void *ctx)        \
    {                                                                         \
        (void)ctx;                                                            \
        __real_##name(d, h, n);                                               \
    }                                                                         \
    void __wrap_##name(uint32_t data, void *destp, uint32_t size)             \
    {                                                                         \
        if (!mi_host_is_guest(destp, size)) {                                 \
            __real_##name(data, destp, size);                                 \
            return;                                                           \
        }                                                                     \
        fill_walk(destp, size, data, name##_run, NULL);                       \
    }

#define MI_WRAP_COPY(name)                                                    \
    extern void __real_##name(const void *srcp, void *destp, uint32_t size);  \
    static void name##_run(const void *s, void *d, uint32_t n, void *ctx)     \
    {                                                                         \
        (void)ctx;                                                            \
        __real_##name(s, d, n);                                               \
    }                                                                         \
    void __wrap_##name(const void *srcp, void *destp, uint32_t size)          \
    {                                                                         \
        int sg = mi_host_is_guest(srcp, size);                                \
        int dg = mi_host_is_guest(destp, size);                               \
                                                                              \
        if (!sg && !dg) {                                                     \
            __real_##name(srcp, destp, size);                                 \
            return;                                                           \
        }                                                                     \
        copy_walk(srcp, destp, size, sg, dg, name##_run, NULL);               \
    }

MI_WRAP_FILL(MIi_CpuClear16)
MI_WRAP_FILL(MIi_CpuClear32)
MI_WRAP_FILL(MIi_CpuClearFast)

MI_WRAP_COPY(MIi_CpuCopy16)
MI_WRAP_COPY(MIi_CpuCopy32)
MI_WRAP_COPY(MIi_CpuCopyFast)
MI_WRAP_COPY(MI_CpuCopy8)

/* MI_CpuFill8's data is a byte and its argument order is the other one. */
extern void __real_MI_CpuFill8(void *dest, uint8_t data, uint32_t size);

static void MI_CpuFill8_run(uint32_t d, void *h, uint32_t n, void *ctx)
{
    (void)ctx;
    __real_MI_CpuFill8(h, (uint8_t)d, n);
}

void __wrap_MI_CpuFill8(void *dest, uint8_t data, uint32_t size)
{
    if (!mi_host_is_guest(dest, size)) {
        __real_MI_CpuFill8(dest, data, size);
        return;
    }
    fill_walk(dest, size, data, MI_CpuFill8_run, NULL);
}

/*
 * Not wrapped, and this is the reason. `MIi_CpuSend32`, `MI_DmaSend16/32` and
 * the three `MI_SendGXCommand` forms do not walk a destination; they stream
 * words at one fixed device address, which on a DS is an I/O register and here
 * is the geometry FIFO. `pc_gpu3d_store_through()` is handed that address as
 * the DS writes it and decides from it; translating it first would hand the GX
 * layer a slab pointer it does not recognise and turn a display list into a
 * trap. Their sources are display lists in main RAM, which this map already
 * gives a host address.
 */

/* The fixed-size copies. Same shape, size baked in. */
#define MI_WRAP_COPY_N(name, bytes)                                           \
    extern void __real_##name(const void *pSrc, void *pDest);                 \
    static void name##_run(const void *s, void *d, uint32_t n, void *ctx)     \
    {                                                                         \
        (void)ctx;                                                            \
        memcpy(d, s, n);                                                      \
    }                                                                         \
    void __wrap_##name(const void *pSrc, void *pDest)                         \
    {                                                                         \
        int sg = mi_host_is_guest(pSrc, (bytes));                             \
        int dg = mi_host_is_guest(pDest, (bytes));                            \
                                                                              \
        if (!sg && !dg) {                                                     \
            __real_##name(pSrc, pDest);                                       \
            return;                                                           \
        }                                                                     \
        copy_walk(pSrc, pDest, (bytes), sg, dg, name##_run, NULL);            \
    }

MI_WRAP_COPY_N(MI_Copy16B, 16u)
MI_WRAP_COPY_N(MI_Copy32B, 32u)
MI_WRAP_COPY_N(MI_Copy36B, 36u)
MI_WRAP_COPY_N(MI_Copy48B, 48u)
MI_WRAP_COPY_N(MI_Copy64B, 64u)

extern void __real_MI_Zero36B(void *pDest);

static void MI_Zero36B_run(uint32_t d, void *h, uint32_t n, void *ctx)
{
    (void)d;
    (void)ctx;
    memset(h, 0, n);
}

void __wrap_MI_Zero36B(void *pDest)
{
    if (!mi_host_is_guest(pDest, 36u)) {
        __real_MI_Zero36B(pDest);
        return;
    }
    fill_walk(pDest, 36u, 0u, MI_Zero36B_run, NULL);
}

/* ------------------------------------------------------------------ */
/* DMA                                                                 */
/* ------------------------------------------------------------------ */

/*
 * The DMA entry points carry a channel number the port ignores, so each is the
 * copy or fill shape with one argument in front.
 *
 * The async forms need their own, and the reason is a property of `--wrap`
 * rather than of this port: the linker redirects UNDEFINED references, so a
 * call from `MI_DmaCopy16Async` to `MI_DmaCopy16`; both in `pc_dma.o`, one
 * resolved inside the object, is not redirected and would reach the real
 * function with an untranslated pointer. Each async form therefore calls this
 * file's own wrapper and then the callback, once, which is also what keeps the
 * callback from firing once per contiguous run.
 */
typedef void (*mi_host_dma_cb)(void *);

#define MI_WRAP_DMA_COPY(name)                                                \
    extern void __real_##name(uint32_t dmaNo, const void *src, void *dest,    \
                              uint32_t size);                                 \
    static uint32_t name##_ch;                                                \
    static void name##_run(const void *s, void *d, uint32_t n, void *ctx)     \
    {                                                                         \
        (void)ctx;                                                            \
        __real_##name(name##_ch, s, d, n);                                    \
    }                                                                         \
    void __wrap_##name(uint32_t dmaNo, const void *src, void *dest,           \
                       uint32_t size)                                         \
    {                                                                         \
        int sg = mi_host_is_guest(src, size);                                 \
        int dg = mi_host_is_guest(dest, size);                                \
                                                                              \
        if (!sg && !dg) {                                                     \
            __real_##name(dmaNo, src, dest, size);                            \
            return;                                                           \
        }                                                                     \
        name##_ch = dmaNo;                                                    \
        copy_walk(src, dest, size, sg, dg, name##_run, NULL);                 \
    }

#define MI_WRAP_DMA_FILL(name, dtype)                                         \
    extern void __real_##name(uint32_t dmaNo, void *dest, dtype data,         \
                              uint32_t size);                                 \
    static uint32_t name##_ch;                                                \
    static void name##_run(uint32_t d, void *h, uint32_t n, void *ctx)        \
    {                                                                         \
        (void)ctx;                                                            \
        __real_##name(name##_ch, h, (dtype)d, n);                             \
    }                                                                         \
    void __wrap_##name(uint32_t dmaNo, void *dest, dtype data, uint32_t size) \
    {                                                                         \
        if (!mi_host_is_guest(dest, size)) {                                  \
            __real_##name(dmaNo, dest, data, size);                           \
            return;                                                           \
        }                                                                     \
        name##_ch = dmaNo;                                                    \
        fill_walk(dest, size, data, name##_run, NULL);                        \
    }

MI_WRAP_DMA_COPY(MI_DmaCopy16)
MI_WRAP_DMA_COPY(MI_DmaCopy32)

MI_WRAP_DMA_FILL(MI_DmaFill16, uint16_t)
MI_WRAP_DMA_FILL(MI_DmaFill32, uint32_t)

#define MI_WRAP_DMA_ASYNC_COPY(name)                                          \
    void __wrap_##name##Async(uint32_t dmaNo, const void *src, void *dest,    \
                              uint32_t size, mi_host_dma_cb cb, void *arg)    \
    {                                                                         \
        __wrap_##name(dmaNo, src, dest, size);                                \
        if (cb != NULL) {                                                     \
            cb(arg);                                                          \
        }                                                                     \
    }

#define MI_WRAP_DMA_ASYNC_FILL(name, dtype)                                   \
    void __wrap_##name##Async(uint32_t dmaNo, void *dest, dtype data,         \
                              uint32_t size, mi_host_dma_cb cb, void *arg)    \
    {                                                                         \
        __wrap_##name(dmaNo, dest, data, size);                               \
        if (cb != NULL) {                                                     \
            cb(arg);                                                          \
        }                                                                     \
    }

MI_WRAP_DMA_ASYNC_COPY(MI_DmaCopy16)
MI_WRAP_DMA_ASYNC_COPY(MI_DmaCopy32)
MI_WRAP_DMA_ASYNC_FILL(MI_DmaFill16, uint16_t)
MI_WRAP_DMA_ASYNC_FILL(MI_DmaFill32, uint32_t)

/* ------------------------------------------------------------------ */
/* Decompression                                                       */
/* ------------------------------------------------------------------ */

/*
 * The LZ decoders cannot be walked. Their back-references read bytes they have
 * already written, at distances that cross any block boundary the walk would
 * cut on, so a chunked destination gives a decoder that reads the wrong bytes
 * rather than one that writes to the wrong place. The 16-bit decoder exists
 * precisely BECAUSE its destination is usually VRAM, so this is not a corner.
 *
 * The output length is the top 24 bits of the stream header, so the size is
 * known before the first byte: decompress into a host buffer and walk that out
 * afterwards. The buffer is the only allocation this file makes and a failure
 * to get one stops the console rather than truncating a texture.
 */
extern void *malloc(size_t n);
extern void free(void *p);

#define MI_WRAP_LZ(name)                                                      \
    extern void __real_##name(const void *srcp, void *destp);                 \
    static void name##_run(const void *s, void *d, uint32_t n, void *ctx)     \
    {                                                                         \
        (void)ctx;                                                            \
        memcpy(d, s, n);                                                      \
    }                                                                         \
    void __wrap_##name(const void *srcp, void *destp)                         \
    {                                                                         \
        uint32_t header;                                                      \
        uint32_t out;                                                         \
        void *bounce;                                                         \
                                                                              \
        if (!mi_host_is_guest(destp, 0u)) {                                   \
            __real_##name(srcp, destp);                                       \
            return;                                                           \
        }                                                                     \
        memcpy(&header, srcp, 4);                                             \
        out = header >> 8;                                                    \
        bounce = malloc(out != 0 ? out : 1u);                                 \
        if (bounce == NULL) {                                                 \
            fault_stop("MI: no room to decompress",                           \
                       "a compressed stream aimed at video memory");          \
        }                                                                     \
        __real_##name(srcp, bounce);                                          \
        copy_walk(bounce, destp, out, 0, 1, name##_run, NULL);                \
        free(bounce);                                                         \
    }

MI_WRAP_LZ(MI_UncompressLZ8)
MI_WRAP_LZ(MI_UncompressLZ16)

/* ------------------------------------------------------------------ */
/* The cartridge                                                       */
/* ------------------------------------------------------------------ */

/*
 * Not a memory primitive, and here for exactly the same reason as one.
 * `CARDi_ReadRom` is the single point every ROM read goes through, and
 * pc/src/pc_card_rom.c answers it with a `read()` into the caller's pointer.
 * The overlay loader's pointer is the overlay's DS RAM address, straight out
 * of the ROM's own overlay table, so the read lands wherever that says.
 *
 * On this console it does not merely miss; it goes out through the file
 * system service, which means the kernel writes the buffer into the process
 * on the app's behalf, and the whole 300 KB of an overlay image was being
 * written to an address the process does not have. Measured as 95 block
 * writes at 0x021D0D80 on the first boot after the fills were fixed, with the
 * app parked in svcWaitSynchronization while the service did it.
 *
 * The walk is the fill's, with the ROM offset advancing alongside the
 * destination, and the callback fires once at the end rather than once per
 * run.
 */
typedef void (*mi_host_card_cb)(void *);

extern void __real_CARDi_ReadRom(uint32_t dma, const void *src, void *dst,
                                 uint32_t len, mi_host_card_cb cb, void *arg,
                                 int isAsync);

void __wrap_CARDi_ReadRom(uint32_t dma, const void *src, void *dst,
                          uint32_t len, mi_host_card_cb cb, void *arg,
                          int isAsync)
{
    uint32_t off;
    uint32_t guest;
    uint32_t left;

    if (!mi_host_is_guest(dst, len)) {
        __real_CARDi_ReadRom(dma, src, dst, len, cb, arg, isAsync);
        return;
    }

    off = (uint32_t)(uintptr_t)src;
    guest = (uint32_t)(uintptr_t)dst;
    left = len;
    sTranslated++;

    while (left != 0) {
        void *host;
        uint32_t n = guest_span(guest, left, &host);

        if (host != NULL) {
            __real_CARDi_ReadRom(dma, (const void *)(uintptr_t)off, host, n,
                                 NULL, NULL, isAsync);
        } else {
            sDropped++;
        }
        guest += n;
        off += n;
        left -= n;
    }

    if (cb != NULL) {
        cb(arg);
    }
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

/*
 * What this checks is the SPLIT, not the bytes: where each run of a fill or a
 * copy ends up, whether a run with nothing behind it is dropped instead of
 * written, and whether a host pointer is left alone. The bytes themselves are
 * pc/src/pc_mi.c's and are unchanged by any of this; each run reaches the
 * real function, so their fidelity is that file's own test's business.
 *
 * The interesting destination is a VRAM window, because it is the only place
 * where consecutive guest addresses are not consecutive host ones. The test
 * builds two of them with the bank registers: an LCDC view with a bank missing
 * from the middle (mapped, nothing, mapped) and a BG view of two banks that
 * are adjacent in the window and far apart in the store.
 */
#ifdef MI_HOST_SELFTEST_VERBOSE
#include <stdio.h>
#define FAILNOTE() fprintf(stderr, "  3ds_mi_host.c:%d failed\n", __LINE__)
#else
#define FAILNOTE() ((void)0)
#endif

#define CHECK(cond)                                                           \
    do {                                                                      \
        ran++;                                                                \
        if (!(cond)) {                                                        \
            failed++;                                                         \
            FAILNOTE();                                                       \
        }                                                                     \
    } while (0)

extern void __wrap_MIi_CpuClearFast(uint32_t data, void *destp, uint32_t size);
extern void __wrap_MIi_CpuCopy32(const void *srcp, void *destp, uint32_t size);

/* Weak, so a test that links this file without the bank model still builds;
 * the VRAM half of the test then does not run. */
extern uint32_t vram_cnt_addr(int bank) __attribute__((weak));
extern void vram_touch(void) __attribute__((weak));

static void mi_selftest_bank(int bank, uint8_t cnt)
{
    uint8_t *reg = (uint8_t *)armrec_host_ptr(vram_cnt_addr(bank));

    if (reg != NULL) {
        *reg = cnt;
    }
}

int mi_host_selftest(int *ranOut)
{
    int ran = 0;
    int failed = 0;
    uint8_t hostbuf[64];
    uint8_t *main_row = (uint8_t *)armrec_host_ptr(0x02000000u);
    unsigned long dropped0;
    int i;

    if (main_row == NULL) {
        if (ranOut != NULL) {
            *ranOut = 0;
        }
        return 1;
    }

    /* The predicate, over one address from each side of the line. */
    CHECK(mi_host_is_guest(hostbuf, sizeof hostbuf) == 0);
    CHECK(mi_host_is_guest(main_row, 4u) == 0);
    CHECK(mi_host_is_guest((void *)0x02000000u, 4u) == 1);
    CHECK(mi_host_is_guest((void *)0x06000000u, 4u) == 1);
    CHECK(mi_host_is_guest((void *)0x00000010u, 4u) == 0);

    /* A host destination is passed straight through. */
    memset(hostbuf, 0, sizeof hostbuf);
    __wrap_MIi_CpuClearFast(0x11111111u, hostbuf, sizeof hostbuf);
    CHECK(hostbuf[0] == 0x11u && hostbuf[sizeof hostbuf - 1] == 0x11u);

    /* A flat guest one lands in that row's backing. */
    __wrap_MIi_CpuClearFast(0x22222222u, (void *)0x02000100u, 64u);
    CHECK(main_row[0x100] == 0x22u && main_row[0x13F] == 0x22u);
    CHECK(main_row[0xFF] == 0x00u || main_row[0xFF] != 0x22u);

    if (vram_cnt_addr == NULL || vram_touch == NULL) {
        if (ranOut != NULL) {
            *ranOut = ran;
        }
        return failed;
    }

    /*
     * LCDC with a hole. A and C are the whole 128 KB each at lcdc, b is off:
     * A fill over all three writes the first and the third and drops the
     * middle, the way hardware drops a write to a bank that is not there.
     */
    for (i = 0; i < 9; i++) {
        mi_selftest_bank(i, 0u);
    }
    mi_selftest_bank(0, 0x80u); /* A, LCDC */
    mi_selftest_bank(2, 0x80u); /* C, LCDC */
    vram_touch();

    {
        uint8_t *a = (uint8_t *)armrec_host_ptr(0x06800000u);
        uint8_t *c = (uint8_t *)armrec_host_ptr(0x06840000u);

        CHECK(a != NULL);
        CHECK(c != NULL);
        CHECK(armrec_host_ptr(0x06820000u) == NULL);

        dropped0 = mi_host_dropped();
        __wrap_MIi_CpuClearFast(0x33333333u, (void *)0x06800000u, 0x60000u);
        CHECK(mi_host_dropped() == dropped0 + 1u);
        if (a != NULL && c != NULL) {
            CHECK(a[0] == 0x33u && a[0x1FFFFu] == 0x33u);
            CHECK(c[0] == 0x33u && c[0x1FFFFu] == 0x33u);
        }
    }

    /*
     * Two banks adjacent in a window and far apart in the store. A copy over
     * the seam is one call and two runs, and the halves have to arrive whole.
     */
    for (i = 0; i < 9; i++) {
        mi_selftest_bank(i, 0u);
    }
    mi_selftest_bank(0, 0x81u);        /* A, BG, offset 0 */
    mi_selftest_bank(2, (uint8_t)0x89u); /* C, BG, offset 1 */
    vram_touch();

    {
        uint8_t *lo = (uint8_t *)armrec_host_ptr(0x06000000u);
        uint8_t *hi = (uint8_t *)armrec_host_ptr(0x06020000u);
        uint8_t src[128];

        CHECK(lo != NULL);
        CHECK(hi != NULL);
        CHECK(lo == NULL || hi == NULL || hi != lo + 0x20000u);

        for (i = 0; i < (int)sizeof src; i++) {
            src[i] = (uint8_t)(0x40 + i);
        }
        /* Straddle the seam: the last 64 bytes of A and the first 64 of C. */
        __wrap_MIi_CpuCopy32(src, (void *)(0x06020000u - 64u), sizeof src);
        if (lo != NULL && hi != NULL) {
            CHECK(lo[0x20000u - 64u] == 0x40u);
            CHECK(lo[0x20000u - 1u] == (uint8_t)(0x40 + 63));
            CHECK(hi[0] == (uint8_t)(0x40 + 64));
            CHECK(hi[63] == (uint8_t)(0x40 + 127));
        }
    }

    /* Leave no bank enabled, which is how a slab starts. */
    for (i = 0; i < 9; i++) {
        mi_selftest_bank(i, 0u);
    }
    vram_touch();

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
