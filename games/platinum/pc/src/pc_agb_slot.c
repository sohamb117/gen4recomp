/*
 * The GBA slot (slot 2) with a cartridge in it: the ROM, its backup chip, and
 * the ARM7's half of CTRDG's cartridge detection. Pal Park ("Migrate from
 * Ruby/Sapphire/Emerald/FireRed/LeafGreen", src/main_menu/gba_migrator.c)
 * is the game feature that reads it.
 *
 * EMPTY SLOT. With no cartridge (np_host_gba_rom_size() == 0, and always on
 * the non-wasm hosts, which have no way to name one) nothing here does
 * anything: the window stays the zero-filled memory pc_main.c's
 * map_agb_slot() describes, every bus access below is the plain memory
 * access the unpatched SDK made, and CTRDG reports no cartridge exactly as
 * it always has.
 *
 * THE ROM. The runtime's image is copied into 0x08000000 once, at boot
 * (pc_agb_slot_insert from map_agb_slot), before any guest code runs; the
 * slot is read at power-on and a different cartridge needs a new core. ROM
 * reads stay plain memory reads: CTRDG's header/logo/game-code checks, the
 * Pokemon games' header at 0x08000100 and the pointers in it (species names,
 * icons) all read the real ROM bytes.
 *
 * THE BACKUP CHIP. On hardware 0x0A000000-0x0A00FFFF is the cartridge's
 * 8-bit backup bus, and a flash chip there is driven by command writes
 * (AA@5555, 55@2AAA, then the command at 5555: 90 ID mode, F0 reset, 80 erase
 * then 10 chip / 30@sector, A0 program one byte, B0 bank select on the 128 KiB
 * parts). The game is compiled natively, so a raw store to 0x0A005555 is just
 * a store: the protocol has to be modelled. The SDK's access points
 * (pc/patches for libraries/ctrdg/src ctrdg_flash_common.c,
 * ctrdg_flash_MX29L010.c, ctrdg_sram.c) go through pc_agb_bus_read8/write8
 * below, and everything above them (identify, erase, program, verify, the
 * task thread, the game's sector code in ov97_02235D18.c) is the real SDK and
 * game code unchanged.
 *
 * Which chip: the one the cartridge has. A GBA game links Nintendo's backup
 * library, whose version string names the backup type, and that is how the
 * type is found here (the same thing every GBA emulator does):
 *   FLASH1M_V    128 KiB flash. Answers ID 0xC2/0x09 (Macronix MX29L010,
 *                the first entry of the SDK's 1M list). All five Gen 3
 *                Pokemon games.
 *   SRAM_V, SRAM_F_V   32 KiB SRAM, mirrored through the 64 KiB window.
 * Anything else (EEPROM, which the DS cannot reach, or 64 KiB flash, whose
 * SDK driver is not routed) has no backup on this bus: reads float to 0xFF.
 *
 * Persistence: np_host_gba_save_load fills the image at insertion (0 = never
 * written: an erased chip, 0xFF); every program/erase/SRAM store marks it
 * dirty, and the whole image goes to np_host_gba_save_store at the first
 * frame boundary (OS_Halt, pc_agb_slot_step) after writes have been quiet
 * for AGB_SETTLE_FRAMES, and at exit.
 *
 * THE ARM7 HALF. CTRDG_Init's module-info handshake (ctrdg_proc.c
 * CTRDGi_InitModuleInfo) runs only when POSTFLG says the system booted, and
 * it then copies the BIOS's Nintendo logo from 0xFFFF0020 and sends PXI
 * CTRDG INIT_MODULE_INFO; the ARM7 compares that logo with the cartridge's
 * (ctrdg_proc_arm7.c CTRDGi_IsNinLogoOfAgb), sets isAgbCartridge and replies.
 * With a cartridge inserted POSTFLG is set, the logo comes from the DS card
 * header's copy (pc_agb_sysrom9_logo; the firmware only boots a card whose
 * header logo is the BIOS's), and the responder below is that ARM7 code.
 * With the slot empty POSTFLG stays clear and the handshake is skipped, as
 * before.
 */
#include <nitro.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__wasm__)
#include <np_guest_abi.h>
#endif

