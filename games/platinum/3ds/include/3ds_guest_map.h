/*
 * 3ds/include/3ds_guest_map.h: everything the guest slab contains, written
 * down exactly once.
 *
 * The PC port does not need this file. There a guest address is a host
 * address: armrec_mem_init() maps 0x02000000 at 0x02000000 and "where does
 * main RAM live" has no answer beyond "where the DS put it". 3DS userspace
 * cannot do that; the application image is at 0x00100000, the heap window is
 * 0x08000000 to 0x0E000000, which is the DS's AGB slot, and svcControlMemory
 * will not punch a mapping at an arbitrary address. So the regions become
 * slices of one allocation and the guest bases become names: the hardware
 * models keep saying 0x04000130, and one translation step turns that into slab
 * plus offset. This header is the table both halves read.
 *
 * Where the rows come from. Nine are `regions[]` in tools/armrec/armrec_rt.c,
 * unchanged, with the bases and sizes still spelled the way armrec_rt.h spells
 * them; 3ds/tests/guest_map_check.c includes both headers and static-asserts
 * every pair. The last two are regions the PC port has but does not put in
 * that array, because on PC they are address space rather than storage:
 *
 *   VRAM   armrec_rt.c maps 16 MB of address space over 0xA4000 of banks with
 *          aliasing mmaps. A slab cannot alias, so the row's backing is the
 *          nine banks and nothing else; the five windows and their mirrors are
 *          armrec_vram_bank_ptr()'s job.
 *   AGB    pc/src/pc_main.c maps 0x02010000 of zeros so the cartridge probe
 *          has something to fail against. 33.6 MB of a 64 MB budget for a
 *          cartridge that is not inserted is not a trade this console can
 *          make, so the backing is what the probe actually reaches: 128 KB.
 *          The header is at 0x08000000 and is 0xC0 bytes, but
 *          CTRDGi_GetModuleIDImageAddr() is HW_CTRDG_ROM + 0x1FFFE and
 *          CTRDG_IsExisting() dereferences it whenever the header's isRomCode
 *          byte is not 0x96, which is what an absent cartridge's zeros say.
 *          64 KB would have translated that read to NULL.
 *
 * WRAM at 0x03000000 is in this table and was not in the first sketch, which
 * listed nine rows summing to 7.90 MB. `regions[]` has the ARM9's 64 KB
 * shared-WRAM window too, and leaving it out would have given the first stray
 * access to it a host pointer of NULL rather than a page of zeros. With it and
 * with the AGB probe buffer the slab is 8,478,720 bytes.
 *
 * One row per store, not per view. There is no row for the ARM7's copy of the
 * first I/O page: armrec_cpu_switch() keeps both processors' pages in a static
 * 8 KB array and copies the live one into the region, which is a save buffer
 * and not guest memory. Likewise the port window is one row even though
 * pc_guest_window.c hands out several kinds of object from it.
 *
 * Sizes are 4 KB multiples, asserted below, so every region begins on a page
 * boundary once the slab itself does. The allocation uses memalign for exactly
 * that reason: an SPU DMA or a 16 KB VRAM block that straddles a page it did
 * not expect to is a bug that reads as data corruption.
 *
 * Nothing here allocates, translates or initialises anything. 3ds_guest.c
 * allocates the slab, 3ds_hostmap.c owns the translation, and the memory
 * backend registers these rows with armrec.
 */

#ifndef POKEPLATINUM_3DS_GUEST_MAP_H
#define POKEPLATINUM_3DS_GUEST_MAP_H

#include <stdint.h>

/*
 * X(id, guest base, guest size, backing size, name)
 *
 * Rows are in ascending guest-address order, which is also their order in the
 * slab, so a region's offset is the sum of the backing above it. `name` is the
 * string armrec_region_at() already reports on PC, pc/src/pc_guest_window.c
 * finds the port window by that string rather than by its address, and a 3DS
 * spelling of its own would break that lookup.
 *
 * guest size == backing size for every row except VRAM and the AGB slot; see
 * the header comment for why those two are windows onto less memory than they
 * address.
 */
