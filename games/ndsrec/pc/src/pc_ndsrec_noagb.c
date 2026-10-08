/*
 * The GBA slot of a ROM that has no GBA-slot code (Black/White: no CTRDG
 * library, no Pal Park; the primitive map places none of its functions).
 *
 * Diamond's host fragment models the slot with a chip behind the bus
 * (games/platinum/pc/src/pc_agb_slot.c) and D's flash timer
 * (games/diamond/pc/src/pc_dp_agb.c); both reach CTRDG functions and
 * statics by name, which such a ROM does not have, so pc/Makefile.wasm
 * leaves them out and links this instead. The slot is the console's empty
 * one: pc_main.c's zero-filled window, which nothing in the game reads.
 * HG/SS's recompiled SDK (lib/asm/nitro.s) does name the backup bus, so
 * its byte accesses arrive here and are the window's plain ones.
 */
#include <stdint.h>
#include <stdio.h>

#if defined(__wasm__)
#include "np_guest_abi.h"
#endif

/* pc_main.c, once the slot window is mapped. */
void pc_agb_slot_insert(void)
{
#if defined(__wasm__)
    if (np_host_gba_rom_size() != 0) {
        fprintf(stderr, "pc_agb_slot: this game has no GBA-slot code; the "
                        "GBA cartridge is ignored\n");
    }
#endif
}

/* pc_os_lite.c, once per frame: there is no backup chip to store. */
void pc_agb_slot_step(void)
{
}

/* armrec_rt.h's ARMREC_AGB_HOOK: no chip behind the bus. */
uint32_t armrec_agb_load8(uint32_t a)
{
    return *(const volatile uint8_t *)(uintptr_t)a;
}

void armrec_agb_store8(uint32_t a, uint32_t v)
{
    *(volatile uint8_t *)(uintptr_t)a = (uint8_t)v;
}
