/*
 * 3ds/src/3ds_io.c: one I/O page per processor, over the slab.
 *
 * 0x04000000 is two different pages on a DS: the ARM9's registers and the
 * ARM7's, at the same addresses, and 83 of the 2,048 halfwords in the first
 * page are private and writable on one side or the other. The map has one row
 * for the region, because a slab row is storage: the suspended processor's
 * registers are a save buffer, not guest memory.
 *
 * Saved and restored, not remapped. armrec_rt.c measured that on PC, where an
 * mmap over a live page is 3,392 ns against 107 ns for two 4 KB copies, and on
 * this console the question does not arise at all: there is no way to remap
 * anything at a DS address, which is why the slab exists.
 *
 * The mirror list is armrec's, not this file's. Two halfwords cross, VCOUNT
 * and KEYINPUT, both read-only to the game and both driven from the host side,
 * so both processors must read the same value. The list is copied here because
 * armrec_rt.c cannot be compiled for this console yet, and 3ds/tests/run.sh
 * greps that file and fails if the two disagree. IPCSYNC and EXMEMCNT are
 * visible across on hardware and are deliberately not here: neither is shared
 * storage, so they are a hardware model's business.
 *
 * This game never switches, and that is measured, not assumed: nothing in the
 * PC port's link references armrec_cpu_switch, because the port runs the ARM7
 * sound driver as host C in an `arm7_` namespace instead of recompiling it,
 * and the rest of the ARM7 stays out of the link. The one caller of
 * armrec_io_page() is pc/hw/pc_spu.c, which asks for the ARM9's page. So what
 * this file has to get right today is that a page lookup answers with a
 * translated pointer; the switch is here because the API armrec publishes has
 * it and the self-test can reach it.
 *
 * The names are local, unlike 3ds_window.c's, because armrec_rt.c defines
 * armrec_cpu_switch() and armrec_io_page() for the PC build and a second
 * definition would be a collision the moment this console compiles that file.
 */

#include "3ds_io.h"

#include <stddef.h>
#include <string.h>

#include "3ds_guest.h"
#include "3ds_guest_map.h"

/* ARM_IO_PERCPU_SIZE in tools/armrec/armrec_rt.h. The first page of the
 * region; the megabyte above it is the same for both processors. */
#define IO_PAGE_BYTES 0x1000u

_Static_assert(GUEST_BACK_IO >= (int)IO_PAGE_BYTES,
               "the I/O region must back at least the per-processor page");

/* Copied from armrec_rt.c's io_mirror[]; 3ds/tests/run.sh pins it there. */
static const struct {
    uint32_t addr;
    uint32_t size;
    const char *name;
} io_mirror[] = {
    { 0x04000006u, 2, "VCOUNT" },
    { 0x04000130u, 2, "KEYINPUT" },
};

#define IO_MIRRORS ((int)(sizeof io_mirror / sizeof io_mirror[0]))

static uint8_t sSaved[2][IO_PAGE_BYTES];
static int     sCpu = IO_CPU_ARM9;
static int     sReady;

int io_mirror_count(void) { return IO_MIRRORS; }

uint32_t io_mirror_at(int i, const char **name)
{
    if (i < 0 || i >= IO_MIRRORS) {
        if (name != NULL) {
            *name = NULL;
        }
        return 0;
    }
    if (name != NULL) {
        *name = io_mirror[i].name;
    }
    return io_mirror[i].addr;
}

int io_cpu(void) { return sCpu; }

void io_pages_init(void)
{
    memset(sSaved, 0, sizeof sSaved);
    sCpu = IO_CPU_ARM9;
    sReady = 1;
}

void *io_page(int cpu)
{
    if (!sReady || cpu < 0 || cpu > IO_CPU_ARM7) {
        return NULL;
    }
    /*
     * The live page for the running processor, its saved copy for the other.
     * Answering with the live page rather than a stale buffer is what lets a
     * hardware model ask for "the processor that is not running" without
     * knowing which that is.
     */
    if (cpu == sCpu) {
        return armrec_host_ptr((uint32_t)GUEST_BASE_IO);
    }
    return sSaved[cpu];
}

