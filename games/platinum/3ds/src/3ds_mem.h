/*
 * 3ds/src/3ds_mem.h: the one allocation the guest lives in.
 *
 * 3ds/include/3ds_guest_map.h says what the slab contains; this says where it
 * is. One block, taken once at startup, never moved and never grown: every
 * host pointer the port hands to guest code is `slab + offset`, so a realloc
 * would invalidate pointers the game has already stored in its own structures
 * and there would be no way to find them again.
 *
 * Translation (guest address -> host pointer) does not live here.
 * What lives here is the allocation and the numbers that say whether it fits:
 * The budget has to answer "does the game run in 64 MB" and it cannot do that from a
 * slab size alone.
 */

#ifndef POKEPLATINUM_3DS_MEM_H
#define POKEPLATINUM_3DS_MEM_H

#include <stdint.h>

/*
 * What the allocation cost and what was left. Filled by mem_init(), constant
 * afterwards. Sizes are bytes.
 *
 * `app_free_*` is the console-wide application region, and under hbmenu it is
 * measured 0 both before and after; the launcher has already been charged
 * for the whole region and hands the process its heap out of that. The number
 * that says whether the game fits is therefore `heap_size - heap_used`, and
 * that is what the budget has to quote.
 */
struct mem_slab {
    uint8_t *base;            /* page-aligned; NULL until mem_init() succeeds */
    uint32_t bytes;           /* GUEST_SLAB_BYTES */
    uint32_t app_free_before; /* osGetMemRegionFree(MEMREGION_APPLICATION) */
    uint32_t app_free_after;
    uint32_t app_region;      /* osGetMemRegionSize(): which SYSMODE we got */
    uint32_t heap_size;       /* envGetHeapSize(): the process heap */
    uint32_t heap_used;       /* mallinfo().uordblks, after the slab */
    uint32_t linear_free;     /* linearSpaceFree(): the framebuffers' heap */
};

/*
 * Take the slab and zero it. Returns 0, or -1 with mem_strerror() set; a
 * failure is fatal to the port and the caller must say so on screen rather
 * than carrying on with a NULL base.
 */
int mem_init(void);

/* Give it back. Safe before mem_init() and safe twice. */
void mem_exit(void);

/* The numbers mem_init() recorded. Never NULL; zeroed before mem_init(). */
const struct mem_slab *mem_info(void);

/*
 * Host pointer to the backing of one GUEST_R_* row. This is bookkeeping, not
 * translation: it answers "where did region N land", not "what host address is
 * guest 0x04000130". NULL before mem_init() or for an index out of range.
 */
void *mem_region_ptr(int index);

/* Why mem_init() failed. "" if it has not. */
const char *mem_strerror(void);

/*
 * The memory budget, on the card.
 *
 * The struct above is what start-up cost, and that is only half the question:
 * The game takes its own heaps out of guest memory as it boots, and the
 * console-side numbers that matter are the ones at the title screen and not
 * the ones before NitroMain. So this writes both, what mem_init() recorded,
 * and a fresh sample taken at the moment of the call, and mem_step() calls
 * it every MEM_REPORT_EVERY frames so whatever scene a run reached, the file
 * holds that scene's numbers.
 *
 * It is a file rather than a screen because the answer has to leave the
 * console. mallinfo() walks the heap's free list, which is why this is not
 * sampled every frame.
 */
#define MEM_REPORT_PATH "sdmc:/3ds/pokeplatinum/mem-report.txt"
#define MEM_REPORT_EVERY 600ul

int mem_report(const char *path);
void mem_step(unsigned long frame);

#endif /* POKEPLATINUM_3DS_MEM_H */
