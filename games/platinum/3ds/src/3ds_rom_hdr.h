/*
 * 3ds/src/3ds_rom_hdr.h: the cartridge header where the firmware leaves it.
 *
 * Includes nothing: 3ds_rom.c is compiled against libctru and this file's
 * implementation against the DS SDK, and no header may be on both sides of
 * that line.
 */

#ifndef POKEPLATINUM_3DS_ROM_HDR_H
#define POKEPLATINUM_3DS_ROM_HDR_H

/*
 * The buffer at guest 0x027FFE00 really is reachable, really holds this
 * cartridge's header, and the copy stops where the SDK's buffer stops.
 *
 * boot.c is why any of it matters: CheckForMemoryTampering() copies
 * HW_CARD_ROM_HEADER_SIZE bytes out of that buffer, wires the "rom" archive's
 * FAT and FNT from the copy, and terminates the game if the maker code is not
 * "01". A header that is absent, short or one byte off is a boot that stops
 * with no message.
 *
 * Places the header itself, from the mounted RomFS, so the check has
 * something to check, pc/src/pc_card_rom.c does the same placement when the
 * game is linked, and this runs where no game is. Requires a bound slab and a
 * mounted filesystem. Leaves the whole 0x027FFE00 page as it found it.
 * Returns failures, 0 for a pass, fills `*ran`.
 */
int rom_hdr_selftest(int *ran);

/* 1-based index of the first check that failed, -1 if none. Console-only, so
 * the index is the whole debugging channel. */
int rom_hdr_first_failure(void);

#endif /* POKEPLATINUM_3DS_ROM_HDR_H */
