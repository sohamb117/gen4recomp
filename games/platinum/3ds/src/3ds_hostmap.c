/*
 * 3ds/src/3ds_hostmap.c: armrec_host_ptr(), memoised for the rasterizer.
 *
 * On the PC port a guest address is a host address, so pc/hw/pc_gpu2d.c reads
 * VRAM through a cast and the address space does the work. Here it cannot: 3DS
 * userland will not map 0x06000000, and which bank answers a VRAM address is
 * VRAMCNT's business. So every one of those reads goes through the translator,
 * and the translator costs a scan over the eleven guest rows plus, for VRAM, a
 * scan over the five windows containing a `%`, which on ARM11 is a call into
 * libgcc because this CPU has no divide instruction.
 *
 * That is fine for a caller that translates once and keeps the pointer, which
 * is nearly all of this port. It is not fine for a software rasterizer, which
 * reads a tile map entry, then a tile row, then the next map entry, tens of
 * thousands of times a frame. The answer is not a faster translator, it is not
 * asking again.
 *
 * 16 KB is the grain because it is VRAM's own placement granularity, the unit
 * the nine banks are placed in. Within one 16 KB block a VRAM address cannot
 * change bank, so one translation describes the whole block.
 *
 * A block carries a limit and not just a base, because the 16 KB assumption is
 * VRAM's and not every region's. The palette and OAM rows are 4 KB inside a
 * 16 KB block, so a pointer taken at the block base and walked 8 KB in would
 * leave the row. The renderer masks its offsets and would never do that, but a
 * cache that is correct only because its caller behaves is a trap for the next
 * caller.
 *
 * The first version refused any block that was not contiguous for its whole
 * 16 KB, and that was measured wrong: the palette is read once per pixel and
 * its block is 4 KB of row and 12 KB of nothing, so 49,344 of one frame's
 * 61,859 lookups took the refusal path. So a miss measures instead: it probes
 * the block at 16 KB, 8 KB, 4 KB and down to 256 bytes and keeps the largest
 * run that is really contiguous. A block with no run even 256 bytes long is
 * refused and counted.
 *
 * A block whose base translates to NULL is unmapped for its whole 16 KB, so it
 * gets the full limit and the zero answer below. Every row in the guest map
 * starts on a 16 KB boundary, so a row can never begin part-way through a
 * block that started unmapped.
 *
 * An unmapped address is not NULL. On hardware a VRAM window with no bank
 * behind it reads zeros, and pc_gpu2d.c is written to that contract: it
 * dereferences the pointer without checking. Returning NULL here would turn
 * "this bank is not mapped yet", a normal frame during set-up, into a data
 * abort. So the answer is a shared 16 KB block of zeros. The cost is that a
 * write through an unmapped address silently lands in it and is lost, which is
 * also what the PC port's floor does.
 *
 * That is a floor, and 3ds_vram.c says at length that the model deliberately
 * does not have one. Both are right and they are about different things. The
 * model must answer NULL, because a store hook has to tell "no bank here" from
 * "here it is". This is a memoising layer above the model, used by one caller
 * written against the PC port's floor. The floor lives here, where it is
 * read-only, rather than in the model where it would take that distinction
 * away.
 *
 * The cache is flushed by the model, not the caller: 3ds_vram.c calls
 * hostmap_flush() through a weak reference every time it rebuilds the bank
 * map, so a bank change cannot leave a stale pointer behind.
 */

#include <string.h>

#include "3ds_guest.h"      /* armrec_host_ptr() */
#include "3ds_hostmap.h"

/*
 * Eight entries, searched linearly, replaced round-robin.
 *
 * Fully associative because direct-mapped was measured useless here. Indexing
 * by the low bits of the block number is the obvious thing and it thrashed:
 * One frame of the test took 12,484 misses over 105,058 lookups. The
 * reason is the guest map itself, I/O is 0x04000000, palette 0x05000000,
 * VRAM 0x06000000, OAM 0x07000000, and every one of those block numbers has
 * zero in its low bits, so the four regions a scanline alternates between all
 * landed in the same set no matter how many sets there were. Hashing the high
 * bits in would work for these four addresses and would be a number chosen to
 * fit today's map.
 *
 * A linear search over eight is about eight compares worst case against an
 * armrec_host_ptr() that scans eleven rows and, for VRAM, five windows with a
 * modulo in each, a call into libgcc on a CPU with no divide instruction. The
 * working set this exists for is about six blocks: a tile map and a tile set
 * per background layer, sprites, and the palette. Eight holds it, and the same
 * frame after the change takes four misses.
 */
