/*
 * 3ds/src/3ds_rom_hdr.c: the cartridge header where the firmware leaves it.
 *
 * A game translation unit, like 3ds_ctrdg.c: HW_ROM_HEADER_BUF is an SDK
 * macro and the shadows moved it onto the slab, so the only honest way to ask where
 * it points is to compile something with GAME_CFLAGS and look.
 *
 * What depends on this. On hardware the firmware copies the cartridge header
 * to 0x027FFE00 before the game's first instruction. boot.c's
 * CheckForMemoryTampering() then copies HW_CARD_ROM_HEADER_SIZE bytes out of
 * it into the shared work area and the backup slot, wires the "rom" archive's
 * FAT and FNT offsets from the copy, and calls OS_Terminate() if the maker
 * code is not "01". There is no firmware here, so pc/src/pc_card_rom.c's
 * pc_rom_init() does the copy, and this file is the check that it lands
 * somewhere the game can read, which cannot wait for the game to link.
 *
 * Why the copy is 0x160 and not a round 0x200. The buffer ends at 0x027FFF60
 * and the 160 bytes above it are the PXI signal words, the two thread-info
 * pointers, HW_BUTTON_XY_BUF, the touch-panel buffer and every lock word in
 * the system. Nothing has written them yet when the header is placed, which
 * is exactly why an overrun there would have been invisible until something
 * re-opened the cartridge. The last two checks below are that overrun.
 */

#include <nitro.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "3ds_guest.h"
#include "3ds_rom.h"
#include "3ds_rom_hdr.h"

/* The DS values, spelled here because the shadow has moved the macros off
 * them: this file's whole job is to say the two still line up. */
#define ROM_HDR_GUEST 0x027FFE00u
#define ROM_HDR_GUEST_END 0x027FFF60u

/* What a byte above the buffer holds while the check runs. Neither 0 nor
 * 0xFF: both are values a real overrun could have written. */
#define GUARD 0x5Au

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

int rom_hdr_first_failure(void)
{
    return (sFirstFail == 0) ? -1 : sFirstFail;
}

