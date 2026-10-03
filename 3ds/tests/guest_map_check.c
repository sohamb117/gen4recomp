/*
 * 3ds/tests/guest_map_check.c: the guest map says what armrec says.
 *
 * 3ds/include/3ds_guest_map.h is the 3DS port's copy of a table that already
 * exists on the PC side, and a copy that can drift is worse than no copy at
 * all: main RAM moving 128 KB in one file and not the other would show up as
 * a save that reads back garbage, months later, in a place no one would think
 * to look. So this program includes *both* headers, the map and
 * tools/armrec/armrec_rt.h: and static-asserts every base and every size
 * against the macro the PC port uses for it. If a row here disagrees with
 * armrec, this file does not compile.
 *
 * The runtime half checks what a table of literals cannot: that the rows are
 * in ascending order, that no two regions overlap, that the offsets the slab
 * hands out are contiguous and cover exactly the total, and that the port
 * window's name is still the string pc/src/pc_guest_window.c looks it up by.
 *
 * Host-compiled, like 3ds/tests/view_dump.c: nothing here is 3DS-specific, and
 * a check that needs a 3DS to run is a check that does not run.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "3ds_guest_map.h"
#include "armrec_rt.h"

/* Every row that armrec also names, pinned to armrec's own macros. The two
 * regions armrec has no macros for (VRAM's store size is ARM_VRAM_STORE; the
 * AGB slot is pc_main.c's map, not a region) are checked just below. */
#define PIN(id, base_macro, size_macro)                                       \
    _Static_assert(GUEST_BASE_##id == (int)(base_macro),                      \
                   #id " base disagrees with armrec_rt.h");                   \
    _Static_assert(GUEST_SIZE_##id == (int)(size_macro),                      \
                   #id " size disagrees with armrec_rt.h")

PIN(ITCM, ARM_ITCM_BASE, ARM_ITCM_SIZE);
PIN(MAIN, ARM_MAIN_RAM_BASE, ARM_MAIN_RAM_SIZE);
PIN(SHARED, ARM_SHARED_BASE, ARM_SHARED_SIZE);
PIN(WINDOW, ARM_PORT_WINDOW_BASE, ARM_PORT_WINDOW_SIZE);
PIN(WRAM, ARM_WRAM_BASE, ARM_WRAM_SIZE);
PIN(ARM7WRAM, ARM_ARM7_WRAM_BASE, ARM_ARM7_WRAM_SIZE);
PIN(IO, ARM_IO_BASE, ARM_IO_SIZE);
PIN(PALETTE, ARM_PALETTE_BASE, ARM_PALETTE_SIZE);
PIN(VRAM, ARM_VRAM_BASE, ARM_VRAM_SIZE);
PIN(OAM, ARM_OAM_BASE, ARM_OAM_SIZE);

/* The nine banks, not the 16 MB of address space they appear in. */
_Static_assert(GUEST_BACK_VRAM == (int)ARM_VRAM_STORE,
               "VRAM backing is the bank store, not the window");
/* The AGB slot is pc/src/pc_main.c's mmap: 0x08000000, ROM window + SRAM. */
_Static_assert(GUEST_BASE_AGB == 0x08000000, "AGB slot base");
_Static_assert(GUEST_SIZE_AGB == 0x02010000, "AGB slot is ROM window + SRAM");

static int failures;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("  %-56s ok\n", what);
    } else {
        printf("  %-56s FAILED\n", what);
        failures++;
    }
}

int main(void)
{
    uint32_t off = 0;
    int i;

    printf("guest map: %d regions, %u bytes of backing (%.2f MB)\n",
           GUEST_R_COUNT, (unsigned)GUEST_SLAB_BYTES,
           GUEST_SLAB_BYTES / (1024.0 * 1024.0));

    for (i = 0; i < GUEST_R_COUNT; i++) {
        const struct guest_region *r = &guest_map[i];

        printf("  %08X..%08X  slab +0x%06X  %7u KB  %s%s\n",
               r->base, r->base + r->size, guest_map_offset(i),
               r->backing / 1024, r->name,
               r->backing == r->size ? "" : "  (window)");
    }

    for (i = 0; i < GUEST_R_COUNT; i++) {
        const struct guest_region *r = &guest_map[i];
        char what[96];

        if (i > 0) {
            const struct guest_region *p = &guest_map[i - 1];

            snprintf(what, sizeof what, "%s starts at or after %s ends",
                     r->name, p->name);
            check(r->base >= p->base + p->size, what);
        }

        snprintf(what, sizeof what, "%s backing is at slab +0x%06X",
                 r->name, off);
        check(guest_map_offset(i) == off, what);
        off += r->backing;
    }

    check(off == (uint32_t)GUEST_SLAB_BYTES,
          "the offsets cover exactly the slab");

    /* pc/src/pc_guest_window.c finds this region by name, not by address, so
     * the string is part of the interface. */
    check(strcmp(guest_map[GUEST_R_WINDOW].name, ARMREC_PORT_WINDOW_NAME) == 0,
          "the port window is named the way armrec names it");

    /* The two the PC port pays address space for and this one must not. */
    check(guest_map[GUEST_R_VRAM].backing == 0xA4000u
              && guest_map[GUEST_R_VRAM].size == 0x1000000u,
          "VRAM is 656 KB of store behind 16 MB of addresses");
    check(guest_map[GUEST_R_AGB].backing == 0x20000u,
          "the AGB slot is a 128 KB probe buffer");

    printf("\n%s\n", failures ? "guest_map_check: FAILED" : "guest_map_check: ok");
    return failures != 0;
}
