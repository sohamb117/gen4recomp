/*
 * 3DS shadow of the SDK's nitro/hw/common/mmap_shared.h.
 *
 * The work is in 3ds/include/3ds_hw_host.h, which this hands off to after the
 * SDK's own definitions are in. It is one file rather than three because the
 * three memory-map headers are mutually recursive through macros and the guest
 * values have to be captured when all of them are defined; see that file.
 */

#ifndef POKEPLATINUM_3DS_MMAP_SHARED_H
#define POKEPLATINUM_3DS_MMAP_SHARED_H

#include_next <nitro/hw/common/mmap_shared.h>

#include <3ds_hw_host.h>

#endif /* POKEPLATINUM_3DS_MMAP_SHARED_H */
