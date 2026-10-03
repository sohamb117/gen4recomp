/*
 * 3ds/src/3ds_guest.c: the translation, and nothing else.
 *
 * A linear scan, not a page table. Eleven rows, checked in order, on a
 * 268 MHz in-order ARM11: the loop is a compare and a branch per row and the
 * hot regions (main RAM at index 1) are near the front. The alternatives cost
 * more than they save at this size, a 4 KB page table over the addressable
 * range would be megabytes of pointers, and a hash needs a hash. If a profile
 * later says translation is hot, the fix is to stop calling it in that path
 * (identity-passing a pointer the caller already has), not to make the scan
 * cleverer.
 *
 * What is not translatable gets NULL, and the list matters more than the
 * arithmetic:
 *
 *   - the holes. 0x02400000-0x027E0000 is 4 MB of nothing between main RAM
 *     and the shared work area, and a DS faults there. So do we.
 *   - VRAM. 3ds_vram.c owns it; see the header.
 *   - the AGB slot above 128 KB. Only the probe is backed, not the cartridge,
 *     and how far the probe reaches was measured: the header at 0x08000000
 *     and the module-ID image word at 0x0801FFFE. An access above that is a
 *     probe that went further than anything this port has seen, and it must
 *     be visible.
 *   - anything below ITCM or above the AGB slot.
 *
 * There is no "close enough" here. Clamping into the nearest region would
 * turn a wild guest pointer into a silent write over real guest state, which
 * is the failure mode this port would be least able to debug.
 */

#include "3ds_guest.h"

#include <stddef.h>
#include <string.h>

#include "3ds_guest_map.h"

static uint8_t *sBase;

/*
 * What HW_REG_BASE expands to in a game translation unit. The SDK writes every
 * register as HW_REG_BASE + offset, so this one pointer is where all 329 of
 * them resolve; 3ds/include/nitro/hw/ARM9/mmap_global.h declares it and says
 * why. It is set here rather than in 3ds_io.c because binding the slab is the
 * moment the answer exists, and a register access before that must be a null
 * dereference rather than a write to a stale row.
 *
 * Declared without including the shadow: this file is on the port chain and
 * must not see the DS SDK. The type is written out so the two spellings can be
 * compared by eye, and 3ds/src/3ds_ioreg.c fails to link if they disagree.
 */
unsigned char *armrec_io_base;

/*
 * And the rows the memory map resolves through, for the same reason:
 * 3ds/include/3ds_hw_host.h turns HW_MAIN_MEM, the shared work area, the
 * palette and OAM into offsets from these. Main RAM and the shared area need
 * separate pointers because the SDK writes the shared area as
 * `HW_MAIN_MEM + 0x007ffxxx`, 8 MB past the start of a main memory this map
 * gives 4 MB, so no single base can put both ends where they belong.
 */
unsigned char *armrec_main_base;
unsigned char *armrec_shared_base;
unsigned char *armrec_palette_base;
unsigned char *armrec_oam_base;

/*
 * And the cartridge slot, which is the one row where a host pointer is a
 * safety measure rather than a convenience. On a DS 0x08000000 is the GBA
 * cartridge; on this console it is the application heap window, so the SDK's
 * `((CTRDGHeader *)HW_CTRDG_ROM)->isRomCode` would read live host memory and
 * the cartridge check would decide from it. 3ds/include/nitro/hw/ARM9/
 * mmap_global.h points HW_CTRDG_ROM here instead: 128 KB of zeros, which is
 * what an empty slot reads as. 4.3.
 */
unsigned char *armrec_agb_base;

