/*
 * 3ds/tests/armrec_mem.c: armrec's memory API over the slab, on the host.
 *
 * 3ds/src/armrec_mem_3ds.c calls nothing but 3ds_mem.h, so the console's
 * allocator is the only thing between it and a build machine, and
 * 3ds/tests/mem_host.c is that allocator. Everything else is the real code,
 * and armrec_mem_selftest() is the same function the 3dsx runs.
 *
 * What the host adds is the two paths the console cannot be made to take on
 * demand: a slab that will not allocate, which has to come back as -1 and a
 * message rather than a NULL base, and armrec_mem_free() followed by a second
 * init.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "armrec_rt.h"

#include "armrec_mem_3ds.h"
#include "3ds_guest.h"
#include "3ds_guest_map.h"
#include "3ds_mem.h"

/* 3ds/tests/mem_host.c supplies mem_init() and the other three over a
 * posix_memalign'd block, filled with 0xA5 rather than zeroed. */
extern int mem_host_refuse;

static int sRan;
static int sFailed;

static void check(int cond, const char *what)
{
    sRan++;
    if (!cond) {
        sFailed++;
        printf("  %-56s FAILED\n", what);
    }
}

int main(void)
{
    const uint8_t *p;
    uint32_t i;
    int ran = 0;
    int failed;
    int n;

    /* A slab that will not allocate: -1, a message, and no memory bound. */
    mem_host_refuse = 1;
    check(armrec_mem_init() == -1, "a slab that will not allocate fails init");
    check(armrec_mem_strerror()[0] != '\0', "and says why");
    check(armrec_mem_ready == 0, "and leaves guest memory unready");
    check(armrec_host_ptr(0x02000000u) == NULL, "and nothing translates");
    mem_host_refuse = 0;

    if (armrec_mem_init() != 0) {
        printf("armrec_mem: init failed: %s\n", armrec_mem_strerror());
        return 2;
    }
    check(armrec_mem_ready == 1, "init reports ready");
    check(armrec_mem_strerror()[0] == '\0', "and clears the message");

    /* Zeroed, whoever allocated it. mem_host.c fills the block with 0xA5 and
     * does not zero it; a DS does not start with uninitialised memory. */
    p = (const uint8_t *)mem_info()->base;
    for (i = 0; i < GUEST_SLAB_BYTES && p[i] == 0; i++) {
        /* nothing */
    }
    check(i == GUEST_SLAB_BYTES, "init zeroes the slab it was handed");

    /* A second init is a no-op rather than a second allocation. */
    check(armrec_mem_init() == 0, "init twice succeeds");
    check(mem_info()->base == p, "and does not move the slab");

    failed = armrec_mem_selftest(&ran);
    sRan += ran;
    sFailed += failed;

    /* What the table says, for the record: this is the map every walker on
     * this console will see. */
    n = armrec_region_count();
    for (i = 0; i < (uint32_t)n; i++) {
        uint32_t base = 0;
        uint32_t size = 0;
        const char *name = NULL;

        if (!armrec_region_at((int)i, &base, &size, &name)) {
            continue;
        }
        printf("  region %2u: 0x%08X + 0x%06X  %s\n", i, base, size,
               name != NULL ? name : "?");
    }

    /* Free, and then back again: the models that hold a pointer into the slab
     * have to let go of it, or a stray access after teardown lands in freed
     * heap. */
    armrec_mem_free();
    check(armrec_mem_ready == 0, "free reports not ready");
    check(armrec_host_ptr(0x02000000u) == NULL, "and nothing translates");
    check(armrec_host_ptr(0x06000000u) == NULL, "VRAM included");
    check(armrec_mem_init() == 0, "init after free succeeds");
    armrec_mem_free();

    printf("armrec_mem: %d checks, %d failed, %d regions\n", sRan, sFailed, n);
    return sFailed != 0;
}
