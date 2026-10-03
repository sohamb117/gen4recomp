/*
 * 3DS shadow of the SDK's nitro/hw/ARM9/mmap_global.h.
 *
 * One macro moves all 329 registers. Every `reg_*` the SDK defines is
 * `(*(REGTypeNv *) REG_x_ADDR)` and every `REG_x_ADDR` is
 * `HW_REG_BASE + REG_x_OFFSET`, checked rather than assumed: of the 329
 * register definitions in the twelve `ioreg_*.h`, not one names an address any
 * other way. So the whole I/O space relocates by redefining `HW_REG_BASE`.
 *
 * On PC that redefinition is not needed: armrec maps 0x04000000 at 0x04000000
 * and the SDK's constant is already a real address. Here it is not an address
 * at all, because 3DS userspace has its image at 0x00100000 and a heap window
 * at 0x08000000, so `HW_REG_BASE` becomes the host pointer to the slab's I/O
 * row.
 *
 * What it costs: `HW_REG_BASE` stops being a compile-time constant, so a
 * register access is a load of one global plus an offset. Everything that used
 * it as a constant now fails to compile, which is the point. The four places
 * that do are mwcc `asm` blocks this compiler cannot build either way, and the
 * 173 other uses across the tree are all run-time.
 *
 * `HW_IOREG` and `HW_IOREG_END` are left alone: they name the address space
 * rather than the storage, and crt0's MPU setup is their only consumer. Only
 * `HW_REG_BASE`, which is what dereferences resolve through, moves.
 *
 * Anything keyed by guest address converts. The GX command ports and the
 * divider are not memory, and the ioreg shadows route them through
 * `armrec_gx_port()` and `armrec_cp_ptr()`, which classify by guest address.
 * Those calls now arrive holding a host pointer and translate on the way in;
 * `HW_REG_BASE_GUEST` is what such a conversion uses.
 *
 * The memory constants in the file this shadows belong to the VRAM shadows.
 * This file touches the register base and nothing else.
 */

#ifndef POKEPLATINUM_3DS_MMAP_GLOBAL_H
#define POKEPLATINUM_3DS_MMAP_GLOBAL_H

#include_next <nitro/hw/ARM9/mmap_global.h>

#ifndef SDK_ASM

/*
 * The guest address the DS puts the registers at. Kept under its own name
 * because the hardware models and the SPU keep speaking guest addresses: a
 * hook that has to classify one converts with
 * `addr - HW_REG_BASE + HW_REG_BASE_GUEST`.
 */
#define HW_REG_BASE_GUEST 0x04000000u

/*
 * The host pointer to the I/O row of the slab. `3ds/src/3ds_guest.c` sets it
 * in guest_bind() and clears it on unbind, so it is exactly as valid as the
 * translator is. NULL until then, a register access before the slab exists
 * is a null dereference with an obvious cause, not a write into whatever
 * happens to be at 0x04000130.
 */
extern unsigned char *armrec_io_base;

/* The generated constants (3ds/gen_absolutes.py) and the shared row's base,
 * for the DTCM block below. 3ds_hw_host.h declares the same pointer; a second
 * identical declaration is not a conflict, and this header is included on its
 * own by translation units that never reach that one. */
extern unsigned char *armrec_shared_base;
#include <3ds_absolutes.h>

#undef HW_REG_BASE
#define HW_REG_BASE ((u32)armrec_io_base)

/*
 * DTCM, which is the one constant in this header that is not a literal.
 *
 * `HW_DTCM` is `(u32)SDK_AUTOLOAD_DTCM_START`, a symbol the ROM's linker
 * script gives an address to and this port's generated `lcf_syms.c`
 * reproduces with a `.set`. Two things follow, and the second is why it took
 * until the game link to notice.
 *
 * It was never moved. The shadow moves constants whose body is a literal or
 * an offset from `HW_MAIN_MEM`, and `3ds/tests/mmap_pin.py` derives its list
 * by the same two shapes, so a constant reached through a *symbol* was
 * invisible to the shadow and to the check written to catch a missing shadow.
 * 71 objects were still reading the DS's 0x027E0000.
 *
 * And a 3DSX cannot carry it at all: the format relocates every ABS32 it
 * holds, and a symbol whose value is not an address in the image cannot be
 * relocated, `3dsxtool` refuses the link over it.
 *
 * DTCM is the head of the shared row: the guest map puts "shared work" at
 * 0x027E0000, which is where a DS puts DTCM, so the row's base pointer IS
 * the DTCM base and nothing needs adding to it. `HW_DTCM_SIZE` (16 KB) and
 * `HW_DTCM_END` follow from it and are left as they are.
 */
