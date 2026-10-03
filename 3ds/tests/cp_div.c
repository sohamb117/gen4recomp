/*
 * 3ds/tests/cp_div.c: the divider and square root, on the host.
 *
 * cp_selftest()'s checks are the ones the 3dsx runs. What this adds is the
 * unbound case, a division before the slab exists has to answer NULL rather
 * than dereference 0x04000280, and a sweep of the square root against the
 * host's own arithmetic, which the console has no room to run.
 *
 * armrec_trap() is the port's fatal path and lives in the export layer, which
 * needs libctru; the one state that reaches it (32-bit INT_MIN / -1 with the
 * remainder read) is unreachable from this program, so a definition that says
 * so is enough to link.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "armrec_rt.h"

#include "3ds_cp.h"
#include "3ds_guest.h"
#include "3ds_guest_map.h"

void armrec_trap(const char *fn, const char *what)
{
    printf("cp_div: unexpected trap in %s: %s\n", fn, what);
    exit(1);
}

/* floor(sqrt(v)) the slow way, for the sweep. Integer only: doubles lose the
 * top bits of a 64-bit operand and would agree with a wrong model. */
static uint32_t ref_sqrt(uint64_t v)
{
    uint64_t lo = 0, hi = 0x100000000ULL;

    while (hi - lo > 1) {
        uint64_t mid = lo + (hi - lo) / 2;

        if (mid * mid <= v) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return (uint32_t)lo;
}

int main(void)
{
    void *slab;
    int ran = 0;
    int failed;
    int sweep = 0;
    int i;

    guest_bind(NULL);
    if (cp_ptr(0x04000280u) != NULL) {
        printf("cp_div: the unbound unit answered a pointer\n");
        return 1;
    }

    if (posix_memalign(&slab, 0x1000, GUEST_SLAB_BYTES) != 0 || slab == NULL) {
        printf("cp_div: no %u bytes for a slab\n", (unsigned)GUEST_SLAB_BYTES);
        return 2;
    }
    guest_bind(slab);

    failed = cp_selftest(&ran);

    /* Both widths, over values chosen to land on and around a perfect square
     * and at the top of each range. */
    for (i = 0; i < 64; i++) {
        uint64_t big = (i < 32) ? (1ULL << i) : ((1ULL << (i - 32)) * 0x9E3779B9ULL);
        uint32_t small = (uint32_t)big;
        uint32_t *res = (uint32_t *)armrec_host_ptr(0x040002B4u);
        uint32_t *cnt = (uint32_t *)armrec_host_ptr(0x040002B0u);
        uint32_t *par = (uint32_t *)armrec_host_ptr(0x040002B8u);

        *cnt = 0;
        par[0] = small;
        par[1] = 0;
        cp_ptr(0x040002B4u);
        ran++;
        if (*res != ref_sqrt(small)) {
            failed++;
            printf("  sqrt32(%08X) = %u, want %u\n", small, *res,
                   ref_sqrt(small));
        }
        sweep++;

        *cnt = 1;
        par[0] = (uint32_t)big;
        par[1] = (uint32_t)(big >> 32);
        cp_ptr(0x040002B4u);
        ran++;
        if (*res != ref_sqrt(big)) {
            failed++;
            printf("  sqrt64(%08X%08X) = %u, want %u\n", (uint32_t)(big >> 32),
                   (uint32_t)big, *res, ref_sqrt(big));
        }
        sweep++;
    }

    printf("cp_div: %d checks, %d failed, %d square roots swept\n", ran, failed,
           sweep);
    if (failed == 0) {
        printf("  %-56s ok\n", "the unit computes where the slab is storage");
    }

    guest_bind(NULL);
    free(slab);
    return failed != 0;
}