#define HOSTMAP_WAYS 8
#define HOSTMAP_BLK_SHIFT 14
#define HOSTMAP_BLK_SIZE (1u << HOSTMAP_BLK_SHIFT)
#define HOSTMAP_BLK_MASK (HOSTMAP_BLK_SIZE - 1u)

/* Tag 0 is empty rather than "block 0", so a cleared cache misses. A cached
 * block whose base is NULL is a real answer: nothing is mapped there. */
static uint32_t sTag[HOSTMAP_WAYS];
static unsigned char *sBase[HOSTMAP_WAYS];
/* Bytes from the block base that are one contiguous mapping. 0 means the block
 * is not usable at all and every access to it translates. */
static uint32_t sLimit[HOSTMAP_WAYS];
static unsigned sVictim;

static unsigned char sZero[HOSTMAP_BLK_SIZE];

static unsigned long sHits;
static unsigned long sMisses;
static unsigned long sUncacheableN;

/*
 * The most recent answer, published so the renderer need not call at all.
 * Measured: a 2D frame makes about 470,000 lookups, one per VRAM, palette or
 * OAM byte the software 2D engine reads, and the whole frame is 46 million
 * cycles, so a cross-object call, an eight-way scan and a counter increment
 * per pixel is a large part of the frame on its own. On the desktop the same
 * macro is a cast and costs nothing, which is why this never showed up there.
 *
 * A scanline renderer walks one block for a long time, so a single entry
 * checked inline catches nearly all of it and the eight-way cache stays
 * behind it for the rest. `hostmap_fast_tag` is 0 when there is nothing to
 * use, which is what makes the inline test safe: a NULL base or a flushed
 * cache simply falls through to the call.
 */
unsigned int hostmap_fast_tag;
unsigned char *hostmap_fast_base;
unsigned int hostmap_fast_limit;

static void fast_publish(uint32_t tag, unsigned char *base, uint32_t limit)
{
    if (base == NULL || limit == 0u) {
        hostmap_fast_tag = 0u;      /* nothing usable: always take the call */
        return;
    }
    hostmap_fast_base = base;
    hostmap_fast_limit = limit;
    hostmap_fast_tag = tag;
}

void hostmap_flush(void)
{
    memset(sTag, 0, sizeof sTag);
    memset(sBase, 0, sizeof sBase);
    memset(sLimit, 0, sizeof sLimit);
    sVictim = 0;
    sHits = 0;
    sMisses = 0;
    sUncacheableN = 0;
    hostmap_fast_tag = 0u;
    hostmap_fast_base = NULL;
    hostmap_fast_limit = 0u;
}

void *hostmap_ptr(uint32_t guest)
{
    uint32_t blk = guest >> HOSTMAP_BLK_SHIFT;
    uint32_t off = guest & HOSTMAP_BLK_MASK;
    uint32_t tag = blk + 1u;      /* 0 is "empty", so a cleared cache misses */
    uint32_t base_addr;
    unsigned char *base;
    uint32_t run;
    unsigned i;

    for (i = 0; i < HOSTMAP_WAYS; i++) {
        if (sTag[i] != tag) {
            continue;
        }
        if (off < sLimit[i]) {
            sHits++;
            fast_publish(tag, sBase[i], sLimit[i]);
            return sBase[i] != NULL ? sBase[i] + off : sZero + off;
        }
        /* Cached, but this address is past the contiguous run, the rest of
         * the block is a different mapping or none. Ask about it alone. */
        sUncacheableN++;
        base = (unsigned char *)armrec_host_ptr(guest);
        return base != NULL ? base : sZero + off;
    }

    sMisses++;
    i = sVictim;
    sVictim = (sVictim + 1u) % HOSTMAP_WAYS;

    base_addr = blk << HOSTMAP_BLK_SHIFT;
    base = (unsigned char *)armrec_host_ptr(base_addr);
    sTag[i] = tag;
    sBase[i] = base;

    if (base == NULL) {
        sLimit[i] = HOSTMAP_BLK_SIZE;
        return sZero + off;
    }

    /* The largest power-of-two run from the block base that is really one
     * mapping. Seven probes at worst, once per block. */
    for (run = HOSTMAP_BLK_SIZE; run >= 256u; run >>= 1) {
        if ((unsigned char *)armrec_host_ptr(base_addr + run - 1u)
                == base + run - 1u) {
            break;
        }
    }
    if (run < 256u) {
        sLimit[i] = 0;
        sUncacheableN++;
        base = (unsigned char *)armrec_host_ptr(guest);
        return base != NULL ? base : sZero + off;
    }
    sLimit[i] = run;
    fast_publish(tag, base, run);

    if (off < run) {
        return sBase[i] + off;
    }
    sUncacheableN++;
    base = (unsigned char *)armrec_host_ptr(guest);
    return base != NULL ? base : sZero + off;
}

