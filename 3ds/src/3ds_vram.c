/*
 * 3ds/src/3ds_vram.c: the nine banks, and which addresses reach them.
 *
 * See 3ds_vram.h for what VRAM is and why the PC port's aliasing mmaps cannot
 * be copied onto this console. What is here is the same placement model with
 * the address space taken out of it.
 *
 * The tables and vram_place_bank() are copied from tools/armrec/armrec_rt.c,
 * deliberately and with a check. They are not a reading of a hardware
 * document: they were derived from a sweep that writes a marker through every
 * one of the 2,304 VRAMCNT configurations on a console and reads it back at
 * every 16 KB address, including the two cases reasoning gets wrong.
 * 3ds/tests/vram_pin.py extracts both copies and fails if a byte differs.
 *
 * What is not copied is everything about the address space. armrec_rt.c's
 * vram_remap() ends in a loop of mmap() calls that alias each window onto the
 * banks it selects, plus an anonymous floor under the whole 16 MB. Here
 * vram_remap() builds the same tables and stops: the lookup happens per
 * access, in vram_host_ptr(). There is no floor, an address with no bank
 * behind it answers NULL, and the caller sees the failure rather than a page
 * of zeros it can write to.
 *
 * That is a deviation, and it is the one to watch. The PC port made the floor
 * writable after a real crash: one overlay's init clears the whole 512 KB BG
 * window while only part of it has a bank behind it, which is a legal no-op on
 * hardware. On this console that clear arrives as a NULL, and the store path
 * has to drop such a write the way hardware drops it rather than fault.
 */

#include "3ds_vram.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "3ds_guest.h"
#include "3ds_guest_map.h"

/*
 * The five windows and their mirrors. Each window is a slot in the address map
 * larger than the memory it can hold, and the surplus repeats: the main BG
 * window is 2 MB of addresses over 512 KB of banks, so block N and block N+32
 * are the same 16 KB. `period` is where it wraps, `span` how much address
 * space it owns. The LCDC window is the odd one: 41 blocks of content in a
 * 1 MB period, so the 23 blocks above 0x068A4000 in each mirror are holes.
 */
enum { VW_ABG, VW_BBG, VW_AOBJ, VW_BOBJ, VW_LCDC, VW_COUNT };

#define VW_MAXBLK 64

static const struct {
    uint32_t base, span, period;
    uint8_t blocks;             /* 16 KB blocks in one period that can be mapped */
    const char *name;
} vram_win[VW_COUNT] = {
    { 0x06000000u, 0x200000u, 0x080000u, 32, "main BG"  },
    { 0x06200000u, 0x200000u, 0x020000u,  8, "sub BG"   },
    { 0x06400000u, 0x200000u, 0x040000u, 16, "main OBJ" },
    { 0x06600000u, 0x200000u, 0x020000u,  8, "sub OBJ"  },
    { 0x06800000u, 0x800000u, 0x100000u, 41, "LCDC"     },
};

/* Bank sizes in 16 KB blocks; they sum to 41 = HW_LCDC_VRAM_SIZE / 0x4000. */
static const uint8_t vram_blocks[VRAM_BANKS] = { 8, 8, 8, 8, 4, 1, 1, 2, 1 };

/*
 * Where LCDC puts each bank, as a block index; which is also each bank's
 * offset in the store, because laying the store out exactly as the LCDC window
 * does means the nine banks are contiguous and the sum is the SDK's own
 * HW_LCDC_VRAM_SIZE. One arrangement, and the same one the PC port uses.
 */
static const uint8_t vram_lcdc_blk[VRAM_BANKS] = { 0, 8, 16, 24, 32, 36, 37, 38, 40 };

/* What each bank keeps of a byte written to its VRAMCNT, from the sweep's
 * readback: the rest is not stored and reads back as zero. */
static const uint8_t vram_cnt_mask[VRAM_BANKS] = {
    0x9B, 0x9B, 0x9F, 0x9F, 0x87, 0x9F, 0x9F, 0x83, 0x83
};

/* Not nine consecutive bytes: WRAMCNT is at 0x04000247, between G and H. */
static const uint32_t vram_cnt_reg[VRAM_BANKS] = {
    0x04000240u, 0x04000241u, 0x04000242u, 0x04000243u, 0x04000244u,
    0x04000245u, 0x04000246u, 0x04000248u, 0x04000249u
};