#define GUEST_MAP(X)                                                          \
    X(ITCM,     0x01FF8000u, 0x00008000u, 0x00008000u, "ITCM")                \
    X(MAIN,     0x02000000u, 0x00400000u, 0x00400000u, "main RAM")            \
    X(SHARED,   0x027E0000u, 0x00020000u, 0x00020000u, "shared work")         \
    X(WINDOW,   0x02A00000u, 0x00200000u, 0x00200000u, "port window")         \
    X(WRAM,     0x03000000u, 0x00010000u, 0x00010000u, "WRAM")                \
    X(ARM7WRAM, 0x037F8000u, 0x00018000u, 0x00018000u, "ARM7 WRAM/IWRAM")     \
    X(IO,       0x04000000u, 0x00100000u, 0x00100000u, "I/O")                 \
    X(PALETTE,  0x05000000u, 0x00001000u, 0x00001000u, "palette")             \
    X(VRAM,     0x06000000u, 0x01000000u, 0x000A4000u, "VRAM")                \
    X(OAM,      0x07000000u, 0x00001000u, 0x00001000u, "OAM")                 \
    X(AGB,      0x08000000u, 0x02010000u, 0x00020000u, "AGB slot")

/* Row indices, in table order. */
enum {
#define X(id, base, gsz, bsz, name) GUEST_R_##id,
    GUEST_MAP(X)
#undef X
    GUEST_R_COUNT
};

/* Per-row constants, for code that names one region rather than walking all
 * of them, GUEST_BASE_IO, GUEST_BACK_VRAM, and so on. */
#define X(id, base, gsz, bsz, name)                                           \
    enum { GUEST_BASE_##id = (int)(base) };
GUEST_MAP(X)
#undef X

#define X(id, base, gsz, bsz, name)                                           \
    enum { GUEST_SIZE_##id = (int)(gsz), GUEST_BACK_##id = (int)(bsz) };
GUEST_MAP(X)
#undef X

/* Total backing: what the allocation asks the application heap for. */
#define X(id, base, gsz, bsz, name) +(bsz)
enum { GUEST_SLAB_BYTES = 0 GUEST_MAP(X) };
#undef X

/*
 * 8,478,720 bytes = 8.09 MB. Written as a literal as well as a sum so that
 * changing a row has to be deliberate: the assert fires, and whoever changed
 * it has to say what the console gained or lost. It has changed once, when
 * grew the AGB probe buffer from 64 KB to 128 KB, because the cartridge
 * check reads a word at 0x0801FFFE and the port would have handed it NULL.
 * 64 MB is the Old 3DS application budget (SYSMODE_O3DS_PROD), so this is
 * 12.6% of it before newlib, libctru, the framebuffers and the game's own
 * statics.
 */
_Static_assert(GUEST_SLAB_BYTES == 0x00816000, "guest slab is 8,478,720 bytes");

/* Page-aligned rows, so a page-aligned slab makes every region page-aligned. */
#define X(id, base, gsz, bsz, name)                                           \
    _Static_assert((bsz) != 0 && (bsz) % 0x1000u == 0,                        \
                   #id " backing must be a non-zero multiple of 4 KB");       \
    _Static_assert((bsz) <= (gsz),                                            \
                   #id " cannot back more memory than it addresses");
GUEST_MAP(X)
#undef X

struct guest_region {
    uint32_t base;    /* guest address; what the hardware models still speak */
    uint32_t size;    /* address space the region owns */
    uint32_t backing; /* bytes of slab behind it */
    const char *name; /* the string armrec_region_at() reports */
};

static const struct guest_region guest_map[GUEST_R_COUNT] = {
#define X(id, base, gsz, bsz, name) { (base), (gsz), (bsz), (name) },
    GUEST_MAP(X)
#undef X
};

/*
 * Byte offset of a row's backing within the slab. A loop rather than eleven
 * written-down constants: an offset that has to be recomputed by hand when a
 * region is added is an offset that will one day be stale, and there is no
 * assert that catches a stale one that is still internally consistent.
 * Constant-folds at every call site the index is a constant.
 */
static inline uint32_t guest_map_offset(int index)
{
    uint32_t off = 0;
    int i;

    for (i = 0; i < index; i++) {
        off += guest_map[i].backing;
    }
    return off;
}

#endif /* POKEPLATINUM_3DS_GUEST_MAP_H */
