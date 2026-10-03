/*
 * 3ds/tests/snd_addr.c: SOUNDxSAD's 27 bits, checked and measured on the
 * host.
 *
 * The checks are snd_addr_selftest()'s, the same ones the 3dsx runs. What this
 * adds is the measurement that explains why the classifier has a `stray` class
 * at all: how much of the memory a 3DS process actually gets back is mapped
 * onto real guest regions by the field's own mask. On PC that number is zero:
 * The image is far above the mask's reach and a truncated pointer lands
 * nowhere, and the port's silent-sound bug was loud because of it. Here it
 * is not zero, and a number is the only honest way to say so.
 *
 * The ranges are the console's, not this machine's: the 3DS loads homebrew at
 * 0x00100000, hands the process its heap out of 0x08000000-0x0E000000, and
 * puts the linear heap (the GPU's) around 0x14000000-0x30000000. mem_init()
 * takes the slab from the application heap, which is the middle range.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "3ds_guest.h"
#include "3ds_guest_map.h"
#include "3ds_snd_addr.h"
#include "3ds_window.h"

#define SAD_MASK 0x07FFFFFCu
#define MB       0x00100000u

struct range {
    const char *name;
    uint32_t lo;
    uint32_t hi;
};

/* Megabytes of a host range whose masked address lands in readable guest
 * memory, i.e. where a truncated pointer would be read as a wave instead of
 * being caught. */
static uint32_t aliasing_mb(const struct range *r, uint32_t *totalOut)
{
    uint32_t at;
    uint32_t alias = 0;
    uint32_t total = 0;

    for (at = r->lo; at < r->hi; at += MB) {
        total++;
        if (armrec_host_ptr(at & SAD_MASK) != NULL) {
            alias++;
        }
    }
    *totalOut = total;
    return alias;
}

int main(void)
{
    static const struct range ranges[] = {
        { "3dsx image", 0x00100000u, 0x00400000u },
        { "application heap", 0x08000000u, 0x0E000000u },
        { "linear heap", 0x14000000u, 0x30000000u },
    };
    void *slab;
    int ran = 0;
    int failed;
    unsigned i;
    uint32_t heapAlias = 0;

    if (posix_memalign(&slab, 0x1000, GUEST_SLAB_BYTES) != 0 || slab == NULL) {
        printf("snd_addr: no %u bytes for a slab\n", (unsigned)GUEST_SLAB_BYTES);
        return 2;
    }

    /* Unbound: an address the SPU asks for answers with nothing, and a host
     * pointer produces no source address. */
    guest_bind(NULL);
    snd_addr_reset();
    if (snd_sad_ptr(0x02A00000u, 4) != NULL
        || snd_sad_from_host(slab, "unbound") != 0
        || snd_addr_bad[SND_ADDR_LOST] != 1
        || snd_addr_bad[SND_ADDR_HOST] != 1) {
        printf("snd_addr: the unbound translator answered something\n");
        free(slab);
        return 1;
    }

    guest_bind(slab);
    failed = snd_addr_selftest(&ran);

    printf("snd_addr: %d checks, %d failed\n", ran, failed);

    for (i = 0; i < sizeof ranges / sizeof ranges[0]; i++) {
        uint32_t total = 0;
        uint32_t alias = aliasing_mb(&ranges[i], &total);

        printf("  %-18s 0x%08X-0x%08X  %3u of %3u MB alias into guest memory\n",
               ranges[i].name, ranges[i].lo, ranges[i].hi, alias, total);
        if (i == 1) {
            heapAlias = alias;
        }
    }

    /*
     * The measurement is the argument for the `stray` class, so it is also a
     * check: if the heap ever stopped aliasing, the classifier could be
     * simplified, and that would want to be a deliberate change rather than a
     * quiet one.
     */
    if (heapAlias == 0) {
        printf("snd_addr: no part of the application heap aliases, the "
               "stray class may no longer be needed\n");
        failed++;
    }

    if (failed == 0) {
        printf("  %-56s ok\n", "sound source addresses, both directions");
    }

    guest_bind(NULL);
    free(slab);
    return failed != 0;
}