/* bank+1 per window block, 0 for "no bank here", and the byte offset into
 * that bank. The offset is stored rather than re-derived from the block index
 * because a bank smaller than its footprint repeats inside it, H covers four
 * sub-BG blocks with 32 KB, and rederiving it invites getting that wrong. */
static uint8_t vram_map[VW_COUNT][VW_MAXBLK];
static uint32_t vram_off[VW_COUNT][VW_MAXBLK];
static uint8_t vram_cnt_live[VRAM_BANKS];  /* what vram_map was built from */
static int vram_mapped;                     /* vram_map/vram_cnt_live valid */
static uint8_t *vram_store;                 /* the nine banks, 0xA4000 */
static unsigned long vram_remap_count;
static unsigned long vram_overlap_count;

unsigned long vram_remaps(void) { return vram_remap_count; }
unsigned long vram_overlaps(void) { return vram_overlap_count; }

uint32_t vram_bank_size(int b) {
    return (b < 0 || b >= VRAM_BANKS) ? 0
                                      : (uint32_t)vram_blocks[b] * VRAM_BLK;
}

uint32_t vram_bank_lcdc(int b) {
    return (b < 0 || b >= VRAM_BANKS)
               ? 0
               : vram_win[VW_LCDC].base + (uint32_t)vram_lcdc_blk[b] * VRAM_BLK;
}

uint32_t vram_cnt_addr(int b) {
    return (b < 0 || b >= VRAM_BANKS) ? 0 : vram_cnt_reg[b];
}

int vram_window_count(void) { return VW_COUNT; }

int vram_window_at(int w, uint32_t *base, uint32_t *size) {
    if (w < 0 || w >= VW_COUNT) return 0;
    if (base != NULL) *base = vram_win[w].base;
    /* Content, not span: `blocks` is how many 16 KB blocks of one period can
     * hold a bank, and the rest of the window is mirrors of them. */
    if (size != NULL) *size = (uint32_t)vram_win[w].blocks * VRAM_BLK;
    return 1;
}

void *vram_bank_ptr(int b) {
    if (vram_store == NULL || b < 0 || b >= VRAM_BANKS) return NULL;
    return vram_store + (size_t)vram_lcdc_blk[b] * VRAM_BLK;
}

/*
 * Two banks in one window block is what hardware answers by ORing the reads
 * and writing to both, and it is the one part of this that a single pointer
 * cannot express. On PC it aborts with the nine register values on stderr;
 * this console has no stderr and a black screen is not a report, so it counts
 * and the first bank placed keeps the block. The SDK makes the case
 * unreachable by construction, GX_SetBankFor* clears a bank out of its
 * previous role before assigning the new one, and OSi_TryLockVram guards the
 * rest, so a non-zero count means either that reasoning is wrong or the game
 * does something this port has never seen. The self-test requires zero.
 */
static void vram_overlap(int win, int blk, int had, int want) {
    (void)win; (void)blk; (void)had; (void)want;
    vram_overlap_count++;
}

static void vram_place(int win, int bank, const uint8_t *blks, int n) {
    uint32_t wrap = vram_bank_size(bank) - 1;
    int i;
    for (i = 0; i < n; i++) {
        int b = blks[i];
        if (vram_map[win][b]) vram_overlap(win, b, vram_map[win][b] - 1, bank);
        vram_map[win][b] = (uint8_t)(bank + 1);
        vram_off[win][b] = ((uint32_t)i * VRAM_BLK) & wrap;
    }
}

/* A run of n consecutive window blocks starting at first. */
static void vram_place_run(int win, int bank, int first, int n) {
    uint8_t blks[VW_MAXBLK];
    int i;
    for (i = 0; i < n; i++) blks[i] = (uint8_t)(first + i);
    vram_place(win, bank, blks, n);
}

/*
 * One bank's placement, straight off the sweep. `mst` is the low bits of
 * VRAMCNT (two for a, b, h and I; three for the rest, which is what the
 * readback masks say) and `ofs` the two bits above them. Every case the sweep
 * showed no CPU-visible address for, A/B mst 3, C/D mst 2, 3, 5-7, E mst
 * 3-7, F/G mst 3-7, H mst 2-3, I mst 3, falls through and maps nothing:
 * Those are the texture, palette and ARM7 roles, which are not in the ARM9's
 * address space at all.
 */
