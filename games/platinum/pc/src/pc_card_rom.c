/*
 * The cartridge, backed by the tree's own ROM build.
 *
 * This decomp builds the real ROM (ninja -C build/rom ->
 * pokeplatinum.us.nds), so the cartridge the game expects to read is a
 * file this port can open. Every ROM read in the SDK funnels through
 * CARDi_ReadRom (CARD_ReadRom / CARD_ReadRomAsync are header inlines over
 * it), so that is the override point: a read of guest "ROM offset" src
 * becomes a pread of the image. Reads complete synchronously, the card
 * bus with a parked CPU, same reasoning as the DMA model, and the
 * callback runs before return, a schedule every SDK caller must tolerate.
 *
 * CARD_Init is replaced too: the SDK's spins up the async transfer
 * thread and the ARM7 handshake, none of which exists when every read
 * completes before returning. The pieces of its state machine that later
 * code observes (cardi_common.flag, the lock word) are set the same way.
 *
 * pc_rom_init() also places the ROM header at HW_ROM_HEADER_BUF
 * (0x027FFE00), which on hardware the firmware does before the game ever
 * runs, boot.c's anti-tamper check reads the FAT/FNT offsets out of
 * that buffer to wire the "rom" archive.
 *
 * The image path: $PC_ROM if set, else <dir of the executable>
 * /../rom/pokeplatinum.us.nds (the build tree layout), else
 * ./build/rom/pokeplatinum.us.nds. The wasm guest names no path: the
 * runtime owns the image and serves np_host_rom_size/np_host_rom_read.
 */
#include "pc_bench.h"
#include <nitro.h>
#include <nitro/card/rom.h>
/* The card library's private state (CARDiCommon, cardi_common), reached
 * through the public include root on purpose, so this file states exactly
 * which internal it shares with the SDK's own card_common.c. Diamond/Pearl's
 * SDK is 3.2-era and lays the command block's chip spec out differently;
 * games/diamond/pc/include/host/pc_dp_card_common.h is its layout. TWL-SDK 5
 * (Black/White, ARMREC_TWL) has 4.2's command block: its
 * CARDi_IdentifyBackupCore (Black 0x020766D8) clears the 0x48-byte spec at
 * +0x18 and writes total_size +0x18, sect_size +0x1C, page_size +0x24,
 * addr_width +0x28, initial_status +0x54 and caps +0x58, and its stream
 * requests set src +0x0C, dst +0x10, len +0x14 after result and type.
 * HeartGold/SoulSilver (PC_HOST_SDK42) are recompiled like D/P but link
 * NitroSDK 4.2, Platinum's own layout. */
#if defined(PC_GAME_DP) && !defined(ARMREC_TWL) && !defined(PC_HOST_SDK42)
#include <pc_dp_card_common.h>
#else
#include <../libraries/card/include/card_common.h>
#endif

#include <fcntl.h>

/* Windows: fds default to TEXT mode, which rewrites bytes; and mingw has no
 * pread/pwrite. A seek+read pair is equivalent here; every access is from
 * one thread. O_BINARY is 0 where the concept does not exist. */
#ifndef O_BINARY
#define O_BINARY 0
#endif
#if defined(_WIN32)
#include <io.h>
static long pc_pread(int fd, void *buf, unsigned len, long long off) {
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    return read(fd, buf, len);
}
static long pc_pwrite(int fd, const void *buf, unsigned len, long long off) {
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    return write(fd, buf, len);
}
#define pread(fd, buf, len, off)  pc_pread(fd, buf, len, off)
#define pwrite(fd, buf, len, off) pc_pwrite(fd, buf, len, off)
#define ssize_t long
#elif defined(__3DS__)
/* Same absence, different C library: newlib has neither call. The seek+read
 * pair is equivalent for the same reason it is on Windows, every access
 * here names its own offset and there is one thread, with the one
 * difference that it moves the file position, which nothing in this file
 * reads. */
#include <unistd.h>
static ssize_t pc_pread(int fd, void *buf, size_t len, off_t off) {
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    return read(fd, buf, len);
}
static ssize_t pc_pwrite(int fd, const void *buf, size_t len, off_t off) {
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    return write(fd, buf, len);
}
#define pread(fd, buf, len, off)  pc_pread(fd, buf, len, off)
#define pwrite(fd, buf, len, off) pc_pwrite(fd, buf, len, off)
#endif
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__wasm__)
#include <pc_wasm.h>

/* Bytes the runtime's image holds; 0 until pc_rom_init succeeded. */
static u32 sRomSize;
#else
static int sRomFd = -1;
#endif

/* The save chip's image (the backup section below); also read through the
 * IR chip's pass-through on the card's SPI bus. */
static u8 *sBackupImage;
static u32 sBackupSize;
/* The cartridge has an IR transceiver in front of its save chip (HG/SS,
 * Black/White: game codes starting with 'I'); see card_spi_transfer(). */
static int sCardIr;

#if defined(__wasm__)
/* No path: the runtime opened the image before the guest started. */
#elif defined(__3DS__)
/* The search happened at mount time: 3ds/src/3ds_rom.c tries the image packed
 * into the 3dsx first and the SD card second, and hands back whichever it
 * opened. There is nothing for this side to look for, no executable
 * directory to walk to, no environment to read, and a working directory that
 * depends on the loader. */
static const char *rom_path(char *buf, size_t bufsize)
{
    extern int rom_fs_init(void);
    extern const char *rom_fs_path(void);

    (void)buf;
    (void)bufsize;
    if (rom_fs_path() == NULL) {
        (void)rom_fs_init();
    }
    /* Still nothing: neither place had an image. Hand back the one the 3dsx
     * is meant to carry so the failure below names a path. */
    return rom_fs_path() ? rom_fs_path() : "romfs:/pokeplatinum.us.nds";
}
#else
static const char *rom_path(char *buf, size_t bufsize)
{
    const char *env = getenv("PC_ROM");
    ssize_t n;
    char *slash;

    if (env) {
        return env;
    }
#if defined(_WIN32)
    /* The exe's own directory, the /proc/self/exe of PE. pcw_module_dir
     * strips the filename and hands back a trailing-slash-less dir. */
    {
        extern int pcw_module_dir(char *out, unsigned cap);

        if (pcw_module_dir(buf, (unsigned)bufsize - 32)) {
            n = (ssize_t)strlen(buf);
            buf[n] = '/';
            buf[n + 1] = '\0';
            n += 1;
        } else {
            n = -1;
        }
    }
#else
    n = readlink("/proc/self/exe", buf, bufsize - 1);
#endif
    if (n > 0) {
        buf[n] = '\0';
        slash = strrchr(buf, '/');
        if (slash) {
            snprintf(slash + 1, bufsize - (size_t)(slash + 1 - buf),
                     "../rom/pokeplatinum.us.nds");
            if (access(buf, R_OK) == 0) {
                return buf;
            }
        }
    }
    return "build/rom/pokeplatinum.us.nds";
}
#endif

