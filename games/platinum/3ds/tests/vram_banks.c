/*
 * 3ds/tests/vram_banks.c: the nine banks, on the host.
 *
 * Two checks in one program. The static asserts pin this port's VRAM
 * constants to tools/armrec/armrec_rt.h's, the way 3ds/tests/guest_map_check.c
 * pins the region table; a bank count or a block size that drifted would
 * produce a model that looks right and puts every bank in the wrong place.
 * 3ds/tests/vram_pin.py covers the tables and the placement code; this covers
 * the numbers a header can state.
 *
 * The rest is vram_selftest(), the same one the 3dsx runs, over a malloc'd
 * slab. The model is pure C (no libctru and no address space) so the host
 * runs all of it, which is the whole benefit of not being able to mmap at
 * 0x06000000.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "3ds_guest.h"
#include "3ds_guest_map.h"
#include "3ds_vram.h"
#include "armrec_rt.h"

_Static_assert(VRAM_BANKS == ARM_VRAM_BANKS, "bank count disagrees with armrec");
_Static_assert(VRAM_BLK == ARM_VRAM_BLK, "block size disagrees with armrec");
_Static_assert(VRAM_STORE_BYTES == ARM_VRAM_STORE,
               "the bank store disagrees with armrec");
_Static_assert(GUEST_BASE_VRAM == (int)ARM_VRAM_BASE, "VRAM base");
_Static_assert(GUEST_BACK_VRAM == (int)ARM_VRAM_STORE,
               "the slab backs the banks, not the 16 MB of addresses");

int main(void)
{
    void *slab;
    int ran = 0;
    int failed;

    if (posix_memalign(&slab, 0x1000, GUEST_SLAB_BYTES) != 0 || slab == NULL) {
        printf("vram_banks: no %u bytes for a slab\n", (unsigned)GUEST_SLAB_BYTES);
        return 2;
    }

    /* Unbound: no store, no translation, no crash. */
    guest_bind(NULL);
    vram_init();
    if (vram_host_ptr(0x06000000u) != NULL || vram_bank_ptr(0) != NULL) {
        printf("vram_banks: the unbound model answered something\n");
        free(slab);
        return 1;
    }

    guest_bind(slab);
    failed = vram_selftest(&ran);

    printf("vram_banks: %d checks, %d failed, %u KB of banks, %lu remaps\n",
           ran, failed, (unsigned)(VRAM_STORE_BYTES / 1024), vram_remaps());
    if (failed == 0) {
        printf("  %-56s ok\n", "banks reachable through the windows VRAMCNT picks");
    }

    guest_bind(NULL);
    free(slab);
    return failed != 0;
}
