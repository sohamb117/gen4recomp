/*
 * 3ds/src/3ds_rom.c: the cartridge, mounted.
 *
 * There is no card slot on this console, so the build packs the .nds image
 * into the 3dsx as its RomFS and the game reads it through
 * pc/src/pc_card_rom.c, whose CARDi_ReadRom override turns a ROM offset into a
 * read of that file. This file is the half that has to talk to libctru:
 * romfsInit(), and the self-test that says the mount worked.
 *
 * The split is the two chains and not tidiness. pc_card_rom.c is a game
 * translation unit; it includes <nitro.h> and the card library's private
 * header, so it may never see <3ds.h>, where romfsInit() is declared. What it
 * can use is newlib: open, lseek and read reach RomFS through the devoptab
 * libctru registers, with no 3DS header in the file at all.
 *
 * Why a self-test for something this small. The mount is reachable long before
 * its only caller is, and the failure it guards against is quiet: a 3dsx built
 * without --romfs, or against a different image, mounts nothing or mounts the
 * wrong bytes, and the first sign is a game that boots to a black screen.
 *
 * Where the image can be, tried in this order:
 *
 *   romfs:/pokeplatinum.us.nds                     packed into the 3dsx
 *   sdmc:/3ds/pokeplatinum/pokeplatinum.us.nds     on the SD card
 *
 * The second is for a 3dsx built without --romfs, which is worth having for
 * netloading: 3dslink pushes the whole file over the wire on every iteration,
 * and 128 MB of it is a minute of waiting per edit. Nothing in this tree ships
 * an image either way; your own dump goes in that directory.
 */

#include <3ds.h>

#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

#include "3ds_rom.h"
#include "3ds_sdcard.h"

/* What the build packs. 0x8000000 exactly: the ROM build pads to the
 * cartridge's own size and the build copies it byte for byte. */
#define ROM_IMAGE_BYTES 0x8000000L

/* The NDS header, at offset 0 of any image this port will ever open. Offsets
 * from the SDK's CARDRomHeader; the values are this US build's. */
#define ROM_HDR_GAME_CODE 0x0C
#define ROM_HDR_FNT_OFF 0x40
#define ROM_HDR_FAT_OFF 0x48
#define ROM_HDR_CRC 0x15E

static int sChecks;
static int sFailed;
static int sFirstFail;
static const char *sPath;
static int sMounted;            /* the RomFS is ours to unmount */

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

int rom_first_failure(void)
{
    return (sFirstFail == 0) ? -1 : sFirstFail;
}

const char *rom_fs_path(void)
{
    return sPath;
}

const char *rom_save_path(void)
{
    static int made;

    if (!made) {
        made = 1;
        sd_make_dirs(ROM_SAVE_PATH);
    }
    return ROM_SAVE_PATH;
}

/* Openable, asked the only way that cannot be wrong about a devoptab: open
 * it. access() goes through stat, which libctru's RomFS answers for names it
 * has and the SD device answers from the FAT; both are one more layer that
 * could disagree with what a read will do. */
static int readable(const char *path)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        return 0;
    }
    close(fd);
    return 1;
}

int rom_fs_init(void)
{
    Result rc;

    if (sPath != NULL) {
        return 0;
    }

    /* The 3dsx built by this tree carries the image, so this is the
     * path every normal launch takes. */
    rc = romfsInit();
    if (R_SUCCEEDED(rc)) {
        if (readable(ROM_FS_PATH)) {
            sPath = ROM_FS_PATH;
            sMounted = 1;
            return 0;
        }
        /* Mounted and empty: a 3dsx packed without --romfs still has a
         * RomFS, it just has nothing in it. Unmount before falling through,
         * so nothing later mistakes an empty archive for the cartridge. */
        romfsExit();
    }

    /* 6.4. No RomFS, or one without the image in it, a skinny 3dsx, which
     * is the build worth netloading, since 3dslink would otherwise push
     * 128 MB over the wire on every iteration. The SD card carries the
     * cartridge instead. No image is committed to this tree either way. */
    if (readable(ROM_SD_PATH)) {
        sPath = ROM_SD_PATH;
        return 0;
    }

    return R_FAILED(rc) ? (int)rc : -1;
}

void rom_fs_exit(void)
{
    /* Only the RomFS path mounted anything. The SD card is a device libctru
     * registered before main() and unmounting it here would take the save
     * file's own device away. */
    if (sMounted) {
        romfsExit();
        sMounted = 0;
    }
    sPath = NULL;
}

/* The header's own checksum, over the 0x15E bytes before it: CRC-16/MODBUS,
 * which is what the DS firmware verifies and what the SDK's CARD_IsPulledOut
 * path recomputes. Here it is not the cartridge that is doubted but the read:
 * A truncated or misaligned RomFS gives a header that still looks like
 * text and still has the right game code. */