int rom_hdr_selftest(int *ran)
{
    u8 *const buf = (u8 *)HW_ROM_HEADER_BUF;
    u8 *const tail = (u8 *)HW_ROM_HEADER_BUF_END;
    const CARDRomHeader *hdr = (const CARDRomHeader *)HW_ROM_HEADER_BUF;
    u8 file[HW_CARD_ROM_HEADER_SIZE];
    /* Everything from the end of the buffer to the end of the shared work
     * area. HW_CMD_AREA is the last word in it, so its end is the bound,
     * and taking it from the SDK's own macros rather than from 0x02800000
     * means the shadow is what is being measured. */
    u32 tailBytes = (u32)(((u8 *)HW_CMD_AREA + 2) - tail);
    u32 i;
    int fd;
    int guarded = 1;

    sChecks = 0;
    sFailed = 0;
    sFirstFail = 0;

    /* The buffer is where the game will look for it. Both directions, the way
     * every other moved constant in this port is checked. */
    expect((void *)buf == armrec_host_ptr(ROM_HDR_GUEST));
    expect(armrec_guest_addr(buf) == ROM_HDR_GUEST);
    expect((void *)tail == armrec_host_ptr(ROM_HDR_GUEST_END));
    expect(HW_ROM_HEADER_BUF_END - HW_ROM_HEADER_BUF == HW_CARD_ROM_HEADER_SIZE);
    expect(tailBytes == 0xA0);
    expect(rom_fs_path() != NULL);

    fd = (rom_fs_path() != NULL) ? open(rom_fs_path(), O_RDONLY) : -1;
    expect(fd >= 0);
    if (fd < 0) {
        if (ran != NULL) {
            *ran = sChecks;
        }
        return sFailed;
    }
    expect(read(fd, file, sizeof file) == (int)sizeof file);
    close(fd);

    /* The 160 bytes an overrun would reach, filled with a value no overrun
     * could leave behind. Saved first: this is the live shared work area, and
     * the console has to come out of the check the way it went in. */
    {
        static u8 saved[0xA0];

        memcpy(saved, tail, tailBytes);
        memset(tail, GUARD, tailBytes);

        memcpy(buf, file, HW_CARD_ROM_HEADER_SIZE);

        /* What the firmware would have left, read back through the SDK's own
         * struct rather than as bytes: this is the view boot.c takes. */
        expect(hdr->game_code == (u32)(('C') | ('P' << 8) | ('U' << 16) | ((u32)'E' << 24)));
        expect(hdr->maker_code == (u16)(('0') | ('1' << 8)));
        expect(memcmp(hdr->game_name, "POKEMON PL", 10) == 0);
        expect(hdr->header_crc == 0x5153u);

        /* The four fields boot.c hands to the "rom" archive. The boot archive is where
         * FS_FindArchive gets them; this is where they arrive. */
        expect(hdr->fat.offset == 0x00432C00u);
        expect(hdr->fat.length == 0x00000E70u);
        expect(hdr->fnt.offset == 0x00431000u);
        expect(hdr->fnt.length == 0x00001BB4u);

        /* 6.5. The SDK's own accessors over the placed header, because those
         * are what FSi_InitRom() reads: it hands CARD_GetRomRegionFNT() and
         * CARD_GetRomRegionFAT() straight to FS_LoadArchive with base 0, and
         * fs_overlay.c takes the two OVT regions the same way. They are
         * header inlines over HW_ROM_HEADER_BUF + 0x40/0x48/0x50/0x58, so
         * they are only right if the shadow is; which is the claim. */
        expect((const void *)CARD_GetRomRegionFNT() == (const void *)(buf + 0x40));
        expect((const void *)CARD_GetRomRegionFAT() == (const void *)(buf + 0x48));
        expect(CARD_GetRomRegionFNT()->offset == 0x00431000u);
        expect(CARD_GetRomRegionFAT()->offset == 0x00432C00u);
        expect(CARD_GetRomRegionOVT(MI_PROCESSOR_ARM9)->offset == 0x00106600u);
        expect(CARD_GetRomRegionOVT(MI_PROCESSOR_ARM9)->length == 0x00000F40u);
        /* This ROM has no ARM7 overlays, and FSi_InitRom's warning path is
         * the one that fires on a zero FNT or FAT, not on a zero OVT. */
        expect(CARD_GetRomRegionOVT(MI_PROCESSOR_ARM7)->offset == 0);
        expect(CARD_GetRomRegionFNT()->offset != 0xFFFFFFFFu
               && CARD_GetRomRegionFAT()->offset != 0xFFFFFFFFu);

        /* The last byte of the buffer came from the file, and the first byte
         * above it did not. */
        expect(buf[HW_CARD_ROM_HEADER_SIZE - 1] == file[HW_CARD_ROM_HEADER_SIZE - 1]);
        expect(tail[0] == GUARD);

        /* And so did none of the rest, including HW_BUTTON_XY_BUF and the
         * lock words, which are the two an overrun would have cost most. */
        for (i = 0; i < tailBytes; i++) {
            if (tail[i] != GUARD) {
                guarded = 0;
            }
        }
        expect(guarded);
        expect(*(const u8 *)HW_BUTTON_XY_BUF == GUARD);
        expect(*(const u8 *)HW_INIT_LOCK_BUF == GUARD);

        memcpy(tail, saved, tailBytes);
    }

    /* Left as found: the header buffer is zeroed again, because a reset
     * console has no cartridge header in it and pc_rom_init() is what puts
     * one there when the game runs. */
    memset(buf, 0, HW_CARD_ROM_HEADER_SIZE);
    expect(buf[0] == 0 && buf[HW_CARD_ROM_HEADER_SIZE - 1] == 0);

    if (ran != NULL) {
        *ran = sChecks;
    }
    return sFailed;
}
