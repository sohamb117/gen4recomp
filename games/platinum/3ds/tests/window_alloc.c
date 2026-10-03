/*
 * 3ds/tests/window_alloc.c: run the port window's allocator on the host.
 *
 * 3ds_window.c is pure C over the translator, which is pure C over a bound
 * slab pointer, so neither needs a 3DS to be checked: this allocates a slab,
 * binds it, and runs the same window_selftest() the 3dsx runs. The console
 * adds that libctru's heap really handed over the memory; it cannot add
 * fifty edge cases in a form anyone can read.
 *
 * What is here and not in the self-test is the unbound case: window_selftest()
 * cannot rebind a slab it was not given, so refusing to allocate before
 * mem_init() is checked from the outside.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "3ds_guest.h"
#include "3ds_guest_map.h"
#include "3ds_window.h"

int main(void)
{
    void *slab;
    int ran = 0;
    int failed;

    if (posix_memalign(&slab, 0x1000, GUEST_SLAB_BYTES) != 0 || slab == NULL) {
        printf("window_alloc: no %u bytes for a slab\n",
               (unsigned)GUEST_SLAB_BYTES);
        return 2;
    }

    /* Unbound: a refusal with a reason, not a pointer into nothing. */
    guest_bind(NULL);
    window_reset();
    if (pc_guest_window_alloc(32, "unbound") != NULL
        || window_strerror()[0] == '\0'
        || pc_guest_window_used() != 0
        || pc_guest_window_blocks() != 0) {
        printf("window_alloc: the unbound allocator answered something\n");
        free(slab);
        return 1;
    }

    guest_bind(slab);
    failed = window_selftest(&ran);

    printf("window_alloc: %d checks, %d failed, window %u KB at 0x%08X\n",
           ran, failed, (unsigned)(pc_guest_window_size() / 1024),
           (unsigned)pc_guest_window_base());
    if (failed == 0) {
        printf("  %-56s ok\n", "port window blocks, alignment and exhaustion");
    }

    guest_bind(NULL);
    free(slab);
    return failed != 0;
}