void guest_bind(void *slab_base)
{
    sBase = (uint8_t *)slab_base;
    if (sBase != NULL) {
        armrec_io_base = (unsigned char *)(sBase + guest_map_offset(GUEST_R_IO));
        armrec_main_base = (unsigned char *)(sBase + guest_map_offset(GUEST_R_MAIN));
        armrec_shared_base =
            (unsigned char *)(sBase + guest_map_offset(GUEST_R_SHARED));
        armrec_palette_base =
            (unsigned char *)(sBase + guest_map_offset(GUEST_R_PALETTE));
        armrec_oam_base = (unsigned char *)(sBase + guest_map_offset(GUEST_R_OAM));
        armrec_agb_base = (unsigned char *)(sBase + guest_map_offset(GUEST_R_AGB));
    } else {
        armrec_io_base = NULL;
        armrec_main_base = NULL;
        armrec_shared_base = NULL;
        armrec_palette_base = NULL;
        armrec_oam_base = NULL;
        armrec_agb_base = NULL;
    }

    /*
     * There is no armrec_vram_base and there cannot be one. Nine banks appear
     * in five windows wherever VRAMCNT puts them; a base plus an offset cannot
     * say which. VRAM addresses stay as the DS writes them and go through
     * vram_host_ptr() one access at a time.
     */
}

void *guest_region_base(int index)
{
    if (sBase == NULL || index < 0 || index >= GUEST_R_COUNT) {
        return NULL;
    }
    return sBase + guest_map_offset(index);
}

/*
 * VRAM's answer, when there is one. 3ds_vram.c defines this; a test that links
 * only this file does not, and a weak reference is how the same object serves
 * both. NULL either way when no bank is mapped at the address, which is also
 * what a console gives, reads there are zeros and writes are dropped.
 */
extern void *vram_host_ptr(uint32_t guest) __attribute__((weak));

void *armrec_host_ptr(uint32_t guest)
{
    int i;

    if (sBase == NULL) {
        return NULL;
    }

    for (i = 0; i < GUEST_R_COUNT; i++) {
        const struct guest_region *r = &guest_map[i];
        uint32_t off;

        if (guest < r->base || guest - r->base >= r->size) {
            continue;
        }

        /* Which bank answers a VRAM address is VRAMCNT's business, so the
         * whole 16 MB window goes to the bank model rather than to the flat
         * arithmetic below. NULL when no bank is mapped there. */
        if (i == GUEST_R_VRAM) {
            return vram_host_ptr != NULL ? vram_host_ptr(guest) : NULL;
        }

        /* Inside the region's addresses, past the memory behind them, the
         * AGB slot above its 128 KB probe buffer. */
        off = guest - r->base;
        if (off >= r->backing) {
            return NULL;
        }
        return sBase + guest_map_offset(i) + off;
    }
    return NULL;
}

int guest_region_index(uint32_t guest)
{
    int i;

    for (i = 0; i < GUEST_R_COUNT; i++) {
        if (guest - guest_map[i].base < guest_map[i].size) {
            return i;
        }
    }
    return -1;
}

/*
 * 16 KB, because that is the grain the placement model works in: every VRAM
 * bank is a whole number of 16 KB units and every window mirrors on a
 * multiple of one, so a run that is contiguous at both ends of a block is
 * contiguous through it. The renderer measured the same grain for its per-pixel cache.
 * The flat rows are contiguous for their whole length and the loop below just
 * runs to `want` over them.
 */
#define GUEST_SPAN_GRAIN 0x4000u

uint32_t guest_span(uint32_t guest, uint32_t want, void **hostOut)
{
    uint8_t *p = (uint8_t *)armrec_host_ptr(guest);
    uint32_t run;

    if (hostOut != NULL) {
        *hostOut = p;
    }
    if (want == 0) {
        return 0;
    }

    /* To the end of the block this address is in, or to the end of the
     * request, whichever comes first. */
    run = GUEST_SPAN_GRAIN - (guest & (GUEST_SPAN_GRAIN - 1u));
    if (run > want) {
        run = want;
    }

    while (run < want) {
        uint8_t *q = (uint8_t *)armrec_host_ptr(guest + run);
        uint32_t step;

        /* Mapped and unmapped are different runs, and two mapped blocks are
         * the same run only if the second really is where the first ends. */
        if ((p == NULL) != (q == NULL)) {
            break;
        }
        if (p != NULL && q != p + run) {
            break;
        }

        step = want - run;
        if (step > GUEST_SPAN_GRAIN) {
            step = GUEST_SPAN_GRAIN;
        }
        run += step;
    }

    return run;
}

