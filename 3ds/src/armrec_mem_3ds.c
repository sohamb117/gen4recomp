/*
 * 3ds/src/armrec_mem_3ds.c: armrec's guest memory, on a console that cannot
 * identity-map.
 *
 * The public functions are armrec's, the implementation is not. Everything
 * that asks the port where guest memory is asks through armrec_mem_init(),
 * armrec_region_count(), armrec_region_at(), armrec_is_guest_addr() and
 * armrec_guest_span_ok(). On PC those live in the mmap half of
 * tools/armrec/armrec_rt.c and the answer is an address, because a guest
 * address is a host address there. This console has no way to put memory at
 * 0x02000000, so the answers come out of the slab and the translator instead,
 * and the callers do not have to know which host they are on.
 *
 * Not a fork of armrec_rt.c, deliberately: splitting that file into a portable
 * half and an mmap half would be a rewrite of a file the sibling diamond port
 * also builds. So this re-implements the same public surface over the slab and
 * armrec_rt.c is left alone. That costs the duplication below, nine rows and
 * five VRAM windows named twice, and a test that keeps them the same.
 *
 * The row list is PC's, not the slab's, and the difference is the point:
 *
 *   - VRAM is not one row here even though it is one row in the slab. PC
 *     appends the five windows to its table, sized by the content each can
 *     address, so a walker sees each bank once instead of once per mirror.
 *   - The AGB slot is not reported at all, because armrec does not map it on
 *     PC either. It is still in the slab and still translates.
 *   - The order is PC's table order, not the slab's address order. A digest
 *     walks in table order, and two ports that disagree about the order
 *     produce two digests that cannot be compared.
 *
 * Zeroed here as well as in mem_init(), because mem_init() returns without
 * doing anything if the slab is already up, and by then the self-tests have
 * scribbled over every region. armrec_mem_init() is the moment guest memory
 * starts existing for the game, so it zeroes what it is about to hand over
 * regardless of who allocated it.
 *
 * No libctru in this file, so 3ds/tests/armrec_mem.c links it on a build
 * machine with a malloc'd slab of its own and runs the same self-test.
 */

#include <stdio.h>
#include <string.h>

#include "armrec_rt.h"

#include "armrec_mem_3ds.h"
#include "3ds_guest.h"
#include "3ds_guest_map.h"
#include "3ds_io.h"
#include "3ds_mem.h"
#include "3ds_vram.h"

int armrec_mem_ready = 0;

static char mem_err[160] = "";

const char *armrec_mem_strerror(void)
{
    return mem_err;
}

/*
 * armrec_rt.c's regions[], as slab rows and in its order. The bases and sizes
 * are not repeated: 3ds/include/3ds_guest_map.h holds them once and
 * 3ds/tests/guest_map_check.c static-asserts every one against armrec_rt.h.
 */
static const uint8_t mem_rows[] = {
    GUEST_R_ITCM,     GUEST_R_MAIN, GUEST_R_SHARED,  GUEST_R_WRAM,
    GUEST_R_ARM7WRAM, GUEST_R_IO,   GUEST_R_PALETTE, GUEST_R_OAM,
    GUEST_R_WINDOW,
};

#define MEM_ROWS ((int)(sizeof mem_rows / sizeof mem_rows[0]))

/* The two rows the region table does not report, for the reasons at the top:
 * VRAM (reported as its five windows) and the AGB slot (not armrec's on PC
 * either). A row added to the map has to be classified rather than silently
 * dropped, so the count is asserted instead of assumed. */
_Static_assert(MEM_ROWS + 2 == GUEST_R_COUNT,
               "every slab row is either reported here or one of VRAM / AGB");

/*
 * The five VRAM windows' names, spelled the way armrec_rt.c's
 * armrec_region_at() spells them. That file keeps its own copy for the same
 * reason this one does: the short names in the placement table are the
 * hardware's and 3ds/tests/vram_pin.py compares that table against armrec's
 * byte for byte, so the reported spelling cannot live there.
 */
static const char *const vram_row_name[] = {
    "VRAM main BG", "VRAM sub BG", "VRAM main OBJ", "VRAM sub OBJ", "VRAM LCDC"
};

#define VRAM_ROWS ((int)(sizeof vram_row_name / sizeof vram_row_name[0]))

