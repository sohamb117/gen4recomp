/*
 * 3ds/src/3ds_hwmem.c: does the DS memory map land on the slab?
 *
 * A game translation unit, like 3ds_ioreg.c and for the same reason: the claim
 * The shadows make a claim about what `HW_ROM_HEADER_BUF` expands to inside code compiled
 * with GAME_CFLAGS, and the only way to check that is to compile some and ask.
 *
 * The check is the same for almost all of them. Each constant was captured from the
 * SDK with its DS value still intact (`HWi_G_<name>`) and then redefined as a
 * host pointer, so the question for every one of them is whether the
 * translator agrees: `armrec_guest_addr(HW_x)` has to give back `HWi_G_x`.
 * That single comparison catches a wrong row base, a missing `#undef`, a
 * constant redefined against the wrong row, and a slab whose rows moved,
 * and it needs no address written twice, because the guest side comes from
 * the SDK header and the host side from the port's own translator.
 *
 * The set is walked with `HWi_MOVED_EACH`, which
 * `3ds/tests/mmap_pin.py --emit` generates from the same parse of the SDK that
 * generates the redefinitions, so this file cannot check a subset by accident.
 */

#include <nitro/hw/ARM9/mmap_global.h>

#include "3ds_guest.h"
#include "3ds_hwmem.h"

static int sChecks;
static int sFailed;
static int sFirstFail;

static void expect(int held)
{
    sChecks++;
    if (!held) {
        if (sFirstFail == 0) {
            sFirstFail = sChecks;
        }
        sFailed++;
    }
}

int hwmem_first_failure(void)
{
    return (sFirstFail == 0) ? -1 : sFirstFail;
}

int hwmem_selftest(int *ran)
{
    volatile unsigned char *main_row;
    volatile unsigned char *shared_row;

    sChecks = 0;
    sFailed = 0;
    sFirstFail = 0;

    main_row = (volatile unsigned char *)armrec_host_ptr(HWi_G_MAIN_MEM);
    shared_row = (volatile unsigned char *)armrec_host_ptr(0x027E0000u);
    expect(main_row != 0);
    expect(shared_row != 0);
    if (main_row == 0 || shared_row == 0) {
        if (ran != 0) {
            *ran = sChecks;
        }
        return sFailed;
    }

    /* Main RAM, whose row base every arena allocation is an offset into. */
    expect((u32)main_row == HW_MAIN_MEM);
    expect(armrec_guest_addr((const void *)HW_MAIN_MEM) == (u32)HWi_G_MAIN_MEM);

    /*
     * And every constant that moved, one at a time. This is the check that
     * says the 4 MB hole between the two rows; which the slab does not carry:
     * has been accounted for: every one of these is written by the SDK as
     * an offset from main RAM, and every one of them has to come out in the
     * other row.
     *
     * The palette and OAM rows go through the same loop; they are flat 4 KB
     * and both engines live in one of each.
     *
     * Some of them are not addresses. `HW_MAIN_MEM_EX_END` is 0x02800000, the
     * byte after the last byte of its row, and no translator can hand back a
     * guest address for a pointer one past the end; that address is in no
     * region and the host pointer is in no row. A bound is checked at its last
     * byte instead, which is the only thing about it that has to be true. The
     * row end travels with each name so the loop can tell the two apart.
     */
#define X(name, short, rowend)                                                \
    expect((u32)HWi_G_##short < (rowend)                                      \
               ? armrec_guest_addr((const void *)(name)) == (u32)HWi_G_##short \
               : armrec_guest_addr((const void *)((name) - 1))                \
                     == (u32)HWi_G_##short - 1u);
    HWi_MOVED_EACH(X)
#undef X

    /*
     * A constant is not a store. The arena info block is the one the SDK
     * dereferences as a struct on every OS_GetArenaLo, so it is the one worth
     * writing through: through the macro, out through the row.
     */
    *(volatile u32 *)HW_ARENA_INFO_BUF = 0xA5A5F00Du;
    expect(*(volatile u32 *)(shared_row + ((u32)HWi_G_ARENA_INFO_BUF - 0x027E0000u))
           == 0xA5A5F00Du);
    *(volatile u32 *)HW_ARENA_INFO_BUF = 0;

    /*
     * The header buffer filled from RomFS, from the other side: write
     * the row, read the constant.
     */
    *(volatile u32 *)(shared_row + ((u32)HWi_G_ROM_HEADER_BUF - 0x027E0000u)) =
        0x4E494E54u;
    expect(*(volatile u32 *)HW_ROM_HEADER_BUF == 0x4E494E54u);
    *(volatile u32 *)HW_ROM_HEADER_BUF = 0;

    /*
     * And the one that must not have moved. A VRAM window base is still the
     * address the DS gives it, because nine banks in five windows cannot be a
     * base plus an offset: the same 16 KB is at two addresses in two frames.
     * These four say so directly, if a later run ever "finishes the job" by
     * moving them, this fails on the first frame rather than at the first
     * garbled tile.
     */
    expect((u32)HW_BG_VRAM == 0x06000000u);
    expect((u32)HW_DB_BG_VRAM == 0x06200000u);
    expect((u32)HW_OBJ_VRAM == 0x06400000u);
    expect((u32)HW_LCDC_VRAM == 0x06800000u);

    /*
     * And that the translator, not arithmetic, is what answers for them. Every
     * VRAMCNT register is zero here, io_selftest and ioreg_selftest both put
     * the I/O row back to zeros, and this runs before vram_selftest touches
     * them, so no bank is mapped in the main BG window and the answer is
     * NULL, which is what a console gives too: reads there are zeros.
     */
    expect(armrec_host_ptr((u32)HW_BG_VRAM) == 0);

    /*
     * The two rows are 4 MB apart on a DS and not on this console. If they ever
     * happen to be 8 MB apart in host memory, every check above would pass
     * whether or not the shared constants were moved, so say out loud that
     * they are not.
     */
    expect((u32)shared_row - (u32)main_row != 0x007E0000u);

    /* The palette and OAM rows, from the other side: through the constant,
     * out through the row, for both engines' halves of each. */
    *(volatile u16 *)HW_BG_PLTT = 0x7C1Fu;
    expect(*(volatile u16 *)armrec_host_ptr(0x05000000u) == 0x7C1Fu);
    *(volatile u16 *)HW_DB_BG_PLTT = 0x03E0u;
    expect(*(volatile u16 *)armrec_host_ptr(0x05000400u) == 0x03E0u);
    *(volatile u16 *)HW_BG_PLTT = 0;
    *(volatile u16 *)HW_DB_BG_PLTT = 0;

    *(volatile u16 *)HW_OAM = 0x1234u;
    expect(*(volatile u16 *)armrec_host_ptr(0x07000000u) == 0x1234u);
    *(volatile u16 *)HW_DB_OAM = 0x5678u;
    expect(*(volatile u16 *)armrec_host_ptr(0x07000400u) == 0x5678u);
    *(volatile u16 *)HW_OAM = 0;
    *(volatile u16 *)HW_DB_OAM = 0;

    if (ran != 0) {
        *ran = sChecks;
    }
    return sFailed;
}