#if defined(ARMREC_TWL)
static void card_firmware_words(void);
static void card_fs_responder(u32 data);
extern void pc_pxi_set_responder(int tag, void (*fn)(u32 data));
#endif

#if defined(__wasm__)
int pc_rom_init(void)
{
    u8 header[0x200];
    u32 size = np_host_rom_size();

    if (size < sizeof header) {
        pc_wasm_fatalf("pc_card_rom: the runtime's ROM image is %u bytes, "
                       "less than its own header", (unsigned)size);
    }
    if (np_host_rom_read(0, header, sizeof header) != 0) {
        pc_wasm_fatal("pc_card_rom: reading the ROM header failed");
    }
#if defined(PC_MB_CHILD)
    /* A Download Play child (games/ndsrec/pc/src/pc_pt_child.c): the
     * runtime's image is the card in slot 1, not the program, so its header
     * is where a child finds the inserted card's (CARD_GetRomHeader), and
     * the firmware's boot state for a child follows the card's words. */
    memcpy((void *)HW_CARD_ROM_HEADER, header, HW_CARD_ROM_HEADER_SIZE);
#else
    /* HW_CARD_ROM_HEADER_SIZE, not sizeof header: see the comment in the
     * file-backed pc_rom_init below. */
    memcpy((void *)HW_ROM_HEADER_BUF, header, HW_CARD_ROM_HEADER_SIZE);
#endif
    sCardIr = header[0x0C] == 'I';
#if defined(ARMREC_TWL)
    card_firmware_words();
    pc_pxi_set_responder(PXI_FIFO_TAG_FS, card_fs_responder);
#endif
#if defined(PC_MB_CHILD)
    {
        extern void pc_mb_child_boot(void);
        pc_mb_child_boot();
    }
#endif
    sRomSize = size;
    fprintf(stderr, "pokeplatinum-wasm: rom: %u bytes from the runtime\n",
            (unsigned)size);
    return 0;
}
#else
int pc_rom_init(void)
{
    char pathbuf[PATH_MAX];
    const char *path = rom_path(pathbuf, sizeof pathbuf);
    u8 header[0x200];
    ssize_t n;

    sRomFd = open(path, O_RDONLY | O_BINARY);
    if (sRomFd < 0) {
        fprintf(stderr,
                "pokeplatinum-pc: cannot open the ROM image at %s\n"
                "  (build it: ninja -C build/rom; or set PC_ROM)\n",
                path);
        return -1;
    }
    n = pread(sRomFd, header, sizeof header, 0);
    if (n != (ssize_t)sizeof header) {
        fprintf(stderr, "pokeplatinum-pc: short read on the ROM header\n");
        return -1;
    }
    /* What the firmware leaves behind: the cart header at
     * HW_ROM_HEADER_BUF. HW_CARD_ROM_HEADER_SIZE and not sizeof header;
     * the buffer ends at 0x027FFF60 and what follows it is the PXI signal
     * words, the thread-info pointers, HW_BUTTON_XY_BUF and every lock
     * word. Nothing had read them yet at this point in start-up, so the
     * 0xA0-byte overrun this used to do was invisible; it would stop being
     * invisible the first time anything re-opened the cartridge. */
    memcpy((void *)HW_ROM_HEADER_BUF, header, HW_CARD_ROM_HEADER_SIZE);
    sCardIr = header[0x0C] == 'I';
    fprintf(stderr, "pokeplatinum-pc: rom: %s\n", path);
    return 0;
}
#endif

/*
 * Every card read is a pread of a 134 MB file, so the first touch of any
 * region is a disk read inside the frame that asked for it; which is
 * exactly the shape of a stall the pacer cannot do anything about. The span
 * is here so the late-frame autopsy can say whether that is what it was.
 */
#if defined(PC_BW_ROMVIEW)
/* Black/White (games/ndsrec/pc/src/pc_bw_romview.c): content packages as a
 * ROM view; 1 = answered, 0 = nothing claimed, read the cartridge. */
int pc_bw_romview_read(u32 src, void *dst, u32 len);
#endif

