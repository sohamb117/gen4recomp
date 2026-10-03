/*
 * 3ds/tests/guest_xlat.c: run the guest translator on the host.
 *
 * 3ds_guest.c is pure C over a slab pointer, so the console is not needed to
 * find out whether the arithmetic is right: this allocates a slab of its own,
 * binds it, and runs the same guest_selftest() the 3dsx runs. What the
 * hardware run adds is that libctru's heap really did hand over that much
 * memory; what it cannot add is a hundred boundary cases in a readable form.
 *
 * The slab here is plain malloc'd memory. Alignment does not matter to the
 * translation; the map's rows are 4 KB multiples and the arithmetic is
 * offsets from a base, but it is rounded up anyway so a failure here is
 * never a host-alignment artefact.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "3ds_guest.h"
#include "3ds_guest_map.h"

int main(void)
{
    void *slab;
    int ran = 0;
    int failed;

    if (posix_memalign(&slab, 0x1000, GUEST_SLAB_BYTES) != 0 || slab == NULL) {
        printf("guest_xlat: no %u bytes for a slab\n",
               (unsigned)GUEST_SLAB_BYTES);
        return 2;
    }

    /* Unbound first: every translation must refuse rather than fault. */
    guest_bind(NULL);
    if (armrec_host_ptr(0x02000000u) != NULL
        || armrec_guest_addr(slab) != 0
        || guest_selftest(&ran) == 0) {
        printf("guest_xlat: unbound translator answered something\n");
        free(slab);
        return 1;
    }

    guest_bind(slab);
    failed = guest_selftest(&ran);

    printf("guest_xlat: %d checks, %d failed, slab %u KB at %p\n",
           ran, failed, (unsigned)(GUEST_SLAB_BYTES / 1024), slab);
    if (failed == 0) {
        printf("  %-56s ok\n", "guest translation over every region");
    }

    guest_bind(NULL);
    free(slab);
    return failed != 0;
}
