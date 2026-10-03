/*
 * 3DS shadow of the SDK's nitro/hw/ARM9/mmap_main.h.
 *
 * Nothing here but the hand-off: HW_MAIN_MEM_SUB names the shared row and has
 * to move with it, and the rest of this header derives from HW_MAIN_MEM and
 * follows once that does. 3ds/include/3ds_hw_host.h does both.
 */

#ifndef POKEPLATINUM_3DS_MMAP_MAIN_H
#define POKEPLATINUM_3DS_MMAP_MAIN_H

#include_next <nitro/hw/ARM9/mmap_main.h>

#include <3ds_hw_host.h>

#endif /* POKEPLATINUM_3DS_MMAP_MAIN_H */