static void vram_place_bank(int bank, uint8_t cnt) {
    static const uint8_t bbg_h[4] = { 0, 1, 4, 5 };
    static const uint8_t bbg_i[4] = { 2, 3, 6, 7 };
    unsigned mst = cnt & 7u, ofs = (cnt >> 3) & 3u;

    if (!(cnt & 0x80u)) return;
    if (bank <= 1 || bank >= 7) mst &= 3u;   /* A, B, H, I keep two bits */

    if (mst == 0) {
        vram_place_run(VW_LCDC, bank, vram_lcdc_blk[bank], vram_blocks[bank]);
        return;
    }
    switch (bank) {
    case 0: case 1:                                   /* A, B */
        if (mst == 1) vram_place_run(VW_ABG, bank, (int)ofs * 8, 8);
        else if (mst == 2) vram_place_run(VW_AOBJ, bank, (int)(ofs & 1u) * 8, 8);
        break;
    case 2:                                           /* C */
        if (mst == 1) vram_place_run(VW_ABG, bank, (int)ofs * 8, 8);
        else if (mst == 4) vram_place_run(VW_BBG, bank, 0, 8);
        break;
    case 3:                                           /* D */
        if (mst == 1) vram_place_run(VW_ABG, bank, (int)ofs * 8, 8);
        else if (mst == 4) vram_place_run(VW_BOBJ, bank, 0, 8);
        break;
    case 4:                                           /* E */
        if (mst == 1) vram_place_run(VW_ABG, bank, 0, 4);
        else if (mst == 2) vram_place_run(VW_AOBJ, bank, 0, 4);
        break;
    case 5: case 6:                                   /* F, G */
        /*
         * Two blocks, not one, and the sweep is what says so: a 16 KB bank at
         * main BG offset 0 answers at 0x06000000 *and* 0x06008000, the same
         * 16 KB twice. The model placed one block until the sweep disagreed
         * with it in 64 of the 2,304 configurations; which is the whole
         * reason the oracle exists, because reasoning from the bank's size
         * gives the wrong answer here and looks right.
         */
        if (mst == 1 || mst == 2) {
            uint8_t base = (uint8_t)((ofs & 1u) + ((ofs & 2u) << 1));
            uint8_t pair[2];
            pair[0] = base;
            pair[1] = (uint8_t)(base + 2);
            vram_place(mst == 1 ? VW_ABG : VW_AOBJ, bank, pair, 2);
        }
        break;
    case 7:                                           /* H */
        if (mst == 1) vram_place(VW_BBG, bank, bbg_h, 4);
        break;
    case 8:                                           /* I */
        if (mst == 1) vram_place(VW_BBG, bank, bbg_i, 4);
        else if (mst == 2) vram_place_run(VW_BOBJ, bank, 0, 8);
        break;
    default: break;
    }
}

/*
 * The roles with no address, the extended palettes and the 3D engine's two
 * spaces. See 3ds_vram.h for what they are; what follows is why the bodies
 * look the way they do.
 *
 * COPIED FROM armrec_rt.c, and not arbitrated by the sweep. Everything else in
 * this file came out of `pcdiff-melon --vram-selftest`, which cannot speak
 * here: it reports "not reachable" for every one of these configurations,
 * because that is exactly what makes them these. The PC port arbitrated them
 * the other way round, by rendering through them and comparing pixels with
 * melonDS, so a second reading written here would be a second model with no
 * oracle behind it. 3ds/tests/vram_pin.py compares the four bodies below
 * against armrec's.
 *
 * Two banks on one slot counts rather than aborts, which is the deviation
 * vram_overlap() above already makes and for the same reason: hardware ORs the
 * two and one pointer cannot express it, PC stops with the nine register
 * values on stderr, and this console has neither a stderr nor any use for a
 * black screen. The first claim keeps the slot, the count is what the
 * self-test requires to be zero, and the `role` string armrec prints is built
 * and dropped so the copy stays comparable.
 */
static void *vram_role_claim(void *had, void *want, const char *role,
                             int hadbank, int wantbank)
{
    (void)role;
    if (had && want && had != want) {
        vram_overlap(-1, -1, hadbank, wantbank);
        return had;
    }
    return want ? want : had;
}

