/*
 * 3ds/tests/io_pages.c: the per-processor I/O pages, on the host.
 *
 * io_selftest()'s checks are the same ones the 3dsx runs. What this adds is
 * the unbound case, a switch with no slab behind it has to fail rather than
 * copy from nothing, and a count of what the two pages cost, because the
 * whole point of a save buffer over a remap is that it is cheap and small.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "3ds_guest.h"
#include "3ds_guest_map.h"
#include "3ds_io.h"

int main(void)
{
    void *slab;
    int ran = 0;
    int failed;
    int i;

    if (posix_memalign(&slab, 0x1000, GUEST_SLAB_BYTES) != 0 || slab == NULL) {
        printf("io_pages: no %u bytes for a slab\n", (unsigned)GUEST_SLAB_BYTES);
        return 2;
    }

    guest_bind(NULL);
    io_pages_init();
    if (io_switch(IO_CPU_ARM7) == 0 || io_page(IO_CPU_ARM9) != NULL) {
        printf("io_pages: the unbound page model answered something\n");
        free(slab);
        return 1;
    }

    guest_bind(slab);
    failed = io_selftest(&ran);

    printf("io_pages: %d checks, %d failed, 2 saved pages of %u bytes\n",
           ran, failed, 0x1000u);
    for (i = 0; i < io_mirror_count(); i++) {
        const char *name = NULL;
        uint32_t addr = io_mirror_at(i, &name);

        printf("  mirrored: 0x%08X %s\n", addr, name != NULL ? name : "?");
    }
    if (failed == 0) {
        printf("  %-56s ok\n", "private registers private, mirrors crossing");
    }

    guest_bind(NULL);
    free(slab);
    return failed != 0;
}
