/*
 * 3ds/src/3ds_ioreg.c: the first game translation unit, and it exists to be
 * proof rather than reasoning.
 *
 * The claim the ioreg shadows make is that a `reg_*` access from decompiled C
 * or from an SDK library reaches the slab's I/O row. That claim cannot be
 * checked by reading the shadow header: it looks right whether or not it is
 * the file the compiler opened, whether or not `armrec_io_base` is the pointer
 * the translator uses, and whether or not `HW_REG_BASE` survived some later
 * redefinition. So this file takes the address of real registers through the
 * SDK's own macros and compares each one against `armrec_host_ptr()` of the
 * guest address the DS puts it at.
 *
 * It is compiled with GAME_CFLAGS. It sees <nitro/...> and must never see
 * <3ds.h>; 3ds/src/3ds_ioreg.h is deliberately empty of includes so 3ds_main.c
 * can still call in.
 *
 * It runs only on the console. Every other model in this port is pure C that
 * the host links, but this one is a statement about a compile line, and the
 * host has a different one. So it reports the index of the first check that
 * failed as well as the count: on a screen with no debugger, "check 27" is the
 * difference between a bug and a mystery.
 *
 * What is not here: the GX command ports and the divider are not memory, and
 * the PC port's shadows route them through armrec_gx_port() and
 * armrec_cp_ptr(). Taking the address of one of those registers would be a
 * call to a function that does not exist yet, so what is checked instead is
 * the arithmetic those hooks will use to recover a guest address. The CP
 * operand registers are not hooked, because they are storage on hardware too.
 */

#include <nitro/hw/ARM9/ioreg.h>

#include "3ds_guest.h"
#include "3ds_ioreg.h"

/*
 * One row per register: the SDK's own lvalue macro, and the guest address a DS
 * answers it at. The addresses are written out rather than computed from
 * `REG_x_OFFSET`, because computing them from the same macro the shadow moved
 * would make the check agree with itself, and because a literal that is
 * wrong fails loudly, which one of these did.
 *
 * The set spans nine of the twelve `ioreg_*.h` this port can take the address
 * of, both display engines (the second is at +0x1000), and one register that
 * is `const` on hardware.
 */
#define IOREG_PLAIN(X)                                                        \
    X(reg_GX_DISPCNT,        0x04000000u) /* GX   */                          \
    X(reg_GX_VCOUNT,         0x04000006u)                                     \
    X(reg_G2_BG0CNT,         0x04000008u) /* G2   */                          \
    X(reg_G2_BG3CNT,         0x0400000Eu)                                     \
    X(reg_OS_TM0CNT_L,       0x04000100u) /* OS   */                          \
    X(reg_OS_IME,            0x04000208u)                                     \
    X(reg_PAD_KEYINPUT,      0x04000130u) /* PAD  */                          \
    X(reg_PAD_KEYCNT,        0x04000132u)                                     \
    X(reg_EXI_SIOCNT,        0x04000128u) /* EXI  */                          \
    X(reg_PXI_SUBPINTF,      0x04000180u) /* PXI  */                          \
    X(reg_MI_DMA0SAD,        0x040000B0u) /* MI   */                          \
    X(reg_MI_DMA3CNT,        0x040000DCu)                                     \
    X(reg_CP_DIV_NUMER_L,    0x04000290u) /* CP, operand side */              \
    X(reg_G3X_DISP3DCNT,     0x04000060u) /* G3X, not a FIFO port */          \
    X(reg_GXS_DB_DISPCNT,    0x04001000u) /* GXS, engine B */                 \
    X(reg_G2S_DB_BG0CNT,     0x04001008u) /* G2S, engine B */

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

int ioreg_first_failure(void)
{
    return (sFirstFail == 0) ? -1 : sFirstFail;
}

int ioreg_selftest(int *ran)
{
    volatile unsigned char *io;

    sChecks = 0;
    sFailed = 0;
    sFirstFail = 0;

    /*
     * The shadow's pointer and the translator's answer for the same guest
     * address have to be the same object. If this fails nothing below means
     * anything, because both sides of every other comparison come from one of
     * these two.
     */
    io = (volatile unsigned char *)armrec_host_ptr(0x04000000u);
    expect(io != 0);
    if (io == 0) {
        if (ran != 0) {
            *ran = sChecks;
        }
        return sFailed;
    }

    expect((u32)io == HW_REG_BASE);

    /*
     * Every register in the table, twice over: the address the SDK macro
     * dereferences is the address the translator hands out, and the offset
     * from the row's base is the offset from 0x04000000. The second is not
     * implied by the first; it fails if the row itself is placed wrongly.
     */
#define X(reg, guest)                                                         \
    expect((const volatile void *)&(reg)                                      \
           == (const volatile void *)armrec_host_ptr(guest));                 \
    expect((u32)(const volatile unsigned char *)&(reg) - (u32)io              \
           == (guest) - 0x04000000u);
    IOREG_PLAIN(X)
#undef X

    /*
     * An address is not a store. Write through the macro the game writes
     * through, read the slab underneath; then write the slab and read the
     * register back. Engine B as well as engine A, because +0x1000 is far
     * enough into the row to catch a row that is too short.
     */
    reg_G2_BG0CNT = 0x1234u;
    expect(*(volatile u16 *)(io + 0x008u) == 0x1234u);

    reg_G2S_DB_BG0CNT = 0x5678u;
    expect(*(volatile u16 *)(io + 0x1008u) == 0x5678u);

    *(volatile u32 *)(io + 0x0B0u) = 0xDEADBEEFu;
    expect(reg_MI_DMA0SAD == 0xDEADBEEFu);

    /*
     * A register whose macro is `const` still has to read what the hardware
     * model wrote. The keypad is the one the reset values already put a number
     * in (0x03FF), so this is the same address from the other side.
     */
    *(volatile u16 *)(io + 0x130u) = 0x03FFu;
    expect(reg_PAD_KEYINPUT == 0x03FFu);

    /*
     * The hooks that classify by guest address get a host pointer now, and
     * this is the arithmetic that recovers what they expect. Checked for a GX
     * command port and for the divider control, which are the two families
     * pc/include shadows.
     */
    expect((u32)REG_MTX_MODE_ADDR - HW_REG_BASE + HW_REG_BASE_GUEST
           == 0x04000440u);
    expect((u32)REG_DIVCNT_ADDR - HW_REG_BASE + HW_REG_BASE_GUEST
           == 0x04000280u);

    /*
     * The row is 1 MB and the map has nothing above it until the palette, so
     * the CARD data FIFO at 0x04100010, which `card_rom.h` still spells
     * `HW_REG_BASE + 0x100010`, is outside it. NULL rather than a pointer:
     * The PC port does not reach that register either (pc_card_rom.c replaces
     * the reader), and if something ever does it should be a crash with an
     * address on it rather than a write past the row.
     */
    expect(armrec_host_ptr(0x04100010u) == 0);

    /* Leave the row the way io_selftest and the reset values expect it. */
    reg_G2_BG0CNT = 0;
    reg_G2S_DB_BG0CNT = 0;
    *(volatile u32 *)(io + 0x0B0u) = 0;
    *(volatile u16 *)(io + 0x130u) = 0;

    if (ran != 0) {
        *ran = sChecks;
    }
    return sFailed;
}
