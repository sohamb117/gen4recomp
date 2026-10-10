/*
 * armrec, static recompiler runtime
 * See armrec_rt.h for the memory and calling-convention model.
 */

#include "armrec_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__wasm__)
/* No mmap, no fd-backed shared objects, no mprotect: the DS map is plain
 * linear memory at its own addresses (np_guest_abi.h), and pc_wasm.h's
 * fatal is the trap. */
#include <pc_wasm.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#if defined(__BIONIC__)
#include <android/sharedmem.h>  /* ASharedMemory_create; see mem_backing */
#endif
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif

int armrec_mem_ready = 0;
int armrec_trace = 0;
uint32_t armrec_sp = ARM_STACK_TOP;

static char mem_err[512] = "";

const char *armrec_mem_strerror(void) { return mem_err; }

/* ------------------------------------------------------------------ */
/* Guest address space                                                */
/* ------------------------------------------------------------------ */

struct region {
    uint32_t base;
    uint32_t size;
    const char *name;
    /*
     * Non-zero if the process image already provides this region, so
     * armrec_mem_init() must not map it and armrec_mem_free() must not unmap
     * it. Exactly one region is like that, the port window, whose storage is
     * a NOBITS section the linker places (see ARM_PORT_WINDOW_BASE) because
     * some of what lives there is link-placed .bss rather than a run-time
     * allocation. Mapping over it would replace those statics with zeroed
     * anonymous pages, and unmapping it would take a piece out of the port's
     * own image.
     */
    int provided;
};

/*
 * VRAM is not here, and that is the doing. armrec_vram_init() maps
 * 0x06000000-0x07000000 itself, out of a shared backing object, because the
 * nine banks move between windows. The five windows are reported by
 * armrec_region_at() below so that everything walking the table, the state
 * digest, --watch's bounds check, the determinism runs, sees them; they are
 * appended rather than listed here because their *sizes* are the addressable
 * content and their *mapping* is not this function's to make.
 */
static const struct region regions[] = {
    { ARM_ITCM_BASE, ARM_ITCM_SIZE, "ITCM", 0 },
    { ARM_MAIN_RAM_BASE, ARM_MAIN_RAM_SIZE, "main RAM", 0 },
    { ARM_SHARED_BASE, ARM_SHARED_SIZE, "shared work", 0 },
    { ARM_WRAM_BASE, ARM_WRAM_SIZE, "WRAM", 0 },
    { ARM_ARM7_WRAM_BASE, ARM_ARM7_WRAM_SIZE, "ARM7 WRAM/IWRAM", 0 },
    { ARM_IO_BASE, ARM_IO_SIZE, "I/O", 0 },
    { ARM_PALETTE_BASE, ARM_PALETTE_SIZE, "palette", 0 },
    { ARM_OAM_BASE, ARM_OAM_SIZE, "OAM", 0 },
    /* Not a console region. See ARM_PORT_WINDOW_BASE in armrec_rt.h for why a
     * hole in the DS's map is mapped here, and pc/src/pc_guest_window.c for
     * the only code that hands any of it out. */
    { ARM_PORT_WINDOW_BASE, ARM_PORT_WINDOW_SIZE, ARMREC_PORT_WINDOW_NAME, 1 },
};

#define NREGIONS ((int)(sizeof(regions) / sizeof(regions[0])))

/* Which regions this process mapped, so armrec_mem_free() unmaps exactly
 * those. Only a `provided` region can differ from `!provided`, and only
 * between the port's own link and a test binary's. */
static uint8_t region_mapped[NREGIONS];

/* ------------------------------------------------------------------ */
/* VRAM bank mapping.                                */
/* ------------------------------------------------------------------ */

/*
 * The table below is measured, not transcribed. `pcdiff-melon
 * --vram-selftest` fills each bank with one marker per 16 KB block, writes
 * every one of the 256 values VRAMCNT can hold through the register, and reads
 * the whole of 0x06000000-0x07000000 back through melonDS's own ARM9Read16,
 * so what it reports is where a *console* puts a bank, not how an emulator
 * represents it. pc/tests/test_vram.c replays all 2,304 configurations
 * against this model at every 16 KB address, which is the same claim from the
 * other end. Nothing here is a guess, and where the sweep says a bank is not
 * CPU-visible at all (texture, texture palette, extended palette, the ARM7's
 * two slots) this model maps nothing rather than inventing somewhere.
 *
 * The five windows and their mirrors. Each window is a slot in the address map
 * that is larger than the memory it can hold, and the surplus repeats: the
 * main BG window is 2 MB of addresses over 512 KB of banks, so block N and
 * block N+32 are the same 16 KB. `period` is where it wraps, `span` how much
 * address space it owns, and both come out of the sweep rather than out of a
 * document. The LCDC window is the odd one: 41 blocks of content in a 1 MB
 * period, so the 23 blocks above 0x068A4000 in each mirror are holes.
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
static const uint8_t vram_blocks[ARM_VRAM_BANKS] = { 8, 8, 8, 8, 4, 1, 1, 2, 1 };

/*
 * Where LCDC puts each bank, as a block index; which is also each bank's
 * offset in the backing object, because laying the store out exactly as the
 * LCDC window does means the nine banks are contiguous and the sum is the
 * SDK's own HW_LCDC_VRAM_SIZE. One arrangement, checkable against mmap.h.
 */
static const uint8_t vram_lcdc_blk[ARM_VRAM_BANKS] = { 0, 8, 16, 24, 32, 36, 37, 38, 40 };

/* What each bank keeps of a byte written to its VRAMCNT, from the sweep's
 * readback: the rest is not stored and reads back as zero. */
static const uint8_t vram_cnt_mask[ARM_VRAM_BANKS] = {
    0x9B, 0x9B, 0x9F, 0x9F, 0x87, 0x9F, 0x9F, 0x83, 0x83
};

/* Not nine consecutive bytes: WRAMCNT is at 0x04000247, between G and H. */
static const uint32_t vram_cnt_reg[ARM_VRAM_BANKS] = {
    0x04000240u, 0x04000241u, 0x04000242u, 0x04000243u, 0x04000244u,
    0x04000245u, 0x04000246u, 0x04000248u, 0x04000249u
};

uint32_t armrec_vram_bank_size(int b) {
    return (b < 0 || b >= ARM_VRAM_BANKS) ? 0
                                          : (uint32_t)vram_blocks[b] * ARM_VRAM_BLK;
}
uint32_t armrec_vram_bank_lcdc(int b) {
    return (b < 0 || b >= ARM_VRAM_BANKS)
               ? 0
               : vram_win[VW_LCDC].base + (uint32_t)vram_lcdc_blk[b] * ARM_VRAM_BLK;
}
uint32_t armrec_vram_cnt_addr(int b) {
    return (b < 0 || b >= ARM_VRAM_BANKS) ? 0 : vram_cnt_reg[b];
}

/* bank+1 per window block, 0 for "no bank here", and the byte offset into
 * that bank. The offset is stored rather than re-derived from the block index
 * because a bank smaller than its footprint repeats inside it, H covers four
 * sub-BG blocks with 32 KB, and rederiving it invites getting that wrong. */
static uint8_t vram_map[VW_COUNT][VW_MAXBLK];
static uint32_t vram_off[VW_COUNT][VW_MAXBLK];
/* The other banks in a block vram_map already holds one of (vram_place):
 * bit b set means bank b is mapped there too, at vram_ovl_off[..][b]. */
static uint16_t vram_ovl[VW_COUNT][VW_MAXBLK];
static uint32_t vram_ovl_off[VW_COUNT][VW_MAXBLK][ARM_VRAM_BANKS];
static uint8_t vram_cnt_live[ARM_VRAM_BANKS];   /* what vram_map was built from */
static int vram_mapped;                          /* vram_map/vram_cnt_live valid */
static void *vram_store;                         /* the nine banks, 0xA4000 */
static unsigned long vram_remap_count;

unsigned long armrec_vram_remaps(void) { return vram_remap_count; }

void *armrec_vram_bank_ptr(int b) {
    if (!vram_store || b < 0 || b >= ARM_VRAM_BANKS) return NULL;
    return (char *)vram_store + (size_t)vram_lcdc_blk[b] * ARM_VRAM_BLK;
}

/*
 * Two banks in one window block is a state the console allows: reads OR the
 * banks and a write goes to both. The SDK passes through it on an ordinary
 * hand-over. GxSetBankForSubOBJ (NitroSDK gx_vramcnt.c; every role's setter
 * has the same shape) stores the new bank first, GX_VRAMCNT_SetSubOBJ_(D),
 * and only then moves the old one out, GX_VRAMCNT_SetLCDC_(~new & (lcdc |
 * old)), so between those two stores D and I are both sub OBJ. The ports'
 * decompiled C never showed it, its hook runs at the setter's return
 * (__cyg_profile_func_exit below); the recompiled SDK's per-store hook
 * (ARMREC_VRAM_HOOK) does, and Black's and White's N's Castle, whose throne
 * room hands sub OBJ from I to the 128 KB D (scr 0556 script 3), stopped on
 * the trap this used to be.
 *
 * So an extra bank is recorded, not refused. The copying model (wasm and
 * Windows: every core the port ships) is exact: the block's window shows the
 * OR of its banks and a byte the guest changes there goes back to every one
 * (vram_push_block, vram_pull_ovl_block). The aliasing model cannot back one
 * page with two objects: it leaves the block inaccessible, so the hand-over
 * passes, a CPU access while it stands faults, and a frame rendered with an
 * overlap still standing stops (armrec_vram_render_begin).
 */
static void vram_place(int win, int bank, const uint8_t *blks, int n) {
    uint32_t wrap = armrec_vram_bank_size(bank) - 1;
    int i;
    for (i = 0; i < n; i++) {
        int b = blks[i];
        uint32_t off = ((uint32_t)i * ARM_VRAM_BLK) & wrap;
        if (vram_map[win][b]) {
            vram_ovl[win][b] |= (uint16_t)(1u << bank);
            vram_ovl_off[win][b][bank] = off;
            continue;
        }
        vram_map[win][b] = (uint8_t)(bank + 1);
        vram_off[win][b] = off;
    }
}

/* Window block (w, i)'s placement against a saved one: the remaps redo only
 * the blocks whose banks or offsets moved. */
