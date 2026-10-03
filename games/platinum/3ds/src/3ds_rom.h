/*
 * 3ds/src/3ds_rom.h: mounting the cartridge that ships inside the 3dsx.
 *
 * Includes nothing, for the reason 3ds_ctrdg.h includes nothing: this file is
 * on both sides of the chain boundary and no DS SDK or libctru header may be.
 */

#ifndef POKEPLATINUM_3DS_ROM_H
#define POKEPLATINUM_3DS_ROM_H

/*
 * Find the cartridge image. Mounts the 3dsx's RomFS and falls back to the SD
 * card if the 3dsx has none. Idempotent, the crt calls it and so does the
 * self-test. 0 on success; on failure the libctru result, or -1 when the
 * filesystem mounted and the image is simply not in it.
 */
int rom_fs_init(void);

/*
 * Unmount, on the way out. Idempotent, and a no-op when the image came
 * off the SD card, that path never mounted anything of its own.
 */
void rom_fs_exit(void);

/*
 * Where the image turned out to be, or NULL before a successful
 * rom_fs_init(). pc/src/pc_card_rom.c opens this and nothing else on this
 * console.
 */
const char *rom_fs_path(void);

/* The two places it can be, in the order they are tried. The SD card is for a
 * skinny 3dsx (one built without --romfs, small enough to netload) and the
 * directory is the homebrew convention, sdmc:/3ds/<app>/. */
#define ROM_FS_PATH "romfs:/pokeplatinum.us.nds"
#define ROM_SD_PATH "sdmc:/3ds/pokeplatinum/pokeplatinum.us.nds"

/*
 * Where the cartridge's backup chip is kept. The PC port puts the save file
 * beside the image it belongs to and names it <rom>.sav; that rule cannot
 * follow the image here, because the usual image is inside the 3dsx and
 * romfs: is read-only. So the name is kept and the directory is the one the
 * SD-card fallback already uses, which is also where every report lands.
 */
#define ROM_SAVE_PATH "sdmc:/3ds/pokeplatinum/pokeplatinum.us.nds.sav"

/*
 * ROM_SAVE_PATH, with its directory made if the card does not have one;
 * a console that has only ever launched this port out of its own RomFS will
 * not. Never NULL; a card that refuses the directory fails at the open
 * instead, which is where the error can name the path.
 */
const char *rom_save_path(void);

/*
 * The image really is mounted and readable: the mount, the file's size, the
 * NDS header out of it, game code, the FAT and FNT the "rom" archive is
 * wired from, and the header's own CRC recomputed over the bytes that were
 * read, and reads at offsets given out of order, which is the one newlib
 * behaviour the card override's seek+read pair depends on. Returns failures,
 * 0 for a pass, fills `*ran`. Reads only.
 */
int rom_selftest(int *ran);

/* 1-based index of the first check that failed, -1 if none. Console-only, so
 * the index is the whole debugging channel. */
int rom_first_failure(void);

#endif /* POKEPLATINUM_3DS_ROM_H */