static void *extpal_claim(void *had, void *want, int which, int slot,
                          int hadbank, int wantbank)
{
    (void)which;
    (void)slot;
    return vram_role_claim(had, want, NULL, hadbank, wantbank);
}

void *vram_extpal(int which, int slot) {
    void *p = NULL;
    int had = -1;

#define CLAIM(bank, cond, off, mask)                                          \
    do {                                                                      \
        if (cond) {                                                           \
            void *q = (char *)vram_bank_ptr(bank) + ((off) & (mask));         \
            p = extpal_claim(p, q, which, slot, had, (bank));                 \
            had = (bank);                                                     \
        }                                                                     \
    } while (0)

    if (!vram_mapped || !vram_store) return NULL;
    if (slot < 0 || slot > 3) return NULL;

    switch (which) {
    case VRAM_EXTPAL_ABG:
        /* E covers all four slots with its first 32 KB; F and G are 16 KB and
         * cover two, chosen by bit 0 of the placement field. */
        CLAIM(4, (vram_cnt_live[4] & 0x87u) == 0x84u,
              (uint32_t)slot * 0x2000u, 0x7FFFu);
        CLAIM(5, (vram_cnt_live[5] & 0x9Fu) == 0x84u &&
                 (slot >> 1) == (int)((vram_cnt_live[5] >> 3) & 1u),
              (uint32_t)slot * 0x2000u, 0x3FFFu);
        CLAIM(6, (vram_cnt_live[6] & 0x9Fu) == 0x84u &&
                 (slot >> 1) == (int)((vram_cnt_live[6] >> 3) & 1u),
              (uint32_t)slot * 0x2000u, 0x3FFFu);
        break;
    case VRAM_EXTPAL_BBG:
        CLAIM(7, (vram_cnt_live[7] & 0x83u) == 0x82u,
              (uint32_t)slot * 0x2000u, 0x7FFFu);
        break;
    case VRAM_EXTPAL_AOBJ:
        if (slot) return NULL;
        CLAIM(5, (vram_cnt_live[5] & 0x9Fu) == 0x85u, 0u, 0x1FFFu);
        CLAIM(6, (vram_cnt_live[6] & 0x9Fu) == 0x85u, 0u, 0x1FFFu);
        break;
    case VRAM_EXTPAL_BOBJ:
        if (slot) return NULL;
        CLAIM(8, (vram_cnt_live[8] & 0x83u) == 0x83u, 0u, 0x1FFFu);
        break;
    default:
        return NULL;
    }
#undef CLAIM
    return p;
}

/*
 * Texture image space is 512 KB in four 128 KB slots, and a, b, c or D at
 * mst 3 fills exactly one of them, chosen by the two bits above mst. A and B
 * keep only two mst bits and C and D three (vram_cnt_mask above, from the
 * sweep's readback), so `(cnt & 0x87) == 0x83` is "enabled at mst 3" for all
 * four: bit 2 is not stored on A and B, and testing it costs nothing there.
 *
 * Texture palette space is 128 KB in eight 16 KB slots, and only six of them
 * have a bank that can reach them. E at mst 3 covers 0-3 with its 64 KB; F or
 * G at mst 3 covers one 16 KB slot, and its two placement bits pick 0, 1, 4 or
 * 5 rather than 0 to 3, the same (ofs & 1) + ((ofs & 2) << 1) spacing F and G
 * use for the BG and OBJ windows, and the reason slots 2 and 3 come only from
 * E. Slots 6 and 7 are unreachable, which is a fact about the hardware rather
 * than a gap here: 64 + 16 + 16 KB is all the VRAM this space has.
 */
void *vram_texture(int slot) {
    void *p = NULL;
    int had = -1, b;

    if (!vram_mapped || !vram_store) return NULL;
    if (slot < 0 || slot > 3) return NULL;

    for (b = 0; b <= 3; b++) {                        /* A, B, C, D */
        if ((vram_cnt_live[b] & 0x87u) != 0x83u) continue;
        if ((int)((vram_cnt_live[b] >> 3) & 3u) != slot) continue;
        {
            char role[32];
            snprintf(role, sizeof role, "texture slot %d", slot);
            p = vram_role_claim(p, vram_bank_ptr(b), role, had, b);
        }
        had = b;
    }
    return p;
}

