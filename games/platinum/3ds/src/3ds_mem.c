/*
 * 3ds/src/3ds_mem.c: allocate the guest slab, once.
 *
 * WHY memalign AND NOT linearAlloc. The linear heap is the one the GPU can
 * DMA from, and on an Old 3DS it is a slice of the same 64 MB the application
 * heap comes out of; what one takes the other cannot have. The framebuffers
 * gfxInitDefault() hands out are linear, citro3d's command buffer and every
 * texture will be linear, and nothing in the guest slab is read by the PICA:
 * The DS's own display hardware is a software model in pc/hw, and the picture
 * reaches the LCDs as a blit out of pc_video's surfaces, not as a
 * texture upload out of guest VRAM. So the slab comes from the application
 * heap and leaves the linear heap alone. If the GPU work ever makes the PICA read
 * guest VRAM directly, that one region moves, not this whole allocation.
 *
 * PAGE-ALIGNED, because every row in the map is a 4 KB multiple: align the
 * slab and every region is aligned too, which is what an SPU DMA out of the
 * port window and a 16 KB VRAM block both quietly assume.
 *
 * Zeroed. A DS does not hand the game uninitialised memory that varies run to
 * run; newlib's heap does. Zeroing 8 MB once at startup costs a few
 * milliseconds and buys determinism, the same reason armrec_mem_init()
 * zeroes its backing on PC.
 *
 * What the numbers mean, because the obvious one is useless.
 * osGetMemRegionFree(MEMREGION_APPLICATION) is a *system* figure: how much of
 * the console's application region no process has taken. Launched from hbmenu
 * it reads **0**, measured, both before and after the allocation, the
 * launcher is charged for the whole region and the process heap libctru
 * reserves before main (__ctru_heap_size) comes out of that charge. So it is
 * recorded, because the plan asked for it and because a 0 that is understood
 * is worth more than an absence, but it is not the budget. The budget is
 * `envGetHeapSize()` against `mallinfo().uordblks`: what this process was
 * given and what it has spent. The budget quotes those two.
 */

#include "3ds_mem.h"

/* By path and not by -I, for pc_sym.h's reason: pc/include is the game
 * chain's and this file is on the port chain. */
#include "../../pc/include/pc_overlay.h"

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "3ds_guest_map.h"
#include "3ds_sdcard.h"

#define MEM_SLAB_ALIGN 0x1000u

static struct mem_slab sSlab;
static char sError[96];
static unsigned long sReportFrame;

int mem_init(void)
{
    void *base;

    if (sSlab.base != NULL) {
        return 0;
    }

    sError[0] = '\0';
    sSlab.bytes = (uint32_t)GUEST_SLAB_BYTES;
    sSlab.app_free_before = osGetMemRegionFree(MEMREGION_APPLICATION);

    base = memalign(MEM_SLAB_ALIGN, GUEST_SLAB_BYTES);
    if (base == NULL) {
        snprintf(sError, sizeof sError, "no %u KB for the slab (heap free %u KB)",
                 (unsigned)(GUEST_SLAB_BYTES / 1024),
                 (unsigned)(sSlab.app_free_before / 1024));
        return -1;
    }

    memset(base, 0, GUEST_SLAB_BYTES);

    sSlab.base = base;
    sSlab.app_free_after = osGetMemRegionFree(MEMREGION_APPLICATION);
    sSlab.app_region = osGetMemRegionSize(MEMREGION_APPLICATION);
    sSlab.heap_size = envGetHeapSize();
    sSlab.heap_used = (uint32_t)mallinfo().uordblks;
    sSlab.linear_free = linearSpaceFree();
    return 0;
}

void mem_exit(void)
{
    if (sSlab.base != NULL) {
        free(sSlab.base);
        sSlab.base = NULL;
    }
}

const struct mem_slab *mem_info(void)
{
    return &sSlab;
}

void *mem_region_ptr(int index)
{
    if (sSlab.base == NULL || index < 0 || index >= GUEST_R_COUNT) {
        return NULL;
    }
    return sSlab.base + guest_map_offset(index);
}

const char *mem_strerror(void)
{
    return sError;
}

/*
 * The budget as a file. Two blocks: what start-up cost, and what the console
 * looks like now.
 *
 * `app-free` is 0 under hbmenu and that is the measured, understood answer;
 * the launcher is charged for the whole application region and this process's
 * heap comes out of that charge. It is written anyway, because a CIA launched
 * from the Home Menu is charged differently and this is the line that would
 * say so. The budget is `heap-size` against `heap-used-now`.
 */
int mem_report(const char *path)
{
    FILE *f = sd_open_write(path);

    if (f == NULL) {
        return -1;
    }
    fprintf(f, "slab-bytes %lu\n", (unsigned long)sSlab.bytes);
    fprintf(f, "slab-base %08lX\n", (unsigned long)(uintptr_t)sSlab.base);
    fprintf(f, "app-region %lu\n", (unsigned long)sSlab.app_region);
    fprintf(f, "app-free-before %lu\n", (unsigned long)sSlab.app_free_before);
    fprintf(f, "app-free-after %lu\n", (unsigned long)sSlab.app_free_after);
    fprintf(f, "heap-size %lu\n", (unsigned long)sSlab.heap_size);
    fprintf(f, "heap-used-at-boot %lu\n", (unsigned long)sSlab.heap_used);
    fprintf(f, "linear-free-at-boot %lu\n", (unsigned long)sSlab.linear_free);

    fprintf(f, "heap-used-now %lu\n", (unsigned long)mallinfo().uordblks);
    fprintf(f, "linear-free-now %lu\n", (unsigned long)linearSpaceFree());
    fprintf(f, "app-free-now %lu\n",
            (unsigned long)osGetMemRegionFree(MEMREGION_APPLICATION));
    /*
     * And whether the overlay statics are being reset. PC_TRACE_OVERLAY
     * is how that is watched on a host with an environment; this console has
     * none, so the totals come out here. `ov-slots -1` means no overlay has
     * loaded yet; 0 would mean the addresses did not resolve, which is the
     * state this whole path exists to leave behind.
     */
    {
        int slots;
        unsigned reloads, restores, bytes, dirty;

        pc_ov_totals(&slots, &reloads, &restores, &bytes, &dirty);
        fprintf(f, "ov-slots %d\n", slots);
        fprintf(f, "ov-loads %lu\n", (unsigned long)reloads);
        fprintf(f, "ov-restores %lu\n", (unsigned long)restores);
        fprintf(f, "ov-restored-bytes %lu\n", (unsigned long)bytes);
        fprintf(f, "ov-dirty-bytes %lu\n", (unsigned long)dirty);
    }

    fprintf(f, "frames %lu\n", sReportFrame);
    fclose(f);
    return 0;
}

/*
 * An INTERVAL and not a multiple. This was `frame % MEM_REPORT_EVERY`, and
 * the frame it is handed is the guest VBlank count; which does not step by
 * one on every published frame. A scripted run reached 2,760 frames and wrote
 * no report at all, because the count never landed on a multiple of six
 * hundred at the moment it was asked. Measuring the distance from the last
 * report instead cannot miss.
 */
void mem_step(unsigned long frame)
{
    static int written;

    if (sSlab.base == NULL) {
        return;
    }
    if (written && frame - sReportFrame < MEM_REPORT_EVERY) {
        return;
    }
    written = 1;
    sReportFrame = frame;
    (void)mem_report(MEM_REPORT_PATH);
}
