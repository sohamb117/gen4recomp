/* Force-included ahead of every D/P game and SDK translation unit of the
 * wasm build (pc/mk/game.mk). Maps the CodeWarrior extensions the
 * decompiled sources use onto clang; the Platinum counterpart is
 * games/platinum/pc/include/pc_prelude.h. */
#ifndef PC_PRELUDE_H
#define PC_PRELUDE_H

/* mwcc spells attributes __declspec(x). */
#define __declspec(x) PC_DECLSPEC_##x
#define PC_DECLSPEC_noreturn __attribute__((noreturn))
#define PC_DECLSPEC_force_export
#define PC_DECLSPEC_weak __attribute__((weak))

/* The u64/s64 alignment fix; see the header for why it is included here
 * rather than found on the search path. */
#include "nitro/types.h"

/* The I/O registers that are not memory (coprocessor, geometry engine)
 * routed through the armrec runtime; see the header. */
#include "pc_dp_registers.h"

#endif /* PC_PRELUDE_H */
