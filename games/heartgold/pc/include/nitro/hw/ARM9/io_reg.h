/*
 * PC port shadow of HG/SS's ARM9 io_reg.h.
 *
 * pokeheartgold carries the ARM9 registers in this one file, where the
 * NitroSDK Platinum builds against splits them into ioreg_CP.h, ioreg_G3.h,
 * ioreg_G3X.h, ... . The port's shadows of those three (the divider and
 * square root, the geometry command ports, GXFIFO/GXSTAT and the result
 * registers, routed through the armrec runtime; pc/mk/game.mk copies them
 * into $(BUILD)/shadow) therefore never applied: no HG/SS file includes
 * them. Every compiled-C store to a geometry port landed in plain memory, so
 * G3_SwapBuffers (fieldmap.c, gf_3d_vramman.c) never reached pc/hw/pc_gpu3d.c,
 * the polygon RAM filled up and stayed full, and nothing was ever rendered;
 * the C divider reads returned whatever memory held.
 *
 * This takes the original and then the three shadows, which #undef and
 * redefine their registers' macros. Their own #include_next finds no SDK
 * header of that name in HG/SS's tree and lands on the -idirafter copy in
 * games/platinum/pc/include, whose include guard is already defined.
 */
#ifndef PC_HG_SHADOW_ARM9_IO_REG_H
#define PC_HG_SHADOW_ARM9_IO_REG_H

#include_next <nitro/hw/ARM9/io_reg.h>

#include <nitro/hw/ARM9/ioreg_CP.h>
#include <nitro/hw/ARM9/ioreg_G3.h>
#include <nitro/hw/ARM9/ioreg_G3X.h>

#endif /* PC_HG_SHADOW_ARM9_IO_REG_H */
