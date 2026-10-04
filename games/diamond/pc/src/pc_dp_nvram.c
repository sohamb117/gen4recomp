/*
 * The ARM7's firmware-flash (NVRAM) responder on PXI tag 4, for Diamond/Pearl.
 *
 * D's boot reads the Nintendo WFC connection settings out of the firmware
 * flash: DWC_backup.s NVRAMm_ExecuteCommand sends a command packet on tag 4
 * and then spins on `nv_cb_occurred` until Callback_NVRAM, the tag's receive
 * callback, sets it. Platinum's DWC init is stubbed (pc/src/pc_dwc_auth.c), so
 * its build never reaches this tag; with no responder pc_pxi.c dropped the
 * words and D hung before its first frame.
 *
 * Wire format, from the ARM7 side (NitroSDK libraries/nvram/src/nvram.c,
 * NVRAM_AnalyzeCommand): 32-bit words, each carrying a 16-bit datum at index
 * bits 16-19; the first word has SPI_PXI_START_BIT (0x02000000), the last
 * SPI_PXI_END_BIT (0x01000000). The command is datum[0] bits 8-15. Operands:
 *   READ/FAST_READ (0x23/0x24): addr = (d0 & 0xff)<<16 | d1,
 *                               size = d2<<16 | d3, buf = d4<<16 | d5
 *   PW/PP (0x25/0x26):          addr as above, size = d2, buf = d3<<16 | d4
 *   PE/SE (0x27/0x28):          addr as above (page / 64 KiB sector erase)
 *   RDSR/RSI (0x22/0x2c):       buf = (d0&0xff)<<24 | d1<<8 | d2>>8, 1 byte
 * The reply (spi_arm7.c SPIi_ReturnResult) is one word on the same tag:
 *   0x03000000 | (command | 0x80) << 8 | result,  result 0 = success.
 *
 * The flash is modelled as an erased 256 KiB part (0xFF), which is what a
 * console with no WFC settings saved holds there: DWC finds no valid
 * connection slot and carries on, exactly as on such a console. Writes and
 * erases are applied to the image so a read-back is consistent within a run;
 * nothing persists (the save chip, not the firmware, is what a player keeps).
 */
#include <nitro.h>
#include <nitro/pxi.h>
#include <string.h>

#define NV_START_BIT 0x02000000u
#define NV_END_BIT   0x01000000u
#define NV_SIZE      0x40000u            /* 256 KiB firmware flash */
#define NV_MAIN_LO   0x02000000u
#define NV_MAIN_HI   0x02800000u         /* HW_MAIN_MEM_EX_END */

enum {
    NV_WREN = 0x20, NV_WRDI, NV_RDSR, NV_READ, NV_FAST_READ, NV_PW, NV_PP,
    NV_PE, NV_SE, NV_DP, NV_RDP, NV_CE, NV_RSI, NV_SR
};

extern void pc_pxi_set_responder(int tag, void (*fn)(u32 data));
extern void pc_pxi_reply(int tag, u32 data);

static u8 sFlash[NV_SIZE];
static u16 sPacket[16];

static int in_main(u32 a, u32 n)
{
    return a >= NV_MAIN_LO && a < NV_MAIN_HI && n <= NV_MAIN_HI - a;
}

static void reply(u32 command, u32 result)
{
    pc_pxi_reply(PXI_FIFO_TAG_NVRAM,
                 0x03000000u | (((command & 0xffu) | 0x80u) << 8) | (result & 0xffu));
}

static void execute(void)
{
    u32 command = (sPacket[0] & 0xff00u) >> 8;
    u32 addr = ((u32)(sPacket[0] & 0xffu) << 16) | sPacket[1];
    u32 size, buf;

    switch (command) {
    case NV_READ:
    case NV_FAST_READ:
        size = ((u32)sPacket[2] << 16) | sPacket[3];
        buf = ((u32)sPacket[4] << 16) | sPacket[5];
        if (!in_main(buf, size)) { reply(command, 2); return; }
        for (u32 i = 0; i < size; i++)
            ((u8 *)(uintptr_t)buf)[i] = sFlash[(addr + i) % NV_SIZE];
        break;
    case NV_PW:
    case NV_PP:
        size = sPacket[2];
        buf = ((u32)sPacket[3] << 16) | sPacket[4];
        if (!in_main(buf, size)) { reply(command, 2); return; }
        for (u32 i = 0; i < size; i++) {
            u8 v = ((const u8 *)(uintptr_t)buf)[i];
            u8 *p = &sFlash[(addr + i) % NV_SIZE];
            *p = command == NV_PP ? (u8)(*p & v) : v; /* program clears bits */
        }
        break;
    case NV_PE:
        memset(&sFlash[addr & (NV_SIZE - 1) & ~0xffu], 0xff, 0x100);
        break;
    case NV_SE:
        memset(&sFlash[addr & (NV_SIZE - 1) & ~0xffffu], 0xff, 0x10000);
        break;
    case NV_CE:
        memset(sFlash, 0xff, sizeof sFlash);
        break;
    case NV_RDSR:
    case NV_RSI:
        buf = ((u32)(sPacket[0] & 0xffu) << 24) | ((u32)sPacket[1] << 8) |
              ((u32)(sPacket[2] & 0xff00u) >> 8);
        if (!in_main(buf, 1)) { reply(command, 2); return; }
        *(u8 *)(uintptr_t)buf = 0; /* status: not busy, not write-enabled */
        break;
    case NV_WREN: case NV_WRDI: case NV_DP: case NV_RDP: case NV_SR:
        break;
    default:
        reply(command, 1); /* SPI_PXI_RESULT_INVALID_COMMAND */
        return;
    }
    reply(command, 0);
}

static void nvram_respond(u32 data)
{
    if (data & NV_START_BIT) memset(sPacket, 0, sizeof sPacket);
    sPacket[(data >> 16) & 0xfu] = (u16)data;
    if (data & NV_END_BIT) execute();
}

void pc_dp_nvram_init(void)
{
    memset(sFlash, 0xff, sizeof sFlash);
    pc_pxi_set_responder(PXI_FIFO_TAG_NVRAM, nvram_respond);
}