int io_switch(int cpu)
{
    uint8_t *live;
    int i;

    if (!sReady || cpu < 0 || cpu > IO_CPU_ARM7) {
        return -1;
    }
    if (cpu == sCpu) {
        return 0;
    }

    live = (uint8_t *)armrec_host_ptr((uint32_t)GUEST_BASE_IO);
    if (live == NULL) {
        return -1;
    }

    memcpy(sSaved[sCpu], live, IO_PAGE_BYTES);
    /* Applied to the incoming copy rather than to the live page after the
     * restore, so each mirrored address is written once and there is no window
     * where the page holds the wrong value. armrec_rt.c does it the same way. */
    for (i = 0; i < IO_MIRRORS; i++) {
        uint32_t off = io_mirror[i].addr - (uint32_t)GUEST_BASE_IO;

        memcpy(sSaved[cpu] + off, sSaved[sCpu] + off, io_mirror[i].size);
    }
    memcpy(live, sSaved[cpu], IO_PAGE_BYTES);
    sCpu = cpu;

    /*
     * The stack pointer and the CPSR travel with the page on PC, because they
     * are the rest of "which processor is running". They are not here: this
     * port has no second recompiled context to have a stack pointer, and
     * armrec_sp is armrec's. Whoever gives this console an ARM7 adds them
     * where armrec keeps them, not here.
     */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#ifdef IO_SELFTEST_VERBOSE
#include <stdio.h>
#define FAILNOTE() fprintf(stderr, "  3ds_io.c:%d failed\n", __LINE__)
#else
#define FAILNOTE() ((void)0)
#endif

#define CHECK(cond)                                                           \
    do {                                                                      \
        ran++;                                                                \
        if (!(cond)) {                                                        \
            failed++;                                                         \
            FAILNOTE();                                                       \
        }                                                                     \
    } while (0)

/* A private, writable halfword on both sides: DISPSTAT is the ARM9's, and the
 * sweep in armrec_rt.h counts 23 halfwords that are private to each. Any
 * address that is not mirrored would do; this one is real. */
#define IO_PRIVATE 0x04000004u

int io_selftest(int *ranOut)
{
    int ran = 0;
    int failed = 0;
    uint16_t *live;
    uint16_t *priv;
    uint16_t *keys;
    int i;

    io_pages_init();

    live = (uint16_t *)armrec_host_ptr((uint32_t)GUEST_BASE_IO);
    if (live == NULL) {
        /* Unbound slab: a switch has nowhere to copy from and must say so. */
        ran++;
        if (io_switch(IO_CPU_ARM7) == 0) {
            failed++;
            FAILNOTE();
        }
        if (ranOut != NULL) {
            *ranOut = ran;
        }
        return failed + 1;
    }

    priv = (uint16_t *)((uint8_t *)live + (IO_PRIVATE - GUEST_BASE_IO));
    keys = (uint16_t *)((uint8_t *)live + (0x04000130u - GUEST_BASE_IO));

    memset(live, 0, IO_PAGE_BYTES);

    /* The ARM9 is running and its page is the live one. */
    CHECK(io_cpu() == IO_CPU_ARM9);
    CHECK(io_page(IO_CPU_ARM9) == live);
    CHECK(io_page(IO_CPU_ARM7) != NULL);
    CHECK(io_page(IO_CPU_ARM7) != (void *)live);
    CHECK(io_page(-1) == NULL);
    CHECK(io_page(2) == NULL);
    CHECK(io_mirror_count() == 2);

    /* A private register the ARM9 wrote, and a mirrored one. */
    *priv = 0x1234u;
    *keys = 0x03FFu;

    CHECK(io_switch(IO_CPU_ARM7) == 0);
    CHECK(io_cpu() == IO_CPU_ARM7);
    /* The ARM7's page starts as zeros; it must not inherit the ARM9's
     * registers, but the mirrored halfwords cross. */
    CHECK(*priv == 0);
    CHECK(*keys == 0x03FFu);
    /* And the ARM9's page is now the saved copy, with its own value intact. */
    CHECK(io_page(IO_CPU_ARM7) == live);
    CHECK(io_page(IO_CPU_ARM9) != (void *)live);
    CHECK(*(uint16_t *)((uint8_t *)io_page(IO_CPU_ARM9)
                        + (IO_PRIVATE - GUEST_BASE_IO)) == 0x1234u);

    /* The ARM7 writes its own private value and a new mirrored one. */
    *priv = 0x5678u;
    *keys = 0x02FFu;

    CHECK(io_switch(IO_CPU_ARM7) == 0); /* a no-op, and free */
    CHECK(*priv == 0x5678u);

    CHECK(io_switch(IO_CPU_ARM9) == 0);
    CHECK(io_cpu() == IO_CPU_ARM9);
    CHECK(*priv == 0x1234u);  /* the ARM9's own, not the ARM7's */
    CHECK(*keys == 0x02FFu);  /* the mirror crosses both ways */
    CHECK(*(uint16_t *)((uint8_t *)io_page(IO_CPU_ARM7)
                        + (IO_PRIVATE - GUEST_BASE_IO)) == 0x5678u);

    /* Every mirrored address, from the table rather than by hand. */
    for (i = 0; i < io_mirror_count(); i++) {
        const char *name = NULL;
        uint32_t addr = io_mirror_at(i, &name);
        uint16_t *at = (uint16_t *)((uint8_t *)live + (addr - GUEST_BASE_IO));

        CHECK(name != NULL);
        CHECK(addr >= (uint32_t)GUEST_BASE_IO
              && addr - (uint32_t)GUEST_BASE_IO < IO_PAGE_BYTES);

        *at = (uint16_t)(0x4000u + i);
        CHECK(io_switch(IO_CPU_ARM7) == 0);
        CHECK(*at == (uint16_t)(0x4000u + i));
        CHECK(io_switch(IO_CPU_ARM9) == 0);
        CHECK(*at == (uint16_t)(0x4000u + i));
    }

    /* IPCSYNC and EXMEMCNT are visible across on hardware and are not mirrored
     * here, because neither is shared storage. Asserting their absence is what
     * keeps a later reading of "visible across" from quietly adding them. */
    for (i = 0; i < io_mirror_count(); i++) {
        uint32_t addr = io_mirror_at(i, NULL);

        CHECK(addr != 0x04000180u);
        CHECK(addr != 0x04000204u);
    }

    CHECK(io_switch(-1) == -1);
    CHECK(io_switch(2) == -1);
    CHECK(io_cpu() == IO_CPU_ARM9);

    /* Leave the page and the copies the way a reset console has them; the input model is
     * the task that turns that into a requirement. */
    memset(live, 0, IO_PAGE_BYTES);
    io_pages_init();

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