#if defined(__wasm__)
static void rom_read(u32 src, void *dst, u32 len)
{
    int32_t rc;

#if defined(PC_BW_ROMVIEW)
    if (pc_bw_romview_read(src, dst, len)) {
        return;
    }
#endif

    /* Same contract as the pread below: a read the image cannot satisfy
     * whole is a fatal port bug, not a short read to hand back. */
    if (src > sRomSize || len > sRomSize - src) {
        pc_wasm_fatalf("pc_card_rom: read of %u bytes at rom:%#x is past "
                       "the %u-byte image",
                       (unsigned)len, (unsigned)src, (unsigned)sRomSize);
    }
    PC_BENCH_BEGIN(bench_t);
    rc = np_host_rom_read(src, dst, len);
    PC_BENCH_END(PC_BENCH_IO, bench_t);
    if (rc != 0) {
        pc_wasm_fatalf("pc_card_rom: read of %u bytes at rom:%#x returned %d",
                       (unsigned)len, (unsigned)src, (int)rc);
    }
}
#else
static void rom_read(u32 src, void *dst, u32 len)
{
    ssize_t n;

    PC_BENCH_BEGIN(bench_t);
    n = pread(sRomFd, dst, len, (off_t)src);
    PC_BENCH_END(PC_BENCH_IO, bench_t);
    if (n != (ssize_t)len) {
        fprintf(stderr,
                "pc_card_rom: read of %u bytes at rom:%#x returned %zd\n",
                (unsigned)len, (unsigned)src, n);
        abort();
    }
}
#endif
#if defined(PC_CARD_READ_TIME)
/*
 * A card read takes time. On a console the reading thread sleeps while the
 * card's DMA runs and the idle thread halts, so every VBlank that falls
 * inside a long read is delivered then, and its handler runs the game's
 * VBlank work in the middle of whatever the reader was doing. Here a read
 * is instant, so a logical frame that loads a lot (HG/SS's FieldMap_Init:
 * the map, the field effects, the overworld sprites) used to see no VBlank
 * at all: the field effects' 32 VBlank-queue tasks (SysTask_CreateOnVBlankQueue,
 * system.c's 32-entry queue) were never drained in between, 22 creations
 * failed, and among them the task that uploads the overworld sprites'
 * textures (ov01_021F19B4), so the player drew from VRAM nothing had written.
 *
 * The model is the card's own timing. The SDK reads the ROM in 0x200-byte
 * pages (NitroSDK card_rom.c: CARD_ROM_PAGE_SIZE), one page-read command
 * each, and keeps the last page it read in a one-page cache
 * (CARDi_ReadFromCache), so a read costs the pages it touches less a first
 * page the previous read already fetched. A page costs its 8 command bytes,
 * the leading gap and the 0x200 data bytes, one card clock per byte, at the
 * clock and gap the cartridge header gives for normal commands (ROMCTRL at
 * header 0x60: gap1 in bits 0-12, bit 27 the clock, 33.51 MHz / 5 or / 8).
 * HG/SS's header (0x00416657: gap1 0x657, 6.7 MHz) makes a page 10715 bus
 * cycles, about 52 pages per 560190-cycle VBlank. A frame's worth owed
 * within one logical frame delivers one VBlank (OS_Halt, the port's whole
 * frame) from inside the read. Reads that fit in a frame cost nothing: the
 * frame's own VBlank, at the next natural halt, absorbs them (pc_card_step
 * clears the count). Only when the reader could be interrupted, as on
 * hardware: IME, the VBlank enable and the CPSR I bit; otherwise the time
 * stays owed until it can. Deterministic: a function of the reads only.
 *
 * Counting bytes instead (0x8000 a frame, the first version) charged a
 * 164-byte read 164 bytes where the card moves a whole page and waits out
 * its gap. FieldEffectManager_InitRenderers creates 56 one-shot VBlank
 * tasks (sub_02069714, texture loads) between small NARC reads; by bytes,
 * 33-35 of them could fall between two VBlanks, the 32-entry queue refused
 * the rest, and when the one refused was ov01_021FA6E0 (the map objects'
 * texture loader) every sprite loaded after it drew untextured and a
 * trainer's approach waited forever on its texture. By pages the worst
 * stretch is 16.
 *
 * Per game (Makefile): D/P/Pt do not define it yet, so their frames and
 * regression hashes are unchanged.
 */
#define PC_CARD_PAGE_SIZE 0x200u
#define PC_CARD_CYCLES_PER_VBLANK 560190u /* 355 dots x 263 lines x 6, as pc_timers.c */

static u32 sCardReadOwed;       /* bus cycles of card time this frame */
static u32 sCardPageCycles;     /* one page read, from the header's ROMCTRL */
static u32 sCardCachedPage = 0xFFFFFFFFu;
static int sCardReadHalting;

static void card_read_elapse(u32 src, u32 len)
{
    u32 first, last, pages;

    if (len == 0) {
        return;
    }
    if (sCardPageCycles == 0) {
        u32 romctrl;

        rom_read(0x60, &romctrl, sizeof romctrl);
        sCardPageCycles = (8u + (romctrl & 0x1FFFu) + PC_CARD_PAGE_SIZE)
                          * ((romctrl & (1u << 27)) ? 8u : 5u);
    }
    first = src / PC_CARD_PAGE_SIZE;
    last = (src + len - 1) / PC_CARD_PAGE_SIZE;
    pages = last - first + 1 - (first == sCardCachedPage);
    sCardCachedPage = last;
    sCardReadOwed += pages * sCardPageCycles;
    while (sCardReadOwed >= PC_CARD_CYCLES_PER_VBLANK && !sCardReadHalting) {
        if (!(reg_OS_IME & 1) || !(reg_OS_IE & OS_IE_V_BLANK)
            || OS_GetCpsrIrq() != OS_INTRMODE_IRQ_ENABLE) {
            return;
        }
        sCardReadOwed -= PC_CARD_CYCLES_PER_VBLANK;
        sCardReadHalting = 1;
        OS_Halt();
        sCardReadHalting = 0;
    }
}
#endif

void CARDi_ReadRom(u32 dma, const void *src, void *dst, u32 len,
                   MIDmaCallback callback, void *arg, BOOL is_async)
{
    (void)dma;
    (void)is_async;
#if defined(__wasm__)
    if (sRomSize == 0) {
        pc_wasm_fatal("pc_card_rom: read before pc_rom_init");
    }
#else
    if (sRomFd < 0) {
        fprintf(stderr, "pc_card_rom: read before pc_rom_init\n");
        abort();
    }
#endif
    rom_read((u32)src, dst, len);
#if defined(PC_CARD_READ_TIME)
    card_read_elapse((u32)src, len);
#endif
    if (callback) {
        callback(arg);
    }
}

void CARD_Init(void)
{
    CARDiCommon *const p = &cardi_common;
    if (!p->flag) {
        p->flag = CARD_STAT_INIT;
        p->src = p->dst = p->len = 0;
        p->dma = (u32)~0;
        p->callback = NULL;
        p->callback_arg = NULL;
        /* The SDK's own state init: lock_owner = OS_LOCK_ID_ERROR (zeroed
         * commons read as "locked by id 0" and the first CARD_LockBackup
         * sleeps forever, measured, not hypothetical), and cmd wired to
         * the file-static command block. What the SDK's CARD_Init does
         * beyond this, the transfer thread, the pull-out callback, the
         * rom accessor, has no counterpart when every request completes
         * synchronously. */
        CARDi_InitCommon();
    }
}

/* Every read completed before it returned; asynchrony is never pending. */
void CARD_WaitRomAsync(void)
{
}