void *vram_texpal(int slot) {
    void *p = NULL;
    int had = -1;

#define TCLAIM(bank, cond, off)                                               \
    do {                                                                      \
        if (cond) {                                                           \
            char role[40];                                                    \
            void *q = (char *)vram_bank_ptr(bank) + (off);                    \
            snprintf(role, sizeof role, "texture palette slot %d", slot);     \
            p = vram_role_claim(p, q, role, had, (bank));                     \
            had = (bank);                                                     \
        }                                                                     \
    } while (0)

    if (!vram_mapped || !vram_store) return NULL;
    if (slot < 0 || slot > 7) return NULL;

    TCLAIM(4, (vram_cnt_live[4] & 0x87u) == 0x83u && slot <= 3,
           (uint32_t)slot * 0x4000u);
    TCLAIM(5, (vram_cnt_live[5] & 0x87u) == 0x83u &&
              slot == (int)(((vram_cnt_live[5] >> 3) & 1u) +
                            (((vram_cnt_live[5] >> 3) & 2u) << 1)), 0u);
    TCLAIM(6, (vram_cnt_live[6] & 0x87u) == 0x83u &&
              slot == (int)(((vram_cnt_live[6] >> 3) & 1u) +
                            (((vram_cnt_live[6] >> 3) & 2u) << 1)), 0u);
#undef TCLAIM
    return p;
}

int vram_bank_in_lcdc(int bank) {
    if (!vram_mapped || bank < 0 || bank >= VRAM_BANKS) return 0;
    /* mst 0 with the enable bit is LCDC for every bank; a, b, h and I keep
     * only two mst bits, which is what vram_place_bank() uses too. */
    return (vram_cnt_live[bank] & 0x80u) &&
           !(vram_cnt_live[bank] & ((bank <= 1 || bank >= 7) ? 3u : 7u));
}

int vram_lookup(uint32_t a, int *bank, uint32_t *off) {
    int w;

    if (!vram_mapped) return 0;
    for (w = 0; w < VW_COUNT; w++) {
        uint32_t rel, blk;
        if (a - vram_win[w].base >= vram_win[w].span) continue;
        rel = (a - vram_win[w].base) % vram_win[w].period;
        blk = rel / VRAM_BLK;
        if (blk >= vram_win[w].blocks || !vram_map[w][blk]) return 0;
        if (bank) *bank = vram_map[w][blk] - 1;
        if (off) *off = vram_off[w][blk] + (rel & (VRAM_BLK - 1));
        return 1;
    }
    return 0;
}

void *vram_host_ptr(uint32_t guest)
{
    int bank = 0;
    uint32_t off = 0;

    if (vram_store == NULL || !vram_lookup(guest, &bank, &off)) {
        return NULL;
    }
    return vram_store + (size_t)vram_lcdc_blk[bank] * VRAM_BLK + off;
}

/*
 * Rebuild vram_map[] from vram_cnt_live[]. On PC this is where the mmaps
 * happen and where the anonymous floor is laid down; here it is nine calls and
 * two memsets, which is the whole difference between the two ports' VRAM.
 */
/*
 * 3ds/src/3ds_hostmap.c memoises armrec_host_ptr() by 16 KB block for the
 * rasterizer, and a remap is the one thing that can make one of those answers
 * wrong. Weak, because the self-test binary links this file without that one,
 * and called from HERE rather than from the renderer so no future consumer has
 * to remember it exists.
 */
extern void hostmap_flush(void) __attribute__((weak));

/* And the tile cache's tile cache, for a stronger reason than the translator's: a remap
 * changes what an address *means*, so every expanded tile it holds is keyed on
 * an address that now points somewhere else. Weak for the same reason. */
extern void tile_cache_reset(void) __attribute__((weak));

static void vram_remap(void)
{
    int b;

    memset(vram_map, 0, sizeof vram_map);
    memset(vram_off, 0, sizeof vram_off);
    for (b = 0; b < VRAM_BANKS; b++) {
        vram_place_bank(b, vram_cnt_live[b]);
    }
    vram_mapped = 1;
    if (tile_cache_reset != NULL) {
        tile_cache_reset();
    }
    if (hostmap_flush != NULL) {
        hostmap_flush();
    }
    vram_remap_count++;
}

