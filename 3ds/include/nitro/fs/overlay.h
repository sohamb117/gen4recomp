/*
 * 3DS shadow of the SDK's nitro/fs/overlay.h.
 *
 * `FS_OVERLAY_ID(name)` is `((u32) & (SDK_OVERLAY_ ## name ## _ID))`: on the
 * DS the linker script gives that symbol an ADDRESS equal to the overlay's
 * ordinal, so the ordinal is reached by taking an address. The PC port keeps
 * that shape with a `.set` in generated C and an ELF host does not mind.
 *
 * A 3DSX does. The format carries its own relocation table so the loader can
 * place the image, and every `R_ARM_ABS32` in it has to point at something
 * inside code, rodata or data. An ordinal is not an address in the image, and
 * `3dsxtool` refuses the entire link over the first one it meets:
 *
 *     absolute @ relSrc=00000068
 *     Relocation to invalid address!
 *
 * 0x68 is overlay 104. So here the ordinal is a constant instead, from an
 * enum 3ds/gen_absolutes.py writes out of the same generated file the PC
 * build links; the ordinals still come from main.lsf and there is still one
 * answer to what they are.
 *
 * `FS_EXTERN_OVERLAY` is left alone. It only declares the symbol, and a
 * declaration nothing takes the address of emits no relocation.
 */

#ifndef POKEPLATINUM_3DS_FS_OVERLAY_H
#define POKEPLATINUM_3DS_FS_OVERLAY_H

#include_next <nitro/fs/overlay.h>

#include <3ds_absolutes.h>

#undef FS_OVERLAY_ID
#define FS_OVERLAY_ID(name) ((u32)(SDK_OVID_##name))

#endif /* POKEPLATINUM_3DS_FS_OVERLAY_H */