enum { HWi_G_DTCM = (int)SDK_LCF_AUTOLOAD_DTCM_START };

#undef HW_DTCM
#define HW_DTCM ((u32)armrec_shared_base)

/*
 * The cartridge slot, and why it is a host pointer too.
 *
 * 0x08000000 is the GBA cartridge on a DS and this console's application heap
 * window on an Old 3DS. Left alone, two SDK macros would read it directly:
 *
 *   CTRDGi_GetHeaderAddr()        ((CTRDGHeader *)HW_CTRDG_ROM)
 *   CTRDGi_GetModuleIDImageAddr() ((u16 *)(HW_CTRDG_ROM + 0x0001fffe))
 *
 * and CTRDG_IsExisting() dereferences both on the way to deciding whether a
 * cartridge is present. Those are plain loads in SDK C, so nothing would
 * translate them: the answer would come out of this process's own heap, and
 * the one wrong answer here is "yes, a cartridge". Pointing the four constants
 * at the slab's AGB row makes the probe read the zeros an empty slot reads.
 *
 * The ends are the backing's, not the DS's. CTRDG_CpuCopy8() classifies its
 * destination with `HW_CTRDG_ROM <= dest && dest < HW_CTRDG_RAM_END` and swaps
 * source and destination when it matches. With host pointers on both sides,
 * the DS's 33 MB span would cover heap this process is using, so an ordinary
 * RAM buffer would be taken for cartridge RAM. The window therefore ends where
 * the memory ends, and the cartridge's SRAM is an empty range.
 *
 * The guest values stay available as HWi_G_CTRDG_*. ctrdg_proc.c's probe DMA
 * is the one caller that wants them, and it never runs.
 */
enum { HWi_G_CTRDG_ROM = (int)(HW_CTRDG_ROM) };
enum { HWi_G_CTRDG_ROM_END = (int)(HW_CTRDG_ROM_END) };
enum { HWi_G_CTRDG_RAM = (int)(HW_CTRDG_RAM) };
enum { HWi_G_CTRDG_RAM_END = (int)(HW_CTRDG_RAM_END) };

/* The bytes behind the slot: GUEST_BACK_AGB, spelled here because this header
 * is on the game chain and 3ds_guest_map.h is not. 3ds/src/3ds_ctrdg.c fails
 * to compile if the two disagree. */
#define HWi_CTRDG_BACKING 0x00020000u

extern unsigned char *armrec_agb_base;

#undef HW_CTRDG_ROM
#undef HW_CTRDG_ROM_END
#undef HW_CTRDG_RAM
#undef HW_CTRDG_RAM_END

#define HW_CTRDG_ROM     ((u32)armrec_agb_base)
#define HW_CTRDG_ROM_END (HW_CTRDG_ROM + HWi_CTRDG_BACKING)
#define HW_CTRDG_RAM     HW_CTRDG_ROM_END
#define HW_CTRDG_RAM_END HW_CTRDG_RAM

#endif /* SDK_ASM */

/*
 * The memory constants (HW_MAIN_MEM and the shared work area) are the same
 * kind of move for a different reason, and they are all in one file because
 * the three memory-map headers are mutually recursive through macros. This
 * include has to come after the SDK's definitions above, and outside the
 * SDK_ASM guard because that file has its own.
 */
#include <3ds_hw_host.h>

#endif /* POKEPLATINUM_3DS_MMAP_GLOBAL_H */