unsigned long hostmap_hits(void)        { return sHits; }
unsigned long hostmap_misses(void)      { return sMisses; }
unsigned long hostmap_uncacheable(void) { return sUncacheableN; }

/* ------------------------------------------------------------------ */
/* The self-test                                                       */
/* ------------------------------------------------------------------ */

int hostmap_selftest(int *ran)
{
    static const uint32_t kProbe[] = {
        0x02000000u, 0x0200FFFFu, 0x023FFFFCu,   /* main RAM, three blocks */
        0x027E0000u, 0x027FFFA8u,                /* the shared work area */
        0x04000000u, 0x04000130u, 0x04001000u,   /* I/O, both engines */
        0x05000000u, 0x050007FEu,                /* palette */
        0x07000000u, 0x070007FEu,                /* OAM */
        0x06000000u, 0x06004000u, 0x06200000u,   /* VRAM windows */
    };
    int n = 0;
    int bad = 0;
    unsigned i;

#define CHECK(cond)          \
    do {                     \
        n++;                 \
        if (!(cond)) bad++;  \
    } while (0)

    hostmap_flush();

    /*
     * The whole claim, one probe at a time: the memoised answer is the
     * translator's answer. Where the translator says NULL the cache says "a
     * zero block", so that case is checked as "reads zero" rather than as
     * pointer equality; they are deliberately different answers.
     */
    for (i = 0; i < sizeof kProbe / sizeof kProbe[0]; i++) {
        void *want = armrec_host_ptr(kProbe[i]);
        void *got = hostmap_ptr(kProbe[i]);

        CHECK(got != NULL);
        if (want != NULL) {
            CHECK(got == want);
        } else {
            CHECK(got != NULL && *(const unsigned char *)got == 0);
        }
    }

    /* Twice through, to exercise the hit path against the same answers. */
    for (i = 0; i < sizeof kProbe / sizeof kProbe[0]; i++) {
        void *want = armrec_host_ptr(kProbe[i]);

        if (want != NULL) {
            CHECK(hostmap_ptr(kProbe[i]) == want);
        }
    }
    CHECK(hostmap_hits() > 0);

    /*
     * The limit. The palette row is 4 KB inside a 16 KB block, so the run from
     * the block base is 4 KB and an address past it must not be answered from
     * the cached base; which is where a pointer into whatever the slab put
     * next would come from. Reached by asking for the row and then for an
     * address past its backing.
     */
    {
        unsigned long before = hostmap_uncacheable();

        (void)hostmap_ptr(0x05000000u);
        (void)hostmap_ptr(0x05003FFFu);
        CHECK(hostmap_uncacheable() > before);
        /* And it still answers correctly for the mapped part. */
        CHECK(hostmap_ptr(0x05000010u) == armrec_host_ptr(0x05000010u));
    }

    /* A flush really forgets. */
    hostmap_flush();
    CHECK(hostmap_hits() == 0);
    CHECK(hostmap_misses() == 0);
    CHECK(hostmap_ptr(0x02000000u) == armrec_host_ptr(0x02000000u));
    CHECK(hostmap_misses() == 1);

    /* The zero block is still zero after all of that; nothing above may
     * have written through an unmapped address. */
    for (i = 0; i < HOSTMAP_BLK_SIZE; i += 512u) {
        CHECK(sZero[i] == 0);
    }

#undef CHECK

    hostmap_flush();
    if (ran != NULL) {
        *ran = n;
    }
    return bad;
}