BOOL CARD_TryWaitRomAsync(void)
{
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* The card bus                                                        */
/* ------------------------------------------------------------------ */

/*
 * The ROM half of the card's register interface, for an SDK whose card layer
 * runs as recompiled code rather than being replaced above (TWL-SDK 5:
 * Black/White read the ROM ID after every read and their download-play
 * signature block at start-up with the CPU, CARDi_ReadRomWithCPU). The
 * recompiled code reaches it through armrec_card_load/armrec_card_store
 * (tools/armrec/armrec_rt.h, ARMREC_CARD_HOOK).
 *
 * A store to ROMCTRL (0x040001A4) with bit 31 set sends the command in the
 * eight bytes at 0x040001A8 and clocks in a reply of the size bits 24-26
 * name (none, 0x100 << n, or 4 bytes for 7). ROMCTRL then reads busy
 * (bit 31) with a word ready (bit 23) until the last word has been loaded
 * from 0x04100010; a reply of no words completes at once. The reply is
 * built whole when the command is sent: the bus has no timing here, as the
 * ROM reads above have none.
 *
 * Commands, by what the SDK sends in normal (post-boot) mode: 0xB7 the data
 * read (big-endian ROM address in bytes 1-4), 0xB8 the chip ID; 0x00 and
 * 0x90 are the same two in raw (header) mode. The bus scrambling the
 * hardware applies in normal mode is not part of what the CPU sees, so
 * there is nothing to model: the reply is the ROM's bytes. Any other
 * command answers an undriven bus, 0xFF.
 *
 * The chip ID is the one the firmware leaves at HW_BOOT_CHECK_INFO_BUF and
 * HW_RED_RESERVED (pc_rom_init); the SDK compares a fresh read with it to
 * tell a pulled-out card. A Macronix-style ID for a 256 MiB mask ROM
 * (maker 0xC2, size byte 0xFF); bit 29, which makes TWL-SDK poll a status
 * command, is clear, as on a NTR-protocol card.
 */
#include "armrec_rt.h"

#define PC_CARD_CHIP_ID   0x0000FFC2u
#define PC_CARD_ROMCTRL   (ARM_CARD_BASE + 0x04u)
#define PC_CARD_CMD       (ARM_CARD_BASE + 0x08u)
#define PC_CARD_BUSY      0x80000000u
#define PC_CARD_READY     0x00800000u
#define PC_CARD_REPLY_MAX 0x4000u /* 0x100 << 6, the largest block */

static u8 sCardReply[PC_CARD_REPLY_MAX];
static u32 sCardReplyLen;
static u32 sCardReplyPos;

/* ROM bytes for the bus: past the end of the image the bus is undriven. */
static void card_rom_fetch(u32 addr, u8 *dst, u32 len)
{
#if defined(__wasm__)
    u32 have = addr < sRomSize ? sRomSize - addr : 0;
#else
    off_t end = lseek(sRomFd, 0, SEEK_END);
    u32 have = end > (off_t)addr ? (u32)(end - (off_t)addr) : 0;
#endif
    if (have > len) {
        have = len;
    }
    if (have) {
        rom_read(addr, dst, have);
    }
    memset(dst + have, 0xFF, len - have);
}

static void card_send_command(u32 ctrl)
{
    const u8 *cmd = (const u8 *)(uintptr_t)PC_CARD_CMD;
    u32 bs = (ctrl >> 24) & 7u;
    u32 len = bs == 0 ? 0 : bs == 7 ? 4 : 0x100u << bs;
    u32 addr = ((u32)cmd[1] << 24) | ((u32)cmd[2] << 16) | ((u32)cmd[3] << 8) | cmd[4];
    u32 i;

    switch (cmd[0]) {
    case 0xB7:
        card_rom_fetch(addr, sCardReply, len);
        break;
    case 0x00:
        for (i = 0; i < len; i += 0x200u) {
            card_rom_fetch(0, sCardReply + i, len - i < 0x200u ? len - i : 0x200u);
        }
        break;
    case 0xB8:
    case 0x90:
        for (i = 0; i < len; i += 4) {
            u32 id = PC_CARD_CHIP_ID;
            memcpy(sCardReply + i, &id, 4);
        }
        break;
    default:
        memset(sCardReply, 0xFF, len);
        break;
    }
    sCardReplyLen = len;
    sCardReplyPos = 0;
}

static u32 card_romctrl(void)
{
    u32 ctrl = *(volatile u32 *)(uintptr_t)PC_CARD_ROMCTRL & ~(PC_CARD_BUSY | PC_CARD_READY);
    if (sCardReplyPos < sCardReplyLen) {
        ctrl |= PC_CARD_BUSY | PC_CARD_READY;
    }
    return ctrl;
}

/*
 * The card's SPI bus (AUXSPICNT 0x040001A0, AUXSPIDATA 0x040001A2), where
 * the cartridge's IR transceiver sits in front of the save chip. Only on a
 * cartridge that has one (game code 'I...'); elsewhere the data register
 * stays the plain register it was, because the save chip itself is reached
 * through card requests (card_backup_request), never this bus.
 *
 * A store to AUXSPIDATA, with the bus enabled in serial mode, clocks one
 * byte out and the reply in; the transfer has no timing here, so busy
 * (AUXSPICNT bit 7) never shows. Chip select stays asserted after a byte
 * while AUXSPICNT bit 6 (hold) is set and drops after a byte sent without
 * it, or when the bus is disabled. The first byte of a selection is the IR
 * chip's command:
 *
 *   0x08  the chip's ID: every later byte answers 0xAA. HG/SS check it on
 *         every field load (fieldmap.c ov01_021E662C), Black/White in ov231.
 *   0x00  pass-through: the rest of the selection is the save chip's own
 *         command, of which READ (0x03, 24-bit address) and RDSR (0x05,
 *         never busy) are answered from the image. Writes go through card
 *         requests on these SDKs, so no other command reaches here.
 *
 * Any other byte (the IR send/receive commands, with no Pokewalker or
 * other console in front of the window) answers an undriven bus, 0xFF.
 */
#define PC_AUXSPICNT      ARM_CARD_BASE
#define PC_AUXSPIDATA     (ARM_CARD_BASE + 0x02u)
#define PC_AUXSPI_HOLD    0x0040u
#define PC_AUXSPI_SERIAL  0x2000u
#define PC_AUXSPI_ENABLE  0x8000u

enum { IR_DESELECTED, IR_ID, IR_FLASH, IR_UNDRIVEN };
static int sIrState;
static u8 sFlashCmd;
static u32 sFlashPos;   /* bytes of the flash command clocked so far */
static u32 sFlashAddr;

static u8 card_flash_byte(u8 out)
{
    u32 pos = sFlashPos++;

    if (pos == 0) {
        sFlashCmd = out;
        sFlashAddr = 0;
        return 0xFF;
    }
    switch (sFlashCmd) {
    case 0x03:
        if (pos <= 3) {
            sFlashAddr = (sFlashAddr << 8) | out;
            return 0xFF;
        }
        if (sBackupImage == NULL || sBackupSize == 0) {
            return 0xFF;
        }
        return sBackupImage[sFlashAddr++ % sBackupSize];
    case 0x05:
        return 0x00;
    default:
        return 0xFF;
    }
}

static void card_spi_transfer(void)
{
    u16 cnt = *(volatile u16 *)(uintptr_t)PC_AUXSPICNT;
    u8 out = *(volatile u8 *)(uintptr_t)PC_AUXSPIDATA;
    u8 in;

    if ((cnt & (PC_AUXSPI_ENABLE | PC_AUXSPI_SERIAL))
        != (PC_AUXSPI_ENABLE | PC_AUXSPI_SERIAL)) {
        return;
    }
    switch (sIrState) {
    case IR_DESELECTED:
        sIrState = out == 0x08 ? IR_ID : out == 0x00 ? IR_FLASH : IR_UNDRIVEN;
        sFlashPos = 0;
        in = 0xFF;
        break;
    case IR_ID:
        in = 0xAA;
        break;
    case IR_FLASH:
        in = card_flash_byte(out);
        break;
    default:
        in = 0xFF;
        break;
    }
    *(volatile u16 *)(uintptr_t)PC_AUXSPIDATA = in;
    if (!(cnt & PC_AUXSPI_HOLD)) {
        sIrState = IR_DESELECTED;
    }
}

void armrec_card_store(uint32_t a, uint32_t v, int size)
{
    switch (size) {
    case 4: *(volatile u32 *)(uintptr_t)a = v; break;
    case 2: *(volatile u16 *)(uintptr_t)a = (u16)v; break;
    default: *(volatile u8 *)(uintptr_t)a = (u8)v; break;
    }
    if (sCardIr) {
        if (a <= PC_AUXSPIDATA && a + (u32)size > PC_AUXSPIDATA) {
            card_spi_transfer();
        } else if (a <= PC_AUXSPICNT + 1u && a + (u32)size > PC_AUXSPICNT + 1u
                   && !(*(volatile u16 *)(uintptr_t)PC_AUXSPICNT & PC_AUXSPI_ENABLE)) {
            sIrState = IR_DESELECTED;
        }
    }
    /* The start bit is ROMCTRL's top byte. */
    if (a <= PC_CARD_ROMCTRL + 3u && a + (u32)size > PC_CARD_ROMCTRL + 3u) {
        u32 ctrl = *(volatile u32 *)(uintptr_t)PC_CARD_ROMCTRL;
        if (ctrl & PC_CARD_BUSY) {
            card_send_command(ctrl);
        }
        *(volatile u32 *)(uintptr_t)PC_CARD_ROMCTRL = card_romctrl();
    }
}

uint32_t armrec_card_load(uint32_t a, int size)
{
    u32 v;

    if (a - ARM_CARD_DATA < 4u) {
        v = 0xFFFFFFFFu;
        if (sCardReplyPos < sCardReplyLen) {
            memcpy(&v, sCardReply + sCardReplyPos, 4);
            sCardReplyPos += 4;
            *(volatile u32 *)(uintptr_t)PC_CARD_ROMCTRL = card_romctrl();
        }
        v >>= 8 * (a & 3u);
    } else {
        if (a - PC_CARD_ROMCTRL < 4u) {
            *(volatile u32 *)(uintptr_t)PC_CARD_ROMCTRL = card_romctrl();
        }
        v = *(volatile u32 *)(uintptr_t)(a & ~3u) >> (8 * (a & 3u));
    }
    return size == 4 ? v : size == 2 ? (v & 0xFFFFu) : (v & 0xFFu);
}

#if defined(ARMREC_TWL)
/*
 * What a console leaves in the shared page besides the header, for a
 * TWL-SDK game whose card layer is its own recompiled code: the card's chip
 * ID at HW_RED_RESERVED and HW_BOOT_CHECK_INFO_BUF (the pulled-out check
 * compares a fresh 0xB8 read with the latter), and the boot type at
 * HW_WM_BOOT_BUF, 1 for a card boot, which TWL-SDK's crt0 also stores when
 * it finds 0 (the host does not run crt0). CARD_Init enables the card and
 * copies the header to HW_CARD_ROM_HEADER only on a card boot.
 */
static void card_firmware_words(void)
{
    *(volatile u32 *)HW_RED_RESERVED = PC_CARD_CHIP_ID;
    *(volatile u32 *)HW_BOOT_CHECK_INFO_BUF = PC_CARD_CHIP_ID;
    *(volatile u16 *)HW_WM_BOOT_BUF = 1;
}
#endif

/* ------------------------------------------------------------------ */
/* The save chip                                                       */
/* ------------------------------------------------------------------ */

/* On hardware every backup operation becomes a CARDi_Request: the command
 * block at cardi_common.cmd goes to the ARM7 over PXI, the ARM7 runs the
 * SPI transaction, writes cmd->result and acks. There is no ARM7 here, so
 * the request IS the operation: it runs against a host-side image,
 * synchronously, before returning, the same completed-before-you-looked
 * schedule as the DMA and ROM models.
 *
 * The chip spec (size, sector/page geometry) is NOT this file's problem:
 * CARDi_IdentifyBackupCore fills cmd->spec host-side from the type the
 * game names before any request is issued. The image allocates lazily at
 * spec.total_size, filled 0xFF (an erased chip, the honest state of a
 * save that has never been written; the game's own "no save data" path
 * handles it exactly as it would a fresh cartridge).
 *
 * Persistence: $PC_SAVE names the image file (default: beside the ROM,
 * <rom>.sav). Loaded whole if present, written through whole on every
 * mutating request; a save-file write is rare and small (<=512 KB), and
 * write-through means a crash never loses a completed save. PC_SAVE=none
 * disables persistence for scripted runs.
 *
 * The wasm guest has no files: the image is loaded from the runtime
 * (np_host_save_load) when the chip is identified, published through
 * pc_wasm_frame.save_image/save_size, flagged in pc_wasm_frame.save_dirty
 * on every mutation, and stored with np_host_save_store at the same settle
 * points the file is written at. No rotation, no temporaries; PC_SAVE=none
 * still disables persistence, any other PC_SAVE value is ignored.
 */
#include <nitro/card/backup.h>

#if !defined(__wasm__)
static char sSavePath[PATH_MAX];

/*
 * ROLLING BACKUP: the previous completed save, kept.
 *
 * The rotate happens once per SAVE, before the first write of it, and copies
 * what is on disk; which is by definition the last save that finished. Not
 * per sector write: the game's save is 506 flushes, and rotating on each
 * would leave `.sav.bak` holding a half-written file, which is worse than no
 * backup at all because it looks like one.
 *
 * A save is one burst, and that was measured rather than assumed: replaying
 * the scripted flow that saves, all 506 writes land inside a single frame,
 * and there is no second save to be confused with. Sixty frames of quiet is
 * far clear of the gap inside a burst and a second short of any two saves a
 * person could make.
 *
 * Best-effort and silent on failure. A backup that could stop a save from
 * happening would have inverted the point of it.
 */
static void backup_rotate(void)
{
    extern unsigned long long pc_irq_frames(void);
    static unsigned long long last_write;
    static int seen;
    unsigned long long now = pc_irq_frames();
    /*
     * Static, not automatic. These two are 66,560 bytes between them, which a
     * desktop thread never notices and a console one does not have: on the
     * 3DS the prologue moved the stack pointer clean out of the thread's
     * stack, every store of the frame went to unmapped memory, and the return
     * read a link register that had never been written, a branch to zero,
     * every time the player saved. The function is already single-threaded
     * and non-reentrant (`seen` and `last_write` below are static too), so
     * there is nothing a per-call copy was buying.
     */
    static char bak[PATH_MAX];
    static char buf[65536];
    int in, out;
    ssize_t n;

    if (seen && now - last_write < 60) {
        last_write = now;
        return;                 /* still the same save */
    }
    seen = 1;
    last_write = now;

    snprintf(bak, sizeof bak, "%s.bak", sSavePath);
    in = open(sSavePath, O_RDONLY | O_BINARY);
    if (in < 0) {
        return;                 /* nothing saved yet: nothing to keep */
    }
    out = open(bak, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (out < 0) {
        close(in);
        return;
    }
    while ((n = read(in, buf, sizeof buf)) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            break;
        }
    }
    close(in);
    close(out);
    fprintf(stderr, "pc_card_rom: previous save kept as %s\n", bak);
}

/*
 * The save file is never the file being written. Opening it O_TRUNC and
 * writing 512 KB leaves a window where the player's save is a partial file,
 * and the console is where that window is reachable: a close request or a
 * flat battery lands whenever the system decides, not between frames. So the
 * image goes to a temporary beside it, and only a complete temporary is
 * renamed over the save.
 *
 * The unlink before the rename is the 3DS's. POSIX rename replaces the
 * destination; libctru's SD device is FAT through FSUSER_RenameFile, which
 * fails when the destination exists. Removing it first costs a window where
 * neither name holds the current save, but the temporary is complete by
 * then, so backup_ready() below adopts it on the next launch, and that is a
 * smaller hole than the one it closes.
 *
 * A failed rename falls back to writing the save directly rather than losing
 * it. That is the old behaviour and the old risk, taken only when the safe
 * path is unavailable.
 */
static int backup_write(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    int ok;

    if (fd < 0) {
        return 0;
    }
    ok = pwrite(fd, sBackupImage, sBackupSize, 0) == (ssize_t)sBackupSize;
    close(fd);
    return ok;
}
#else
/* The runtime holds the save: set once the image was loaded from it, clear
 * for PC_SAVE=none, which keeps the chip in memory only. */
static int sSaveHost;
#endif

/*
 * Why the write is deferred, and what it cost to learn.
 *
 * A save is one burst of about 506 card commands, and this used to write the
 * whole 512 KB image on every one of them, 259 MB of file writes for one
 * save. On a desktop and under an emulator that is invisible. On a real
 * console it is minutes: measured on an Old 3DS, the save froze the port past
 * the 20-second hang detector, which killed the process partway through the
 * burst and left a half-written save the game then refused to load.
 *
 * Nothing the game can observe depends on the file. Every read, verify and
 * erase above works on sBackupImage in memory; the file is only ever written.
 * So a command marks the image dirty and the write happens once, after the
 * burst has been quiet for CARD_SETTLE_FRAMES; one write per save instead of
 * 506.
 *
 * The window this opens is a power cut in the fraction of a second between the
 * last command and the write, and it is closed from three sides: the settle is
 * short, the exit path syncs, and on the console the hang detector syncs before
 * it gives up. CARD_DIRTY_MAX is the backstop for a game that dribbles writes
 * out slowly enough never to settle.
 */
#define CARD_SETTLE_FRAMES 8ull
#define CARD_DIRTY_MAX     300ull

void pc_card_backup_sync(void);

static int sBackupDirty;
static unsigned long long sBackupDirtyAt;

static void backup_touch(void)
{
    extern unsigned long long pc_irq_frames(void);

#if defined(__wasm__)
    /* While the guest was parked the runtime may have stored the image and
     * cleared save_dirty: what was pending is persisted, and this touch is
     * the start of a new burst whose settle counts from now. */
    if (sSaveHost && !pc_wasm_frame.save_dirty) {
        sBackupDirty = 0;
    }
#endif
    if (!sBackupDirty) {
        sBackupDirty = 1;
        sBackupDirtyAt = pc_irq_frames();
    }
#if defined(__wasm__)
    if (sSaveHost) {
        pc_wasm_frame.save_dirty = 1;
    }
#endif
}

#if defined(__wasm__)
static void backup_write_now(void)
{
    int32_t rc;

    if (!sSaveHost || !pc_wasm_frame.save_dirty) {
        return;                 /* no persistence, or the runtime stored it */
    }
    rc = np_host_save_store(sBackupImage, sBackupSize);
    if (rc != 0) {
        /* save_dirty stays 1: the runtime may store it itself while the
         * guest is parked, and the exit sync tries again. */
        fprintf(stderr, "pc_card_rom: save store to the runtime failed (%d)\n",
                (int)rc);
        return;
    }
    pc_wasm_frame.save_dirty = 0;
}
#else

static void backup_write_now(void)
{
    static char tmp[PATH_MAX];

    if (sSavePath[0] == '\0') {
        return;
    }
    backup_rotate();

    snprintf(tmp, sizeof tmp, "%s.tmp", sSavePath);
    if (backup_write(tmp)) {
        remove(sSavePath);
        if (rename(tmp, sSavePath) == 0) {
            return;
        }
        remove(tmp);
    }
    if (!backup_write(sSavePath)) {
        fprintf(stderr, "pc_card_rom: save write to %s failed\n", sSavePath);
    }
}
#endif

/*
 * Write the image out now if anything is waiting. Public because more than the
 * frame boundary needs it: the exit path calls it, and on the console so does
 * the hang detector; a port that is about to be given up on should not take
 * the player's save with it.
 */
void pc_card_backup_sync(void)
{
#if defined(__wasm__)
    /* save_dirty is the authority, not sBackupDirty: a store that failed
     * left it set with sBackupDirty already cleared. */
    sBackupDirty = 0;
    backup_write_now();
#else
    if (!sBackupDirty) {
        return;
    }
    sBackupDirty = 0;
    backup_write_now();
#endif
}

/* One guest frame. OS_Halt calls this, so both hosts get it from the same
 * place the rest of the port's per-frame work happens. */
void pc_card_step(void)
{
    extern unsigned long long pc_irq_frames(void);
    unsigned long long waited;

#if defined(PC_CARD_READ_TIME)
    /* A frame boundary the game reached by itself: the reads since the last
     * one fitted in its frame (card_read_elapse). */
    if (!sCardReadHalting) {
        sCardReadOwed = 0;
    }
#endif
    if (!sBackupDirty) {
        return;
    }
#if defined(__wasm__)
    if (sSaveHost && !pc_wasm_frame.save_dirty) {
        sBackupDirty = 0;       /* the runtime stored it while we were parked */
        return;
    }
#endif
    waited = pc_irq_frames() - sBackupDirtyAt;
    if (waited >= CARD_SETTLE_FRAMES || waited >= CARD_DIRTY_MAX) {
        pc_card_backup_sync();
    }
}

static int backup_ready(CARDiCommandArg *cmd)
{
    if (sBackupImage) {
        return 1;
    }
    if (cmd->spec.total_size == 0) {
        return 0;
    }
    sBackupSize = cmd->spec.total_size;
    sBackupImage = malloc(sBackupSize);
    if (!sBackupImage) {
        return 0;
    }
    memset(sBackupImage, 0xFF, sBackupSize);

#if defined(__wasm__)
    {
        const char *env = getenv("PC_SAVE");
        int32_t rc;

        if (env && strcmp(env, "none") == 0) {
            fprintf(stderr, "pc_card_rom: save: none (PC_SAVE=none)\n");
            return 1;
        }
        rc = np_host_save_load(sBackupImage, sBackupSize);
        if (rc < 0) {
            pc_wasm_fatalf("pc_card_rom: the runtime failed to load the "
                           "%u-byte save", (unsigned)sBackupSize);
        }
        /* rc == 0: no save yet, the erased fill above is the chip. */
        sSaveHost = 1;
        pc_wasm_frame.save_dirty = 0;
        pc_wasm_frame.save_size = sBackupSize;
        pc_wasm_frame.save_image = (uint32_t)(uintptr_t)sBackupImage;
        /* Same reason as the file hosts' atexit below. */
        atexit(pc_card_backup_sync);
        fprintf(stderr, "pc_card_rom: save: %s (%u bytes)\n",
                rc > 0 ? "loaded from the runtime" : "new, erased chip",
                (unsigned)sBackupSize);
    }
    return 1;
#else
#if defined(__3DS__)
    /*
     * No environment to read on this console, and the rule the other hosts
     * follow (the save sits beside the ROM) cannot be applied: the usual
     * ROM is packed inside the 3dsx and romfs: is read-only. 3ds/src/3ds_rom.c
     * names the SD-card directory instead and makes it if the card has none.
     */
    {
        extern const char *rom_save_path(void);

        snprintf(sSavePath, sizeof sSavePath, "%s", rom_save_path());
    }
#else
    {
        const char *env = getenv("PC_SAVE");
        if (env && strcmp(env, "none") == 0) {
            sSavePath[0] = '\0';
        } else if (env) {
            snprintf(sSavePath, sizeof sSavePath, "%s", env);
        } else {
            char buf[PATH_MAX];
            snprintf(sSavePath, sizeof sSavePath, "%s.sav",
                     rom_path(buf, sizeof buf));
        }
    }
#endif
    if (sSavePath[0]) {
        int fd;

        /* The save is written after the burst settles, so a run that ends
         * between the last card command and that write would otherwise lose
         * it. Registered once, here, because this is where the path is first
         * known. */
        atexit(pc_card_backup_sync);

        fd = open(sSavePath, O_RDONLY | O_BINARY);
        if (fd < 0) {
            /*
             * The save is gone but backup_flush()'s temporary is not: the run
             * before this one was killed between the remove and the rename.
             * The temporary was complete before either happened, so it is the
             * save, adopt it under the right name rather than starting the
             * player on an erased chip.
             */
            char tmp[PATH_MAX];

            snprintf(tmp, sizeof tmp, "%s.tmp", sSavePath);
            if (rename(tmp, sSavePath) == 0) {
                fprintf(stderr, "pc_card_rom: recovered %s from %s\n",
                        sSavePath, tmp);
                fd = open(sSavePath, O_RDONLY | O_BINARY);
            }
        }
        if (fd >= 0) {
            ssize_t n = pread(fd, sBackupImage, sBackupSize, 0);
            close(fd);
            fprintf(stderr, "pc_card_rom: save: %s (%zd bytes)\n",
                    sSavePath, n);
        } else {
            fprintf(stderr, "pc_card_rom: save: %s (new, erased chip)\n",
                    sSavePath);
        }
    }
    return 1;
#endif
}

/* One backup request against the image, as the ARM7 runs it on the command
 * block: cmd->result says how it went. */
static BOOL card_backup_request(CARDiCommandArg *cmd, int req_type)
{
    switch (req_type) {
    case CARD_REQ_INIT:
    case CARD_REQ_ACK:
    case CARD_REQ_IDENTIFY:
    case CARD_REQ_READ_ID:
        break;
    case CARD_REQ_READ_BACKUP:
        if (!backup_ready(cmd)) {
            cmd->result = CARD_RESULT_FAILURE;
            return FALSE;
        }
        memcpy((void *)cmd->dst, sBackupImage + cmd->src, cmd->len);
        break;
    case CARD_REQ_WRITE_BACKUP:
    case CARD_REQ_PROGRAM_BACKUP:
        if (!backup_ready(cmd)) {
            cmd->result = CARD_RESULT_FAILURE;
            return FALSE;
        }
        memcpy(sBackupImage + cmd->dst, (const void *)cmd->src, cmd->len);
        backup_touch();
        break;
    case CARD_REQ_VERIFY_BACKUP:
        if (!backup_ready(cmd) ||
            memcmp(sBackupImage + cmd->dst, (const void *)cmd->src,
                   cmd->len) != 0) {
            cmd->result = CARD_RESULT_FAILURE;
            return FALSE;
        }
        break;
    case CARD_REQ_ERASE_PAGE_BACKUP:
        if (!backup_ready(cmd)) {
            cmd->result = CARD_RESULT_FAILURE;
            return FALSE;
        }
        memset(sBackupImage + cmd->dst, 0xFF, cmd->spec.page_size);
        backup_touch();
        break;
    case CARD_REQ_ERASE_SECTOR_BACKUP:
        if (!backup_ready(cmd)) {
            cmd->result = CARD_RESULT_FAILURE;
            return FALSE;
        }
        memset(sBackupImage + cmd->dst, 0xFF, cmd->spec.sect_size);
        backup_touch();
        break;
#if !defined(PC_GAME_DP) || defined(ARMREC_TWL) || defined(PC_HOST_SDK42)
    /* SDK 4.2's subsector erase; 3.2's chip spec has no subsector size and
     * its SDK never issues the request. */
    case CARD_REQ_ERASE_SUBSECTOR_BACKUP:
        if (!backup_ready(cmd)) {
            cmd->result = CARD_RESULT_FAILURE;
            return FALSE;
        }
        memset(sBackupImage + cmd->dst, 0xFF, cmd->spec.subsect_size);
        backup_touch();
        break;
#endif
    case CARD_REQ_ERASE_CHIP_BACKUP:
        if (!backup_ready(cmd)) {
            cmd->result = CARD_RESULT_FAILURE;
            return FALSE;
        }
        memset(sBackupImage, 0xFF, sBackupSize);
        backup_touch();
        break;
    case CARD_REQ_READ_STATUS:
        /* The status register with no operation in flight: the chip's
         * quiescent value, which the spec records. */
        *(u8 *)cmd->dst = cmd->spec.initial_status;
        break;
    case CARD_REQ_WRITE_STATUS:
        break;
    default:
        fprintf(stderr, "pc_card_rom: unmodeled card request %d\n", req_type);
        cmd->result = CARD_RESULT_UNSUPPORTED;
        return FALSE;
    }
    cmd->result = CARD_RESULT_SUCCESS;
    return TRUE;
}

BOOL CARDi_Request(CARDiCommon *p, int req_type, int retry_count)
{
    (void)retry_count;
    return card_backup_request(p->cmd, req_type);
}

#if defined(ARMREC_TWL)
/*
 * TWL-SDK's backup requests, which this port does not replace: the ARM9's
 * CARDi_Request (Black 0x020763F0) flushes the 0x60-byte command block,
 * sets CARD_STAT_REQ, sends the request number on the FS tag (and, for
 * CARD_REQ_INIT, the block's address as a second word), then sleeps until
 * its FS receiver (Black 0x020763BC), which acts only on a word with the
 * error bit set as the ARM7's CARDi_SendPxi sends it, clears the flag.
 * The ARM7's side, here: INIT records the block, every other request runs
 * against the image (card_backup_request) and is answered at once, so the
 * flag is clear before the ARM9 looks.
 */
static CARDiCommandArg *sCardCmd;
static int sCardAwaitBlock;

extern void pc_pxi_reply_err(int tag, u32 data, BOOL err);

static void card_fs_responder(u32 data)
{
    if (sCardAwaitBlock) {
        sCardAwaitBlock = 0;
        sCardCmd = (CARDiCommandArg *)(uintptr_t)data;
        sCardCmd->result = CARD_RESULT_SUCCESS;
    } else if (data == CARD_REQ_INIT) {
        sCardAwaitBlock = 1;
        return;
    } else if (sCardCmd == NULL) {
        pc_wasm_fatalf("pc_card_rom: card request %u before CARD_REQ_INIT",
                       (unsigned)data);
    } else {
        (void)card_backup_request(sCardCmd, (int)data);
    }
    pc_pxi_reply_err(PXI_FIFO_TAG_FS, CARD_REQ_ACK, TRUE);
}
#endif

/* The async task queue: tasks ran on a dedicated card thread on hardware
 * because requests slept on the ARM7. Every request above completes
 * synchronously, so the task can simply run here and now, the callback
 * ordering the caller observes is the legal "finished immediately"
 * schedule. The SDK's CARDi_SetTask also bumped thread priority and woke
 * the card thread; with no card thread there is nothing to wake. */
void CARDi_SetTask(void (*task)(CARDiCommon *))
{
    CARDiCommon *const p = &cardi_common;
    p->cur_th = OS_GetCurrentThread();
    task(p);
}
