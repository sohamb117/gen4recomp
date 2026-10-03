/*
 * 3ds/tests/state_digest.c: the guest digest, on the host.
 *
 * state_selftest() is the same function the 3dsx runs. What this file adds is
 * the thing a self-test cannot honestly do for itself: it recomputes the
 * zero-state digest from the region table with a *second* implementation;
 * its own FNV loop over a zeroed buffer, its own fold, so STATE_ZERO_DIGEST
 * is a number two pieces of code agree on rather than one piece of code's
 * opinion of itself.
 *
 * That constant is the port's one cross-host pin. It depends on the region
 * table, the region sizes and the arithmetic, and on nothing about this
 * machine: a PC run digested with guest memory zeroed and no bank enabled owes
 * the same value.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "armrec_rt.h"

#include "armrec_mem_3ds.h"
#include "3ds_guest.h"
#include "3ds_state.h"

#define REF_OFFSET 0xcbf29ce484222325ULL
#define REF_PRIME  0x00000100000001b3ULL

/* Deliberately not pc_state_fnv1a(): a check that shares its implementation
 * with the thing it checks is a tautology. */
static uint64_t ref_fnv(uint64_t h, const unsigned char *p, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        h = (h ^ (uint64_t)p[i]) * REF_PRIME;
    }
    return h;
}

/* The digest of a zeroed map: FNV over as many zero bytes as each region
 * reports, folded big-endian in table order. Real memory, not a shortcut, so
 * the reference does not share the "a hole is a multiply" trick either. */
static uint64_t ref_zero_digest(void)
{
    unsigned char zeros[4096];
    uint64_t total = REF_OFFSET;
    int i;
    int n = armrec_region_count();

    memset(zeros, 0, sizeof zeros);
    for (i = 0; i < n; i++) {
        uint32_t base = 0;
        uint32_t size = 0;
        uint64_t d = REF_OFFSET;
        unsigned char be[8];
        int k;

        if (!armrec_region_at(i, &base, &size, NULL)) {
            continue;
        }
        while (size != 0) {
            uint32_t n2 = size < sizeof zeros ? size : (uint32_t)sizeof zeros;

            d = ref_fnv(d, zeros, n2);
            size -= n2;
        }
        for (k = 0; k < 8; k++) {
            be[k] = (unsigned char)(d >> (56 - 8 * k));
        }
        total = ref_fnv(total, be, 8);
    }
    return total;
}

int main(void)
{
    uint64_t ref;
    uint64_t got;
    int ran = 0;
    int failed;
    int bad = 0;

    if (armrec_mem_init() != 0) {
        printf("state_digest: init failed: %s\n", armrec_mem_strerror());
        return 2;
    }

    ref = ref_zero_digest();
    got = pc_state_digest();
    printf("  zero-state digest %016llX (reference %016llX)\n",
           (unsigned long long)got, (unsigned long long)ref);
    if (got != ref) {
        printf("  %-56s FAILED\n", "the digest matches an independent one");
        bad++;
    }
    if (got != STATE_ZERO_DIGEST) {
        printf("  %-56s FAILED\n", "STATE_ZERO_DIGEST is that number");
        printf("      3ds_state.h says %016llX\n",
               (unsigned long long)STATE_ZERO_DIGEST);
        bad++;
    }

    failed = state_selftest(&ran);

    /* Nothing the self-test did may have survived it: the digest is only
     * useful if the state it reports is the state that was there. */
    if (pc_state_digest() != ref) {
        printf("  %-56s FAILED\n", "the self-test left guest memory as it was");
        bad++;
    }

    armrec_mem_free();
    printf("state_digest: %d checks, %d failed\n", ran + 3, failed + bad);
    return (failed + bad) != 0;
}