static uint16_t crc16(const uint8_t *p, unsigned len)
{
    uint16_t crc = 0xFFFF;
    unsigned i;
    int bit;

    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

int rom_selftest(int *ran)
{
    uint8_t header[0x200];
    uint8_t fat[8];
    uint8_t fnt[8];
    uint8_t again[4];
    int fd;
    off_t size;

    sChecks = 0;
    sFailed = 0;
    sFirstFail = 0;

    expect(rom_fs_init() == 0);
    /* One of the two, and the same one pc_card_rom.c will open. */
    expect(rom_fs_path() != NULL);
    if (rom_fs_path() == NULL) {
        if (ran != NULL) {
            *ran = sChecks;
        }
        return sFailed;
    }

    fd = open(rom_fs_path(), O_RDONLY);
    expect(fd >= 0);
    if (fd < 0) {
        /* Nothing below can run without the file, and reporting twelve
         * failures for one cause hides which cause it was. */
        if (ran != NULL) {
            *ran = sChecks;
        }
        return sFailed;
    }

    size = lseek(fd, 0, SEEK_END);
    expect(size == ROM_IMAGE_BYTES);

    expect(lseek(fd, 0, SEEK_SET) == 0);
    expect(read(fd, header, sizeof header) == (ssize_t)sizeof header);

    /* POKEMON PL / CPUE / 01: the title, the game code boot.c checks and the
     * maker. Byte by byte, because a string compare on a field that is not
     * NUL-terminated reads past it. */
    expect(header[0] == 'P' && header[1] == 'O' && header[2] == 'K'
           && header[3] == 'E' && header[4] == 'M' && header[5] == 'O'
           && header[6] == 'N' && header[7] == ' ' && header[8] == 'P'
           && header[9] == 'L');
    expect(header[ROM_HDR_GAME_CODE + 0] == 'C'
           && header[ROM_HDR_GAME_CODE + 1] == 'P'
           && header[ROM_HDR_GAME_CODE + 2] == 'U'
           && header[ROM_HDR_GAME_CODE + 3] == 'E');
    expect(header[0x10] == '0' && header[0x11] == '1');

    /* What the "rom" archive is wired from. Both inside the image, and
     * the FAT after the FNT, which is this build's layout. */
    expect(le32(header + ROM_HDR_FNT_OFF) == 0x431000u);
    expect(le32(header + ROM_HDR_FAT_OFF) == 0x432C00u);
    expect(le32(header + ROM_HDR_FAT_OFF) < (uint32_t)ROM_IMAGE_BYTES);

    expect(crc16(header, ROM_HDR_CRC) == 0x5153u);
    expect(((uint16_t)header[ROM_HDR_CRC]
            | (uint16_t)(header[ROM_HDR_CRC + 1] << 8)) == 0x5153u);

    /* A read 4 MB in, at an offset the header named rather than one this file
     * chose: the FAT's first entry, which is the first file's start and end.
     * A RomFS that mounted the wrong file, or mounted this one at the wrong
     * base, gets the header right and this wrong. */
    expect(lseek(fd, (off_t)0x432C00, SEEK_SET) == (off_t)0x432C00);
    expect(read(fd, fat, sizeof fat) == (ssize_t)sizeof fat);
    expect(le32(fat) == 0x107600u);
    expect(le32(fat + 4) == 0x107620u);

    /* 6.5. The FNT root, which is the first thing the "rom" archive reads
     * once FS_LoadArchive has the two offsets above: the subtable offset,
     * the id of the first file and the directory count. Reading it here says
     * the FAT and FNT the header names really are a filesystem, and not two
     * numbers that happen to be inside the image. */
    expect(lseek(fd, (off_t)0x431000, SEEK_SET) == (off_t)0x431000);
    expect(read(fd, fnt, sizeof fnt) == (ssize_t)sizeof fnt);
    expect(le32(fnt) == 0x2C8u);
    expect(((uint16_t)fnt[4] | (uint16_t)(fnt[5] << 8)) == 0x7Au);
    expect(((uint16_t)fnt[6] | (uint16_t)(fnt[7] << 8)) == 0x59u);

    /* Backwards, to offset 0 again. pc_card_rom.c has no pread on this
     * console (newlib does not carry one) so its override seeks and
     * reads, and the SDK hands it offsets in whatever order the game asks
     * for them. This is that pattern, once, with the answer already known. */
    expect(lseek(fd, 0, SEEK_SET) == 0);
    expect(read(fd, again, sizeof again) == (ssize_t)sizeof again);
    expect(again[0] == header[0] && again[1] == header[1]
           && again[2] == header[2] && again[3] == header[3]);

    close(fd);

    if (ran != NULL) {
        *ran = sChecks;
    }
    return sFailed;
}