uint32_t armrec_guest_addr(const void *host)
{
    const uint8_t *p = (const uint8_t *)host;
    uint32_t slab_off;
    int i;

    /*
     * Bounds first, subtraction second. A 3DS pointer is 32 bits and the
     * difference always fits, but the host test that runs this code is LP64:
     * subtracting first and truncating to uint32_t lets a pointer 4 GB away
     * land inside the slab. The check that caught it was a stack address.
     */
    if (sBase == NULL || p < sBase || p >= sBase + GUEST_SLAB_BYTES) {
        return 0;
    }
    slab_off = (uint32_t)(size_t)(p - sBase);

    for (i = 0; i < GUEST_R_COUNT; i++) {
        uint32_t start = guest_map_offset(i);

        if (slab_off - start < guest_map[i].backing) {
            /* VRAM's backing is the bank store, and which guest address a
             * byte of it answers to depends on VRAMCNT. 3ds_vram.c answers this;
             * a fixed one would be wrong as soon as a bank moved. */
            if (i == GUEST_R_VRAM) {
                return 0;
            }
            return guest_map[i].base + (slab_off - start);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

/*
 * A count is enough on the console; there is nowhere for a message to go and
 * the screen has room for one line. The host test builds with
 * GUEST_SELFTEST_VERBOSE and gets the line number of every failure, which is
 * the difference between "one of 93 checks failed" and knowing which.
 */
#ifdef GUEST_SELFTEST_VERBOSE
#include <stdio.h>
#define FAILNOTE() fprintf(stderr, "  3ds_guest.c:%d failed\n", __LINE__)
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

int guest_selftest(int *ranOut)
{
    int ran = 0;
    int failed = 0;
    int i;

    if (sBase == NULL) {
        if (ranOut != NULL) {
            *ranOut = 0;
        }
        return 1;
    }

    /*
     * The one the plan names: a halfword at the keypad register, written
     * through the translator and read back through plain slab arithmetic.
     * Anything less than both directions would pass on a translator that was
     * consistently wrong.
     */
    {
        uint16_t *keys = (uint16_t *)armrec_host_ptr(0x04000130u);
        uint8_t *raw = sBase + guest_map_offset(GUEST_R_IO) + 0x130;

        CHECK(keys != NULL);
        if (keys != NULL) {
            *keys = 0xDEADu;
            CHECK(*(uint16_t *)raw == 0xDEADu);
            *(uint16_t *)raw = 0x03FFu;
            CHECK(*keys == 0x03FFu);
            CHECK(armrec_guest_addr(keys) == 0x04000130u);
        }
    }

    /* Every region: first byte, last addressable byte, one past the end, and
     * a round trip through both directions. */
    for (i = 0; i < GUEST_R_COUNT; i++) {
        const struct guest_region *r = &guest_map[i];
        uint8_t *store = sBase + guest_map_offset(i);
        uint8_t *first;
        uint8_t *last;

        if (i == GUEST_R_VRAM) {
            /* Addressed and backed, and translatable only through the bank
             * model: with no bank enabled; which is how a slab starts and
             * how this test finds it; every VRAM address answers NULL, the
             * way an unmapped read on hardware answers zeros. 3ds_vram.c's
             * own self-test is what exercises a mapped one. */
            CHECK(armrec_host_ptr(r->base) == NULL);
            CHECK(armrec_host_ptr(r->base + r->backing - 1) == NULL);
            CHECK(armrec_guest_addr(store) == 0);
            continue;
        }

        first = (uint8_t *)armrec_host_ptr(r->base);
        last = (uint8_t *)armrec_host_ptr(r->base + r->backing - 1);
        CHECK(first == store);
        CHECK(last == store + r->backing - 1);

        if (first == NULL || last == NULL) {
            continue;
        }

        *first = (uint8_t)(0xA0 + i);
        *last = (uint8_t)(0x50 + i);
        CHECK(store[0] == (uint8_t)(0xA0 + i));
        CHECK(store[r->backing - 1] == (uint8_t)(0x50 + i));

        CHECK(armrec_guest_addr(first) == r->base);
        CHECK(armrec_guest_addr(last) == r->base + r->backing - 1);

        /*
         * One past the region is the next region's first byte where the two
         * are adjacent, ITCM ends exactly where main RAM begins, and they
         * are adjacent in the slab too, so the pointer really is the same one,
         * and nothing at all where there is a hole. What must hold either
         * way is that it does not answer to this region's addresses.
         */
        {
            void *past = armrec_host_ptr(r->base + r->size);

            CHECK(past == NULL || armrec_guest_addr(past) == r->base + r->size);
        }

        /* Backing shorter than the window means the gap is a NULL, not a
         * wrap into the next region's storage. */
        if (r->backing < r->size) {
            CHECK(armrec_host_ptr(r->base + r->backing) == NULL);
            CHECK(armrec_host_ptr(r->base + r->size - 1) == NULL);
        }
    }

    /* Holes, and the ends of the map. */
    CHECK(armrec_host_ptr(0x00000000u) == NULL);
    CHECK(armrec_host_ptr(0x01FF7FFFu) == NULL); /* one below ITCM */
    CHECK(armrec_host_ptr(0x02400000u) == NULL); /* main RAM .. shared work */
    CHECK(armrec_host_ptr(0x02900000u) == NULL); /* shared .. port window */
    CHECK(armrec_host_ptr(0x03500000u) == NULL); /* WRAM .. ARM7 WRAM */
    CHECK(armrec_host_ptr(0x04FFFFFFu) == NULL); /* I/O .. palette */
    CHECK(armrec_host_ptr(0x0A010000u) == NULL); /* above the AGB slot */
    CHECK(armrec_host_ptr(0xFFFFFFFFu) == NULL);

    /* VRAM is the whole window, not just the part a store would cover, and
     * with no bank enabled none of it translates. */
    CHECK(armrec_host_ptr(0x06000000u) == NULL);
    CHECK(armrec_host_ptr(0x06800000u) == NULL);

    /*
     * The span iterator. Its interesting cases are VRAM's and need a bank
     * map, which is 3ds_vram.c's self-test and 3ds_mi_host.c's; what has to
     * hold everywhere is that it never stalls and never reports a run longer
     * than the mapping under it.
     */
    {
        void *h;
        uint32_t n;

        CHECK(guest_span(0x02000000u, 0u, &h) == 0);

        /* A flat row is one run for its whole length, and not past it. */
        n = guest_span(0x02000000u, 0x00400000u, &h);
        CHECK(h == armrec_host_ptr(0x02000000u));
        CHECK(n == 0x00400000u);

        /* Asking past the end of the row stops at the end of the row. */
        n = guest_span(0x023FF000u, 0x00002000u, &h);
        CHECK(h != NULL);
        CHECK(n == 0x00001000u);

        /* And the hole after it is a run of nothing, not a run of zero. */
        n = guest_span(0x02400000u, 0x00001000u, &h);
        CHECK(h == NULL);
        CHECK(n == 0x00001000u);

        /* Starting part-way into a block still answers for that address. */
        n = guest_span(0x02000123u, 0x100u, &h);
        CHECK(h == (uint8_t *)armrec_host_ptr(0x02000000u) + 0x123u);
        CHECK(n == 0x100u);
    }

    /* Host pointers that are not in the slab. */
    CHECK(armrec_guest_addr(NULL) == 0);
    CHECK(armrec_guest_addr(sBase - 1) == 0);
    CHECK(armrec_guest_addr(sBase + GUEST_SLAB_BYTES) == 0);
    CHECK(armrec_guest_addr(&ran) == 0);

    /* Leave the keypad words the way pc_input would find them; the input model is the
     * task that makes that a requirement rather than a courtesy. */
    memset(sBase + guest_map_offset(GUEST_R_IO), 0, 0x1000);

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
