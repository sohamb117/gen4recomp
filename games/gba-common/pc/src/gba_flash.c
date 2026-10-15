/*
 * The cartridge's flash chip, as the agb_flash API the decomps call
 * (agb_flash*.c are not compiled): a 64 or 128 KiB image in the port's
 * memory, loaded from the host at boot and published in the frame
 * descriptor (save_image/save_size/save_dirty) for the host to store.
 * The chip identifies as a Macronix MX29L010 (128 KiB, Emerald) or a
 * Panasonic MN63F805MNP (64 KiB, Ruby/Sapphire).
 */
#include <string.h>

#include "global.h"
#include "gba/flash_internal.h"
#include "gba_port.h"
#include "np_guest_abi.h"

#define SECTOR 0x1000u

static uint8_t s_chip[0x20000];
static uint32_t s_size;
uint32_t gba_flash_dirty;

static const struct FlashType s_type_128k = {
    .romSize = 0x20000, .sector = {.size = SECTOR, .shift = 12, .count = 32, .top = 0},
    .wait = {3, 1}, .ids = {.joined = 0x09C2}};
static const struct FlashType s_type_64k = {
    .romSize = 0x10000, .sector = {.size = SECTOR, .shift = 12, .count = 16, .top = 0},
    .wait = {3, 1}, .ids = {.joined = 0x1B32}};
static const u16 s_max_time[] = {10, 65469, 0, 0, 10, 65469, 0, 0, 2000, 65469, 0, 0, 2000, 65469, 0, 0};

/* pokeruby declares the sector buffers void *, pokeemerald u8 * */
#if defined(RUBY) || defined(SAPPHIRE)
typedef void flash_buf;
#else
typedef u8 flash_buf;
#endif

/* RAM pointers the decomp keeps (their addresses are the cartridge's) */
const struct FlashType *gFlash;
const u16 *gFlashMaxTime;
u16 (*ProgramFlashByte)(u16, u32, u8);
u16 (*ProgramFlashSector)(u16, flash_buf *);
u16 (*EraseFlashChip)(void);
u16 (*EraseFlashSector)(u16);
u16 (*WaitForFlashWrite)(u8, u8 *, u8);

void gba_flash_boot(void) {
    s_size = gba_game.save_size;
    memset(s_chip, 0xFF, sizeof s_chip);
    if (np_host_save_load(s_chip, s_size) != 1) memset(s_chip, 0xFF, s_size);
    gba_flash_dirty = 0;
}

void gba_flash_publish(void);

uint8_t *gba_flash_image(void) { return s_chip; }
uint32_t gba_flash_size(void) { return s_size; }

static uint8_t *sector_ptr(u16 n) {
    if ((u32)n * SECTOR >= s_size) gba_fatal("flash: sector %u outside the %u-byte chip", n, s_size);
    return s_chip + (u32)n * SECTOR;
}

u16 ProgramFlashByte_Port(u16 sector, u32 offset, u8 data) {
    sector_ptr(sector)[offset & (SECTOR - 1)] = data;
    gba_flash_dirty = 1;
    return 0;
}

u16 ProgramFlashSector_Port(u16 sector, flash_buf *src) {
    memcpy(sector_ptr(sector), src, SECTOR);
    gba_flash_dirty = 1;
    return 0;
}

u16 EraseFlashChip_Port(void) {
    memset(s_chip, 0xFF, s_size);
    gba_flash_dirty = 1;
    return 0;
}

u16 EraseFlashSector_Port(u16 sector) {
    memset(sector_ptr(sector), 0xFF, SECTOR);
    gba_flash_dirty = 1;
    return 0;
}

u16 WaitForFlashWrite_Port(u8 phase, u8 *addr, u8 last) {
    (void)phase, (void)addr, (void)last;
    return 0;
}

u16 IdentifyFlash(void) {
    gFlash = s_size > 0x10000 ? &s_type_128k : &s_type_64k;
    gFlashMaxTime = s_max_time;
    ProgramFlashByte = ProgramFlashByte_Port;
    ProgramFlashSector = ProgramFlashSector_Port;
    EraseFlashChip = EraseFlashChip_Port;
    EraseFlashSector = EraseFlashSector_Port;
    WaitForFlashWrite = WaitForFlashWrite_Port;
    return 0;
}

u16 SetFlashTimerIntr(u8 timerNum, void (**intrFunc)(void)) {
    (void)timerNum, (void)intrFunc;
    return 0;
}

void ReadFlash(u16 sector, u32 offset, flash_buf *dest, u32 size) {
    memcpy(dest, sector_ptr(sector) + offset, size);
}

u32 ReadFlash1(u16 sector, u32 offset) {
    return sector_ptr(sector)[offset & (SECTOR - 1)];
}

u32 VerifyFlashSector(u16 sector, u8 *src) {
    return memcmp(sector_ptr(sector), src, SECTOR) ? (u32)(uintptr_t)sector_ptr(sector) : 0;
}

u32 VerifyFlashSectorNBytes(u16 sector, u8 *src, u32 n) {
    return memcmp(sector_ptr(sector), src, n) ? (u32)(uintptr_t)sector_ptr(sector) : 0;
}

u32 ProgramFlashSectorAndVerify(u16 sector, u8 *src) {
    ProgramFlashSector_Port(sector, src);
    return 0;
}

u32 ProgramFlashSectorAndVerifyNBytes(u16 sector, flash_buf *src, u32 n) {
    EraseFlashSector_Port(sector);
    memcpy(sector_ptr(sector), src, n);
    gba_flash_dirty = 1;
    return 0;
}

void SwitchFlashBank(u8 bank) { (void)bank; }