int armrec_mem_init(void)
{
    const struct mem_slab *slab;

    if (armrec_mem_ready) {
        return 0;
    }
    mem_err[0] = '\0';

    if (mem_init() != 0) {
        snprintf(mem_err, sizeof mem_err, "cannot take the guest slab: %s",
                 mem_strerror());
        return -1;
    }

    slab = mem_info();
    if (slab->base == NULL) {
        snprintf(mem_err, sizeof mem_err,
                 "the guest slab reports success with no memory");
        return -1;
    }

    guest_bind(slab->base);
    memset(slab->base, 0, slab->bytes);

    /*
     * Ready before the two models that read guest memory, which is
     * armrec_mem_init()'s order on PC and for the same reason: vram_init()
     * reads the nine VRAMCNT registers back out of the I/O row, and the I/O
     * row is only guest memory once the slab is bound.
     */
    armrec_mem_ready = 1;
    io_pages_init();
    vram_init();
    return 0;
}

void armrec_mem_free(void)
{
    armrec_mem_ready = 0;
    /*
     * Unbind, then let the VRAM model re-read its store pointer through the
     * unbound translator (it comes back NULL) and only then give the
     * memory back. In the other order the bank map would hold a pointer into
     * freed heap and a stray VRAM access would land in it.
     */
    guest_bind(NULL);
    vram_init();
    mem_exit();
}

int armrec_region_count(void)
{
    return MEM_ROWS + VRAM_ROWS;
}

int armrec_region_at(int i, uint32_t *base, uint32_t *size, const char **name)
{
    if (i >= MEM_ROWS && i < MEM_ROWS + VRAM_ROWS) {
        int w = i - MEM_ROWS;

        if (!vram_window_at(w, base, size)) {
            return 0;
        }
        if (name != NULL) {
            *name = vram_row_name[w];
        }
        return 1;
    }
    if (i < 0 || i >= MEM_ROWS) {
        return 0;
    }

    {
        const struct guest_region *r = &guest_map[mem_rows[i]];

        if (base != NULL) {
            *base = r->base;
        }
        /* Backing, not the addressed size. They are equal for all nine rows,
         * and the self-test checks that rather than trusting it: saying
         * `backing` is saying that a reported size is memory that exists. */
        if (size != NULL) {
            *size = r->backing;
        }
        if (name != NULL) {
            *name = r->name;
        }
    }
    return 1;
}

int armrec_is_guest_addr(uint32_t addr)
{
    int i;

    for (i = 0; i < MEM_ROWS; i++) {
        const struct guest_region *r = &guest_map[mem_rows[i]];

        if (addr >= r->base && addr - r->base < r->backing) {
            return 1;
        }
    }
    return 0;
}

/*
 * The address-space question, not the region-table one, armrec_rt.c has the
 * argument. The whole of 0x06000000-0x07000000 is a fair VRAM address even
 * though armrec_region_at() reports only the content of each window, and a
 * mirror is a real address. Whether a *bank* answers there is VRAMCNT's
 * business and armrec_host_ptr()'s answer, not this function's.
 */