extern void pc_pxi_set_responder(int tag, void (*fn)(u32 data));
extern void pc_pxi_reply(int tag, u32 data);
extern unsigned long long pc_irq_frames(void);

#define AGB_BUS_BASE      0x0A000000u
#define AGB_BUS_SIZE      0x00010000u
#define AGB_FLASH1M_SIZE  0x00020000u
#define AGB_SRAM_SIZE     0x00008000u
#define AGB_FLASH_MAKER   0xC2 /* Macronix */
#define AGB_FLASH_DEVICE  0x09 /* MX29L010 */
#define AGB_SETTLE_FRAMES 8ull
#define AGB_DIRTY_MAX     300ull

enum agb_backup {
    AGB_BACKUP_NONE,
    AGB_BACKUP_FLASH1M,
    AGB_BACKUP_SRAM,
};

/* Flash command sequencer: where in an unlock sequence the chip is. */
enum agb_cmd {
    CMD_IDLE,
    CMD_AA,       /* AA@5555 seen */
    CMD_55,       /* AA@5555, 55@2AAA seen: next write at 5555 is the command */
    CMD_ERASE,    /* 80 seen: an erase needs a second unlock */
    CMD_ERASE_AA,
    CMD_ERASE_55,
    CMD_PROGRAM,  /* A0 seen: the next write programs one byte */
    CMD_BANK,     /* B0 seen: the next write at 0000 selects the bank */
};

static int sPresent;
static enum agb_backup sBackup;
static u8 *sImage;
static u32 sImageSize;
static enum agb_cmd sCmd;
static int sIdMode;
static u32 sBank;
static int sDirty;
static unsigned long long sLastWrite, sDirtySince;

static const char *backup_name(enum agb_backup b)
{
    switch (b) {
    case AGB_BACKUP_FLASH1M:
        return "128 KiB flash (MX29L010)";
    case AGB_BACKUP_SRAM:
        return "32 KiB SRAM";
    default:
        return "none";
    }
}

/* The backup library's version string, word-aligned in the ROM. */
static enum agb_backup find_backup(const u8 *rom, u32 size)
{
    u32 i;

    for (i = 0; i + 12 <= size; i += 4) {
        if (rom[i] == 'F' && memcmp(rom + i, "FLASH1M_V", 9) == 0) {
            return AGB_BACKUP_FLASH1M;
        }
        if (rom[i] == 'S' && (memcmp(rom + i, "SRAM_V", 6) == 0 ||
                              memcmp(rom + i, "SRAM_F_V", 8) == 0)) {
            return AGB_BACKUP_SRAM;
        }
    }
    return AGB_BACKUP_NONE;
}

static void store_now(void)
{
#if defined(__wasm__)
    int32_t rc;

    if (!sDirty) {
        return;
    }
    rc = np_host_gba_save_store(sImage, sImageSize);
    if (rc != 0) {
        /* Stays dirty: the next settle and the exit sync try again. */
        fprintf(stderr, "pc_agb_slot: GBA save store failed (%d)\n", (int)rc);
        return;
    }
    sDirty = 0;
#endif
}

static void agb_slot_sync(void)
{
    store_now();
}

static void touch(void)
{
    unsigned long long now = pc_irq_frames();

    if (!sDirty) {
        sDirty = 1;
        sDirtySince = now;
    }
    sLastWrite = now;
}

/* ------------------------------------------------------------- the chip */

static u8 *flash_cell(u32 off)
{
    return &sImage[(sBank * AGB_BUS_SIZE + off) % sImageSize];
}