static int vram_block_same(int w, int i, uint8_t map, uint32_t off,
                           uint16_t ovl, const uint32_t *ovl_off) {
    int b;
    if (vram_map[w][i] != map || vram_ovl[w][i] != ovl) return 0;
    if (!map) return 1;
    if (vram_off[w][i] != off) return 0;
    for (b = 0; b < ARM_VRAM_BANKS; b++)
        if ((ovl & (1u << b)) && vram_ovl_off[w][i][b] != ovl_off[b]) return 0;
    return 1;
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
 * The four roles that have no address in the ARM9's map: extended palettes,
 * the 3D engine's texture and texture-palette spaces, and a bank scanned out
 * directly by DISPCNT's VRAM display mode.
 *
 * They are not in vram_place_bank() because the sweep above measures where a
 * bank appears to the CPU, and these do not appear to it at all. That leaves
 * the mapping unmeasured by that oracle, so it is arbitrated the other way
 * round: pc/tests/test_bg.c and pc/tests/test_gpu3d_raster.c render through it
 * and compare pixels with melonDS, so a wrong slot shows up as a wrong colour.
 *
 * Two banks on one slot traps, for the same reason vram_overlap() does:
 * hardware ORs them and one pointer cannot. The SDK assigns one arrangement at
 * a time and gives every bank in it a distinct slot, so a firing means the
 * game does something this port has not seen.
 */
static void *vram_role_claim(void *had, void *want, const char *role,
                             int hadbank, int wantbank) {
    if (had && want && had != want) {
        fprintf(stderr,
                "armrec: VRAM banks %c and %c both hold %s.\n"
                "  Hardware ORs them; this port hands the renderer one\n"
                "  pointer and cannot, so it stops rather than choosing.\n",
                'A' + hadbank, 'A' + wantbank, role);
        abort();
    }
    return want ? want : had;
}

static void *extpal_claim(void *had, void *want, int which, int slot,
                          int hadbank, int wantbank) {
    char role[64];
    snprintf(role, sizeof role, "extended-palette region %d slot %d",
             which, slot);
    return vram_role_claim(had, want, role, hadbank, wantbank);
}

void *armrec_vram_extpal(int which, int slot) {
    void *p = NULL;
    int had = -1;

#define CLAIM(bank, cond, off, mask)                                          \
    do {                                                                      \
        if (cond) {                                                           \
            void *q = (char *)armrec_vram_bank_ptr(bank) + ((off) & (mask));  \
            p = extpal_claim(p, q, which, slot, had, (bank));                 \
            had = (bank);                                                     \
        }                                                                     \
    } while (0)

    if (!vram_mapped || !vram_store) return NULL;
    if (slot < 0 || slot > 3) return NULL;

    switch (which) {
    case ARMREC_EXTPAL_ABG:
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
    case ARMREC_EXTPAL_BBG:
        CLAIM(7, (vram_cnt_live[7] & 0x83u) == 0x82u,
              (uint32_t)slot * 0x2000u, 0x7FFFu);
        break;
    case ARMREC_EXTPAL_AOBJ:
        if (slot) return NULL;
        CLAIM(5, (vram_cnt_live[5] & 0x9Fu) == 0x85u, 0u, 0x1FFFu);
        CLAIM(6, (vram_cnt_live[6] & 0x9Fu) == 0x85u, 0u, 0x1FFFu);
        break;
    case ARMREC_EXTPAL_BOBJ:
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
 * The 3D engine's two spaces.
 *
 * Texture image space is 512 KB in four 128 KB slots, and A, B, C or D at
 * mst 3 fills exactly one of them, chosen by the two bits above mst. A and B
 * keep only two mst bits and C and D three, so `(cnt & 0x87) == 0x83` reads as
 * "enabled at mst 3" for all four.
 *
 * Texture palette space is 128 KB in eight 16 KB slots, and only six have a
 * bank that can reach them. E at mst 3 covers 0 to 3 with its 64 KB; F or G at
 * mst 3 covers one slot, and its two placement bits pick 0, 1, 4 or 5. Slots 6
 * and 7 are unreachable, which is a fact about the hardware: 64 + 16 + 16 KB
 * is all the VRAM this space has.
 *
 * NULL means no bank is mapped there, and a caller must read it as all zeros
 * rather than "do not draw"; an unmapped texture read gives 0 on hardware.
 */
void *armrec_vram_texture(int slot) {
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
            p = vram_role_claim(p, armrec_vram_bank_ptr(b), role, had, b);
        }
        had = b;
    }
    return p;
}

void *armrec_vram_texpal(int slot) {
    void *p = NULL;
    int had = -1;

#define TCLAIM(bank, cond, off)                                               \
    do {                                                                      \
        if (cond) {                                                           \
            char role[40];                                                    \
            void *q = (char *)armrec_vram_bank_ptr(bank) + (off);             \
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

int armrec_vram_bank_in_lcdc(int bank) {
    if (!vram_mapped || bank < 0 || bank >= ARM_VRAM_BANKS) return 0;
    /* mst 0 with the enable bit is LCDC for every bank; a, b, h and I keep
     * only two mst bits, which is what vram_place_bank() uses too. */
    return (vram_cnt_live[bank] & 0x80u) &&
           !(vram_cnt_live[bank] & ((bank <= 1 || bank >= 7) ? 3u : 7u));
}

int armrec_vram_lookup(uint32_t a, int *bank, uint32_t *off) {
    int w;

    if (!vram_mapped) return 0;
    for (w = 0; w < VW_COUNT; w++) {
        uint32_t rel, blk;
        if (a - vram_win[w].base >= vram_win[w].span) continue;
        rel = (a - vram_win[w].base) % vram_win[w].period;
        blk = rel / ARM_VRAM_BLK;
        if (blk >= vram_win[w].blocks || !vram_map[w][blk]) return 0;
        if (bank) *bank = vram_map[w][blk] - 1;
        if (off) *off = vram_off[w][blk] + (rel & (ARM_VRAM_BLK - 1));
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */

/*
 * Which of the two models below a host gets. Aliasing, one backing object with
 * every window an mmap view of it, wherever mmap can place a 16 KB view at a
 * 16 KB offset. Copying, a separate store reconciled with the windows by
 * vram_copy(), where it cannot: Windows for its 64 KB granularity, wasm
 * because linear memory has no mappings at all.
 */
#if defined(_WIN32) || defined(__wasm__)
#define ARMREC_VRAM_COPY 1
#endif

/*
 * The backing object. One shared mapping of all nine banks, laid out as the
 * LCDC window lays them out, so that every guest address a bank is reachable
 * at is a view of the same bytes; which is the whole point, since a store
 * through the BG window has to be visible through LCDC after
 * GX_SetBankForLCDC and hardware has one memory, not five.
 */
#if !defined(ARMREC_VRAM_COPY)
static int vram_fd = -1;

/*
 * An anonymous shared object of `size` bytes, or -1 with mem_err set. Two
 * callers (VRAM's nine banks and the two I/O pages) which is
 * why `what` and `tag` are arguments rather than the literals this held while
 * there was one.
 */
static int mem_backing(const char *what, const char *tag, uint32_t size) {
    int fd = -1;

    /*
     * MFD_CLOEXEC lives behind _GNU_SOURCE and this file does not define it,
     * so `defined(MFD_CLOEXEC)` was false on every host and this branch had
     * Never compiled in, measured 2026-08-25 on both toolchains; every
     * build so far reached the shm_open fallback below and /proc/self/maps
     * says so (`/dev/shm/armrec-vram-0 (deleted)`).
     *
     * That was invisible on a desktop, where the fallback works. It is not
     * invisible on the handheld this is being pointed at: android has no
     * /dev/shm, so shm_open there fails and the port cannot back VRAM at all,
     * while memfd_create is exactly what Android does have.
     *
     * SYS_memfd_create comes from <sys/syscall.h>, which is included above
     * and needs no _GNU_SOURCE, so the syscall is reachable whether or not
     * the flag's name is. Spelling the constant here rather than defining
     * _GNU_SOURCE keeps every other header on this file's include path
     * exactly as it was.
     */
#if defined(__linux__) && defined(SYS_memfd_create)
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 1U /* <linux/memfd.h> */
#endif
    fd = (int)syscall(SYS_memfd_create, tag, MFD_CLOEXEC);
#endif
#if defined(__BIONIC__)
    /*
     * Bionic has no shm_open AND NO /dev/shm, and that is the libc rather
     * than the kernel: Android's own answer to "a file descriptor backing
     * anonymous memory" is ashmem, reached through ASharedMemory_create since
     * API 26. It comes back already sized, so the ftruncate below is skipped
     * for it, calling it on an ashmem region fails.
     *
     * Reached only when memfd_create above did not answer. It should: the
     * syscall has been permitted by Android's seccomp policy since API 29 and
     * both devices this port is aimed at are far past that. This is here for
     * the one that is not, and because a backing object the port cannot make
     * is a port that cannot show a picture at all.
     */
    if (fd < 0) {
        fd = ASharedMemory_create(tag, size);
        if (fd >= 0)
            return fd;
    }
#else
    if (fd < 0) {
        /*
         * No memfd_create: an unlinked POSIX shared segment does as well. The
         * name is counted up rather than built from getpid(), which is what
         * the first version did, test_determinism's NONDETERMINISM_SYMS list
         * caught the reference, and it was right to: nothing host-derived
         * belongs in this binary even where it could not reach guest memory,
         * because "could not" is an argument and the absent symbol is a fact.
         * O_EXCL makes the collision loop honest; the object is unlinked
         * immediately, so the name lives for a microsecond.
         */
        char name[48];
        int i;
        for (i = 0; i < 64 && fd < 0; i++) {
            snprintf(name, sizeof name, "/%s-%d", tag, i);
            fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        }
        if (fd >= 0) shm_unlink(name);
    }
#endif
    if (fd < 0) {
        snprintf(mem_err, sizeof mem_err,
                 "cannot create the %s backing object: %s", what,
                 strerror(errno));
        return -1;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        snprintf(mem_err, sizeof mem_err,
                 "cannot size the %s backing object: %s", what,
                 strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int vram_backing(void) {
    int fd = mem_backing("VRAM", "armrec-vram", ARM_VRAM_STORE);
    if (fd < 0) return -1;
    vram_fd = fd;
    return 0;
}
#endif

/*
 * Lay the current VRAMCNT out over the address space.
 *
 * A hole reads zero and absorbs a write. The reading half is what the sweep
 * shows melonDS answering; the writing half was a refusal; a read-only
 * floor whose segfault named the address, on the MIi_UncompressBackward
 * argument that emulating the console's silence would need the faulting
 * store decoded and skipped, until the boot's own successor found the
 * legitimate writer the old comment predicted: overlay 11's battle init
 * clears the whole 512 KB BG window while only part of it has a bank, a
 * no-op on hardware and a crash at the first wild battle here. So the floor
 * is writable now, no decoder needed; the bounded deviation that buys and
 * why it cannot reach the renderer is argued at the mmap below.
 */
#if defined(ARMREC_VRAM_COPY)
/*
 * On Windows the windows model is copying, not aliasing, and the granularity
 * is why. MapViewOfFileEx needs the target address and the file offset 64 KB
 * aligned; this map moves 16 KB blocks to 16 KB offsets, so the POSIX trick of
 * making every window a view of one backing object cannot be expressed.
 *
 * So the windows and the store are separate memory, reconciled by copying at
 * the only moments ownership changes hands in a single-threaded port: a
 * VRAMCNT remap, and the frame's render. Sequential execution makes that exact
 * for every access pattern except one bank read through two views inside one
 * frame slice with no remap between, and writes through a mirror other than
 * the first, which propagate only at the next remap. Both are recorded here
 * rather than solved; the Linux build with its true aliasing is the oracle.
 *
 * wasm32 takes the same model unchanged, for a stronger reason: a guest
 * address is a linear-memory offset and there is nothing to map, so no view
 * of any granularity exists. The store is a static buffer (linked above
 * NP_GUEST_C_BASE, so outside every DS address) and 0x06000000-0x07000000 is
 * plain linear memory standing in for the windows.
 *
 * Why a store at all, rather than keeping each bank's bytes at "its" window
 * address and moving them on a remap: a bank is in at most one window at a
 * time (vram_place_bank() takes one case per VRAMCNT value) but not at one
 * address. Every mirror of its window repeats it (vram_win[].span/period),
 * F and G sit at two blocks of each mirror and H and I wrap over four or
 * eight (vram_place_bank()'s F/G/H/I cases, vram_place()'s wrap), and a bank
 * in a texture, texture-palette or extended-palette role is at no CPU
 * address at all (vram_place_bank()'s fall-through). The store is the one
 * place every bank always is, and armrec_vram_bank_ptr() names it.
 */
static char *vram_sto(int bank, uint32_t off) {
    return (char *)vram_store + (size_t)vram_lcdc_blk[bank] * ARM_VRAM_BLK +
           off;
}

/*
 * An overlapped block's window as of its last push, in the first mirror (the
 * one a pull reads): the OR of its banks, which is what a pull compares the
 * window against to find the bytes the guest wrote since. Allocated while
 * the overlap stands and freed when it goes (vram_remap), so the usual
 * one-bank map costs nothing.
 */
static uint8_t *vram_ovl_snap[VW_COUNT][VW_MAXBLK];

/* Store -> window for one block: its bank, ORed with any other bank there
 * (vram_place). */
static void vram_push_block(int w, int i, char *win, int first_mirror) {
    uint8_t *d = (uint8_t *)win;
    int b;

    memcpy(win, vram_sto(vram_map[w][i] - 1, vram_off[w][i]), ARM_VRAM_BLK);
    if (!vram_ovl[w][i]) return;
    for (b = 0; b < ARM_VRAM_BANKS; b++) {
        const uint8_t *s;
        uint32_t k;
        if (!(vram_ovl[w][i] & (1u << b))) continue;
        s = (const uint8_t *)vram_sto(b, vram_ovl_off[w][i][b]);
        for (k = 0; k < ARM_VRAM_BLK; k++) d[k] |= s[k];
    }
    if (!first_mirror) return;
    if (!vram_ovl_snap[w][i] &&
        !(vram_ovl_snap[w][i] = (uint8_t *)malloc(ARM_VRAM_BLK))) {
        fprintf(stderr, "armrec: no memory for an overlapped VRAM block\n");
        abort();
    }
    memcpy(vram_ovl_snap[w][i], win, ARM_VRAM_BLK);
}

/*
 * Window -> store for an overlapped block: every byte the guest changed since
 * the push is written to each of the block's banks, as the console's write
 * is. A byte rewritten with the value the OR already showed is not seen; on
 * the console it would have left both banks holding it.
 */
static void vram_pull_ovl_block(int w, int i, const char *win,
                                uint8_t *claimed) {
    const uint8_t *src = (const uint8_t *)win;
    uint8_t *snap = vram_ovl_snap[w][i];
    uint16_t banks = (uint16_t)(vram_ovl[w][i] | (1u << (vram_map[w][i] - 1)));
    int b, changed = 0;
    uint32_t k;

    for (k = 0; k < ARM_VRAM_BLK; k++) {
        if (src[k] == snap[k]) continue;
        changed = 1;
        for (b = 0; b < ARM_VRAM_BANKS; b++)
            if (banks & (1u << b))
                ((uint8_t *)vram_sto(b, b == vram_map[w][i] - 1
                                            ? vram_off[w][i]
                                            : vram_ovl_off[w][i][b]))[k] = src[k];
    }
    if (!changed) return;
    memcpy(snap, win, ARM_VRAM_BLK);
    for (b = 0; b < ARM_VRAM_BANKS; b++)
        if (banks & (1u << b))
            claimed[(size_t)(vram_sto(b, b == vram_map[w][i] - 1
                                             ? vram_off[w][i]
                                             : vram_ovl_off[w][i][b]) -
                             (char *)vram_store) / ARM_VRAM_BLK] = 1;
}

static void vram_copy(int to_windows, int all_mirrors) {
    int w, i;
    /* One flag per 16 KB store block, for the windows->store direction: the
     * first alias that differs claims the block, and the later aliases,
     * which now differ only because the store moved under them, must not
     * write it back. Cleared per pass; the store is 41 blocks. */
    uint8_t claimed[ARM_VRAM_STORE / ARM_VRAM_BLK] = { 0 };

    for (w = 0; w < VW_COUNT; w++) {
        uint32_t mirrors =
            all_mirrors ? vram_win[w].span / vram_win[w].period : 1u;
        uint32_t m;

        for (m = 0; m < mirrors; m++) {
            uint32_t at = vram_win[w].base + m * vram_win[w].period;

            for (i = 0; i < vram_win[w].blocks; i++) {
                int bank = vram_map[w][i] - 1;
                char *win, *sto;

                if (!vram_map[w][i]) continue;
                win = (char *)(uintptr_t)(at + (uint32_t)i * ARM_VRAM_BLK);
                if (to_windows) {
                    vram_push_block(w, i, win, m == 0);
                    continue;
                }
                if (vram_ovl[w][i]) {
                    if (m == 0) vram_pull_ovl_block(w, i, win, claimed);
                    continue;
                }
                sto = vram_sto(bank, vram_off[w][i]);
                if (!claimed[(size_t)(sto - (char *)vram_store) / ARM_VRAM_BLK] &&
                    memcmp(sto, win, ARM_VRAM_BLK) != 0) {
                    /*
                     * Only a block that changed, because a bank can be
                     * smaller than its window. Bank I is 16 KB and mst 2
                     * lays it over eight window blocks by wrapping, so here
                     * they are eight pages aliasing one store block. Copying
                     * all eight unconditionally let the seven the guest never
                     * wrote clobber the one it did.
                     *
                     * Every store to windows push leaves all aliases equal to
                     * the store, so at reconcile time a block that differs is
                     * one the guest wrote since. Two aliases both written
                     * differently in one slice is the shape this cannot
                     * arbitrate; the first one this loop meets wins.
                     */
                    memcpy(sto, win, ARM_VRAM_BLK);
                    claimed[(size_t)(sto - (char *)vram_store) / ARM_VRAM_BLK] = 1;
                }
            }
        }
    }
}
#endif

/* The frame's render, bracketed, no-ops where the views alias for real; there
 * a block two banks share is inaccessible (vram_place), so a frame rendered
 * while one stands would read a fault, and it stops here instead. */
void armrec_vram_render_begin(void) {
#if defined(ARMREC_VRAM_COPY)
    if (vram_mapped) vram_copy(0, 0);
#else
    int w, blk;
    if (!vram_mapped) return;
    for (w = 0; w < VW_COUNT; w++)
        for (blk = 0; blk < vram_win[w].blocks; blk++) {
            if (!vram_ovl[w][blk]) continue;
            fprintf(stderr,
                    "armrec: VRAM window %s block %d (0x%08X) has two banks at"
                    " a\n"
                    "  render (VRAMCNT = %02X %02X %02X %02X %02X %02X %02X"
                    " %02X %02X).\n"
                    "  The console ORs them; this model aliases each window"
                    " onto\n"
                    "  one bank's storage and cannot, so it stops rather than\n"
                    "  choosing. The copying model (ARMREC_VRAM_COPY) is"
                    " exact.\n",
                    vram_win[w].name, blk,
                    vram_win[w].base + (uint32_t)blk * ARM_VRAM_BLK,
                    vram_cnt_live[0], vram_cnt_live[1], vram_cnt_live[2],
                    vram_cnt_live[3], vram_cnt_live[4], vram_cnt_live[5],
                    vram_cnt_live[6], vram_cnt_live[7], vram_cnt_live[8]);
            abort();
        }
#endif
}

void armrec_vram_render_end(void) {
#if defined(ARMREC_VRAM_COPY)
    if (vram_mapped) vram_copy(1, 0);
#endif
}

static int vram_remap(void) {
    int w, b, i;

#if defined(ARMREC_VRAM_COPY)
    /* Writes made through the old arrangement go home first; the map they
     * were made under is still in vram_map until the rebuild below. */
    if (vram_mapped) vram_copy(0, 0);

    /*
     * Incremental after the first build, the same diff as the POSIX path
     * below and for the same measured reason, a visible stutter: the
     * full form re-zeroed the whole 16 MB window span and re-pushed every
     * mapped block of all fifty-two mirrors on every VRAMCNT flip, and the
     * game flips more than once a frame streaming textures, on the
     * owner's machine those were the 21-31 ms work spikes. The diff zeroes
     * only the blocks a bank vacated and pushes only the blocks whose
     * mapping moved, in every mirror of their window; an unchanged block
     * keeps its bytes. The coherence this narrows is one the model had
     * already narrowed: between remaps only the first mirror is refreshed
     * (the render bracket's vram_copy(1, 0)), so a non-primary mirror was
     * never current except momentarily after a remap.
     */
    {
        static uint8_t old_map[VW_COUNT][VW_MAXBLK];
        static uint32_t old_off[VW_COUNT][VW_MAXBLK];
        static uint16_t old_ovl[VW_COUNT][VW_MAXBLK];
        static uint32_t old_ovl_off[VW_COUNT][VW_MAXBLK][ARM_VRAM_BANKS];
        int full = !vram_mapped;

        memcpy(old_map, vram_map, sizeof old_map);
        memcpy(old_off, vram_off, sizeof old_off);
        memcpy(old_ovl, vram_ovl, sizeof old_ovl);
        memcpy(old_ovl_off, vram_ovl_off, sizeof old_ovl_off);

        memset(vram_map, 0, sizeof vram_map);
        memset(vram_off, 0, sizeof vram_off);
        memset(vram_ovl, 0, sizeof vram_ovl);
        memset(vram_ovl_off, 0, sizeof vram_ovl_off);
        for (b = 0; b < ARM_VRAM_BANKS; b++)
            vram_place_bank(b, vram_cnt_live[b]);
        /* An overlap gone takes its push-time copy with it. */
        for (w = 0; w < VW_COUNT; w++)
            for (i = 0; i < VW_MAXBLK; i++)
                if (!vram_ovl[w][i] && vram_ovl_snap[w][i]) {
                    free(vram_ovl_snap[w][i]);
                    vram_ovl_snap[w][i] = NULL;
                }

        if (full) {
            /* The floor: every VRAM address readable and zero, as on POSIX:
             * except writable, because there is no cheap way to trap a
             * write here. On wasm it is zero already, linear memory starts
             * zeroed and armrec_vram_free() re-zeroes it, and writing 16 MB
             * of zeros would only make the native runtime commit the pages
             * of a span the game mostly never touches. */
#if !defined(__wasm__)
            memset((void *)(uintptr_t)ARM_VRAM_BASE, 0, ARM_VRAM_SIZE);
#endif
            vram_copy(1, 1);
        } else {
            for (w = 0; w < VW_COUNT; w++) {
                uint32_t mirrors = vram_win[w].span / vram_win[w].period;
                uint32_t m;
                for (m = 0; m < mirrors; m++) {
                    uint32_t at = vram_win[w].base + m * vram_win[w].period;
                    for (i = 0; i < vram_win[w].blocks; i++) {
                        char *win;

                        if (vram_block_same(w, i, old_map[w][i], old_off[w][i],
                                            old_ovl[w][i], old_ovl_off[w][i]))
                            continue;
                        win = (char *)(uintptr_t)
                                  (at + (uint32_t)i * ARM_VRAM_BLK);
                        if (!vram_map[w][i])
                            memset(win, 0, ARM_VRAM_BLK);
                        else
                            vram_push_block(w, i, win, m == 0);
                    }
                }
            }
        }
    }

    vram_mapped = 1;
    vram_remap_count++;
    return 0;
#else
    /*
     * The floor: every VRAM address readable, zero, and writable. Hardware
     * drops a write where no bank is mapped, and the game leans on that:
     * one overlay's init clears the whole 512 KB BG window while only part of
     * it has a bank behind it, which is a legal no-op on the console and was a
     * segfault on a read-only floor. The deviation writable buys is bounded: a
     * guest CPU read of a hole it wrote returns the write rather than
     * hardware's 0. The renderer never sees any of it, because it reads banks
     * through armrec_vram_* pointers, where a hole is NULL and NULL is zeros.
     *
     * Laid down once, patched incrementally after. The first version rebuilt
     * everything on every VRAMCNT change, about ninety mmaps a time. That read
     * as correct and priced as a stutter: the game flips a bank through LCDC
     * more than once a frame while streaming textures, which measured 500,705
     * mmap2 calls in 4,300 frames and about 4.3 ms of every frame. So after
     * the first full build, a remap diffs the new placement against the live
     * one and touches only the blocks that moved.
     *
     * The diff keeps one semantic narrower than the rebuild: a hole that stays
     * a hole is no longer re-zeroed on every remap, so a guest write into
     * never-mapped space survives remaps. A hole a bank vacates is freshly
     * zeroed, because the diff maps new anonymous pages over those blocks.
     */
    {
        static uint8_t old_map[VW_COUNT][VW_MAXBLK];
        static uint32_t old_off[VW_COUNT][VW_MAXBLK];
        static uint16_t old_ovl[VW_COUNT][VW_MAXBLK];
        static uint32_t old_ovl_off[VW_COUNT][VW_MAXBLK][ARM_VRAM_BANKS];
        int full = !vram_mapped;

        memcpy(old_map, vram_map, sizeof old_map);
        memcpy(old_off, vram_off, sizeof old_off);
        memcpy(old_ovl, vram_ovl, sizeof old_ovl);
        memcpy(old_ovl_off, vram_ovl_off, sizeof old_ovl_off);

        if (full
            && mmap((void *)(uintptr_t)ARM_VRAM_BASE, ARM_VRAM_SIZE,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0)
                   == MAP_FAILED) {
            snprintf(mem_err, sizeof mem_err, "cannot map the VRAM floor: %s",
                     strerror(errno));
            return -1;
        }

        memset(vram_map, 0, sizeof vram_map);
        memset(vram_off, 0, sizeof vram_off);
        memset(vram_ovl, 0, sizeof vram_ovl);
        memset(vram_ovl_off, 0, sizeof vram_ovl_off);
        for (b = 0; b < ARM_VRAM_BANKS; b++)
            vram_place_bank(b, vram_cnt_live[b]);

        for (w = 0; w < VW_COUNT; w++) {
            uint32_t mirrors = vram_win[w].span / vram_win[w].period;
            uint32_t m;
            for (m = 0; m < mirrors; m++) {
                uint32_t at = vram_win[w].base + m * vram_win[w].period;
                for (i = 0; i < vram_win[w].blocks;) {
                    int bank = vram_map[w][i] - 1;
                    int n = 1;

                    /* Unchanged blocks keep their pages, the whole point. */
                    if (!full && vram_block_same(w, i, old_map[w][i],
                                                 old_off[w][i], old_ovl[w][i],
                                                 old_ovl_off[w][i])) {
                        i++;
                        continue;
                    }
                    if (vram_ovl[w][i]) {
                        /* Two banks: no page can be both (vram_place). */
                        if (mmap((void *)(uintptr_t)
                                     (at + (uint32_t)i * ARM_VRAM_BLK),
                                 ARM_VRAM_BLK, PROT_NONE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
                                 0) == MAP_FAILED) {
                            snprintf(mem_err, sizeof mem_err,
                                     "cannot fence VRAM at 0x%08X: %s",
                                     at + (uint32_t)i * ARM_VRAM_BLK,
                                     strerror(errno));
                            return -1;
                        }
                        i++;
                        continue;
                    }
                    if (!vram_map[w][i]) {
                        /* A bank left: fresh zeros over exactly its blocks,
                         * coalescing the vacated run. (On the full build the
                         * floor already zeroed everything.) */
                        while (i + n < vram_win[w].blocks
                               && !vram_map[w][i + n]
                               && !(vram_map[w][i + n] == old_map[w][i + n]))
                            n++;
                        if (!full
                            && mmap((void *)(uintptr_t)
                                        (at + (uint32_t)i * ARM_VRAM_BLK),
                                    (size_t)n * ARM_VRAM_BLK,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                                    -1, 0) == MAP_FAILED) {
                            snprintf(mem_err, sizeof mem_err,
                                     "cannot re-floor VRAM at 0x%08X: %s",
                                     at + (uint32_t)i * ARM_VRAM_BLK,
                                     strerror(errno));
                            return -1;
                        }
                        i += n;
                        continue;
                    }
                    /* Coalesce a run of blocks that is contiguous in the bank
                     * too, so a 512 KB window costs one mmap and not
                     * thirty-two, but only while every block in the run
                     * actually changed, or an unchanged block would be
                     * re-mapped for being between two that moved (harmless,
                     * and paid for). */
                    while (i + n < vram_win[w].blocks &&
                           vram_map[w][i + n] == vram_map[w][i] &&
                           !vram_ovl[w][i + n] &&
                           vram_off[w][i + n] ==
                               vram_off[w][i] + (uint32_t)n * ARM_VRAM_BLK &&
                           !(!full && vram_block_same(w, i + n,
                                                      old_map[w][i + n],
                                                      old_off[w][i + n],
                                                      old_ovl[w][i + n],
                                                      old_ovl_off[w][i + n])))
                        n++;
                    if (mmap((void *)(uintptr_t)
                                 (at + (uint32_t)i * ARM_VRAM_BLK),
                             (size_t)n * ARM_VRAM_BLK, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_FIXED, vram_fd,
                             (off_t)((uint32_t)vram_lcdc_blk[bank]
                                         * ARM_VRAM_BLK +
                                     vram_off[w][i])) == MAP_FAILED) {
                        snprintf(mem_err, sizeof mem_err,
                                 "cannot map VRAM bank %c at 0x%08X: %s",
                                 'A' + bank, at + (uint32_t)i * ARM_VRAM_BLK,
                                 strerror(errno));
                        return -1;
                    }
                    i += n;
                }
            }
        }
    }
    vram_mapped = 1;
    vram_remap_count++;
    return 0;
#endif
}

/*
 * "VRAMCNT may have changed." Cheap when it has not, which is every call but
 * the handful a scene change makes: nine byte loads and a compare.
 *
 * Called after the store, from both halves of the port, ARM_ST8's
 * ARMREC_VRAM_HOOK form for recompiled code, __cyg_profile_func_exit for the
 * three SDK files that write these registers in C. Also called from the frame
 * hook, which is not a correctness argument but is what turns "a write got
 * past both hooks" from a wrong picture into a wrong picture that at least
 * corrects itself within a frame.
 */
void armrec_vram_touch(void) {
    uint8_t now[ARM_VRAM_BANKS];
    int b;

    if (!armrec_mem_ready || !vram_store) return;
    for (b = 0; b < ARM_VRAM_BANKS; b++)
        now[b] = (uint8_t)(*(volatile uint8_t *)(uintptr_t)vram_cnt_reg[b] &
                           vram_cnt_mask[b]);
    if (vram_mapped && !memcmp(now, vram_cnt_live, sizeof now)) return;
    memcpy(vram_cnt_live, now, sizeof now);
    if (vram_remap() != 0) {
        fprintf(stderr, "armrec: %s\n", mem_err);
        abort();
    }
}

/*
 * -finstrument-functions, applied to pc/Makefile's VRAMCNT_SRCS and nothing
 * else. Those three files are the whole of the decompiled C that writes a
 * VRAMCNT register, 113 assignments, and pc/tests/test_vram.c re-derives
 * that list from the tree and fails if a fourth file appears.
 *
 * Why exit and not the assignment. `reg_GX_VRAMCNT_A = 0x80;` is a store
 * through a macro, and C has no way to make a macro run code *after* an
 * assignment through it; which is why reg_CP_*'s pointer-returning shape
 * does not work here: that hook fires before the store, and what the model
 * needs is the value after. Every one of these functions writes registers and
 * touches no VRAM, so the return is the first moment the new mapping can
 * matter and the last moment it is still free to fix.
 */
__attribute__((no_instrument_function))
void __cyg_profile_func_enter(void *this_fn, void *call_site) {
    (void)this_fn; (void)call_site;
}

__attribute__((no_instrument_function))
void __cyg_profile_func_exit(void *this_fn, void *call_site) {
    (void)this_fn; (void)call_site;
    armrec_vram_touch();
}

static int armrec_vram_init(void) {
#if defined(_WIN32)
    /* Separate store and windows, reconciled by vram_copy(), the model and
     * its two caveats are at that function. */
    vram_store = VirtualAlloc(NULL, ARM_VRAM_STORE, MEM_RESERVE | MEM_COMMIT,
                              PAGE_READWRITE);
    if (vram_store == NULL) {
        snprintf(mem_err, sizeof mem_err,
                 "VirtualAlloc failed for the VRAM banks, error %lu",
                 (unsigned long)GetLastError());
        return -1;
    }
    if (VirtualAlloc((LPVOID)(uintptr_t)ARM_VRAM_BASE, ARM_VRAM_SIZE,
                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == NULL) {
        snprintf(mem_err, sizeof mem_err,
                 "VirtualAlloc failed for VRAM at 0x%08X, error %lu",
                 ARM_VRAM_BASE, (unsigned long)GetLastError());
        return -1;
    }
    memset(vram_cnt_live, 0, sizeof vram_cnt_live);
    return vram_remap();
#elif defined(__wasm__)
    /* The Windows model with nothing to allocate: the store is the static
     * below, linked above NP_GUEST_C_BASE with the rest of the C runtime's
     * data, and the windows are linear memory at their own addresses. Both
     * are zero here, at instantiation and after armrec_vram_free(). */
    static uint8_t vram_store_wasm[ARM_VRAM_STORE]
        __attribute__((aligned(ARM_VRAM_BLK)));
    vram_store = vram_store_wasm;
    memset(vram_cnt_live, 0, sizeof vram_cnt_live);
    return vram_remap();
#else
    if (vram_backing() != 0) return -1;
    vram_store = mmap(NULL, ARM_VRAM_STORE, PROT_READ | PROT_WRITE,
                      MAP_SHARED, vram_fd, 0);
    if (vram_store == MAP_FAILED) {
        snprintf(mem_err, sizeof mem_err, "cannot map the VRAM banks: %s",
                 strerror(errno));
        vram_store = NULL;
        return -1;
    }
    /* Reserve the address range before anything is placed in it, so a
     * collision is an error here rather than a surprise at the first remap. */
    if (mmap((void *)(uintptr_t)ARM_VRAM_BASE, ARM_VRAM_SIZE, PROT_READ,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) ==
        MAP_FAILED) {
        snprintf(mem_err, sizeof mem_err, "cannot map VRAM at 0x%08X: %s",
                 ARM_VRAM_BASE, strerror(errno));
        return -1;
    }
    memset(vram_cnt_live, 0, sizeof vram_cnt_live);
    return vram_remap();
#endif
}

static void armrec_vram_free(void) {
#if defined(_WIN32)
    if (vram_store) VirtualFree(vram_store, 0, MEM_RELEASE);
    VirtualFree((LPVOID)(uintptr_t)ARM_VRAM_BASE, 0, MEM_RELEASE);
#elif defined(__wasm__)
    /* Nothing to unmap. Zeroed instead, so the next armrec_vram_init()
     * finds what a fresh mapping gives the other hosts. */
    if (vram_store) memset(vram_store, 0, ARM_VRAM_STORE);
    memset((void *)(uintptr_t)ARM_VRAM_BASE, 0, ARM_VRAM_SIZE);
#else
    if (vram_store) munmap(vram_store, ARM_VRAM_STORE);
    munmap((void *)(uintptr_t)ARM_VRAM_BASE, ARM_VRAM_SIZE);
    if (vram_fd >= 0) close(vram_fd);
    vram_fd = -1;
#endif
#if defined(ARMREC_VRAM_COPY)
    {
        int w, i;
        for (w = 0; w < VW_COUNT; w++)
            for (i = 0; i < VW_MAXBLK; i++) {
                free(vram_ovl_snap[w][i]);
                vram_ovl_snap[w][i] = NULL;
            }
    }
#endif
    vram_store = NULL;
    vram_mapped = 0;
}

/* ------------------------------------------------------------------ */
/* The per-processor I/O page.                     */
/* ------------------------------------------------------------------ */

/*
 * Saved and restored, not remapped.
 *
 * The I/O pages could use VRAM's trick at a different address: back the page
 * with a shared object and mmap the running processor's half over 0x04000000.
 * It was built that way first and it is wrong twice over, both found by
 * measuring.
 *
 *   It leaks across fork(). A MAP_SHARED page is shared with every child, so
 *   guest register state written by a child is visible in the parent, which
 *   the anonymous MAP_PRIVATE page it replaced never was.
 *
 *   It is also 32 times slower. Over 200,000 iterations each, an mmap of
 *   MAP_SHARED|MAP_FIXED over a live page is 3,392 ns and saving and restoring
 *   4 KB is 107 ns. A remap tears down page tables and shoots down the TLB;
 *   4 KB is thirty-two cache lines.
 *
 * So the live page is the anonymous page armrec_mem_init() already maps, and
 * each processor's copy lives in an ordinary buffer.
 */
int armrec_cpu = ARMREC_CPU_ARM9;

static uint8_t io_saved[2][ARM_IO_PERCPU_SIZE];
static int io_ready;

/*
 * What both processors read the same value for, and nothing else. Derived from
 * `pcdiff-melon --io-selftest` by one rule with no judgement in it: neither
 * processor's write to the address sticks anywhere, and both read the same
 * value. That is KEYINPUT out of the write sweep and VCOUNT out of the
 * running-console half.
 *
 * Restricting it to read-only registers is what makes the argument work
 * without settling the hardware question. A sweep cannot tell "one counter
 * both processors read" from "two counters driven identically". But the port
 * drives these from the host side, so under either reading the correct
 * behaviour is that both processors see the same value.
 *
 * EXTKEYIN (0x04000136) is deliberately absent: the sweep has the ARM9 reading
 * 0x0000 there and the ARM7 reading 0x007F, so it is the ARM7's register.
 * Mirroring it would hand the ARM9's zero to the ARM7 and wedge X and Y.
 */
static const struct {
    uint32_t addr;
    uint32_t size;
    const char *name;
} io_mirror[] = {
    { 0x04000006u, 2, "VCOUNT" },
    { 0x04000130u, 2, "KEYINPUT" },
};

#define IO_MIRRORS ((int)(sizeof(io_mirror) / sizeof(io_mirror[0])))

int armrec_io_mirror_count(void) { return IO_MIRRORS; }

uint32_t armrec_io_mirror_at(int i, const char **name) {
    if (i < 0 || i >= IO_MIRRORS) { if (name) *name = NULL; return 0; }
    if (name) *name = io_mirror[i].name;
    return io_mirror[i].addr;
}

/*
 * The running processor's page is the live one at 0x04000000; the other one's
 * is its saved copy. Returning the live address rather than a stale buffer is
 * what makes this usable for "read the processor that is not running" without
 * the caller having to know which that is.
 */
void *armrec_io_page(int cpu) {
    if (!io_ready || cpu < 0 || cpu > ARMREC_CPU_ARM7) return NULL;
    if (cpu == armrec_cpu) return (void *)(uintptr_t)ARM_IO_BASE;
    return io_saved[cpu];
}

/*
 * The suspended processor's registers start as zeros, which is what the region
 * loop leaves at 0x04000000 for the ARM9, so the two processors start from
 * the same state a single mapping used to give them, rather than one of them
 * starting from whatever the other had done.
 */
static void armrec_io_init(void) {
    memset(io_saved, 0, sizeof io_saved);
    armrec_cpu = ARMREC_CPU_ARM9;
    io_ready = 1;
}

static void armrec_io_free(void) {
    io_ready = 0;
    armrec_cpu = ARMREC_CPU_ARM9;
}

/*
 * The rest of "which processor is running". Only the suspended one's copy is
 * meaningful; the running one's lives in armrec_sp and in armrec_mrs()'s word.
 * The ARM7's initial CPSR is an ARM core's reset value, supervisor mode with
 * both interrupt bits set, and its stack pointer is deliberately zero, so an
 * ARM7 entered without pc/src/pc_arm7.c setting one dies on the first push at
 * a null address rather than somewhere plausible.
 */
static struct { uint32_t sp, cpsr; } cpu_saved[2] = {
    { ARM_STACK_TOP, 0x0000001Fu },
    { 0u, 0x000000D3u },
};

int armrec_cpu_switch(int cpu) {
    uint8_t *live = (uint8_t *)(uintptr_t)ARM_IO_BASE;
    int i;

    if (cpu < 0 || cpu > ARMREC_CPU_ARM7) {
        snprintf(mem_err, sizeof mem_err, "no such processor: %d", cpu);
        return -1;
    }
    if (!io_ready) {
        snprintf(mem_err, sizeof mem_err,
                 "armrec_cpu_switch() before armrec_mem_init()");
        return -1;
    }
    if (cpu == armrec_cpu) return 0;

    memcpy(io_saved[armrec_cpu], live, ARM_IO_PERCPU_SIZE);
    /* The mirror is applied to the incoming copy rather than to the live page
     * after the restore, so there is one write of each mirrored address rather
     * than two and no window where the page holds the wrong one. */
    for (i = 0; i < IO_MIRRORS; i++) {
        uint32_t off = io_mirror[i].addr - ARM_IO_BASE;
        memcpy(io_saved[cpu] + off, io_saved[armrec_cpu] + off,
               io_mirror[i].size);
    }
    memcpy(live, io_saved[cpu], ARM_IO_PERCPU_SIZE);

    /*
     * The stack pointer and the CPSR travel with the page, because they are
     * the rest of "which processor is running": recompiled ARM7 code pushes
     * through ARM_SP exactly as recompiled ARM9 code does, and
     * OS_DisableInterrupts reads the CPSR whichever processor calls it. Going
     * through armrec_mrs/armrec_msr rather than the static below means a
     * strong override in pc/src/ sees the switch too.
     */
    cpu_saved[armrec_cpu].sp = armrec_sp;
    cpu_saved[armrec_cpu].cpsr = armrec_mrs(0);
    armrec_cpu = cpu;
    armrec_sp = cpu_saved[cpu].sp;
    armrec_msr(0, 0xFFFFFFFFu, cpu_saved[cpu].cpsr);
    return 0;
}

/*
 * We map each DS region at its literal address in the host process so guest
 * and host pointers coincide. These addresses (0x01FF8000 - 0x07001000) have
 * to stay clear of everything the host process puts in its own address space,
 * which is why pc/Makefile links the port at 0x10000000: in the 32-bit build
 * the default i386 text address is 0x08048000 with the heap growing up behind
 * it, close enough to the guest regions to be worth moving out of the way
 * deliberately rather than relying on the gap. The shared libraries and the
 * mmap region sit near the top of the address space, well clear.
 * MAP_FIXED_NOREPLACE turns a collision into an error rather than silently
 * clobbering something.
 */
/* ------------------------------------------------------------------ */
/* Host stacks at fixed addresses.                 */
/* ------------------------------------------------------------------ */

/*
 * Why the port does not run on the stack the kernel gives it.
 *
 * Decompiled C is host code, so its locals are on the host stack, and the SDK
 * stores pointers to caller buffers inside guest structures as a matter of
 * course. A pointer to a decompiled caller's local is therefore a host stack
 * address sitting in guest memory, and the kernel's stack moves with ASLR,
 * with the environment and with argv. That breaks the determinism contract
 * for a reason that has nothing to do with what the guest computed.
 *
 * Measured: exactly three bytes of the 4 MB of main RAM differed between two
 * runs, and they were bytes 0 to 2 of a host pointer at 0x02260390.
 *
 * So every stack the port runs on comes from here, at a fixed address below
 * the text segment and clear of every guest region. `setarch -R` then changes
 * nothing, and neither does an 8 KB environment variable, which is what
 * test_determinism checks.
 *
 * A bump allocator is enough because nothing frees a stack: re-initialising a
 * context keeps its host stack, since OSi_ExitThread re-inits the running
 * thread's context and freeing would free the stack the call stands on.
 */
#define ARM_HOST_STACK_BASE 0x0E000000u
#define ARM_HOST_STACK_SIZE 0x02000000u   /* 32 MB, up to -Ttext-segment */

#if !defined(__wasm__)
static char *host_stack_next;
#endif

void *armrec_host_stack(size_t size) {
#ifdef _WIN32
    (void)size;
    return NULL;
#elif defined(__wasm__)
    /*
     * No fixed address here, and none needed for the reason above's first
     * half: a wasm module has no ASLR and no kernel stack. Its stacks are
     * shadow stacks in linear memory (np_guest_abi.h, fiber_create), the
     * main one placed by the link, and 0x0E000000 is inside the C runtime's
     * own heap span (NP_GUEST_C_BASE up), so the arena becomes ordinary heap.
     *
     * The second half survives in part. The allocator is deterministic for an
     * identical sequence of allocations, but wasi-libc mallocs argv
     * (__main_void) and environ (on the first getenv) from this same heap, so
     * a different argv or environment size moves every later block. Equal
     * argv and environment from the native runtime make it exact again.
     *
     * No guard page: linear memory has no page protection. 16-byte aligned,
     * the wasm C ABI's stack alignment; never freed, as above.
     */
    size = (size + 15u) & ~(size_t)15u;
    return aligned_alloc(16, size);
#else
    char *p;

    if (host_stack_next == NULL) {
        void *base = mmap((void *)(uintptr_t)ARM_HOST_STACK_BASE,
                          ARM_HOST_STACK_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE
#ifdef MAP_FIXED_NOREPLACE
                          | MAP_FIXED_NOREPLACE
#else
                          | MAP_FIXED
#endif
                          , -1, 0);
        if (base == MAP_FAILED
            || (uintptr_t)base != (uintptr_t)ARM_HOST_STACK_BASE) {
            return NULL;
        }
        host_stack_next = (char *)base;
    }

    /* Page-align, and leave one page unmapped below every stack: an overflow
     * then faults on a guard rather than walking into the stack beneath it,
     * which is a silent corruption of another thread's frames. */
    size = (size + 0xFFFu) & ~(size_t)0xFFF;
    if (size + 0x1000u > ARM_HOST_STACK_SIZE
        || (size_t)(host_stack_next
                    - (char *)(uintptr_t)ARM_HOST_STACK_BASE)
               > ARM_HOST_STACK_SIZE - size - 0x1000u) {
        return NULL;
    }
    if (mprotect(host_stack_next, 0x1000, PROT_NONE) != 0) return NULL;
    p = host_stack_next + 0x1000;
    host_stack_next = p + size;
    return p;
#endif
}

#if defined(_WIN32)
/*
 * What is already there, appended to the failure that found it. A fixed
 * address map that is refused says only that the address was refused, and the
 * next question is always what holds it; which is far easier to answer from
 * inside the process than from outside it, and impossible to answer later.
 *
 * VirtualAlloc rounds a reservation's base DOWN to the allocation granularity,
 * so the span that has to be free is wider than the one asked for and the scan
 * starts at the rounded base. A span that reports free is itself the answer:
 * The refusal was about the address rather than about an occupant.
 */
static void mem_note_occupant(char *out, size_t cap, uint32_t addr,
                              uint32_t size) {
    SYSTEM_INFO si;
    uintptr_t gran, lo, hi, a;
    size_t n = strlen(out);

    if (n + 8 >= cap) return;
    GetSystemInfo(&si);
    gran = si.dwAllocationGranularity != 0 ? si.dwAllocationGranularity
                                           : 0x10000u;
    lo = (uintptr_t)addr & ~(gran - 1);
    hi = (uintptr_t)addr + size;
    for (a = lo; a < hi; a += gran) {
        MEMORY_BASIC_INFORMATION mbi;
        const char *what;
        char mod[MAX_PATH];

        if (VirtualQuery((LPCVOID)a, &mbi, sizeof mbi) != sizeof mbi) break;
        if (mbi.State == MEM_FREE) continue;
        switch (mbi.Type) {
        case MEM_IMAGE:   what = "a loaded image"; break;
        case MEM_MAPPED:  what = "a mapped file";  break;
        case MEM_PRIVATE: what = "private memory"; break;
        default:          what = "memory";         break;
        }
        mod[0] = '\0';
        if (mbi.Type == MEM_IMAGE) {
            if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, mod,
                                   sizeof mod) == 0) {
                mod[0] = '\0';
            }
        } else if (mbi.Type == MEM_MAPPED) {
            /* Reached by hand, never imported: naming a mapped file is worth
             * one GetProcAddress and NOT worth a DLL in the load-time set --
             * this port needs the low 64 MB of its address space free when
             * the loader is done, which is the very thing being reported
             * here. The call lives in kernel32 from Windows 7 and in psapi
             * before that. */
            typedef DWORD (WINAPI *pc_mapped_name)(HANDLE, LPVOID, LPSTR,
                                                   DWORD);
            static pc_mapped_name named;
            static int tried;

            if (!tried) {
                HMODULE k = GetModuleHandleA("kernel32.dll");

                tried = 1;
                if (k != NULL) {
                    named = (pc_mapped_name)(void (*)(void))
                            GetProcAddress(k, "K32GetMappedFileNameA");
                }
                if (named == NULL) {
                    HMODULE ps = LoadLibraryA("psapi.dll");

                    if (ps != NULL) {
                        named = (pc_mapped_name)(void (*)(void))
                                GetProcAddress(ps, "GetMappedFileNameA");
                    }
                }
            }
            if (named == NULL
                || named(GetCurrentProcess(), mbi.BaseAddress, mod,
                         sizeof mod) == 0) {
                mod[0] = '\0';
            }
        }
        snprintf(out + n, cap - n, "; 0x%08lX+0x%lX (allocation 0x%08lX) is"
                                   " %s%s%s",
                 (unsigned long)(uintptr_t)mbi.BaseAddress,
                 (unsigned long)mbi.RegionSize,
                 (unsigned long)(uintptr_t)mbi.AllocationBase, what,
                 mod[0] != '\0' ? " " : "", mod);
        return;
    }
    snprintf(out + n, cap - n, "; 0x%08lX-0x%08lX is free, so the address"
                               " itself was refused",
             (unsigned long)lo, (unsigned long)hi);
}

/*
 * The one cause worth naming, because it is invisible from outside and the
 * remedy is not guessable. Windows injects its application-compatibility
 * shims (AcLayers.dll over apphelp.dll) into any process carrying a
 * compatibility layer, and a layer is inherited by every child of a process
 * that has one. The shim heap is a pagefile-backed section of about twenty
 * megabytes that Windows places LOW: measured here at 0x014B0000 through
 * 0x028B1000, over the DS's ITCM and its main RAM both. A port that maps the
 * console's memory at the console's own addresses cannot survive that, and
 * the failure says only "error 487" unless something says this.
 *
 * Nothing here can undo it in flight; the shims are loaded before main(),
 * so the message carries the remedy instead.
 */
static void mem_note_shim(char *out, size_t cap) {
    size_t n = strlen(out);

    if (GetModuleHandleA("AcLayers.dll") == NULL
        && GetModuleHandleA("apphelp.dll") == NULL) {
        return;
    }
    if (n + 8 >= cap) return;
    snprintf(out + n, cap - n,
             ". Windows has applied a COMPATIBILITY LAYER to this program and"
             " its shim heap is sitting on the console's memory map. Clear it:"
             " right-click the exe, Properties, Compatibility, untick"
             " everything, or unset __COMPAT_LAYER in the environment that"
             " starts it");
}

/*
 * The whole low map, printed once when a region is refused. The occupant note
 * above names one block; this says what the process looked like when the
 * loader handed over, which is the only way to tell a stray allocation from a
 * layout that was never going to fit. Fatal path only, nobody sees it
 * unless the port is already not starting.
 */
static void mem_dump_low_map(void) {
    uintptr_t a = 0x10000u;

    fprintf(stderr, "pokeplatinum-pc: the low address space as the loader"
                    " left it:\n");
    while (a < 0x7F000000u) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t next;

        if (VirtualQuery((LPCVOID)a, &mbi, sizeof mbi) != sizeof mbi) break;
        next = (uintptr_t)mbi.BaseAddress + (uintptr_t)mbi.RegionSize;
        if (next <= a) break;
        /* Below the guest map, everything; above it, only images; which is
         * the DLL list, and the question above 0x0C000000 is always which
         * module rather than how much. */
        if (mbi.State != MEM_FREE
            && (a < 0x0C000000u || mbi.Type == MEM_IMAGE)) {
            const char *type = mbi.Type == MEM_IMAGE   ? "image"
                             : mbi.Type == MEM_MAPPED  ? "mapped"
                             : mbi.Type == MEM_PRIVATE ? "private" : "?";
            char mod[MAX_PATH];

            mod[0] = '\0';
            if (mbi.Type == MEM_IMAGE) {
                if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, mod,
                                       sizeof mod) == 0) {
                    mod[0] = '\0';
                }
            }
            fprintf(stderr, "  0x%08lX+0x%08lX %-7s %-9s %s\n",
                    (unsigned long)(uintptr_t)mbi.BaseAddress,
                    (unsigned long)mbi.RegionSize, type,
                    mbi.State == MEM_COMMIT ? "committed" : "reserved", mod);
        }
        a = next;
    }
    fflush(stderr);
}
#endif

#if defined(__wasm__)
/*
 * The one way a map nobody makes can go wrong: the C runtime sitting on it.
 * pc/Makefile.wasm links the data at --global-base=NP_GUEST_C_BASE, the main
 * shadow stack after it (--no-stack-first) and the heap after that, so every
 * byte the C runtime owns is above every DS address, the GBA slot included.
 * Checked rather than trusted, from the linker's own symbols and from a
 * static's and a local's address as the direct evidence, because a C object
 * on top of main RAM would surface frames later as guest memory corruption.
 * The memory itself has to reach the C base too, or the DS map is not there.
 */
static void mem_check_c_runtime(void) {
    extern char __global_base[], __heap_base[];
    volatile char probe = 0;
    const struct { const char *what; uintptr_t at; } c[] = {
        { "__global_base (the C data)", (uintptr_t)__global_base },
        { "a static (mem_err)", (uintptr_t)mem_err },
        { "the shadow stack (a local)", (uintptr_t)&probe },
        { "__heap_base", (uintptr_t)__heap_base },
    };
    size_t i;

    for (i = 0; i < sizeof c / sizeof c[0]; i++) {
        if (c[i].at < NP_GUEST_C_BASE) {
            pc_wasm_fatalf("armrec: %s is at 0x%08lX, below NP_GUEST_C_BASE "
                           "0x%08X, inside the DS address map; the wasm link "
                           "must place the C runtime above it "
                           "(pc/Makefile.wasm, --global-base, "
                           "--no-stack-first)",
                           c[i].what, (unsigned long)c[i].at,
                           (unsigned)NP_GUEST_C_BASE);
        }
    }
    if ((uint64_t)__builtin_wasm_memory_size(0) * 65536u
        < (uint64_t)NP_GUEST_MEMORY_BYTES) {
        pc_wasm_fatalf("armrec: linear memory is 0x%llX bytes, the guest ABI "
                       "fixes it at 0x%X",
                       (unsigned long long)__builtin_wasm_memory_size(0)
                           * 65536u,
                       (unsigned)NP_GUEST_MEMORY_BYTES);
    }
}
#endif

int armrec_mem_init(void) {
    int i;

    if (armrec_mem_ready) return 0;

#if defined(_WIN32)
    /* PC_MEMMAP=1: the same map on a run that WORKS, which is the only way to
     * read the one printed by a run that does not. */
    if (getenv("PC_MEMMAP") != NULL) mem_dump_low_map();
    for (i = 0; i < NREGIONS; i++) {
        void *p;
        /*
         * `provided` is not skipped here, unlike POSIX: it means the ELF
         * link placed .guestwin at this address, and a PE image cannot have
         * a section below its own base, so nothing provides it on Windows
         * and the region is mapped like any other. What is lost is only the
         * *static* placement (PC_GUEST_BSS, i.e. sound.c's statics, the
         * gap Makefile.win's header records); pc_guest_window.c's runtime
         * allocations need the memory to exist either way, and skipping it
         * was the access violation that ended a run at its first mirror
         * write.
         */
        region_mapped[i] = 1;
        p = VirtualAlloc((LPVOID)(uintptr_t)regions[i].base, regions[i].size,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p == NULL) {
            snprintf(mem_err, sizeof mem_err,
                     "VirtualAlloc failed for %s at 0x%08X (size 0x%X), error %lu",
                     regions[i].name, regions[i].base, regions[i].size,
                     (unsigned long)GetLastError());
            mem_note_occupant(mem_err, sizeof mem_err, regions[i].base,
                              regions[i].size);
            mem_note_shim(mem_err, sizeof mem_err);
            mem_dump_low_map();
            armrec_mem_free();
            return -1;
        }
    }
#elif defined(__wasm__)
    /*
     * Nothing to map: each region is linear memory at its own address, there
     * since instantiation and zero, because no data segment lies below
     * NP_GUEST_C_BASE (checked here) and armrec_mem_free() re-zeroes what it
     * releases. `provided` means nothing here either: as on Windows, nothing
     * link-places into the port window (wasm-ld cannot put a section at
     * 0x02A00000), so pc_guest_window.c's weak __pc_guest_window_free stays
     * undefined, reads as 0, and its allocator starts at the region's base.
     */
    mem_check_c_runtime();
    for (i = 0; i < NREGIONS; i++) region_mapped[i] = 1;
#else
    for (i = 0; i < NREGIONS; i++) {
        void *want = (void *)(uintptr_t)regions[i].base;
        void *p;
        p = mmap(want, regions[i].size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        /*
         * A `provided` region may already be there, and MAP_FIXED_NOREPLACE
         * failing is exactly how we find out; it fails only when something
         * occupies the address. The port's own link places the game's statics
         * inside the port window (pc/Makefile, PC_GUEST_BSS), so there it is
         * occupied and mapping over it would replace them with zeroed pages;
         * a test binary links armrec_rt.c without that fragment, so there it
         * is not and the region has to be mapped like any other. One code path
         * decides which, by asking, rather than two builds having to agree.
         */
        if (regions[i].provided && p != want) {
            if (p != MAP_FAILED) munmap(p, regions[i].size);
            continue;
        }
        region_mapped[i] = 1;
        if (p == MAP_FAILED || p != want) {
            snprintf(mem_err, sizeof mem_err,
                     "cannot map %s at 0x%08X (size 0x%X): %s",
                     regions[i].name, regions[i].base, regions[i].size,
                     p == MAP_FAILED ? strerror(errno) : "address already in use");
            if (p != MAP_FAILED && p != want) munmap(p, regions[i].size);
            armrec_mem_free();
            return -1;
        }
    }
#endif

    /*
     * VRAM last, and after armrec_mem_ready, because it reads VRAMCNT out of
     * the I/O region the loop above has just mapped.
     */
    armrec_mem_ready = 1;
    /*
     * The per-processor I/O page is armed before VRAM, because
     * armrec_vram_init() reads VRAMCNT back out of the live page and a switch
     * has to be able to save what it finds there.
     */
    armrec_io_init();
    if (armrec_vram_init() != 0) {
        armrec_mem_free();
        return -1;
    }
    armrec_sp = ARM_STACK_TOP;
    return 0;
}

/*
 * The five VRAM windows are appended to the fixed table. Each one's *size* is
 * the content it can address rather than the address space it owns, 512 KB
 * of main BG in a 2 MB slot, so a walker sees each bank once instead of
 * once per mirror, which is what the state digest wants. What no walker sees
 * is a bank assigned to texture or to an extended palette: those are not in
 * the ARM9's address space at all, so their contents drop out of the digest
 * until the 3D layer gives them a reader.
 */
int armrec_region_count(void) { return NREGIONS + VW_COUNT; }

int armrec_region_at(int i, uint32_t *base, uint32_t *size, const char **name) {
    static const char *const wname[VW_COUNT] = {
        "VRAM main BG", "VRAM sub BG", "VRAM main OBJ", "VRAM sub OBJ",
        "VRAM LCDC"
    };
    if (i >= NREGIONS && i < NREGIONS + VW_COUNT) {
        int w = i - NREGIONS;
        if (base) *base = vram_win[w].base;
        if (size) *size = (uint32_t)vram_win[w].blocks * ARM_VRAM_BLK;
        if (name) *name = wname[w];
        return 1;
    }
    if (i < 0 || i >= NREGIONS) return 0;
    if (base) *base = regions[i].base;
    if (size) *size = regions[i].size;
    if (name) *name = regions[i].name;
    return 1;
}

void armrec_mem_free(void) {
#if defined(__wasm__)
    /* Nothing to unmap; zeroed instead, so the next armrec_mem_init() finds
     * what a fresh mapping gives the other hosts. */
    int i;
    for (i = 0; i < NREGIONS; i++)
        if (region_mapped[i]) {
            memset((void *)(uintptr_t)regions[i].base, 0, regions[i].size);
            region_mapped[i] = 0;
        }
#elif !defined(_WIN32)
    int i;
    for (i = 0; i < NREGIONS; i++)
        if (region_mapped[i]) {
            munmap((void *)(uintptr_t)regions[i].base, regions[i].size);
            region_mapped[i] = 0;
        }
#endif
    armrec_vram_free();
    armrec_io_free();
    armrec_mem_ready = 0;
}

#ifdef ARMREC_CHECKED_MEM
static void check(uint32_t a, uint32_t n) {
    int i;
    for (i = 0; i < NREGIONS; i++)
        if (a >= regions[i].base && a + n <= regions[i].base + regions[i].size) return;
    fprintf(stderr, "armrec: out-of-bounds access at 0x%08X (%u bytes)\n", a, n);
    abort();
}
uint32_t armrec_ld32(uint32_t a) {
    check(a, 4);
    if (ARM_CP_HIT(a)) return armrec_cp_read32(a);
    return *(uint32_t *)(uintptr_t)a;
}
uint32_t armrec_ld16(uint32_t a) {
    check(a, 2);
    if (ARM_CP_HIT(a)) return armrec_cp_read16(a);
    return *(uint16_t *)(uintptr_t)a;
}
uint32_t armrec_ld8(uint32_t a) {
    check(a, 1);
    if (ARM_CP_HIT(a)) return armrec_cp_read8(a);
    return *(uint8_t *)(uintptr_t)a;
}
void armrec_st32(uint32_t a, uint32_t v) { check(a, 4); *(uint32_t *)(uintptr_t)a = v; }
void armrec_st16(uint32_t a, uint32_t v) { check(a, 2); *(uint16_t *)(uintptr_t)a = (uint16_t)v; }
void armrec_st8(uint32_t a, uint32_t v)  { check(a, 1); *(uint8_t *)(uintptr_t)a = (uint8_t)v; }
#endif

/* ------------------------------------------------------------------ */
/* The maths coprocessor                                              */
/* ------------------------------------------------------------------ */

/*
 * The DS ARM9 has a hardware divider and square-root unit behind
 * 0x04000280 to 0x040002BF. On hardware they are engines: a write to a control
 * or operand register starts a calculation, a busy bit is set for 18 to 34
 * cycles, and the result registers hold whatever the last one produced.
 *
 * There is no write hook because identity mapping makes those addresses plain
 * memory, and there is no one hook that catches every writer: eleven
 * decompiled C files write these registers through registers.h's `reg_CP_*`
 * macros and thirteen assembly files write them from recompiled code. A macro
 * used as an lvalue cannot become a call, and one overlay stores through a raw
 * pointer instead. So the operands stay storage.
 *
 * The answer is produced at a completion point instead, which is any touch of
 * a register the unit owns. Every user spells the wait `while (CNT & 0x8000)`,
 * so a result is only read after the unit says it is idle, which makes this
 * observationally equivalent to hardware for every caller here.
 *
 * An earlier version stored into a private shadow, which only serves the
 * pointer that fetched it. Writing the register means the value outlives the
 * pointer.
 *
 * Semantics follow melonDS's NDS::DivDone/SqrtDone. The square root is written
 * independently as floor(sqrt(v)) rather than transcribed, so the two agreeing
 * is evidence. DIVCNT bit 14 and the busy bit are not modelled; nothing here
 * reads them.
 */
#define CP_DIVCNT_ADDR  0x04000280u
#define CP_NUMER_ADDR   0x04000290u
#define CP_DENOM_ADDR   0x04000298u
#define CP_RESULT_ADDR  0x040002A0u
#define CP_REM_ADDR     0x040002A8u
#define CP_SQRTCNT_ADDR 0x040002B0u
#define CP_SQRTRES_ADDR 0x040002B4u

static uint32_t cp_ld32(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static uint16_t cp_ld16(uint32_t a) { return *(volatile uint16_t *)(uintptr_t)a; }

static void cp_st32(uint32_t a, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)a = v;
}

static uint64_t cp_ld64(uint32_t a) {
    return (uint64_t)cp_ld32(a) | ((uint64_t)cp_ld32(a + 4) << 32);
}

/*
 * melonDS leaves DIVREM_RESULT *unchanged* for 32-bit INT_MIN / -1, where the
 * other two modes zero it. A model that computes from the operands has no
 * "unchanged" to return, and guessing between 0 and the previous remainder is
 * exactly the kind of plausible wrong answer that costs a week later, so this
 * says so and stops. It needs a numerator of exactly -2^31 with a denominator
 * of -1, which no caller in this tree can produce.
 *
 * A completion does not trap, it declines to write: the register is
 * the storage, so "leave the previous remainder in place" is now something the
 * port can express, and it is what melonDS does. Asking for the remainder in
 * that state still traps, because answering with the previous division's is a
 * claim about hardware timing this project has no oracle for; the port would
 * have to have run the same sequence of divisions the console did. What it
 * leaves behind is worth knowing: with the state now present, a later run that
 * can show the two agree could delete the trap, and test_cp_div's forked child
 * is where that would be settled.
 */
static void cp_unmodelled(int32_t num, int32_t den) {
    fprintf(stderr,
            "armrec: DIVREM_RESULT read after %d / %d in 32-bit mode.\n"
            "  The hardware's remainder for INT_MIN / -1 is not something this\n"
            "  model can produce: melonDS leaves the previous division's\n"
            "  remainder in place there, and the coprocessor here is stateless\n"
            "  (see armrec_cp_read32() in tools/armrec/armrec_rt.c).\n"
            "  Answering 0 would be a guess.\n",
            num, den);
    abort();
}

/* The 64-bit quotient and remainder for the operands currently in memory. */
static void cp_divide(uint64_t *quot, uint64_t *rem, int want_rem) {
    uint32_t mode = cp_ld16(CP_DIVCNT_ADDR) & 3u;

    if (mode == 0) {
        int32_t num = (int32_t)cp_ld32(CP_NUMER_ADDR);
        int32_t den = (int32_t)cp_ld32(CP_DENOM_ADDR);
        if (den == 0) {
            /* Not a sign-extended +/-1: the low word is the 32-bit answer and
             * the high word carries the opposite sign. melonDS's own words. */
            *quot = (num < 0) ? 0xFFFFFFFF00000001ULL : 0x00000000FFFFFFFFULL;
            *rem = (uint64_t)(int64_t)num;
        } else if (num == (-0x7FFFFFFF - 1) && den == -1) {
            *quot = 0x0000000080000000ULL;
            if (want_rem) cp_unmodelled(num, den);
            *rem = 0;
        } else {
            *quot = (uint64_t)(int64_t)(num / den);
            *rem = (uint64_t)(int64_t)(num % den);
        }
        return;
    }

    {
        int64_t num = (int64_t)cp_ld64(CP_NUMER_ADDR);
        int64_t den = (mode == 2) ? (int64_t)cp_ld64(CP_DENOM_ADDR)
                                  : (int64_t)(int32_t)cp_ld32(CP_DENOM_ADDR);
        if (den == 0) {
            *quot = (uint64_t)(int64_t)((num < 0) ? 1 : -1);
            *rem = (uint64_t)num;
        } else if (num == (-0x7FFFFFFFFFFFFFFFLL - 1) && den == -1) {
            *quot = 0x8000000000000000ULL;
            *rem = 0;
        } else {
            *quot = (uint64_t)(num / den);
            *rem = (uint64_t)(num % den);
        }
    }
}

/*
 * floor(sqrt(v)) over the unsigned operand, 32 or 64 bits by SQRTCNT bit 0.
 * Written from the definition rather than transcribed from the oracle, so that
 * test_cp_div comparing the two is a measurement.
 */
static uint32_t cp_sqrt(void) {
    uint64_t v = (cp_ld16(CP_SQRTCNT_ADDR) & 1u) ? cp_ld64(0x040002B8u)
                                                 : (uint64_t)cp_ld32(0x040002B8u);
    uint64_t r = 0, bit;
    int shift = 62;

    while (shift > 0 && (v >> shift) == 0) shift -= 2;
    for (bit = 1ULL << shift; bit; bit >>= 2) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
    }
    return (uint32_t)r;
}

/*
 * 32-bit INT_MIN / -1 is the one state whose remainder has no answer here; see
 * cp_unmodelled() above. A completion has to recognise it *without* computing,
 * because the whole point is that it leaves DIVREM alone rather than trapping.
 */
static int cp_rem_unmodelled(void) {
    return (cp_ld16(CP_DIVCNT_ADDR) & 3u) == 0
        && (int32_t)cp_ld32(CP_NUMER_ADDR) == (-0x7FFFFFFF - 1)
        && (int32_t)cp_ld32(CP_DENOM_ADDR) == -1;
}

/*
 * The divider finishing: both result registers written from the operands as
 * they stand. `want_rem` is whether the caller is asking for the remainder;
 * a plain completion is not, and must not trap on a state it can simply leave
 * as it found it.
 */
static void cp_div_complete(int want_rem) {
    uint64_t q, r;

    if (!want_rem && cp_rem_unmodelled()) {
        cp_divide(&q, &r, 0);
        cp_st32(CP_RESULT_ADDR, (uint32_t)q);
        cp_st32(CP_RESULT_ADDR + 4, (uint32_t)(q >> 32));
        return;                         /* DIVREM keeps what it had */
    }
    cp_divide(&q, &r, want_rem);
    cp_st32(CP_RESULT_ADDR, (uint32_t)q);
    cp_st32(CP_RESULT_ADDR + 4, (uint32_t)(q >> 32));
    cp_st32(CP_REM_ADDR, (uint32_t)r);
    cp_st32(CP_REM_ADDR + 4, (uint32_t)(r >> 32));
}

static void cp_sqrt_complete(void) { cp_st32(CP_SQRTRES_ADDR, cp_sqrt()); }

void *armrec_cp_ptr(uint32_t addr) {
    switch (addr & ~3u) {
    case CP_DIVCNT_ADDR:                  /* the `while (CNT & 0x8000)` wait */
    case CP_RESULT_ADDR:
    case CP_RESULT_ADDR + 4:
        cp_div_complete(0);
        break;
    case CP_REM_ADDR:
    case CP_REM_ADDR + 4:
        cp_div_complete(1);
        break;
    case CP_SQRTCNT_ADDR:
    case CP_SQRTRES_ADDR:
        cp_sqrt_complete();
        break;
    default:                              /* operands and the gaps: storage */
        break;
    }
    return ARM_HOSTPTR(addr);
}

uint32_t armrec_cp_read32(uint32_t a) {
    return *(volatile uint32_t *)armrec_cp_ptr(a);
}

uint32_t armrec_cp_read16(uint32_t a) {
    uint32_t w = armrec_cp_read32(a & ~3u);
    return (w >> ((a & 2u) * 8)) & 0xFFFFu;
}

uint32_t armrec_cp_read8(uint32_t a) {
    uint32_t w = armrec_cp_read32(a & ~3u);
    return (w >> ((a & 3u) * 8)) & 0xFFu;
}

/* ------------------------------------------------------------------ */
/* The geometry engine's command ports and result registers            */
/* ------------------------------------------------------------------ */

/*
 * pc/hw/pc_gpu3d.c has the model; these are the three entry points into it and
 * the *weak* fallbacks that stand in when it is not linked. A test that builds
 * a subset without the engine gets identity-mapped memory back, which is what
 * the whole port had before the geometry engine, so nothing that does not want one
 * engine has to know one exists.
 */
__attribute__((weak)) void pc_gpu3d_write(uint32_t a, uint32_t v, int size) {
    switch (size) {
    case 1: *(volatile uint8_t  *)ARM_HOSTPTR(a) = (uint8_t)v;  break;
    case 2: *(volatile uint16_t *)ARM_HOSTPTR(a) = (uint16_t)v; break;
    default: *(volatile uint32_t *)ARM_HOSTPTR(a) = v;          break;
    }
}

__attribute__((weak)) uint32_t pc_gpu3d_read(uint32_t a, int size, int *handled) {
    (void)a; (void)size;
    *handled = 0;
    return 0;
}

__attribute__((weak)) uint32_t *pc_gpu3d_stage(uint32_t a) {
    return (uint32_t *)ARM_HOSTPTR(a);
}

__attribute__((weak)) void pc_gpu3d_flush(void) { }

__attribute__((weak)) void pc_gpu3d_refresh_regs(void) { }

__attribute__((weak)) int pc_gpu3d_store_through(void *dst, uint32_t v) {
    (void)dst; (void)v;
    return 0;
}

/* pc_video.c calls this at every frame boundary and pc_main.c at startup, so
 * both need a definition in a build that leaves the engine out; which is
 * most of the tests. */
__attribute__((weak)) void pc_gpu3d_vblank(void) { }

__attribute__((weak)) void pc_gpu3d_install(void) { }

/*
 * The 3D rasterizer's wide line, pc/hw/pc_gpu3d_soft.c overrides this.
 * The weak default says "native width, no line", which is what a build
 * without the rasterizer should answer: pc/src/pc_view.c asks only when a
 * wide width was configured, and nothing configures one in such a build.
 */
__attribute__((weak)) const uint32_t *pc_gpu3d_soft_line_wide(int y, int *w)
{
    (void)y;
    if (w != NULL) *w = 256;
    return NULL;
}

__attribute__((weak)) const uint32_t *pc_gpu3d_soft_line(int y)
{
    (void)y;
    return NULL;
}

__attribute__((weak)) const uint32_t *pc_gpu3d_soft_line_hd(int hy, int *w,
                                                            int *s)
{
    (void)hy;
    if (w != NULL) *w = 256;
    if (s != NULL) *s = 1;
    return NULL;
}

/*
 * The SPU's keyon note, pc/hw/pc_spu.c overrides this with the real one.
 * The ARM7's sound driver starts a channel by ORing the start bit into a
 * control word it may have just rewritten whole, all inside one sequencer
 * tick; the SPU's latch diffs register *state* between two instants and a
 * bit that reads 1 at both ends is not an edge, however many times it was
 * written in between. So the driver's two start sites name the event
 * explicitly (arm7/lib/include/registers.h, reg_SOUNDxCNT_KEYON) and this
 * is where the name lands. Weak so a build without the mixer still links.
 */
__attribute__((weak)) void pc_spu_keyon_note(int idx) { (void)idx; }

void armrec_gx_store(uint32_t a, uint32_t v, int size) {
    pc_gpu3d_write(a, v, size);
}

uint32_t armrec_gx_load(uint32_t a, int size) {
    int handled = 0;
    uint32_t v = pc_gpu3d_read(a, size, &handled);
    if (handled) return v;
    switch (size) {
    case 1: return *(volatile uint8_t  *)ARM_HOSTPTR(a);
    case 2: return *(volatile uint16_t *)ARM_HOSTPTR(a);
    default: return *(volatile uint32_t *)ARM_HOSTPTR(a);
    }
}

uint32_t *armrec_gx_port(uint32_t addr) {
    return pc_gpu3d_stage(addr);
}

void *armrec_gx_reg(uint32_t addr) {
    pc_gpu3d_refresh_regs();
    return ARM_HOSTPTR(addr);
}

/*
 * The IPC block. The model is pc/src/pc_ipc.c and these are
 * the weak defaults for a build without it, which is the GX hook's arrangement
 * and for the same reason: test_armrec_data and test_overlays recompile the
 * whole tree, so arm7/asm/PXI_fifo.s reaches these in binaries that have no
 * platform layer at all. Plain memory is what they had before this task.
 */
__attribute__((weak)) void pc_ipc_write(uint32_t a, uint32_t v, int size) {
    switch (size) {
    case 1: *(volatile uint8_t  *)ARM_HOSTPTR(a) = (uint8_t)v;  break;
    case 2: *(volatile uint16_t *)ARM_HOSTPTR(a) = (uint16_t)v; break;
    default: *(volatile uint32_t *)ARM_HOSTPTR(a) = v;          break;
    }
}

__attribute__((weak)) uint32_t pc_ipc_read(uint32_t a, int size) {
    switch (size) {
    case 1: return *(volatile uint8_t  *)ARM_HOSTPTR(a);
    case 2: return *(volatile uint16_t *)ARM_HOSTPTR(a);
    default: return *(volatile uint32_t *)ARM_HOSTPTR(a);
    }
}

void armrec_ipc_store(uint32_t a, uint32_t v, int size) {
    pc_ipc_write(a, v, size);
}

uint32_t armrec_ipc_load(uint32_t a, int size) {
    return pc_ipc_read(a, size);
}

/*
 * The ARM7's SPI bus, the same arrangement one window over.
 * pc/src/pc_spi.c defines the strong pair; these weak ones keep the four
 * arm7/asm/SPI*.s and NVRAM.s files linkable in a binary with no platform layer,
 * where plain memory is what they had before this task.
 */
__attribute__((weak)) void pc_spi_write(uint32_t a, uint32_t v, int size) {
    switch (size) {
    case 1: *(volatile uint8_t  *)ARM_HOSTPTR(a) = (uint8_t)v;  break;
    case 2: *(volatile uint16_t *)ARM_HOSTPTR(a) = (uint16_t)v; break;
    default: *(volatile uint32_t *)ARM_HOSTPTR(a) = v;          break;
    }
}

__attribute__((weak)) uint32_t pc_spi_read(uint32_t a, int size) {
    switch (size) {
    case 1: return *(volatile uint8_t  *)ARM_HOSTPTR(a);
    case 2: return *(volatile uint16_t *)ARM_HOSTPTR(a);
    default: return *(volatile uint32_t *)ARM_HOSTPTR(a);
    }
}

void armrec_spi_store(uint32_t a, uint32_t v, int size) {
    pc_spi_write(a, v, size);
}

uint32_t armrec_spi_load(uint32_t a, int size) {
    return pc_spi_read(a, size);
}

/* ------------------------------------------------------------------ */
/* Guest address -> recompiled function                               */
/* ------------------------------------------------------------------ */

struct entry {
    uint32_t addr;
    armrec_fn fn;
    const char *name;
    int owner; /* overlay id, or -1 for always-resident code */
};

/* Open-addressed table; sized well above the ~22k functions in the game. */
#define TABLE_BITS 16
#define TABLE_SIZE (1u << TABLE_BITS)
#define TABLE_MASK (TABLE_SIZE - 1)

static struct entry table[TABLE_SIZE];
static int table_count = 0;

/*
 * Residency, and the windows the resident overlays occupy. One byte and two
 * words per overlay, beside the table rather than inside it.
 *
 * This is the whole of "evict overlay NN": nothing in the table is touched, so
 * a probe chain cannot be broken by a load and there are no tombstones to
 * accumulate. It also makes a swap O(1) rather than O(the overlay's ~1,700
 * entries), which matters because an overlay transition happens on every map
 * change. See the header for the model.
 */
static unsigned char ovl_resident[ARMREC_MAX_OVERLAYS];
static uint32_t ovl_base[ARMREC_MAX_OVERLAYS];
static uint32_t ovl_size[ARMREC_MAX_OVERLAYS];

static inline uint32_t hash_addr(uint32_t a) {
    a = (a >> 2) * 2654435761u;
    return a & TABLE_MASK;
}

static inline int entry_live(const struct entry *e) {
    return e->owner < 0 || ovl_resident[e->owner];
}

/*
 * Insert (addr, fn, name) owned by `owner`.
 *
 * Two claimants of one address are legal only when they are *different*
 * overlays, which is the fact this whole model exists for: 1,389 guest
 * addresses in this tree are claimed by more than one overlay. Anything else
 * is a hard error, and that is the change of behaviour rather than a new check:
 * This function used to keep the first registration for an address and
 * return, silently discarding 1,764 of them, and that silence is what hid the
 * bug for the life of the project. Registering the same function twice is
 * still fine: armrec_bind_externs() can reach one host address by two names.
 */
static void register_owned(uint32_t addr, armrec_fn fn, const char *name,
                           int owner) {
    uint32_t i = hash_addr(addr);
    if (table_count >= (int)(TABLE_SIZE * 3 / 4)) {
        fprintf(stderr, "armrec: dispatch table full (%d entries)\n", table_count);
        abort();
    }
    while (table[i].addr != 0) {
        if (table[i].addr == addr && table[i].owner == owner) {
            if (table[i].fn == fn) return; /* the same registration twice */
            fprintf(stderr,
                    "armrec: two different functions are registered at guest "
                    "address 0x%08X.\n"
                    "  %s and %s, both in %s.\n"
                    "  One address cannot mean two things in one module; this "
                    "used to be\n"
                    "  resolved by keeping the first and saying nothing.\n",
                    addr, table[i].name, name,
                    owner < 0 ? "always-resident code" : "one overlay");
            abort();
        }
        if (table[i].addr == addr && (table[i].owner < 0 || owner < 0)) {
            fprintf(stderr,
                    "armrec: guest address 0x%08X is claimed by both an "
                    "overlay and always-resident code\n"
                    "  (%s and %s). Residency cannot choose between them.\n",
                    addr, table[i].name, name);
            abort();
        }
        i = (i + 1) & TABLE_MASK;
    }
    table[i].addr = addr;
    table[i].fn = fn;
    table[i].name = name;
    table[i].owner = owner;
    table_count++;
}

void armrec_register(uint32_t addr, armrec_fn fn, const char *name) {
    register_owned(addr, fn, name, -1);
}

void armrec_register_overlay(uint32_t addr, armrec_fn fn, const char *name,
                             int ovl) {
    if (ovl < 0 || ovl >= ARMREC_MAX_OVERLAYS) {
        fprintf(stderr, "armrec: overlay id %d is out of range for %s\n", ovl, name);
        abort();
    }
    register_owned(addr, fn, name, ovl);
}

/*
 * Resolve an address to the claimant that is *live*.
 *
 * All entries sharing an address share a hash, so they all sit in one probe
 * run and this finds them without a second index. `any` is filled with the
 * first claimant whatever its residency, so a failed lookup can still name who
 * owns the address; which is the whole return on the overlay model: today's
 * silent wrong call becomes a located one.
 */
static struct entry *lookup_ex(uint32_t addr, struct entry **any) {
    uint32_t i = hash_addr(addr);
    if (any) *any = NULL;
    while (table[i].addr != 0) {
        if (table[i].addr == addr) {
            if (any && *any == NULL) *any = &table[i];
            if (entry_live(&table[i])) return &table[i];
        }
        i = (i + 1) & TABLE_MASK;
    }
    return NULL;
}

static struct entry *lookup(uint32_t addr) { return lookup_ex(addr, NULL); }

/*
 * Say who owns an address that resolved to nothing, and what is resident.
 *
 * There are three outcomes at dispatch, not two: no slot at all ("never
 * translated"), a slot whose owner is not resident ("overlay NN owns this and
 * is not loaded"), and a call. Only the middle one used to exist as a silently
 * wrong call, so this message is the fix as much as the model is.
 */
static void report_claimants(uint32_t addr) {
    uint32_t i = hash_addr(addr & ~1u);
    int n = 0, j;
    while (table[i].addr != 0) {
        if (table[i].addr == (addr & ~1u)) {
            if (table[i].owner < 0)
                fprintf(stderr, "  claimed by always-resident code: %s\n",
                        table[i].name);
            else
                fprintf(stderr, "  claimed by overlay %d: %s (%s)\n",
                        table[i].owner, table[i].name,
                        ovl_resident[table[i].owner] ? "resident" : "not loaded");
            n++;
        }
        i = (i + 1) & TABLE_MASK;
    }
    if (n == 0) {
        fprintf(stderr, "  no overlay or module claims it; nothing was ever "
                        "translated at that address\n");
        return;
    }
    fprintf(stderr, "  resident overlays right now:");
    for (j = 0; j < ARMREC_MAX_OVERLAYS; j++)
        if (ovl_resident[j]) fprintf(stderr, " %d", j);
    fprintf(stderr, "\n");
}

void armrec_load_overlay(int id, uint32_t ram_address, uint32_t total_size) {
    int i;

    if (id < 0 || id >= ARMREC_MAX_OVERLAYS) {
        fprintf(stderr, "armrec: overlay id %d is out of range\n", id);
        abort();
    }
    /*
     * Evict every resident overlay whose live range intersects the incoming
     * one. Not "the overlay that shares this base address": 116 pairs at
     * different bases intersect, because a 32-byte overlay can sit inside a
     * 250 KB one. Half-open intervals, so overlays that merely abut do not
     * evict each other.
     */
    for (i = 0; i < ARMREC_MAX_OVERLAYS; i++) {
        if (i == id || !ovl_resident[i]) continue;
        if (ovl_base[i] < ram_address + total_size &&
            ram_address < ovl_base[i] + ovl_size[i]) {
            if (armrec_trace)
                fprintf(stderr, "armrec: overlay %d evicts overlay %d\n", id, i);
            ovl_resident[i] = 0;
        }
    }
    ovl_base[id] = ram_address;
    ovl_size[id] = total_size;
    ovl_resident[id] = 1;
    if (armrec_trace)
        fprintf(stderr, "armrec: overlay %d resident at 0x%08X..0x%08X\n",
                id, ram_address, ram_address + total_size);
    /* The window's contents belong to this overlay now, so write them. */
    armrec_overlay_data(id);
}

void armrec_unload_overlay(int id) {
    if (id < 0 || id >= ARMREC_MAX_OVERLAYS) {
        fprintf(stderr, "armrec: overlay id %d is out of range\n", id);
        abort();
    }
    ovl_resident[id] = 0;
}

int armrec_overlay_resident(int id) {
    if (id < 0 || id >= ARMREC_MAX_OVERLAYS) return 0;
    return ovl_resident[id];
}

/*
 * No generated code in this link, so no overlay has any data. The strong
 * definition is in armrec_init.c; see emit in tools/armrec/armrec.py.
 */
__attribute__((weak)) void armrec_overlay_data(int id) { (void)id; }

/*
 * Likewise for the decompiled functions whose names encode a guest address:
 * The strong definition is the generated build/pc/armrec_decomp_syms.c, and a
 * test that links the runtime alone has none of them to register.
 *
 */
__attribute__((weak))
void armrec_register_decompiled(void) {}
__attribute__((weak)) const int armrec_decomp_sym_count = 0;

/*
 * Resolve a code address, exactly first and then with bit 0 cleared.
 *
 * Bit 0 selects Thumb on the ARM and is not part of a guest address, so
 * clearing it is right for everything armrec itself registers. It is *wrong*
 * for a host address: armrec_extern_addr() registers decompiled C functions at
 * their real host addresses, and the host aligns functions to whatever it
 * likes, gcc put one of pc/tests/test_os_fiber.c's at 0x100012EB. Masking
 * that misses the entry and, worse, calling it jumps one byte into the
 * function. Trying the exact address first costs one extra probe and cannot
 * collide: registered odd addresses are all host ones, which live above
 * 0x10000000, and no guest region reaches that far.
 */
static struct entry *lookup_code(uint32_t addr) {
    struct entry *e = lookup(addr);
    return e ? e : lookup(addr & ~1u);
}

const char *armrec_name_of(uint32_t addr) {
    struct entry *any = NULL;
    struct entry *e = lookup_ex(addr & ~1u, &any);
    if (e) return e->name;
    /* A non-resident overlay's address still has a name, and naming it is the
     * point; this is what a diagnostic asks for. */
    return any ? any->name : "<unknown>";
}

int armrec_code_live(uint32_t addr) { return lookup_code(addr) != NULL; }

/*
 * An indirect call needs the stack arguments too, for the same reason a direct
 * one does, armrec_extern_addr() registers decompiled C functions in this
 * table at their host addresses, so e->fn is as likely to be a C callee with
 * six parameters as a recompiled body. Nothing has to be passed in to find
 * them: armrec_sp is global and still points where the guest's r13 does.
 *
 * Passing them unconditionally is right rather than merely convenient. A
 * recompiled callee declares four parameters and reads its own stack from
 * armrec_sp, so the extra words are cdecl padding it never looks at; deciding
 * per entry would mean the table knowing which half of the port it points
 * into, which is a second source of truth about something the callee's own
 * declaration already settles.
 */
#if defined(__wasm__) && defined(PC_GAME_DP)
/*
 * Diamond/Pearl on wasm32 (IRBridge): every entry is a recompiled body or a
 * c2u$ adapter, both armrec_fn, and both read arguments five and up from
 * armrec_sp themselves. A call through any other type traps on wasm.
 */
#define ARMREC_FWD(fn, a0, a1, a2, a3) \
    ((armrec_fn)(fn))((a0), (a1), (a2), (a3))
#else
#define ARMREC_FWD(fn, a0, a1, a2, a3) \
    ((armrec_extfn)(fn))((a0), (a1), (a2), (a3), ARMREC_STACK_ARGS)
#endif

uint64_t armrec_dispatch(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    struct entry *e;
#if defined(__wasm__) && defined(PC_GAME_DP)
    /* A C function pointer is a wasm table index; armrec_bridge.h. */
    if (addr < ARMREC_WASM_FNPTR_END) return armrec_bridge_call(addr, a0, a1, a2, a3);
#endif
    e = lookup_code(addr);
    if (e == NULL) {
        /*
         * Not a guest address at all, so it is a host one: a `blx rN` whose
         * register holds the address of a decompiled C function. Identity
         * mapping is what makes that directly callable, and the guard keeps
         * this from swallowing a real failure. An address inside a mapped
         * region with nothing registered there is a corrupted pointer or an
         * untranslated function, and it still aborts below.
         *
         * armrec_call_code() has always had this fallback for the host to
         * guest direction; the guest to host direction gained one when the RTC
         * needed it. RtcCommonCallback ends `blx r4` on the callback the
         * caller supplied, and every RTC caller here supplies decompiled C.
         *
         * ARMREC_FWD, unlike armrec_call_code(): this really is a call out of
         * recompiled code, so arguments five and up are on the emulated stack
         * exactly as they are for a direct BL.
         *
         * The guard is the program's own text range and not merely "outside
         * every guest region", which was the first version and was wrong. An
         * overlay id is an absolute symbol equal to a small integer, and the
         * loose guard turned "an overlay id is not a code address" from an
         * abort into a jump to address 59.
         */
        if (armrec_is_host_text(addr)) {
            if (armrec_trace)
                fprintf(stderr, "armrec: -> host code at 0x%08X\n", addr);
            return ARMREC_FWD((armrec_fn)(uintptr_t)addr, a0, a1, a2, a3);
        }
        fprintf(stderr,
                "armrec: indirect branch to guest address 0x%08X, which no "
                "resident code owns\n",
                addr);
        report_claimants(addr);
        abort();
    }
    if (armrec_trace) fprintf(stderr, "armrec: -> %s (0x%08X)\n", e->name, addr & ~1u);
    return ARMREC_FWD(e->fn, a0, a1, a2, a3);
}

/*
 * Decompiled C calling a recompiled function, with the arguments past the
 * fourth put where the callee looks for them. The whole model, and why it
 * needs no arity, is in armrec_rt.h above the declaration.
 *
 * armrec_sp is per-processor and per-fiber, so this is the live emulated stack
 * of whoever is running. Nothing here is static.
 */
/*
 * The calls that have not returned yet.
 *
 * The save and restore below is only half an answer, because a call does not
 * have to return for the run to end. The ARM7 spends most of its life parked
 * inside one, and whenever the port publishes guest state after that, those
 * sixteen words are still sitting below its emulated stack pointer. One of
 * them is a stale `char **` from main()'s frame, so it moves with ASLR.
 *
 * So the live frames are threaded on a list and put back on demand as well as
 * on return. Order is not cosmetic: undoing runs innermost first and
 * re-applying outermost first, or an overlap restores the wrong bytes.
 */
struct stkargs_live {
    struct stkargs_live *outer;
    uint32_t sp;
    uint32_t under[ARMREC_EXT_STACK_WORDS];
    uint32_t args[ARMREC_EXT_STACK_WORDS];
};

static struct stkargs_live *stkargs_top;
static int stkargs_settled;

static void stkargs_write(const struct stkargs_live *f, const uint32_t *w) {
    int i;
    for (i = 0; i < ARMREC_EXT_STACK_WORDS; i++)
        ARM_ST32(f->sp + (uint32_t)(4 * i), w[i]);
}

void armrec_stkargs_settle(void) {
    struct stkargs_live *f;

    if (stkargs_settled++ != 0) return;
    for (f = stkargs_top; f != NULL; f = f->outer) stkargs_write(f, f->under);
}

static void stkargs_reapply(struct stkargs_live *f) {
    if (f == NULL) return;
    stkargs_reapply(f->outer);
    stkargs_write(f, f->args);
}

void armrec_stkargs_resume(void) {
    if (stkargs_settled == 0 || --stkargs_settled != 0) return;
    stkargs_reapply(stkargs_top);
}

int armrec_stkargs_live_count(void) {
    struct stkargs_live *f;
    int n = 0;
    for (f = stkargs_top; f != NULL; f = f->outer) n++;
    return n;
}

uint64_t armrec_stkargs_call(armrec_extfn fn, uint32_t a0, uint32_t a1,
                             uint32_t a2, uint32_t a3, ARMREC_STK_PARAMS) {
    const uint32_t words[ARMREC_EXT_STACK_WORDS] = {
        s0, s1, s2,  s3,  s4,  s5,  s6,  s7,
        s8, s9, s10, s11, s12, s13, s14, s15
    };
    struct stkargs_live live;
    uint32_t saved = ARM_SP;
    uint64_t ret;
    int i;

    /*
     * Put the bytes back, not just the pointer, and this is not tidiness;
     * it is. Only the words the caller really pushed are
     * arguments; the rest are whatever its frame held, which on a host varies
     * with ASLR and with the environment. Leaving them in guest main RAM makes
     * the port's state a function of its host, and `--state-digest` under
     * `env -i` says so immediately: test_determinism, test_input, test_video,
     * test_view, test_sym and test_diff all failed on it in one run.
     */
    ARM_SP = saved - (uint32_t)(4 * ARMREC_EXT_STACK_WORDS);
    live.sp = ARM_SP;
    for (i = 0; i < ARMREC_EXT_STACK_WORDS; i++) {
        live.under[i] = ARM_LD32(live.sp + (uint32_t)(4 * i));
        live.args[i] = words[i];
    }
    stkargs_write(&live, live.args);
    live.outer = stkargs_top;
    stkargs_top = &live;

    ret = fn(a0, a1, a2, a3, ARMREC_STK_FORWARD);

    if (stkargs_top == &live) {
        stkargs_top = live.outer;
    } else {
        struct stkargs_live *f;
        for (f = stkargs_top; f != NULL && f->outer != &live; f = f->outer) { }
        if (f != NULL) f->outer = live.outer;
    }
    stkargs_write(&live, live.under);
    ARM_SP = saved;
    return ret;
}

void armrec_abi_trap(const char *name, const char *why) {
    fprintf(stderr,
            "armrec: refusing to call %s from recompiled code.\n"
            "  %s\n"
            "  The call site is real; the ABI is not expressible as a word\n"
            "  sequence, so armrec.py --abi-trap names it rather than letting\n"
            "  it corrupt the caller's stack.\n",
            name, why);
    abort();
}

/*
 * Is this address a host function in *this* program's text?
 *
 * The linker knows, and it is the only thing that does. __executable_start and
 * _etext are GNU ld's own bounds on the loaded text, so this asks the question
 * directly instead of guessing from "not a guest address"; which is true of
 * an overlay id, a small integer and every other piece of garbage a corrupted
 * pointer can hold. Under -no-pie they are absolute, and
 * the port and every test here link that way.
 */
#if defined(_WIN32)
/*
 * The PE headers answer at runtime what GNU ld's symbols answer at link
 * time: __ImageBase is the loader's own name for where this image begins,
 * and the optional header carries the code span. Parsed once; the image
 * does not move (pc/Makefile.win links --disable-dynamicbase, and even
 * relocated the header would still describe itself).
 */
extern char __ImageBase[];

static void pe_code_bounds(uintptr_t *lo, uintptr_t *hi) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)__ImageBase;
    IMAGE_NT_HEADERS32 *nt =
        (IMAGE_NT_HEADERS32 *)(__ImageBase + dos->e_lfanew);

    *lo = (uintptr_t)__ImageBase + nt->OptionalHeader.BaseOfCode;
    *hi = *lo + nt->OptionalHeader.SizeOfCode;
}
#endif

int armrec_is_host_text(uint32_t addr) {
#if defined(__ELF__)
    extern char __executable_start[], _etext[];
    uintptr_t lo = (uintptr_t)__executable_start;
    uintptr_t hi = (uintptr_t)_etext;
    return addr != 0 && (uintptr_t)addr >= lo && (uintptr_t)addr < hi;
#elif defined(_WIN32)
    uintptr_t lo, hi;

    pe_code_bounds(&lo, &hi);
    return addr != 0 && (uintptr_t)addr >= lo && (uintptr_t)addr < hi;
#else
    /* No portable way to ask, so nothing is host text and an indirect branch
     * to a host function aborts with the diagnostic rather than guessing. */
    (void)addr;
    return 0;
#endif
}

int armrec_is_guest_addr(uint32_t addr) {
    int i;
    for (i = 0; i < NREGIONS; i++)
        if (addr >= regions[i].base && addr < regions[i].base + regions[i].size)
            return 1;
    return 0;
}

/*
 * "Is this span guest memory?" is a different question from "is it in the
 * region table?", and conflating them was the first thing that broke.
 * armrec_region_at() reports each VRAM window's content size, because that is
 * what a digest should walk. But every address from ARM_VRAM_BASE to
 * ARM_VRAM_END is mapped and readable, so the address-space answer is the
 * whole range.
 */
/*
 * Is this whole span inside this program's loaded image: text, rodata and
 * initialised data?
 *
 * A decompiled file's `static const` lives in the port's own rodata, not in
 * guest memory, because it is host C the host compiler placed. On the console
 * the same bytes are in the overlay's rodata in main RAM, so the game is
 * entitled to hand that address to a DMA channel, and one does. pc/src/pc_dma.c
 * asks this so such a source is copied rather than treated as a corrupt
 * pointer.
 *
 * __executable_start to _edata, not to _end: `_end` in this binary is the
 * guest's symbol at 0x02C00000, the SDK's end-of-image that its heap is built
 * on. .bss is deliberately outside, so a DMA out of the port's own .bss still
 * traps, and it should, because nothing has shown one.
 */
int armrec_host_data_span_ok(uint32_t addr, uint32_t len) {
#if defined(__ELF__)
    extern char __executable_start[], _edata[];
    uintptr_t lo = (uintptr_t)__executable_start;
    uintptr_t hi = (uintptr_t)_edata;
    return addr != 0 && (uintptr_t)addr >= lo &&
           (uint64_t)addr + len <= (uint64_t)hi;
#elif defined(_WIN32)
    /*
     * Walk the section table for the end of *initialised* data, keeping the
     * ELF branch's deliberate exclusion of .bss: a DMA out of uninitialised
     * host memory should still trap with its diagnostic.
     */
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)__ImageBase;
    IMAGE_NT_HEADERS32 *nt =
        (IMAGE_NT_HEADERS32 *)(__ImageBase + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t lo = (uintptr_t)__ImageBase;
    uintptr_t hi = lo;
    int i;

    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uintptr_t end;

        if (sec[i].Characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA)
            continue;
        end = lo + sec[i].VirtualAddress + sec[i].Misc.VirtualSize;
        if (end > hi) hi = end;
    }
    return addr != 0 && (uintptr_t)addr >= lo &&
           (uint64_t)addr + len <= (uint64_t)hi;
#else
    (void)addr; (void)len;
    return 0;
#endif
}

int armrec_guest_span_ok(uint32_t addr, uint32_t len) {
    int i;
    if (addr >= ARM_VRAM_BASE &&
        (uint64_t)addr + len <= (uint64_t)ARM_VRAM_END)
        return 1;
    for (i = 0; i < NREGIONS; i++) {
        uint32_t base = regions[i].base;
        if (addr >= base && (uint64_t)addr + len <= (uint64_t)base + regions[i].size)
            return 1;
    }
    return 0;
}

uint64_t armrec_call_code(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2,
                          uint32_t a3) {
    struct entry *e;
#if defined(__wasm__) && defined(PC_GAME_DP)
    if (addr < ARMREC_WASM_FNPTR_END) return armrec_bridge_call(addr, a0, a1, a2, a3);
#endif
    e = lookup_code(addr);

    /*
     * No ARMREC_FWD here, and the asymmetry with armrec_dispatch() above is
     * deliberate. This entry point is called *from host C*, pc_os_fiber.c
     * entering a guest thread, FS_overlay.c calling a guest function pointer,
     * so the four arguments it was given are all the arguments there are,
     * and the emulated stack holds the last recompiled frame's locals rather
     * than anything for this callee. Reading it would be meaningless where it
     * is harmless and a fault where armrec_sp is not currently a real guest
     * pointer, which is exactly what test_os_fiber does on purpose. A
     * recompiled callee still finds its own stack arguments, because it reads
     * armrec_sp itself.
     */
    if (e != NULL) {
        if (armrec_trace)
            fprintf(stderr, "armrec: -> %s (0x%08X)\n", e->name, e->addr);
        return e->fn(a0, a1, a2, a3);
    }
    if (armrec_is_host_text(addr)) {
        /* Host code, called at its exact address; see lookup_code(). The
         * bound was `not a guest address`, which is also true of
         * an overlay id and of every small integer a corrupted pointer can
         * hold; armrec_dispatch() needed the same test, so both now ask the
         * linker rather than each keeping a rule. */
        if (armrec_trace)
            fprintf(stderr, "armrec: -> host code at 0x%08X\n", addr);
        return ((armrec_fn)(uintptr_t)addr)(a0, a1, a2, a3);
    }
    /*
     * Neither registered, nor host text. That is a real guest address whose
     * function was never translated, or a corrupted pointer; either way,
     * calling it would jump into data.
     */
    fprintf(stderr,
            "armrec: cannot call code address 0x%08X\n"
            "  It is %s, and no resident function is registered there.\n",
            addr, addr == 0 ? "null"
                            : armrec_is_guest_addr(addr)
                                  ? "inside a mapped guest region"
                                  : "neither a mapped guest address nor host "
                                    "text");
    report_claimants(addr);
    abort();
}

/*
 * The same resolution, handed back rather than called.
 *
 * armrec_call_code() answers "call this address with these four arguments".
 * Decompiled guest C needs something else: it calls a function pointer with
 * C's own `p(a, b)` and its own prototype, so the arguments are already in the
 * caller's frame and only the target is wrong. icall_thunk.py rewrites every
 * such call to go through armrec_icall below, which asks this for the host
 * entry point and jumps to it with the frame untouched.
 *
 * The three cases and their order are armrec_call_code()'s: the table first,
 * host text second, and anything else is a corrupted pointer or an
 * untranslated function, which aborts by name rather than jumping into data.
 * Before this existed the port jumped to 0x021D8D11 and the backtrace said
 * `?? ()`.
 */
void *armrec_resolve_code(uint32_t addr) {
    struct entry *e;
#if defined(__wasm__) && defined(PC_GAME_DP)
    /* A table index already is the native entry point. */
    if (addr < ARMREC_WASM_FNPTR_END && addr != 0) return (void *)(uintptr_t)addr;
#endif
    e = lookup_code(addr);

    if (e != NULL) {
        if (armrec_trace)
            fprintf(stderr, "armrec: icall -> %s (0x%08X)\n", e->name, e->addr);
        return (void *)e->fn;
    }
    if (armrec_is_host_text(addr)) {
        if (armrec_trace)
            fprintf(stderr, "armrec: icall -> host code at 0x%08X\n", addr);
        return (void *)(uintptr_t)addr;
    }
    fprintf(stderr,
            "armrec: cannot call code address 0x%08X through a function "
            "pointer\n"
            "  It is %s, and no resident function is registered there.\n"
            "  The call site is decompiled C.\n",
            addr, addr == 0 ? "null"
                            : armrec_is_guest_addr(addr)
                                  ? "inside a mapped guest region"
                                  : "neither a mapped guest address nor host "
                                    "text");
    report_claimants(addr);
    abort();
}

/*
 * armrec_icall and armrec_icall_tail, the two shapes icall_thunk.py emits.
 *
 * i386 cdecl is what makes this five instructions instead of a marshalling
 * layer: every argument is already on the caller's stack, the return value
 * comes back in eax and edx, and the caller cleans up. So the thunk only has
 * to put the stack back the way `call *OP` would have left it and jump to the
 * resolved address. The callee returns straight to the original call site, so
 * the thunk never appears in a backtrace.
 *
 * The rewrite pushes the target and then calls (or jumps) here:
 *
 *   call form: esp -> [ret][target][args...]
 *   tail form: esp -> [target][args...]        (`ret` is the caller's own)
 *
 * eax, ecx and edx are call-clobbered in cdecl, so the shuffle is free to use
 * them. The two `subl $8` and `addl $12` are alignment, not padding: GCC keeps
 * esp 16-byte aligned before a call, and without the adjustment any SSE spill
 * in the callee faults.
 */
#if defined(__i386__) && defined(_WIN32)
/*
 * The same two entry points in PE spelling: every C symbol gains a leading
 * underscore, and .type/.size are ELF directives the PE assembler rejects.
 * armrec_icall_common is file-local and stays bare.
 */
__asm__(
    ".text\n"
    ".globl _armrec_icall\n"
    "_armrec_icall:\n"
    "    popl  %eax\n"              /* return address */
    "    popl  %ecx\n"              /* target */
    "    pushl %eax\n"              /* frame is now exactly post-`call *OP` */
    "    movl  %ecx, %eax\n"
    "    jmp   armrec_icall_common\n"
    ".globl _armrec_icall_tail\n"
    "_armrec_icall_tail:\n"
    "    popl  %eax\n"              /* target; the caller's own `ret` is next */
    "armrec_icall_common:\n"
    "    subl  $8, %esp\n"
    "    pushl %eax\n"
    "    call  _armrec_resolve_code\n"
    "    addl  $12, %esp\n"
    "    jmp   *%eax\n");
#elif defined(__i386__)
__asm__(
    ".text\n"
    ".globl armrec_icall\n"
    ".type  armrec_icall, @function\n"
    "armrec_icall:\n"
    "    popl  %eax\n"              /* return address */
    "    popl  %ecx\n"              /* target */
    "    pushl %eax\n"              /* frame is now exactly post-`call *OP` */
    "    movl  %ecx, %eax\n"
    "    jmp   armrec_icall_common\n"
    ".size  armrec_icall, .-armrec_icall\n"
    ".globl armrec_icall_tail\n"
    ".type  armrec_icall_tail, @function\n"
    "armrec_icall_tail:\n"
    "    popl  %eax\n"              /* target; the caller's own `ret` is next */
    "armrec_icall_common:\n"
    "    subl  $8, %esp\n"
    "    pushl %eax\n"
    "    call  armrec_resolve_code\n"
    "    addl  $12, %esp\n"
    "    jmp   *%eax\n"
    ".size  armrec_icall_tail, .-armrec_icall_tail\n");
#elif defined(__arm__)
/*
 * ARM (pc/Makefile.arm, the handheld track). The thunks exist for code
 * icall_thunk.py rewrote, and this port has none: Platinum is decompiled C
 * end to end and never runs armrec.py, so nothing in the image calls either
 * entry point. But armrec_rt.c is one file for every host, the __asm__ above
 * is unconditional in the translation unit, and an #error here would stop an
 * armhf build over dead code.
 *
 * So they are defined and they abort. A trap rather than a stub, because the
 * only way to reach one is for a tree that DOES recompile ARM to be built for
 * an ARM host, and that needs a real thunk written against AAPCS rather than
 * a silent return to the wrong place.
 */
void armrec_icall(void)
{
    fprintf(stderr, "armrec: armrec_icall reached on ARM; the thunks are "
                    "i386 cdecl and this build has none\n");
    abort();
}

void armrec_icall_tail(void)
{
    fprintf(stderr, "armrec: armrec_icall_tail reached on ARM, the thunks "
                    "are i386 cdecl and this build has none\n");
    abort();
}
#elif defined(__wasm__)
/*
 * wasm32, for the ARM branch's reason: nothing in Platinum calls either entry
 * point, and a frame-preserving tail jump to a resolved address has no wasm
 * spelling at all (a call target is a table index with a fixed signature).
 * Defined so the one file still links, and trapping so a tree that does
 * recompile ARM finds out at the first call rather than returning wrongly.
 */
void armrec_icall(void)
{
    pc_wasm_fatal("armrec: armrec_icall reached on wasm32; the thunks are "
                  "i386 cdecl and this build has none");
}

void armrec_icall_tail(void)
{
    pc_wasm_fatal("armrec: armrec_icall_tail reached on wasm32; the thunks "
                  "are i386 cdecl and this build has none");
}
#else
#error "armrec_icall is i386 cdecl; the port is -m32"
#endif

void armrec_load_data(uint32_t addr, const void *src, uint32_t len) {
    if (!armrec_mem_ready) {
        fprintf(stderr, "armrec: armrec_load_data before armrec_mem_init()\n");
        abort();
    }
    /*
     * This used to be a bare memcpy, so a blob destined for an address no
     * region covers wrote through a wild pointer and the process died with no
     * clue as to which. It was not hypothetical: the ARM7's own IWRAM
     * (0x037F8000-0x03810000) was not in the table above and the recompiled
     * ARM7 files hold hundreds of blobs there, so armrec_init_all() could not
     * run to completion. The ARM7 work mapped that region; it is
     * ARM_ARM7_WRAM_BASE above now, so this trap is no longer standing in
     * for an open question; it is the general guard, and the class it still
     * catches is the ARM7's `Autoload EXT` at 0x06000000, which is VRAM the
     * ARM9 cannot be given. See ARM7_DROP in pc/Makefile.
     */
#ifdef ARMREC_TWL
    /*
     * TWL-SDK's crt0 autoloads every entry of the NTR list, and one of them
     * is a 32-byte NTR-mode stand-in at 0x02400000 (the start of the TWL's
     * extended main RAM). On a DS that address is the main-RAM mirror, so
     * the loader's write lands at 0x02000000, which is where it goes here.
     * The shared region (DTCM, shared page) above the mirror is storage of
     * its own (ARM_SHARED_BASE).
     */
    if (addr >= ARM_MAIN_RAM_BASE + ARM_MAIN_RAM_SIZE && addr < ARM_SHARED_BASE)
        addr = ARM_MAIN_RAM_BASE + ((addr - ARM_MAIN_RAM_BASE) & (ARM_MAIN_RAM_SIZE - 1));
#endif
    if (armrec_guest_span_ok(addr, len)) {
        memcpy((void *)(uintptr_t)addr, src, len);
        return;
    }
    fprintf(stderr,
            "armrec: no guest region holds 0x%08X..0x%08X, so this data has "
            "nowhere to go.\n"
            "  Either the address is wrong or the region is missing from "
            "regions[] in armrec_rt.c.\n",
            addr, (uint32_t)(addr + len));
    abort();
}

/*
 * Address of a symbol that lives in hand-decompiled C rather than in
 * recompiled assembly.
 *
 * This has to be the symbol's *real* address, not a synthetic token, because
 * the name may refer to data rather than a function: recompiled code that
 * loads a decompiled global has to end up dereferencing the actual object.
 * Identity mapping makes that work, but only if the address fits in the 32
 * bits an ARM register holds, which is why the PC binary is linked no-pie
 * with its text segment placed low (see pc/Makefile).
 *
 * Registering it in the dispatch table as well means an indirect call through
 * a stored function pointer resolves too.
 */
uint32_t armrec_extern_addr(const char *name, void *sym) {
    uintptr_t a = (uintptr_t)sym;
#if UINTPTR_MAX > 0xFFFFFFFFu
    /*
     * Only reachable on an LP64 host. The port is built -m32 (pc/Makefile),
     * where every host pointer already fits in 32 bits and this check is
     * tautological, kept behind the #if so it stays correct rather than
     * becoming dead code that quietly compiles to nothing.
     */
    if (a > 0xFFFFFFFFu) {
        fprintf(stderr,
                "armrec: symbol %s is at %p, which does not fit in a 32-bit "
                "guest pointer.\n"
                "  Link the port with -no-pie and -Wl,-Ttext-segment=0x10000000.\n",
                name, sym);
        abort();
    }
#endif
    /*
     * Register it as callable only if it *could* be code. Nothing is mapped
     * below the first guest region on either side of the port, ITCM at
     * 0x01FF8000 is the lowest thing armrec_mem_init() places, and host text
     * starts at 0x10000000, so an address below that is a symbol whose value
     * is a small integer rather than a location.
     *
     * That is not hypothetical, and it is the overlay work's second half: the SDK's
     * FS_OVERLAY_ID(name) is the *address* of SDK_OVERLAY_OVERLAY_nn_ID, and
     * pc/src/pc_platform.c now defines each of those as the absolute value nn,
     * because FS_LoadOverlay indexes the ROM's overlay table with it. Binding
     * them here would enter 83 callable entries at addresses 4 through 86.
     * Overlay 0 would be address 0, which this table reads as an empty slot,
     * harmless by accident, which is the kind of thing worth removing rather
     * than relying on.
     */
#if defined(__wasm__) && defined(PC_GAME_DP)
    /*
     * Diamond/Pearl on wasm32: a function is its table index, which the
     * dispatcher resolves through the bridge's c2u$ adapter table, and data
     * is its linear-memory address. Neither goes in the guest table: an
     * index is not a guest address, and a C object above NP_GUEST_C_BASE is
     * not code.
     */
    (void)name;
#else
    if (a >= ARM_ITCM_BASE)
        armrec_register((uint32_t)a, (armrec_fn)sym, name);
#endif
    return (uint32_t)a;
}

void armrec_trap(const char *fn, const char *what) {
    fprintf(stderr, "armrec: trap in %s: %s\n", fn, what);
    abort();
}

/* ------------------------------------------------------------------ */
/* Weak platform hooks                                                */
/* ------------------------------------------------------------------ */

/*
 * These are the seams where recompiled code leaves the CPU and touches the
 * machine. The PC platform layer overrides them; these defaults keep the
 * recompiler testable on its own.
 */

__attribute__((weak)) uint32_t armrec_swi(uint32_t num, uint32_t r0, uint32_t r1,
                                          uint32_t r2, uint32_t r3) {
    (void)r0; (void)r1; (void)r2; (void)r3;
    fprintf(stderr, "armrec: unhandled SWI 0x%02X\n", num);
    abort();
}

__attribute__((weak)) uint32_t armrec_mrc(uint32_t cp, uint32_t op1, uint32_t crn,
                                          uint32_t crm, uint32_t op2) {
    (void)cp; (void)op1; (void)crn; (void)crm; (void)op2;
    return 0;
}

__attribute__((weak)) void armrec_mcr(uint32_t cp, uint32_t op1, uint32_t crn,
                                      uint32_t crm, uint32_t op2, uint32_t v) {
    (void)cp; (void)op1; (void)crn; (void)crm; (void)op2; (void)v;
}

static uint32_t cpsr = 0x0000001F; /* System mode, interrupts enabled */

__attribute__((weak)) uint32_t armrec_mrs(int spsr) {
    (void)spsr;
    return cpsr;
}

__attribute__((weak)) void armrec_msr(int spsr, uint32_t mask, uint32_t v) {
    if (spsr) return;
    cpsr = (cpsr & ~mask) | (v & mask);
}
