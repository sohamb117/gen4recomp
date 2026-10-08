/* Force-included ahead of every HG/SS game and SDK translation unit of the
 * wasm build (pc/mk/game.mk). Maps the CodeWarrior extensions the
 * decompiled sources use onto clang; the counterparts are
 * games/platinum/pc/include/pc_prelude.h and
 * games/diamond/pc/include/pc_prelude.h. */
#ifndef PC_PRELUDE_H
#define PC_PRELUDE_H

/* mwcc spells attributes __declspec(x). */
#define __declspec(x) PC_DECLSPEC_##x
#define PC_DECLSPEC_noreturn __attribute__((noreturn))
#define PC_DECLSPEC_force_export
#define PC_DECLSPEC_weak __attribute__((weak))
#define PC_DECLSPEC_noinline __attribute__((noinline))

/* nitro/math/math.h's MATH_CountLeadingZerosInline is an mwcc `asm`
 * intrinsic; Platinum's prelude gives it the same spelling. */
#define PLATFORM_INTRINSIC_FUNCTION_BIT_CLZ32(x) \
    ((x) ? (unsigned)__builtin_clz((unsigned)(x)) : 32u)

/* MSL's implicit __alloca. */
#define __alloca(n) __builtin_alloca(n)

/* The u64/s64 alignment fix (mwcc aligns them to 4): Platinum's 4.2
 * nitro/types.h shadow, which pc/mk/game.mk copies ahead of lib/include. */
#include <nitro/types.h>

#endif /* PC_PRELUDE_H */