static void flash_write(u32 off, u8 v)
{
    if (sCmd == CMD_PROGRAM) {
        /* Programming only clears bits; the SDK erases first. */
        *flash_cell(off) &= v;
        sCmd = CMD_IDLE;
        touch();
        return;
    }
    if (sCmd == CMD_BANK) {
        if (off == 0) {
            sBank = v & 1u;
        }
        sCmd = CMD_IDLE;
        return;
    }
    if (v == 0xF0) {
        /* Reset / exit ID mode, from any state. */
        sIdMode = 0;
        sCmd = CMD_IDLE;
        return;
    }
    switch (sCmd) {
    case CMD_IDLE:
        sCmd = (off == 0x5555 && v == 0xAA) ? CMD_AA : CMD_IDLE;
        break;
    case CMD_AA:
        sCmd = (off == 0x2AAA && v == 0x55) ? CMD_55 : CMD_IDLE;
        break;
    case CMD_55:
        sCmd = CMD_IDLE;
        if (off != 0x5555) {
            break;
        }
        if (v == 0x90) {
            sIdMode = 1;
        } else if (v == 0x80) {
            sCmd = CMD_ERASE;
        } else if (v == 0xA0) {
            sCmd = CMD_PROGRAM;
        } else if (v == 0xB0) {
            sCmd = CMD_BANK;
        }
        break;
    case CMD_ERASE:
        sCmd = (off == 0x5555 && v == 0xAA) ? CMD_ERASE_AA : CMD_IDLE;
        break;
    case CMD_ERASE_AA:
        sCmd = (off == 0x2AAA && v == 0x55) ? CMD_ERASE_55 : CMD_IDLE;
        break;
    case CMD_ERASE_55:
        sCmd = CMD_IDLE;
        if (off == 0x5555 && v == 0x10) {
            memset(sImage, 0xFF, sImageSize);
            touch();
        } else if (v == 0x30) {
            memset(flash_cell(off & 0xF000u), 0xFF, 0x1000);
            touch();
        }
        break;
    default:
        sCmd = CMD_IDLE;
        break;
    }
}

static u8 flash_read(u32 off)
{
    if (sIdMode && off < 2) {
        return off == 0 ? AGB_FLASH_MAKER : AGB_FLASH_DEVICE;
    }
    return *flash_cell(off);
}

/* ----------------------------------------------- the SDK's access points */

u8 pc_agb_bus_read8(const volatile void *adr)
{
    u32 a = (u32)(uintptr_t)adr;

    if (!sPresent || a - AGB_BUS_BASE >= AGB_BUS_SIZE) {
        return *(const volatile u8 *)adr;
    }
    switch (sBackup) {
    case AGB_BACKUP_FLASH1M:
        return flash_read(a - AGB_BUS_BASE);
    case AGB_BACKUP_SRAM:
        return sImage[(a - AGB_BUS_BASE) % AGB_SRAM_SIZE];
    default:
        return 0xFF;
    }
}

void pc_agb_bus_write8(volatile void *adr, u8 v)
{
    u32 a = (u32)(uintptr_t)adr;

    if (!sPresent || a - AGB_BUS_BASE >= AGB_BUS_SIZE) {
        *(volatile u8 *)adr = v;
        return;
    }
    switch (sBackup) {
    case AGB_BACKUP_FLASH1M:
        flash_write(a - AGB_BUS_BASE, v);
        break;
    case AGB_BACKUP_SRAM:
        sImage[(a - AGB_BUS_BASE) % AGB_SRAM_SIZE] = v;
        touch();
        break;
    default:
        break;
    }
}

/*
 * The SDK times its flash waits (ID mode entry, program/erase status polls)
 * with OS_GetTick, and the port has no hardware timer to advance it. The chip
 * above finishes every command at the write that issues it, so with a
 * cartridge in, any wait the SDK is timing has already elapsed. Empty slot:
 * FALSE, and the SDK's own timer code runs as before.
 */
BOOL pc_agb_bus_wait_elapsed(void)
{
    return sPresent ? TRUE : FALSE;
}

/* What CTRDGi_InitModuleInfo copies from the BIOS (0xFFFF0020, 0x9C bytes):
 * the Nintendo logo, whose verified copy is the DS card header's. */
const void *pc_agb_sysrom9_logo(void)
{
    return (const void *)(HW_ROM_HEADER_BUF + 0xC0);
}

/* ------------------------------------------------------------ the ARM7 */

/* ctrdg_proc_arm7.c CTRDGi_IsNinLogoOfAgb: the logo the ARM9 sent (the
 * BIOS's) against the cartridge's, two bits of the debug/boot flags masked. */
