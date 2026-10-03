/*
 * pc/handheld_probe.c: the questions only a device can answer, asked by a
 * program instead of by a code block someone retypes.
 *
 * The handheld notes end on a list of shell lines to run on a small ARM
 * Three of them are not shell at all: "does a 32-bit ARM ELF run",
 * "does a non-PIE one", "can we identity map 0x02000000", and the usual way
 * to answer those is a hello-world that proves almost nothing. This answers
 * them with the port's OWN memory model: it links tools/armrec/armrec_rt.c
 * and calls armrec_mem_init(), so a pass here is the real map at the real
 * addresses and not a toy mmap that happens to succeed.
 *
 * Built static and non-PIE by `make -f pc/Makefile.arm probe`, so:
 *
 *   * it running at all answers "does a 32-bit ARM ELF run", the kernel
 *     kept CONFIG_COMPAT and AArch32 at EL0 is enabled;
 *   * it being ET_EXEC answers "does a non-PIE one", which Android has
 *     refused for application processes since API 21 and a Linux handheld
 *     has not;
 *   * armrec_mem_init() answers the identity map, region by region, and
 *     names the one that failed rather than saying no.
 *
 * Exit status: 0 every region mapped, 1 the map failed, 2 it could not start
 * far enough to try, which the shell cannot tell apart from the binary not
 * running, and does not need to.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

#include "armrec_rt.h"

int main(void)
{
    struct utsname u;
    int i, n, bad = 0;

    printf("== the host ==\n");
    if (uname(&u) == 0) {
        printf("  kernel   %s %s %s\n", u.sysname, u.release, u.machine);
    }
    printf("  pointer  %zu bytes (this build is armhf; 8 would mean the wrong one shipped)\n",
           sizeof(void *));
    printf("  page     %ld bytes\n", sysconf(_SC_PAGESIZE));
    printf("  cores    %ld online\n", sysconf(_SC_NPROCESSORS_ONLN));
    /* What this binary IS, not what the makefile that usually builds it
     * chooses. The line used to say "non-PIE" outright, and then the same
     * source was built three other ways on one afternoon, a PIE executable
     * and a shared object, both with the NDK, and reported something untrue
     * about itself each time. A probe that misdescribes its own build is
     * worse than one that says less. */
#if defined(__PIE__) || defined(__pic__)
    printf("  running  yes, a 32-bit ARM ELF starts here, and this one is "
           "position-independent\n");
#else
    printf("  running  yes, a 32-bit ARM ELF starts here, and this one is "
           "non-PIE\n");
#endif

    printf("== the identity map, by the port's own armrec_mem_init ==\n");
    if (armrec_mem_init() != 0) {
        printf("  FAILED: %s\n", armrec_mem_strerror());
        printf("  a handheld that says this needs the slab, not the map\n");
        return 1;
    }
    n = armrec_region_count();
    for (i = 0; i < n; i++) {
        uint32_t base = 0, size = 0;
        const char *name = NULL;
        volatile uint8_t *p;

        if (!armrec_region_at(i, &base, &size, &name)) {
            continue;
        }
        /* Mapped is not the same as usable: touch the first and last byte. */
        p = (volatile uint8_t *)(uintptr_t)base;
        p[0] = 0x5A;
        p[size - 1] = 0xA5;
        if (p[0] != 0x5A || p[size - 1] != 0xA5) {
            printf("  %08X + %06X  %-20s MAPPED BUT NOT WRITABLE\n",
                   base, size, name ? name : "?");
            bad = 1;
        } else {
            printf("  %08X + %06X  %-20s ok\n", base, size, name ? name : "?");
        }
    }
    printf("== verdict ==\n");
    printf("  %s\n", bad ? "a region is not usable, report which, above"
                         : "all regions identity-mapped and writable; this "
                           "device needs no guest-memory work");
    return bad;
}
