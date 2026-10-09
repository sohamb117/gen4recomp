/*
 * The GBA slot (Pal Park's migration) for HeartGold/SoulSilver: the one piece
 * of the SDK's CTRDG that the shared chip model
 * (games/platinum/pc/src/pc_agb_slot.c) cannot answer from the bus, as
 * games/diamond/pc/src/pc_dp_agb.c is for Diamond/Pearl.
 *
 * HG/SS's flash driver is the SDK's assembly in lib/asm/nitro.s, recompiled by
 * armrec. The file names the backup bus, so armrec gives it ARMREC_AGB_HOOK
 * and its byte accesses reach the chip model; the BIOS logo copy goes through
 * pc/patches/lib/asm/nitro.s.patch (CTRDGi_InitModuleInfo); the ARM7's logo
 * check is pc_agb_slot.c's PXI responder. What is left is time:
 * CTRDGi_ReadFlashID waits 20 ms for ID mode in a loop on CheckFlashTimer,
 * and the program/erase status polls call it too. It reads OS_GetTick, which
 * no hardware timer advances inside a loop here, so the wait would never end.
 *
 * CheckFlashTimer (nitro.s, 0x020E1D6C) is therefore a host override
 * (pc/host_overrides.txt): with a cartridge in, the chip has finished every
 * command at the write that issued it, so any wait being timed has elapsed
 * (pc_agb_bus_wait_elapsed). With the slot empty it is the original function:
 * ticks since StartFlashTimer, in milliseconds (ticks * 64 / 0x82EA), against
 * the phase's limit, setting the timeout flag.
 *
 * The data are the recompiled SDK's work area at 0x021E4F2C in main RAM
 * (nitro.s's _021E4F2C; HeartGold's and SoulSilver's ARM9 lay it out at the
 * same address, as both builds' recompiled nitro.c name it):
 *   +0x02  u16  timeout flag (CTRDGi_PollingSR*, CTRDGi_ReadFlashID)
 *   +0x20  u64  the phase's limit in ms (StartFlashTimer)
 *   +0x28  u64  the tick StartFlashTimer read
 */
#include <nitro.h>

extern BOOL pc_agb_bus_wait_elapsed(void);

#define FLASH_WORK 0x021E4F2Cu

void CheckFlashTimer(void)
{
    volatile u16 *timeout = (volatile u16 *)(FLASH_WORK + 0x02);
    const volatile u32 *limit = (const volatile u32 *)(FLASH_WORK + 0x20);
    const volatile u32 *start = (const volatile u32 *)(FLASH_WORK + 0x28);
    u64 ms;

    if (pc_agb_bus_wait_elapsed()) {
        *timeout = 1;
        return;
    }
    ms = ((OS_GetTick() - ((u64)start[1] << 32 | start[0])) << 6) / 0x82EA;
    if (((u64)limit[1] << 32 | limit[0]) <= ms) {
        *timeout = 1;
    }
}
