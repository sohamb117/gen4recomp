/*
 * 3ds/tests/input_reset.c: the keypad words, on the host.
 *
 * input_selftest() is the same function the 3dsx runs. What this adds is the
 * unbound case, publishing with no slab has to fail rather than write
 * somewhere, and the two words printed, because "0x03FF at 0x04000130" is
 * the claim this phase ends on and it should be readable in the log.
 */

#include <stdint.h>
#include <stdio.h>

#include "armrec_rt.h"

#include "armrec_mem_3ds.h"
#include "3ds_guest.h"
#include "3ds_input.h"

int main(void)
{
    int ran = 0;
    int failed;
    int bad = 0;

    guest_bind(NULL);
    if (pc_input_init() != 0 || input_publish_keys(0) != 0) {
        printf("  %-56s FAILED\n", "publishing with no slab writes nothing");
        bad++;
    }

    if (armrec_mem_init() != 0) {
        printf("input_reset: init failed: %s\n", armrec_mem_strerror());
        return 2;
    }

    failed = input_selftest(&ran);

    printf("  KEYINPUT  %08X = %04X\n", (unsigned)INPUT_KEYS_ADDR,
           *(const uint16_t *)armrec_host_ptr(INPUT_KEYS_ADDR));
    printf("  XY/pen    %08X = %04X\n", (unsigned)INPUT_XY_ADDR,
           *(const uint16_t *)armrec_host_ptr(INPUT_XY_ADDR));

    armrec_mem_free();
    printf("input_reset: %d checks, %d failed\n", ran + 1, failed + bad);
    return (failed + bad) != 0;
}
