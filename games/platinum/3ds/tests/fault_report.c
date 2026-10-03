/*
 * 3ds/tests/fault_report.c: what the fault screen would say, on the host.
 *
 * The half of 3ds_fault.c that matters after a crash is the one sentence
 * naming the address, and it is pure C over the translator: compiled here
 * without __3DS__, the file is that function and nothing else. What the
 * console adds is the drawing, and that is checked by pressing L+R+Y on it.
 *
 * The cases are the ones a real abort in this port produces. "NULL+off" is the
 * important one: on hardware it is what a translation that answered NULL and
 * was used anyway looks like, and the offset is the field that was reached
 * for.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "armrec_rt.h"

#include "armrec_mem_3ds.h"
#include "3ds_fault.h"
#include "3ds_guest.h"
#include "3ds_guest_map.h"

static int sRan;
static int sFailed;

static void expect(const char *got, const char *want)
{
    sRan++;
    if (strcmp(got, want) != 0) {
        sFailed++;
        printf("  %-32s FAILED, wanted %s\n", got, want);
    }
}

static const char *describe(uintptr_t addr)
{
    static char buf[32];

    fault_describe(addr, buf, (int)sizeof buf);
    return buf;
}

static const char *describe_ptr(const void *p)
{
    return describe((uintptr_t)p);
}

int main(void)
{
    char line[32];

    if (armrec_mem_init() != 0) {
        printf("fault_report: init failed: %s\n", armrec_mem_strerror());
        return 2;
    }

    /* A translation that answered NULL, used anyway, at a struct offset. */
    expect(describe(0), "NULL+0");
    expect(describe(0x14), "NULL+14");
    expect(describe(0xFFF), "NULL+FFF");

    /* Guest memory: the address the port speaks, recovered from the host one
     * the ARM11 faulted on. */
    expect(describe_ptr(armrec_host_ptr(0x02003F00u)), "GUEST 02003F00");
    expect(describe_ptr(armrec_host_ptr(0x04000130u)), "GUEST 04000130");
    expect(describe_ptr(armrec_host_ptr(0x02A00000u)), "GUEST 02A00000");
    expect(describe_ptr(armrec_host_ptr(0x08000000u)), "GUEST 08000000");

    /* The bank store, which no fixed guest address names. */
    expect(describe_ptr(guest_region_base(GUEST_R_VRAM)), "VRAM+0");
    expect(describe_ptr((const uint8_t *)guest_region_base(GUEST_R_VRAM) + 0x1234),
           "VRAM+1234");

    /* The port's own memory is not guest state, and saying so is the point. */
    sRan++;
    if (strncmp(describe_ptr(&sRan), "HOST ", 5) != 0) {
        sFailed++;
        printf("  a host address is not named as one: %s\n", describe_ptr(&sRan));
    }

    /* The four abort types, and anything else. */
    expect(fault_type_name(FAULT_DATA_ABORT), "DATA");
    expect(fault_type_name(FAULT_PREFETCH_ABORT), "PREFETCH");
    expect(fault_type_name(FAULT_UNDEFINED), "UNDEF");
    expect(fault_type_name(FAULT_VFP), "VFP");
    expect(fault_type_name(99), "?");

    /* A short buffer truncates rather than overruns. */
    memset(line, 0x7F, sizeof line);
    fault_describe(0x14, line, 4);
    sRan++;
    if (strcmp(line, "NUL") != 0) {
        sFailed++;
        printf("  a short buffer gave %s\n", line);
    }

    /* Nothing translates once the slab is gone: the report must still be a
     * sentence, because a fault during teardown is a fault too. */
    armrec_mem_free();
    expect(describe(0x02003F00u), "HOST 02003F00");

    printf("fault_report: %d checks, %d failed\n", sRan, sFailed);
    return sFailed != 0;
}
