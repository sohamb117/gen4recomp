/*
 * 3ds/src/3ds_state.c: the guest state digest, through the translator.
 *
 * Determinism is a claim about two runs, and the only way to settle it is to
 * take a number from each and compare. pc/src/pc_state.c does that on PC:
 * FNV-1a over every region armrec reports, folded into one 64-bit value. This
 * is the same arithmetic on a console where a guest address is not a host
 * address, so the walk goes through armrec_host_ptr() instead of a cast.
 *
 * The function names are pc_state's, deliberately. This file supplies the
 * strong symbols and pc/src/pc_state.c is not compiled here, so a caller asks
 * for pc_state_digest() and does not learn which host it is on.
 *
 * Why it cannot be one fnv1a() call per region, which is what PC does. Both
 * reasons are VRAM:
 *
 *   - a window's contents are not contiguous in the bank store. The main BG
 *     window's 32 blocks can come from four different banks in any order, so
 *     the walk is per 16 KB block.
 *   - a block with no bank behind it hashes as zeros, because that is what the
 *     console reads there and what PC hashes: armrec_rt.c maps an anonymous
 *     zero floor under the whole window. Skipping the block instead would give
 *     a different number for the same guest state.
 *
 * Zero bytes are not copied from anywhere: FNV-1a of a zero byte is a multiply
 * per byte and needs no buffer.
 *
 * Host pointers in guest memory are marked. The PC port keeps a list of guest
 * words that hold host pointers, and the digest skips exactly those, because a
 * relink moves them and nothing else. The marks are placed at the write, by
 * patched game code. With an empty mark set the arithmetic is byte-identical
 * to what it was.
 *
 * What counts as a host pointer differs here. PC asks whether the word is at
 * least 0x10000000, which works because a guest address is a host address
 * there. Here a host pointer is either into the slab, which
 * armrec_guest_addr() answers, or into this image, which is what a function
 * pointer is. Hence the two tests.
 *
 * One ambiguity is worth stating rather than hiding. The 3DS puts the
 * application heap at 0x08000000 and the DS puts the GBA slot's ROM there, so
 * a guest word holding a genuine AGB-slot address in the slab's range would be
 * read as a host pointer and skipped. The AGB slot is a 128 KB zero buffer on
 * this port and nothing stores an address into it, but if a digest ever loses
 * a word it should not have, this is the first reason to look for.
 *
 * No libctru here, so 3ds/tests/state_digest.c runs all of it on the host.
 */

#include <stddef.h>
#include <stdlib.h>

#include "armrec_rt.h"

#include "3ds_state.h"
#include "3ds_guest.h"
#include "3ds_guest_map.h"
#include "3ds_vram.h"

#define FNV64_OFFSET 0xcbf29ce484222325ULL
#define FNV64_PRIME  0x00000100000001b3ULL

uint64_t pc_state_fnv1a(uint64_t h, const void *buf, uint32_t n)
{
    const unsigned char *p = (const unsigned char *)buf;
    uint32_t i;

    for (i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= FNV64_PRIME;
    }
    return h;
}

/* `n` zero bytes, without a buffer to read them out of. */
static uint64_t fnv1a_zeros(uint64_t h, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        h *= FNV64_PRIME;
    }
    return h;
}

/*
 * How many bytes from `addr` are one contiguous host run, capped at `max`.
 * Inside a VRAM window that is the rest of the 16 KB block, the next block
 * may be a different bank, or none. Everywhere else it is the rest of the
 * region, because a slab row is one allocation.
 */
static uint32_t host_run(uint32_t addr, uint32_t max)
{
    uint32_t n;
    int i;

    if (addr >= ARM_VRAM_BASE && addr < ARM_VRAM_END) {
        n = VRAM_BLK - (addr & (VRAM_BLK - 1u));
        return n < max ? n : max;
    }
    for (i = 0; i < GUEST_R_COUNT; i++) {
        const struct guest_region *r = &guest_map[i];

        if (addr >= r->base && addr - r->base < r->size) {
            n = r->size - (addr - r->base);
            return n < max ? n : max;
        }
    }
    /* Outside the map entirely. Nothing armrec reports is here, so this is a
     * caller's span rather than a region; hash it as the hole it is, a page at
     * a time so the loop still makes progress. */
    return max < 0x1000u ? max : 0x1000u;
}

