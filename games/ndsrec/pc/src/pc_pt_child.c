/*
 * Poké Transfer's Download Play child (pc/Makefile.wasm VER=poketransfer):
 * what the host needs from the child program that the child does not link.
 *
 * The child is dl_rom/child_r_eng.srl of Black/White's cartridge (one image,
 * the same bytes in both versions, SHA-1 e9250e6a41d98e49f3713781abcfeab53350a074),
 * which they send from the Poké Transfer Lab to a second DS (docs/BW_PLAN.md
 * "Poké Transfer"). Its station receives it first (pc_pt_dlplay.c); the slot-1
 * card is the runtime's ROM (pc_card_rom.c under PC_MB_CHILD).
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
