/*
 * 3ds/src/3ds_ctrdg.c: the cartridge slot that is not there.
 *
 * A game translation unit, like 3ds_ioreg.c and 3ds_hwmem.c: the claim is
 * about what the SDK's own macros expand to inside code compiled with
 * GAME_CFLAGS, and the only way to check that is to compile some and ask.
 *
 * Why this row gets its own file. Every other wrong address in this port
 * faults. This one does not: 0x08000000 is the GBA cartridge on a DS and the
 * application heap window on an Old 3DS, so a read there succeeds and returns
 * whatever this process put in its own heap. CTRDG_IsExisting() reads two
 * words through that address and decides from them, and the migrator and the
 * distribution check both hang off that answer. A wrong "yes" is a menu
 * offering to migrate a cartridge made of this port's own memory.
 *
 * What actually reads the slot, measured rather than assumed:
 *
 *   ctrdg_proc.c  the boot probe, inside CTRDGi_InitModuleInfo(). It never
 *                 runs: the function returns early unless `reg_OS_PAUSE & 1`,
 *                 and nothing ever writes POSTFLG, so it reads 0 here and on
 *                 PC. The BIOS sets it on hardware.
 *   ctrdg.c       CTRDG_IsExisting(), the isRomCode byte at HW_CTRDG_ROM +
 *                 0xB2 and, because zeros are not 0x96, the module-ID image
 *                 word at HW_CTRDG_ROM + 0x1FFFE. These do run, from
 *                 CTRDG_IsAgbCartridge() in the main menu.
 *   the copies    every one is behind CTRDG_IsAgbCartridge(), which is false
 *                 over zeros. The two above the buffer never run, and if they
 *                 ever did they would translate to NULL rather than to
 *                 somebody's heap.
 *
 * So the model is 128 KB of zeros at the row, HW_CTRDG_ROM pointing at it, and
 * the same "nothing inserted" answer the PC port gives from its 33 MB.
 */

#include <nitro/ctrdg.h>
#include <nitro/hw/ARM9/mmap_global.h>

#include "3ds_guest.h"
#include "3ds_guest_map.h"
#include "3ds_ctrdg.h"

/* The shadow spells the backing itself because it is a header on the game
 * chain; this is where the two spellings meet. */
_Static_assert(HWi_CTRDG_BACKING == (unsigned)GUEST_BACK_AGB,
               "the cartridge window and the slab row must be the same size");

/* 4 + 0x9c + 12 + 4 + 2 + 4 + 3 + 4 + 1 + 1 + 2. The probe copies everything
 * above 0x80, so the header's size is the probe's length. */
_Static_assert(sizeof(CTRDGHeader) == 0xC0, "the AGB header is 0xC0 bytes");

/* Where CTRDG_IsExisting() looks when the header does not say 0x96. It is
 * past 64 KB, which is why the row is 128 KB. */
#define CTRDG_MODULE_ID_IMAGE_OFF 0x0001FFFEu

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

int ctrdg_first_failure(void)
{
    return (sFirstFail == 0) ? -1 : sFirstFail;
}

int ctrdg_selftest(int *ran)
{
    const CTRDGHeader *chp = CTRDGi_GetHeaderAddr();
    const u16 *idImage = CTRDGi_GetModuleIDImageAddr();
    const u8 *bytes = (const u8 *)chp;
    u32 i;
    int zeroed = 1;

    sChecks = 0;
    sFailed = 0;
    sFirstFail = 0;

    /* The two the SDK dereferences, against the translator. Both directions:
     * The macro has to give the row's host pointer, and that pointer has to
     * answer to the DS address the macro was written for. */
    expect((const void *)chp == armrec_host_ptr((u32)HWi_G_CTRDG_ROM));
    expect(armrec_guest_addr(chp) == (u32)HWi_G_CTRDG_ROM);
    expect((const void *)idImage
           == armrec_host_ptr((u32)HWi_G_CTRDG_ROM + CTRDG_MODULE_ID_IMAGE_OFF));
    expect(armrec_guest_addr(idImage)
           == (u32)HWi_G_CTRDG_ROM + CTRDG_MODULE_ID_IMAGE_OFF);

    /* The boot probe's DMA source and its last byte, for the day POSTFLG is
     * set and it starts running. */
    expect(armrec_host_ptr((u32)HWi_G_CTRDG_ROM + 0x80) != NULL);
    expect(armrec_host_ptr((u32)HWi_G_CTRDG_ROM + sizeof(CTRDGHeader) - 1)
           != NULL);

    if ((const void *)chp == NULL || (const void *)idImage == NULL) {
        if (ran != NULL) {
            *ran = sChecks;
        }
        return sFailed;
    }

    /* An empty slot reads as zeros, and the port's answer has to be the same
     * everywhere the SDK looks. */
    for (i = 0; i < sizeof(CTRDGHeader); i++) {
        if (bytes[i] != 0) {
            zeroed = 0;
        }
    }
    expect(zeroed);
    expect(*idImage == 0);

    /*
     * And the verdict those zeros produce, which is the thing that matters.
     * CTRDG_IsExisting() compares the header against the module info in the
     * shared work area; CTRDG_IsAgbCartridge() is that and `isAgbCartridge`.
     * The SDK is not linked yet, so the compare is written out here over the
     * same two objects the SDK reads, and this is what says the answer is
     * "no cartridge" rather than "whatever the heap held".
     */
    {
        const CTRDGModuleInfo *cip = CTRDGi_GetModuleInfoAddr();

        expect(armrec_guest_addr(cip) == (u32)HWi_G_CTRDG_MODULE_INFO_BUF);
        expect(chp->isRomCode != CTRDG_IS_ROM_CODE);
        expect(cip->moduleID.raw == *idImage);   /* no pull-out detected */
        expect(cip->isAgbCartridge == 0);        /* ... and no AGB cartridge */
        expect(cip->gameCode == 0);
    }

    /*
     * The window ends where the memory does. CTRDG_CpuCopy8() decides whether
     * its destination is cartridge RAM with `HW_CTRDG_ROM <= dest &&
     * dest < HW_CTRDG_RAM_END`, and on this console that test is applied to
     * host pointers, so if it still spanned the DS's 33 MB it would claim
     * the heap, which is where the slab itself lives.
     */
    {
        u32 rom = HW_CTRDG_ROM;
        u32 mainRow = (u32)armrec_host_ptr(0x02000000u);
        u32 inside = (u32)(rom + HWi_CTRDG_BACKING - 1);

        expect(HW_CTRDG_ROM_END - rom == HWi_CTRDG_BACKING);
        expect(HW_CTRDG_RAM_END == HW_CTRDG_RAM); /* no cartridge SRAM */
        expect(rom <= inside && inside < HW_CTRDG_RAM_END);
        expect(!(rom <= mainRow && mainRow < HW_CTRDG_RAM_END));
    }

    /* Above the buffer nothing translates, including the two addresses the
     * distribution cartridge reads, which is where a probe that went further
     * than this port has ever seen would show up. */
    expect(armrec_host_ptr((u32)HWi_G_CTRDG_ROM + HWi_CTRDG_BACKING) == NULL);
    expect(armrec_host_ptr(0x08020000u) == NULL);
    expect(armrec_host_ptr(0x08100000u) == NULL);
    expect(armrec_host_ptr((u32)HWi_G_CTRDG_RAM) == NULL); /* the DS's SRAM */

    if (ran != NULL) {
        *ran = sChecks;
    }
    return sFailed;
}
