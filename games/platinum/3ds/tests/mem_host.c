/*
 * 3ds/tests/mem_host.c: 3ds_mem.c's four functions, on a build machine.
 *
 * 3ds/src/armrec_mem_3ds.c is the only file in this port that touches the
 * console's allocator, and it does it through 3ds_mem.h. So a host test links
 * this instead of 3ds_mem.c and gets the real armrec_mem_init() over a
 * posix_memalign'd block, no libctru, no 3DS.
 *
 * NOT ZEROED, on purpose. The console's mem_init() zeroes what it allocates;
 * this fills with 0xA5 so that a test can tell whether armrec_mem_init() zeroed
 * the slab itself, which it has to, because mem_init() returns 0 without
 * touching a slab that is already up.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "3ds_guest_map.h"
#include "3ds_mem.h"

/* Set to make the next mem_init() fail the way an Old 3DS with nothing left
 * would. */
int mem_host_refuse;

static struct mem_slab sSlab;
static char sError[96];

int mem_init(void)
{
    void *base;

    if (sSlab.base != NULL) {
        return 0;
    }
    sError[0] = '\0';
    sSlab.bytes = (uint32_t)GUEST_SLAB_BYTES;

    if (mem_host_refuse) {
        snprintf(sError, sizeof sError, "no %u KB for the slab (heap free 0 KB)",
                 (unsigned)(GUEST_SLAB_BYTES / 1024));
        return -1;
    }
    if (posix_memalign(&base, 0x1000, GUEST_SLAB_BYTES) != 0 || base == NULL) {
        snprintf(sError, sizeof sError, "posix_memalign refused %u bytes",
                 (unsigned)GUEST_SLAB_BYTES);
        return -1;
    }

    memset(base, 0xA5, GUEST_SLAB_BYTES);
    sSlab.base = base;
    return 0;
}

void mem_exit(void)
{
    free(sSlab.base);
    sSlab.base = NULL;
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
