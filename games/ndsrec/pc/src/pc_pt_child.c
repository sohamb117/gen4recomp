/*
 * Poké Transfer's Download Play child (pc/Makefile.wasm VER=poketransfer):
 * what the host does for it that a cartridge game's boot does not.
 *
 * The child is dl_rom/child_r_eng.srl of Black/White's cartridge (one image,
 * the same bytes in both versions, SHA-1 e9250e6a41d98e49f3713781abcfeab53350a074),
 * which they send from the Poké Transfer Lab to a second DS (docs/BW_PLAN.md
 * "Poké Transfer"). On a console the firmware's Download Play client
 * receives it, places it and boots it. What it leaves behind is a
 * multiboot child's boot state, which pc_mb_child_boot() writes here in its
 * place (pc_card_rom.c calls it once the inserted card is open):
 *
 *   HW_ROM_HEADER_BUF   the child's own ROM header, as received
 *   HW_WM_BOOT_BUF      MBParam (NitroSDK include/nitro/mb/mb.h): boot type
 *                       MB_TYPE_MULTIBOOT and the parent's BSS description,
 *                       which the child reconnects to
 *   HW_DOWNLOAD_PARAMETER  the parent's 32-byte user parameter
 *   HW_CARD_ROM_HEADER  the inserted card's header (pc_card_rom.c): CARD_Init
 *                       copies the own header there only on a card boot, so
 *                       for a child it stays the slot-1 card's, which is how
 *                       the child knows which Gen 4 game is inserted
 *
 * The header comes from the build (pc_pt_image.c, generated from the
 * extracted image); the parent's BSS description and user parameter are
 * what the parent hands over, so until the Download Play link carries them
 * they are empty, and the child's reconnect finds no parent.
 *
 * OS_GetIrqFunction, which the host's VBlank and timer delivery reads the
 * registered handlers through, is not linked into the child (nothing of its
 * own calls it). It is TWL-SDK 5's, over the child's own tables: OS_IRQTable
 * at 0x02FE0020 (DTCM, as in Black) and OSi_IrqCallbackInfo at 0x0213C64C,
 * both from the literal pool of its OS_SetIrqFunction (0x0204F218), whose
 * mapping of interrupt bits to callback slots this follows: DMA 0-3 (bits
 * 8-11) slots 0-3, NDMA 0-3 (bits 28-31) slots 4-7, timers 0-3 (bits 3-6)
 * slots 8-11, 12 bytes each {function, enable, argument}.
 */
#include <nitro.h>
#include <nitro/mb.h>
#include <string.h>

#define PT_OS_IRQ_TABLE ((OSIrqFunction *)0x02FE0020u)
#define PT_IRQ_CALLBACK_INFO ((const u32 *)0x0213C64Cu)

OSIrqFunction OS_GetIrqFunction(OSIrqMask bit)
{
    int i;

    for (i = 0; i < 32; i++, bit >>= 1) {
        if (!(bit & 1)) continue;
        if (i >= 8 && i <= 11) return (OSIrqFunction)PT_IRQ_CALLBACK_INFO[3 * (i - 8)];
        if (i >= 28) return (OSIrqFunction)PT_IRQ_CALLBACK_INFO[3 * (i - 24)];
        if (i >= 3 && i <= 6) return (OSIrqFunction)PT_IRQ_CALLBACK_INFO[3 * (i + 5)];
        return PT_OS_IRQ_TABLE[i];
    }
    return NULL;
}

extern const u8 pc_pt_child_header[HW_CARD_ROM_HEADER_SIZE];

void pc_mb_child_boot(void)
{
    MBParam *param = (MBParam *)HW_WM_BOOT_BUF;

    memcpy((void *)HW_ROM_HEADER_BUF, pc_pt_child_header, HW_CARD_ROM_HEADER_SIZE);
    memset(param, 0, sizeof *param);
    param->boot_type = MB_TYPE_MULTIBOOT;
    memset((void *)HW_DOWNLOAD_PARAMETER, 0, HW_DOWNLOAD_PARAMETER_SIZE);
}
