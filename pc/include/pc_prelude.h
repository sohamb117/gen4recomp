/* Force-included ahead of every translation unit in the PC build.
 * Maps the CodeWarrior extensions the decompiled sources use onto GCC/Clang.
 * Started as a copy of pokediamond's pc/include/pc_prelude.h. */
#ifndef PC_PRELUDE_H
#define PC_PRELUDE_H

/* mwcc spells attributes __declspec(x); only "noreturn" and "weak" are
 * load-bearing here. */
#define __declspec(x) PC_DECLSPEC_##x
#define PC_DECLSPEC_noreturn __attribute__((noreturn))
#define PC_DECLSPEC_force_export
#define PC_DECLSPEC_weak __attribute__((weak))
#define PC_DECLSPEC_thread __thread

#if defined(_WIN32)
#define PC_DECLSPEC_dllimport __attribute__((dllimport))
#define PC_DECLSPEC_dllexport __attribute__((dllexport))
#define PC_DECLSPEC_deprecated __attribute__((deprecated))
#endif

/* nitro/math/math.h defines MATH_CountLeadingZerosInline as an mwcc `asm`
 * block unless the platform supplies an intrinsic. Supply one, so every file
 * that includes <nitro.h> does not die on mwcc asm syntax. The macro expands
 * at the use site, after u32 exists. */
#define PLATFORM_INTRINSIC_FUNCTION_BIT_CLZ32(x) \
    ((x) ? (unsigned)__builtin_clz((unsigned)(x)) : 32u)

/* mwcc's MSL exposes __alloca as an implicit intrinsic (g2d_CharCanvas.c
 * calls it with no declaration). It must be a macro, not a function: a
 * callee cannot allocate in its caller's frame, so a forwarding function
 * would hand back a dangling pointer. pc_os_lite.c still carries a trap
 * symbol of the same name for any object compiled without this prelude. */
#define __alloca(n) __builtin_alloca(n)

#endif /* PC_PRELUDE_H */

/*
 * PC_HIDE_WIN32: the wifi-era GameSpy/DWC/NitroWiFi sources carry their own
 * platform detection, and under mingw `#elif defined(_WIN32)` outranks the
 * `_NITRO` branch the ROM build takes, windows.h and winsock arrive, and
 * their SOCKET collides with the tree's own. Those TUs are DS code and must
 * compile as DS code on every host; pc/Makefile.win sets this for exactly
 * those subtrees and nothing else (armrec_rt.c and pc/src keep their real
 * _WIN32 paths).
 */
#if defined(PC_HIDE_WIN32) && defined(_WIN32)
/*
 * msvcrt's <time.h> spells time() as a `static __inline` forwarding to
 * _time32, and the DS's network library defines a time() of its own, a
 * definition cannot override an inline one, so the translation unit does not
 * compile. Rename the header's out of the way while the DS trees are being
 * preloaded. Every other host declares time() and links the tree's own
 * definition over the C library's; this keeps that true here rather than
 * changing which time() the game reads on one platform. The macro is gone
 * again before any DS source is seen.
 */
#define time pc_crt_time_unused

/* mingw's own system headers refuse to parse without _WIN32 ("Only Win32
 * target is supported!"), so everything a DS translation unit could pull is
 * included NOW, while the macro still stands; later includes hit their
 * guards. Only then does the macro go away for the tree's platform checks. */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#undef time
#undef _WIN32
#undef _WIN64
#undef WIN32
#undef __WIN32__
#endif
