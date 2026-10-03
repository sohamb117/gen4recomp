/*
 * wasm shadow of the SDK's nitro/fs/overlay.h.
 *
 * `FS_OVERLAY_ID(name)` is `((u32) & (SDK_OVERLAY_ ## name ## _ID))`: on the
 * DS the linker script gives that symbol an ADDRESS equal to the overlay's
 * ordinal. The ELF port keeps that shape with a `.set` in generated C; a wasm
 * object has no absolute symbols at all, so here the ordinal is a constant,
 * from the enum 3ds/gen_absolutes.py writes out of the same overlay_ids.c the
 * ELF build links (the 3DS port answers its relocation problem the same
 * way, 3ds/include/nitro/fs/overlay.h). The ordinals still come from main.lsf.
 *
 * `FS_EXTERN_OVERLAY` only declares the symbol; nothing references it.
 */

#ifndef POKEPLATINUM_WASM_FS_OVERLAY_H
#define POKEPLATINUM_WASM_FS_OVERLAY_H

#include_next <nitro/fs/overlay.h>

#include <pc_wasm_absolutes.h>

#undef FS_OVERLAY_ID
#define FS_OVERLAY_ID(name) ((u32)(SDK_OVID_##name))

#endif /* POKEPLATINUM_WASM_FS_OVERLAY_H */