void vram_touch(void)
{
    uint8_t now[VRAM_BANKS];
    int b;

    if (vram_store == NULL) {
        return;
    }
    for (b = 0; b < VRAM_BANKS; b++) {
        const uint8_t *reg = (const uint8_t *)armrec_host_ptr(vram_cnt_reg[b]);

        now[b] = reg != NULL ? (uint8_t)(*reg & vram_cnt_mask[b]) : 0u;
    }
    if (vram_mapped && memcmp(now, vram_cnt_live, sizeof now) == 0) {
        return;
    }
    memcpy(vram_cnt_live, now, sizeof now);
    vram_remap();
}

void vram_init(void)
{
    vram_store = (uint8_t *)guest_region_base(GUEST_R_VRAM);
    vram_mapped = 0;
    vram_remap_count = 0;
    vram_overlap_count = 0;
    memset(vram_cnt_live, 0, sizeof vram_cnt_live);
    memset(vram_map, 0, sizeof vram_map);
    memset(vram_off, 0, sizeof vram_off);
    /* Read the registers once, so the map is valid before the first access
     * rather than only after the first write to a VRAMCNT. */
    vram_touch();
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#ifdef VRAM_SELFTEST_VERBOSE
#define FAILNOTE() fprintf(stderr, "  3ds_vram.c:%d failed\n", __LINE__)
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

/* Write one bank's VRAMCNT and tell the model, the way the game will. */
static void vram_set_cnt(int bank, uint8_t value)
{
    uint8_t *reg = (uint8_t *)armrec_host_ptr(vram_cnt_reg[bank]);

    if (reg != NULL) {
        *reg = value;
    }
    vram_touch();
}

int vram_selftest(int *ranOut)
{
    int ran = 0;
    int failed = 0;
    int b;
    uint32_t total = 0;
    uint8_t *a_bank;

    vram_init();

    if (vram_store == NULL) {
        /* Unbound slab: nothing translates and nothing crashes. */
        ran++;
        if (vram_host_ptr(0x06000000u) != NULL) {
            failed++;
            FAILNOTE();
        }
        if (ranOut != NULL) {
            *ranOut = ran;
        }
        return failed + 1;
    }

    /* The store is the nine banks and nothing else, 0xA4000, not the 16 MB
     * of addresses they appear in. This is the check the task names. */
    for (b = 0; b < VRAM_BANKS; b++) {
        CHECK(vram_bank_ptr(b) == vram_store + vram_bank_lcdc(b) - 0x06800000u);
        CHECK(vram_bank_size(b) == (uint32_t)vram_blocks[b] * VRAM_BLK);
        total += vram_bank_size(b);
    }
    CHECK(total == VRAM_STORE_BYTES);
    CHECK((uint32_t)GUEST_BACK_VRAM == VRAM_STORE_BYTES);
    CHECK(GUEST_SLAB_BYTES == 0x00816000);

    /* Nothing enabled: every window answers NULL, on hardware's terms. */
    CHECK(vram_host_ptr(0x06000000u) == NULL);
    CHECK(vram_host_ptr(0x06200000u) == NULL);
    CHECK(vram_host_ptr(0x06400000u) == NULL);
    CHECK(vram_host_ptr(0x06600000u) == NULL);
    CHECK(vram_host_ptr(0x06800000u) == NULL);
    CHECK(armrec_host_ptr(0x06000000u) == NULL);
    CHECK(vram_lookup(0x06000000u, NULL, NULL) == 0);

    /*
     * The task's own check. Bank A into LCDC, a marker written at the LCDC
     * address, then A into the main BG window, and the marker is there, at a
     * completely different guest address, because both are views of the same
     * bank. On PC that is two mmaps of one object; here it is two offsets into
     * one store, and this is the check that says the substitution holds.
     */
    a_bank = (uint8_t *)vram_bank_ptr(0);
    vram_set_cnt(0, 0x80u); /* A, enabled, mst 0 -> LCDC */
    CHECK(vram_host_ptr(0x06800000u) == a_bank);
    CHECK(armrec_host_ptr(0x06800000u) == a_bank);
    CHECK(vram_host_ptr(0x06800000u + 0x1000u) == a_bank + 0x1000u);

    *(uint32_t *)(void *)(a_bank + 0x1000u) = 0xC0FFEEu;
    CHECK(*(uint32_t *)(void *)armrec_host_ptr(0x06801000u) == 0xC0FFEEu);

    vram_set_cnt(0, 0x81u); /* A, mst 1, ofs 0 -> main BG at 0x06000000 */
    CHECK(vram_host_ptr(0x06001000u) == a_bank + 0x1000u);
    CHECK(*(uint32_t *)(void *)armrec_host_ptr(0x06001000u) == 0xC0FFEEu);
    /* ...and it has left LCDC, which is the half a copy model would fail. */
    CHECK(vram_host_ptr(0x06800000u) == NULL);

    /* A write through the new window is the same memory as the old view. */
    *(uint32_t *)(void *)armrec_host_ptr(0x06001004u) = 0xBEEFu;
    CHECK(*(uint32_t *)(void *)(a_bank + 0x1004u) == 0xBEEFu);

    /* The main BG window is 2 MB of addresses over 512 KB, so its blocks
     * repeat four times. The mirror is the same bytes, not a copy. */
    CHECK(vram_host_ptr(0x06081000u) == a_bank + 0x1000u);
    CHECK(vram_host_ptr(0x06101000u) == a_bank + 0x1000u);
    CHECK(vram_host_ptr(0x06181000u) == a_bank + 0x1000u);

    /* Bank A is 128 KB in a 512 KB window: the rest of the window has no bank
     * behind it and must not wrap into A. */
    CHECK(vram_host_ptr(0x06020000u) == NULL);

    /* ofs moves it: mst 1 ofs 1 puts A at the second 128 KB of the window. */
    vram_set_cnt(0, 0x89u);
    CHECK(vram_host_ptr(0x06000000u) == NULL);
    CHECK(vram_host_ptr(0x06021000u) == a_bank + 0x1000u);

    /*
     * F and G answer at two window blocks, not one: 16 KB of bank appearing at
     * 0x06000000 and 0x06008000. This is the case the console sweep found and
     * reasoning gets wrong, so it is worth a check of its own.
     */
    vram_set_cnt(0, 0x00u);
    vram_set_cnt(5, 0x81u); /* F, mst 1, ofs 0 */
    {
        uint8_t *f = (uint8_t *)vram_bank_ptr(5);

        CHECK(vram_bank_size(5) == 0x4000u);
        CHECK(vram_host_ptr(0x06000000u) == f);
        CHECK(vram_host_ptr(0x06008000u) == f);
        *(uint32_t *)(void *)f = 0x5A5A5A5Au;
        CHECK(*(uint32_t *)(void *)armrec_host_ptr(0x06008000u) == 0x5A5A5A5Au);
        CHECK(vram_host_ptr(0x06004000u) == NULL);
    }

    /* A bank in a role with no CPU address at all (E as a texture) is
     * reachable through no window. */
    vram_set_cnt(5, 0x00u);
    vram_set_cnt(4, 0x83u); /* E, mst 3: texture, not in the ARM9's map */
    CHECK(vram_host_ptr(0x06000000u) == NULL);
    CHECK(vram_host_ptr(0x06800000u) == NULL);

    /* Sub BG, where H covers four blocks with 32 KB and so repeats. */
    vram_set_cnt(4, 0x00u);
    vram_set_cnt(7, 0x81u); /* H, mst 1 -> sub BG blocks 0,1,4,5 */
    {
        uint8_t *h = (uint8_t *)vram_bank_ptr(7);

        CHECK(vram_bank_size(7) == 0x8000u);
        CHECK(vram_host_ptr(0x06200000u) == h);
        CHECK(vram_host_ptr(0x06204000u) == h + 0x4000u);
        CHECK(vram_host_ptr(0x06210000u) == h);          /* block 4 wraps */
        CHECK(vram_host_ptr(0x06208000u) == NULL);       /* blocks 2,3 are I's */
    }

    /*
     * The four roles no address reaches. Every one of these configurations is
     * a bank that vram_lookup() answers 0 for, which is why the queries exist
     * at all, so each check is in two halves: the slot hands back the bank,
     * and no window does.
     */
    CHECK(vram_extpal(VRAM_EXTPAL_ABG, 0) == NULL);
    CHECK(vram_texture(0) == NULL);
    CHECK(vram_texpal(0) == NULL);

    vram_set_cnt(4, 0x84u); /* E, mst 4: the main BG extended palettes */
    {
        uint8_t *e = (uint8_t *)vram_bank_ptr(4);

        CHECK(vram_extpal(VRAM_EXTPAL_ABG, 0) == e);
        CHECK(vram_extpal(VRAM_EXTPAL_ABG, 3) == e + 3 * 0x2000u);
        CHECK(vram_extpal(VRAM_EXTPAL_ABG, 4) == NULL);
        CHECK(vram_extpal(VRAM_EXTPAL_BBG, 0) == NULL);
        CHECK(vram_host_ptr(0x06000000u) == NULL);
    }

    /*
     * F is 16 KB and covers two of the four slots. Only ofs 0 is checked, and
     * the reason is a defect in the model this file copies rather than a gap
     * here: armrec's condition masks the placement bits into the comparison
     * (`(cnt & 0x9F) == 0x84`), so the ofs-1 arrangement the SDK writes for
     * GX_VRAM_BGEXTPLTT_23_G, 0x8C, and eight sites in this game ask for it:
     * matches nothing and reads as an unmapped slot on both ports. Pinning
     * that answer here would make the copy's job to keep it.
     */
    vram_set_cnt(4, 0x00u);
    vram_set_cnt(5, 0x84u); /* F, mst 4, ofs 0 -> slots 0 and 1 */
    {
        uint8_t *f = (uint8_t *)vram_bank_ptr(5);

        CHECK(vram_extpal(VRAM_EXTPAL_ABG, 0) == f);
        CHECK(vram_extpal(VRAM_EXTPAL_ABG, 1) == f + 0x2000u);
        CHECK(vram_extpal(VRAM_EXTPAL_ABG, 2) == NULL);
    }

    /* C at mst 3 with ofs 2 is texture slot 2, and E at mst 3 covers texture
     * palette slots 0 to 3 with its 64 KB. Slots 6 and 7 have no bank. */
    vram_set_cnt(5, 0x00u);
    vram_set_cnt(2, 0x93u); /* C, mst 3, ofs 2 */
    vram_set_cnt(4, 0x83u); /* E, mst 3 */
    {
        uint8_t *c = (uint8_t *)vram_bank_ptr(2);
        uint8_t *e = (uint8_t *)vram_bank_ptr(4);

        CHECK(vram_texture(2) == c);
        CHECK(vram_texture(0) == NULL);
        CHECK(vram_texpal(0) == e);
        CHECK(vram_texpal(3) == e + 3 * 0x4000u);
        CHECK(vram_texpal(6) == NULL);
        CHECK(vram_texpal(7) == NULL);
        CHECK(vram_host_ptr(0x06000000u) == NULL);
        CHECK(vram_host_ptr(0x06800000u) == NULL);
    }

    /* DISPCNT's VRAM display mode reads a bank that is in LCDC and nothing
     * else, so this is the question no window answers either. */
    CHECK(vram_bank_in_lcdc(2) == 0);
    vram_set_cnt(2, 0x80u);
    CHECK(vram_bank_in_lcdc(2) == 1);
    CHECK(vram_bank_in_lcdc(4) == 0);
    CHECK(vram_bank_in_lcdc(-1) == 0);
    CHECK(vram_bank_in_lcdc(VRAM_BANKS) == 0);
    vram_set_cnt(2, 0x00u);
    vram_set_cnt(4, 0x00u);

    /* A remap happens when VRAMCNT changes and not otherwise. */
    {
        unsigned long before = vram_remaps();

        vram_touch();
        vram_touch();
        CHECK(vram_remaps() == before);
        vram_set_cnt(7, 0x00u);
        CHECK(vram_remaps() == before + 1u);
    }

    /* Two banks were never in one block. */
    CHECK(vram_overlaps() == 0);

    /* Registers and banks back to how a reset console has them. */
    for (b = 0; b < VRAM_BANKS; b++) {
        vram_set_cnt(b, 0x00u);
    }
    memset(vram_store, 0, VRAM_STORE_BYTES);
    CHECK(vram_host_ptr(0x06000000u) == NULL);

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