int armrec_guest_span_ok(uint32_t addr, uint32_t len)
{
    int i;

    if (addr >= ARM_VRAM_BASE && (uint64_t)addr + len <= (uint64_t)ARM_VRAM_END) {
        return 1;
    }
    for (i = 0; i < MEM_ROWS; i++) {
        const struct guest_region *r = &guest_map[mem_rows[i]];

        if (addr >= r->base && (uint64_t)addr + len <= (uint64_t)r->base + r->backing) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#ifdef ARMREC_MEM_SELFTEST_VERBOSE
#define FAILNOTE() fprintf(stderr, "  armrec_mem_3ds.c:%d failed\n", __LINE__)
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

/*
 * What the region table has to say once armrec_mem_init() has run. Read-only:
 * It translates and round-trips addresses but writes nothing, so it can run
 * before the models that scribble and it does not have to clean up after
 * itself.
 *
 * Requires a successful armrec_mem_init(). Returns failures, fills `*ran`.
 */
int armrec_mem_selftest(int *ranOut)
{
    static const uint32_t vram_base[] = {
        0x06000000u, 0x06200000u, 0x06400000u, 0x06600000u, 0x06800000u
    };
    static const uint32_t vram_content[] = {
        32u * 0x4000u, 8u * 0x4000u, 16u * 0x4000u, 8u * 0x4000u, 41u * 0x4000u
    };
    int ran = 0;
    int failed = 0;
    int i;
    int found_window = 0;

    CHECK(armrec_mem_ready == 1);
    CHECK(armrec_region_count() == MEM_ROWS + VRAM_ROWS);
    /* The window count is the VRAM model's, and the names are this file's;
     * a window added there without a name here would be unreportable. */
    CHECK(vram_window_count() == VRAM_ROWS);

    if (!armrec_mem_ready) {
        if (ranOut != NULL) {
            *ranOut = ran;
        }
        return failed;
    }

    /* The nine memory rows: what is reported, that it translates, and that the
     * translation answers to the address it was reported at. */
    for (i = 0; i < MEM_ROWS; i++) {
        const struct guest_region *r = &guest_map[mem_rows[i]];
        uint32_t base = 0;
        uint32_t size = 0;
        const char *name = NULL;
        void *first;
        void *last;

        CHECK(armrec_region_at(i, &base, &size, &name) == 1);
        CHECK(base == r->base);
        CHECK(size == r->backing);
        CHECK(name == r->name);
        /* A reported row is memory that exists, all of it: a row whose
         * addresses outran its backing would hand a digest a base and a
         * length that stop translating part way through. */
        CHECK(r->backing == r->size);

        first = armrec_host_ptr(base);
        last = armrec_host_ptr(base + size - 1);
        CHECK(first != NULL);
        CHECK(last != NULL);
        CHECK(first == NULL || armrec_guest_addr(first) == base);
        CHECK(last == NULL || armrec_guest_addr(last) == base + size - 1);

        CHECK(armrec_is_guest_addr(base));
        CHECK(armrec_is_guest_addr(base + size - 1));
        CHECK(armrec_guest_span_ok(base, size));
        CHECK(!armrec_guest_span_ok(base, size + 1));

        if (name != NULL && strcmp(name, ARMREC_PORT_WINDOW_NAME) == 0) {
            found_window = 1;
            CHECK(base == ARM_PORT_WINDOW_BASE);
            CHECK(size == ARM_PORT_WINDOW_SIZE);
        }
    }

    /* pc_guest_window.c finds its memory by that string and by nothing else. */
    CHECK(found_window == 1);

    /* The five VRAM windows, reported by content and not by span. */
    for (i = 0; i < VRAM_ROWS; i++) {
        uint32_t base = 0;
        uint32_t size = 0;
        const char *name = NULL;

        CHECK(armrec_region_at(MEM_ROWS + i, &base, &size, &name) == 1);
        CHECK(base == vram_base[i]);
        CHECK(size == vram_content[i]);
        CHECK(name == vram_row_name[i]);
    }

    /* Out of range says so and leaves the caller's variables alone. */
    {
        uint32_t base = 0xA5A5A5A5u;
        uint32_t size = 0xA5A5A5A5u;
        const char *name = (const char *)&ran;

        CHECK(armrec_region_at(-1, &base, &size, &name) == 0);
        CHECK(armrec_region_at(MEM_ROWS + VRAM_ROWS, &base, &size, &name) == 0);
        CHECK(base == 0xA5A5A5A5u);
        CHECK(size == 0xA5A5A5A5u);
        CHECK(name == (const char *)&ran);
        CHECK(armrec_region_at(0, NULL, NULL, NULL) == 1);
    }

    /* The holes, and the two regions that are deliberately not in the table.
     * VRAM and the AGB slot answer the same way here as they do on PC. */
    CHECK(!armrec_is_guest_addr(0x00000000u));
    CHECK(!armrec_is_guest_addr(0x01FF7FFFu)); /* one below ITCM */
    CHECK(!armrec_is_guest_addr(0x02400000u)); /* main RAM .. shared work */
    CHECK(!armrec_is_guest_addr(0x02900000u)); /* shared work .. port window */
    CHECK(!armrec_is_guest_addr(ARM_VRAM_BASE));
    CHECK(!armrec_is_guest_addr(GUEST_BASE_AGB));
    CHECK(!armrec_is_guest_addr(0xFFFFFFFFu));

    /* ... but a VRAM span is still guest address space, mirrors included, and
     * a span that leaves it is not. */
    CHECK(armrec_guest_span_ok(ARM_VRAM_BASE, ARM_VRAM_SIZE));
    CHECK(armrec_guest_span_ok(0x068A4000u, 0x4000u)); /* a hole in LCDC */
    CHECK(!armrec_guest_span_ok(ARM_VRAM_END - 4u, 8u));

    /* A span that straddles the 4 MB hole above main RAM passes both ends and
     * has to fail anyway. */
    CHECK(!armrec_guest_span_ok(ARM_MAIN_RAM_BASE + ARM_MAIN_RAM_SIZE - 4u,
                                ARM_SHARED_BASE - ARM_MAIN_RAM_BASE));
    CHECK(armrec_guest_span_ok(ARM_MAIN_RAM_BASE + ARM_MAIN_RAM_SIZE, 0u));

    /* The AGB slot is in the slab and translates; it is just not armrec's. */
    CHECK(armrec_host_ptr(GUEST_BASE_AGB) != NULL);
    CHECK(!armrec_guest_span_ok(GUEST_BASE_AGB, 4u));

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
