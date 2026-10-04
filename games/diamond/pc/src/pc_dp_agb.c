/*
 * The GBA slot (Pal Park) for Diamond/Pearl: the one piece of D's CTRDG that
 * the shared chip model (games/platinum/pc/src/pc_agb_slot.c) cannot answer
 * from the bus.
 *
 * D's flash driver is SDK 3.2 assembly, recompiled by armrec. Its bus accesses
 * reach the chip model through ARMREC_AGB_HOOK (armrec_rt.h), its BIOS logo
 * copy through pc/patches/arm9/asm/CTRDG_proc.s.patch, and the ARM7's
 * logo check through pc_agb_slot.c's PXI responder. What is left is time:
 * CTRDGi_ReadFlashID waits 20 ms for ID mode in a loop on CheckFlashTimer,
 * and the program/erase status polls call it too. It reads OS_GetTick, which
 * no hardware timer advances inside a loop here, so the wait would never end.
 *
 * CheckFlashTimer (arm9/asm/CTRDG_flash_common.s, 0x020DC3BC) is therefore a
 * host override (pc/host_overrides.txt): with a cartridge in, the chip has
 * finished every command at the write that issued it, so any wait being timed
 * has elapsed (pc_agb_bus_wait_elapsed, the same answer Platinum's patched
 * ctrdg_flash_common.c gets). With the slot empty it is the original function:
 * ticks since StartFlashTimer, in milliseconds (OS_TicksToMilliSeconds:
 * ticks * 64 / 0x82EA), against the phase's limit, setting the timeout flag.
 *
 * The data are the recompiled SDK's bss, by their ROM labels:
 *   UNK_021D6B10  u16  timeout flag (CTRDG_PollingSR*, CTRDGi_ReadFlashID)
 *   UNK_021D6B3C  u64  the phase's limit in ms (StartFlashTimer)
 *   UNK_021D6B44  u64  the tick StartFlashTimer read
 * u64 as two words: the labels are 4-aligned.
 */
#include <nitro.h>

extern BOOL pc_agb_bus_wait_elapsed(void);

extern u16 UNK_021D6B10;
extern u32 UNK_021D6B3C[2];
extern u32 UNK_021D6B44[2];

void CheckFlashTimer(void)
{
    u64 start, limit, ms;

    if (pc_agb_bus_wait_elapsed()) {
        UNK_021D6B10 = 1;
        return;
    }
    start = (u64)UNK_021D6B44[1] << 32 | UNK_021D6B44[0];
    limit = (u64)UNK_021D6B3C[1] << 32 | UNK_021D6B3C[0];
    ms = ((OS_GetTick() - start) << 6) / 0x82EA;
    if (limit <= ms) {
        UNK_021D6B10 = 1;
    }
}
