/*
 * 3ds/tests/pc_zero_digest.c: the same number, from the PC port.
 *
 * STATE_ZERO_DIGEST claims that a zeroed guest map digests to one value on
 * either host. This is the other half of that claim: it links the *PC* files:
 * tools/armrec/armrec_rt.c and pc/src/pc_state.c, unmodified, calls
 * armrec_mem_init(), and prints the digest of the identity-mapped guest space
 * before anything has written to it. No 3DS code is in this binary at all.
 *
 * 3ds/tests/run.sh builds it -m32 and skips where a machine has no 32-bit
 * libraries, rather than pretending. It used to say -m32 was not optional,
 * because armrec_rt.c's call thunks are i386 cdecl and it refused to compile
 * anywhere else; that is no longer true; the thunks have an ARM branch now,
 * and pc/Makefile.arm's `zero-digest` target builds this same file for armhf
 * and checks the same pin from a third toolchain.
 */

#include <stdio.h>

#include "armrec_rt.h"
#include "pc_state.h"

int main(void)
{
    int i;
    int n;

    if (armrec_mem_init() != 0) {
        printf("pc_zero_digest: init failed: %s\n", armrec_mem_strerror());
        return 2;
    }

    n = armrec_region_count();
    for (i = 0; i < n; i++) {
        uint32_t base = 0;
        uint32_t size = 0;
        const char *name = NULL;

        if (!armrec_region_at(i, &base, &size, &name)) {
            continue;
        }
        printf("  region %2d: 0x%08X + 0x%06X  %s\n", i, base, size,
               name != NULL ? name : "?");
    }
    printf("pc_zero_digest: %016llX (%d regions)\n",
           (unsigned long long)pc_state_digest(), n);
    return 0;
}