static BOOL logo_is_agb(const u16 *logop)
{
    const u16 *cart = (const u16 *)((const CTRDGHeader *)HW_CTRDG_ROM)->nintendoLogo;
    int i;

    for (i = 0; i < (0xA0 - 4) / 2; i++) {
        u16 mask = 0xFFFF;

        if (i == (0x9C - 4) / 2) {
            mask ^= 0x84;
        } else if (i == (0x9E - 4) / 2) {
            mask ^= 3;
        }
        if ((logop[i] & mask) != cart[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * The INIT_MODULE_INFO word carries the ARM9's header buffer as a 32-byte
 * granule offset into main RAM (19 bits). In the port that buffer is the SDK's
 * static, which lives with the rest of the guest's C data above the DS's
 * memory map, so the offset cannot reach it; the logo the ARM9 copied into it
 * is pc_agb_sysrom9_logo()'s, and that is what is compared.
 */
static void ctrdg_arm7(u32 data)
{
    if ((data & CTRDG_PXI_COMMAND_MASK) == CTRDG_PXI_COMMAND_INIT_MODULE_INFO) {
        CTRDGModuleInfo *cip = (CTRDGModuleInfo *)HW_CTRDG_MODULE_INFO_BUF;

        cip->isAgbCartridge = logo_is_agb((const u16 *)pc_agb_sysrom9_logo()) ? 1 : 0;
        pc_pxi_reply(PXI_FIFO_TAG_CTRDG, CTRDG_PXI_COMMAND_INIT_MODULE_INFO);
    }
    /* TERMINATE (the ARM9 shutting down for a pulled cartridge) needs no
     * answer; a cartridge here is never pulled. */
}

/* ------------------------------------------------------------- lifecycle */

void pc_agb_slot_insert(void)
{
#if defined(__wasm__)
    u32 size = np_host_gba_rom_size();
    int32_t rc;

    if (size == 0) {
        return;
    }
    if (size > NP_GBA_ROM_MAX) {
        fprintf(stderr, "pc_agb_slot: GBA ROM is %u bytes, over the 32 MiB "
                        "window; slot left empty\n", (unsigned)size);
        return;
    }
    if (np_host_gba_rom_read(0, (void *)HW_CTRDG_ROM, size) != 0) {
        memset((void *)HW_CTRDG_ROM, 0, size);
        fprintf(stderr, "pc_agb_slot: GBA ROM read failed; slot left empty\n");
        return;
    }

    sBackup = find_backup((const u8 *)HW_CTRDG_ROM, size);
    sImageSize = sBackup == AGB_BACKUP_FLASH1M ? AGB_FLASH1M_SIZE
               : sBackup == AGB_BACKUP_SRAM    ? AGB_SRAM_SIZE
                                               : 0;
    if (sImageSize != 0) {
        sImage = malloc(sImageSize);
        if (sImage == NULL) {
            memset((void *)HW_CTRDG_ROM, 0, size);
            fprintf(stderr, "pc_agb_slot: no memory for the GBA backup\n");
            return;
        }
        memset(sImage, 0xFF, sImageSize);
        rc = np_host_gba_save_load(sImage, sImageSize);
        if (rc < 0) {
            fprintf(stderr, "pc_agb_slot: GBA save load failed; using an "
                            "erased chip, which will not be stored\n");
            memset(sImage, 0xFF, sImageSize);
        }
        atexit(agb_slot_sync);
    } else {
        rc = 0;
    }

    sPresent = 1;
    reg_OS_PAUSE |= REG_OS_PAUSE_CHK_MASK;
    pc_pxi_set_responder(PXI_FIFO_TAG_CTRDG, ctrdg_arm7);

    {
        const CTRDGHeader *h = (const CTRDGHeader *)HW_CTRDG_ROM;
        char title[13], code[5];

        memcpy(title, h->titleName, 12);
        title[12] = '\0';
        memcpy(code, &h->gameCode, 4);
        code[4] = '\0';
        fprintf(stderr, "pc_agb_slot: GBA cartridge %s (%s), %u bytes, "
                        "backup %s%s\n", title, code, (unsigned)size,
                backup_name(sBackup),
                sImageSize == 0 ? "" : rc > 0 ? ", save loaded" : ", erased");
    }
#endif
}

/* Once per frame (OS_Halt): store the image once writes have gone quiet. */
void pc_agb_slot_step(void)
{
    unsigned long long now;
    if (!sDirty) {
        return;
    }
    now = pc_irq_frames();
    if (now - sLastWrite >= AGB_SETTLE_FRAMES || now - sDirtySince >= AGB_DIRTY_MAX) {
        store_now();
    }
}
