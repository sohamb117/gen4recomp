/*
 * 3ds/tests/mi_host.c: the MI/DMA walk, on the host.
 *
 * 3ds_mi_host.c is pure C over the translation and the bank model, so the
 * build machine runs the same mi_host_selftest() the 3dsx runs: a host
 * destination left alone, a flat guest one, a fill across a VRAM window with a
 * bank missing from the middle, and a copy across a seam between two banks
 * that are adjacent in the window and far apart in the store.
 *
 * THE `__real_` functions are stubs here, and they are the same three lines
 * the real ones are: on the console the linker points these at pc/src/pc_mi.c,
 * whose bodies are a transcription of the SDK's asm and whose fidelity is that
 * file's own test's business. What is under test here is where each run of a
 * split goes, not what the bytes of one run are, and a plain word loop is
 * enough destination to see that.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "3ds_guest.h"
#include "3ds_guest_map.h"
#include "3ds_mi_host.h"
#include "3ds_vram.h"

void fault_stop(const char *top, const char *bottom)
{
    printf("  fault_stop: %s, %s\n", top ? top : "", bottom ? bottom : "");
    exit(3);
}

void __real_MIi_CpuClear16(uint32_t data, void *destp, uint32_t size)
{
    uint8_t *d = destp;
    uint32_t i;

    for (i = 0; i < size; i += 2) {
        memcpy(d + i, &data, 2);
    }
}

void __real_MIi_CpuClear32(uint32_t data, void *destp, uint32_t size)
{
    uint8_t *d = destp;
    uint8_t *end = d + size;

    while (d < end) {
        memcpy(d, &data, 4);
        d += 4;
    }
}

void __real_MIi_CpuClearFast(uint32_t data, void *destp, uint32_t size)
{
    __real_MIi_CpuClear32(data, destp, size);
}

void __real_MIi_CpuCopy16(const void *srcp, void *destp, uint32_t size)
{
    memcpy(destp, srcp, size);
}

void __real_MIi_CpuCopy32(const void *srcp, void *destp, uint32_t size)
{
    memcpy(destp, srcp, size);
}

void __real_MIi_CpuCopyFast(const void *srcp, void *destp, uint32_t size)
{
    memcpy(destp, srcp, size);
}

void __real_MI_CpuCopy8(const void *srcp, void *destp, uint32_t size)
{
    memcpy(destp, srcp, size);
}

void __real_MI_CpuFill8(void *dest, uint8_t data, uint32_t size)
{
    memset(dest, data, size);
}

void __real_MI_Copy16B(const void *s, void *d) { memcpy(d, s, 16); }
void __real_MI_Copy32B(const void *s, void *d) { memcpy(d, s, 32); }
void __real_MI_Copy36B(const void *s, void *d) { memcpy(d, s, 36); }
void __real_MI_Copy48B(const void *s, void *d) { memcpy(d, s, 48); }
void __real_MI_Copy64B(const void *s, void *d) { memcpy(d, s, 64); }
void __real_MI_Zero36B(void *d) { memset(d, 0, 36); }

void __real_MI_UncompressLZ8(const void *s, void *d) { (void)s; (void)d; }
void __real_MI_UncompressLZ16(const void *s, void *d) { (void)s; (void)d; }

void __real_MI_DmaCopy16(uint32_t n, const void *s, void *d, uint32_t z)
{
    (void)n;
    memcpy(d, s, z);
}

void __real_MI_DmaCopy32(uint32_t n, const void *s, void *d, uint32_t z)
{
    (void)n;
    memcpy(d, s, z);
}

void __real_MI_DmaFill16(uint32_t n, void *d, uint16_t data, uint32_t z)
{
    (void)n;
    __real_MIi_CpuClear16(data, d, z);
}

void __real_MI_DmaFill32(uint32_t n, void *d, uint32_t data, uint32_t z)
{
    (void)n;
    __real_MIi_CpuClear32(data, d, z);
}

/* The cartridge read is wrapped for the same reason and is not MI. Nothing
 * here calls it (the self-test has no ROM) but the wrapper references it,
 * so the link needs a name. */
void __real_CARDi_ReadRom(uint32_t dma, const void *src, void *dst,
                          uint32_t len, void (*cb)(void *), void *arg,
                          int isAsync)
{
    (void)dma;
    (void)src;
    (void)arg;
    (void)isAsync;
    memset(dst, 0, len);
    if (cb != NULL) {
        cb(arg);
    }
}

int main(void)
{
    void *slab;
    int ran = 0;
    int failed;

    if (posix_memalign(&slab, 0x1000, GUEST_SLAB_BYTES) != 0 || slab == NULL) {
        printf("mi_host: no %u bytes for a slab\n", (unsigned)GUEST_SLAB_BYTES);
        return 2;
    }
    memset(slab, 0, GUEST_SLAB_BYTES);

    guest_bind(slab);
    vram_init();

    failed = mi_host_selftest(&ran);
    printf("mi_host: %d checks, %d failed\n", ran, failed);

    guest_bind(NULL);
    free(slab);
    return failed != 0 || ran == 0;
}