/* ------------------------------------------------------------------ */
/* Host-pointer marks                                           */
/* ------------------------------------------------------------------ */

#ifdef __3DS__
extern char __start__[];
extern char __end__[];
#endif

static uint32_t sImageLo;
static uint32_t sImageHi;

void state_set_image_bounds(uint32_t lo, uint32_t hi)
{
    sImageLo = lo;
    sImageHi = hi;
}

static int in_image(uint32_t w)
{
    uint32_t lo = sImageLo;
    uint32_t hi = sImageHi;

#ifdef __3DS__
    /* 3dsx.ld's names. pc/hw/pc_spu.c asks for the same two addresses under
     * the names a glibc link gives them, and the link aliases those
     * rather than editing a file both ports compile. */
    if (lo == 0 && hi == 0) {
        lo = (uint32_t)(uintptr_t)__start__;
        hi = (uint32_t)(uintptr_t)__end__;
    }
#endif
    return hi > lo && w >= lo && w < hi;
}

static uint32_t *sMarks;
static uint32_t sMarkN;
static uint32_t sMarkCap;

/* First mark at or after `addr`. The list is sorted, so the digest can walk
 * it once alongside the span instead of searching per word. */
static uint32_t mark_lb(uint32_t addr)
{
    uint32_t lo = 0;
    uint32_t hi = sMarkN;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;

        if (sMarks[mid] < addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

void pc_state_mark_host_word(uint32_t addr)
{
    uint32_t i;

    addr &= ~3u;
    /* A word the digest will never read is not worth remembering: a mark of a
     * host-side field (a static OSThread, a table in .bss) is not guest
     * memory. armrec_host_ptr() is the map itself answering. */
    if (armrec_host_ptr(addr) == NULL) {
        return;
    }

    i = mark_lb(addr);
    if (i < sMarkN && sMarks[i] == addr) {
        return;
    }

    if (sMarkN == sMarkCap) {
        uint32_t ncap = sMarkCap != 0 ? sMarkCap * 2u : 256u;
        uint32_t *n = (uint32_t *)realloc(sMarks, ncap * sizeof *n);

        /* Out of memory keeps the word in the digest, which makes a later
         * comparison notice the miss. Dropping the digest instead would hide
         * it. Same choice pc/src/pc_state.c makes. */
        if (n == NULL) {
            return;
        }
        sMarks = n;
        sMarkCap = ncap;
    }
    if (i < sMarkN) {
        uint32_t j;

        for (j = sMarkN; j > i; j--) {
            sMarks[j] = sMarks[j - 1];
        }
    }
    sMarks[i] = addr;
    sMarkN++;
}

void pc_state_mark_host_field(const void *field)
{
    /* PC is handed a guest address, because there it is also the host one.
     * Here the caller has a host pointer and the translator says where it
     * lives. */
    uint32_t guest = field != NULL ? armrec_guest_addr(field) : 0u;

    if (guest != 0) {
        pc_state_mark_host_word(guest);
    }
}

static int is_host_ptr(uint32_t w)
{
    return armrec_guest_addr((const void *)(uintptr_t)w) != 0 || in_image(w);
}

void pc_state_scan_mark_host_words(const void *obj, uint32_t n)
{
    const uint32_t *p = (const uint32_t *)obj;
    uint32_t base;
    uint32_t words;
    uint32_t i;

    if (obj == NULL || n < 4) {
        return;
    }
    base = armrec_guest_addr(obj);
    if (base == 0) {
        return;
    }
    words = n / 4;
    for (i = 0; i < words; i++) {
        if (is_host_ptr(p[i])) {
            pc_state_mark_host_word(base + i * 4u);
        }
    }
}

uint32_t pc_state_host_word_count(void)
{
    return sMarkN;
}

void state_clear_host_marks(void)
{
    sMarkN = 0;
}

uint64_t pc_state_digest_span(uint32_t base, uint32_t size)
{
    uint64_t h = FNV64_OFFSET;
    uint32_t pos = base;
    uint32_t left = size;
    uint32_t m;

    if (!armrec_mem_ready) {
        return 0;
    }
    m = mark_lb(base);
    while (left != 0) {
        uint32_t n = host_run(pos, left);
        uint32_t end = pos + n;

        /*
         * The run, minus every marked word inside it. A span with no marks
         * hashes exactly as it did before marking (one call, same bytes) so
         * STATE_ZERO_DIGEST and every pinned number stay where they were.
         */
        while (m < sMarkN && sMarks[m] < end) {
            uint32_t mark = sMarks[m];

            if (mark >= pos) {
                const void *p = armrec_host_ptr(pos);

                if (mark > pos) {
                    h = p != NULL ? pc_state_fnv1a(h, p, mark - pos)
                                  : fnv1a_zeros(h, mark - pos);
                }
                pos = mark + 4;
            }
            m++;
        }
        if (pos < end) {
            const void *p = armrec_host_ptr(pos);

            h = p != NULL ? pc_state_fnv1a(h, p, end - pos)
                          : fnv1a_zeros(h, end - pos);
        }
        pos = end;
        left -= n;
    }
    return h;
}

uint64_t pc_state_digest(void)
{
    uint64_t total = FNV64_OFFSET;
    int i;
    int n;

    if (!armrec_mem_ready) {
        return 0;
    }
    n = armrec_region_count();
    for (i = 0; i < n; i++) {
        uint32_t base = 0;
        uint32_t size = 0;
        uint64_t d;
        unsigned char be[8];
        int k;

        if (!armrec_region_at(i, &base, &size, NULL)) {
            continue;
        }
        d = pc_state_digest_span(base, size);
        /* Big-endian, so the combined value is a property of the guest's
         * contents and not of the host's byte order. armrec_rt.c's caller
         * folds it the same way. */
        for (k = 0; k < 8; k++) {
            be[k] = (unsigned char)(d >> (56 - 8 * k));
        }
        total = pc_state_fnv1a(total, be, 8);
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#ifdef STATE_SELFTEST_VERBOSE
#include <stdio.h>
#define FAILNOTE() fprintf(stderr, "  3ds_state.c:%d failed\n", __LINE__)
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

int state_selftest(int *ranOut)
{
    int ran = 0;
    int failed = 0;
    uint64_t zero;
    uint64_t d;
    uint8_t *pal;
    uint8_t *lcdc;

    if (!armrec_mem_ready) {
        if (ranOut != NULL) {
            *ranOut = 0;
        }
        return 1;
    }

    /*
     * The cross-host pin. Guest memory zeroed, no bank mapped, no host
     * pointer anywhere in it: the one state whose digest is a property of the
     * map and the arithmetic alone, so a PC run at the same point owes the
     * same number. 3ds/tests/state_digest.c recomputes it from the region
     * sizes with a second implementation rather than trusting this one.
     */
    zero = pc_state_digest();
    CHECK(zero == STATE_ZERO_DIGEST);
    /* And it is a function of the state and nothing else. Checked over one
     * region rather than the whole map: a full digest is 8 MB of FNV, which
     * is about half a second on this console, so the console pays for three
     * of them here and not for six. */
    CHECK(pc_state_digest_span(GUEST_BASE_MAIN, GUEST_SIZE_MAIN)
          == pc_state_digest_span(GUEST_BASE_MAIN, GUEST_SIZE_MAIN));

    /* An empty span is the basis, and a span outside the map hashes as the
     * zeros a console reads there. */
    CHECK(pc_state_digest_span(GUEST_BASE_MAIN, 0) == 0xcbf29ce484222325ULL);
    CHECK(pc_state_digest_span(0x00100000u, 0x1000u)
          == pc_state_digest_span(GUEST_BASE_OAM, 0x1000u));

    /* One byte changes its region and the total. */
    pal = (uint8_t *)armrec_host_ptr(GUEST_BASE_PALETTE);
    CHECK(pal != NULL);
    if (pal != NULL) {
        d = pc_state_digest_span(GUEST_BASE_PALETTE, GUEST_SIZE_PALETTE);
        pal[0x321] = 0x5A;
        CHECK(pc_state_digest_span(GUEST_BASE_PALETTE, GUEST_SIZE_PALETTE) != d);
        CHECK(pc_state_digest() != zero);
        pal[0x321] = 0x00;
        CHECK(pc_state_digest_span(GUEST_BASE_PALETTE, GUEST_SIZE_PALETTE) == d);
    }

    /*
     * VRAM, which is the whole reason this walk is not one call per region.
     * Bank A into the LCDC window, a marker written through it, and the main
     * BG window's digest must not move; no bank is there. Then the same
     * bank into main BG, where the marker has to show up in that window's
     * digest at the address the placement model says.
     */
    {
        uint8_t *cnt_a = (uint8_t *)armrec_host_ptr(vram_cnt_addr(0));
        uint64_t lcdc_zero = pc_state_digest_span(0x06800000u, 41u * VRAM_BLK);
        uint64_t abg_zero = pc_state_digest_span(0x06000000u, 32u * VRAM_BLK);

        CHECK(cnt_a != NULL);
        if (cnt_a != NULL) {
            *cnt_a = 0x80; /* A enabled, LCDC */
            vram_touch();
            lcdc = (uint8_t *)armrec_host_ptr(0x06800000u);
            CHECK(lcdc != NULL);
            if (lcdc != NULL) {
                lcdc[0x1234] = 0xC3;
            }
            CHECK(pc_state_digest_span(0x06800000u, 41u * VRAM_BLK) != lcdc_zero);
            CHECK(pc_state_digest_span(0x06000000u, 32u * VRAM_BLK) == abg_zero);

            *cnt_a = 0x81; /* A enabled, main BG, offset 0 */
            vram_touch();
            CHECK(pc_state_digest_span(0x06000000u, 32u * VRAM_BLK) != abg_zero);
            /* And LCDC is back to zeros, because a bank shows there only
             * while its VRAMCNT says LCDC: the bytes did not move, the
             * addresses that reach them did. */
            CHECK(pc_state_digest_span(0x06800000u, 41u * VRAM_BLK) == lcdc_zero);

            lcdc = (uint8_t *)armrec_host_ptr(0x06000000u);
            CHECK(lcdc != NULL && lcdc[0x1234] == 0xC3);

            /* Put it back the way a reset console has it. */
            if (lcdc != NULL) {
                lcdc[0x1234] = 0x00;
            }
            *cnt_a = 0x00;
            vram_touch();
            CHECK(pc_state_digest_span(0x06000000u, 32u * VRAM_BLK) == abg_zero);
            CHECK(pc_state_digest_span(0x06800000u, 41u * VRAM_BLK) == lcdc_zero);
        }
    }

    /*
     * The marks, 7.12. The property is not "a marked word hashes as zero";
     * a skipped word is four bytes the fold never sees, which is a different
     * number from four zero bytes. It is that the digest stops depending on
     * that word at all, so every check here writes two different values into
     * a marked word and asks for the same digest twice.
     */
    {
        uint32_t addr = GUEST_BASE_PALETTE + 0x40u;
        uint32_t *w = (uint32_t *)armrec_host_ptr(addr);
        uint64_t d0;

        CHECK(w != NULL);
        CHECK(pc_state_host_word_count() == 0);
        if (w != NULL) {
            w[0] = 0xAABBCCDDu;
            pc_state_mark_host_word(addr);
            CHECK(pc_state_host_word_count() == 1);
            /* The same word again, unaligned, is not a second word. */
            pc_state_mark_host_word(addr + 2u);
            CHECK(pc_state_host_word_count() == 1);

            d0 = pc_state_digest_span(GUEST_BASE_PALETTE, GUEST_SIZE_PALETTE);
            w[0] = 0x11223344u;
            CHECK(pc_state_digest_span(GUEST_BASE_PALETTE, GUEST_SIZE_PALETTE)
                  == d0);
            /* Its neighbour is still in the digest. */
            w[1] = 0x55667788u;
            CHECK(pc_state_digest_span(GUEST_BASE_PALETTE, GUEST_SIZE_PALETTE)
                  != d0);

            /* By host pointer, which is the form a caller on this console
             * has: the translator says which guest word that is. */
            pc_state_mark_host_field(&w[1]);
            CHECK(pc_state_host_word_count() == 2);
            d0 = pc_state_digest_span(GUEST_BASE_PALETTE, GUEST_SIZE_PALETTE);
            w[1] = 0x99999999u;
            CHECK(pc_state_digest_span(GUEST_BASE_PALETTE, GUEST_SIZE_PALETTE)
                  == d0);

            /* Neither a word outside the map nor a host-side field is a
             * guest word, and neither is remembered. */
            pc_state_mark_host_word(0x00100000u);
            pc_state_mark_host_field(&ran);
            CHECK(pc_state_host_word_count() == 2);

            state_clear_host_marks();
            CHECK(pc_state_host_word_count() == 0);

            /*
             * The scan, which is what a patched constructor calls: every
             * aligned word whose value is a host pointer. The image bounds
             * are set here rather than read off the linker, so the console
             * and the build machine check the same four words.
             */
            state_set_image_bounds(0x00100000u, 0x00200000u);
            w[0] = 0x00100040u;                 /* into the image  */
            w[1] = 0x00000007u;                 /* a number        */
            w[2] = 0x02000000u;                 /* a guest address */
            w[3] = 0x001FFFFCu;                 /* the image's top */
            pc_state_scan_mark_host_words(w, 16u);
            CHECK(pc_state_host_word_count() == 2);

            d0 = pc_state_digest_span(GUEST_BASE_PALETTE, GUEST_SIZE_PALETTE);
            w[0] = 0x00100FFFu;
            w[3] = 0x00100000u;
            CHECK(pc_state_digest_span(GUEST_BASE_PALETTE, GUEST_SIZE_PALETTE)
                  == d0);
            /* ... and the two words it did not mark still count. */
            w[2] = 0x02000004u;
            CHECK(pc_state_digest_span(GUEST_BASE_PALETTE, GUEST_SIZE_PALETTE)
                  != d0);

            /*
             * The other half of "is this a host pointer" is a pointer into
             * the slab, and only a 32-bit host can put one in a guest word.
             * This console is one; the build machine running the same code
             * is not, and truncating its pointer would check nothing.
             */
            state_clear_host_marks();
            if (sizeof(void *) == 4) {
                w[0] = (uint32_t)(uintptr_t)w;
                pc_state_scan_mark_host_words(w, 4u);
                CHECK(pc_state_host_word_count() == 1);
            }

            /* An object that is not in guest memory has no guest words. */
            state_clear_host_marks();
            pc_state_scan_mark_host_words(&d0, 8u);
            CHECK(pc_state_host_word_count() == 0);

            w[0] = 0;
            w[1] = 0;
            w[2] = 0;
            w[3] = 0;
        }
        state_set_image_bounds(0, 0);
        state_clear_host_marks();
    }

    /* And back to where it started, byte for byte, marks included, because
     * an empty mark set is the digest this file had before 7.12. */
    CHECK(pc_state_digest() == zero);

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
