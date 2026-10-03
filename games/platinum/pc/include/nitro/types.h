/*
 * PC port override for subprojects/NitroSDK-4.2.30001/include/nitro/types.h.
 *
 * pc/include sits ahead of the SDK on the search path, so this file shadows
 * the SDK one. The only real change is the width of the 32-bit types: the DS
 * build targets ARM32, where long is 32 bits, so the original header writes
 * u32 as "unsigned long". On an LP64 host that silently becomes 64 bits and
 * every piece of code that relies on 32-bit wraparound quietly changes
 * behaviour. stdint pins them whatever the host ABI (the build is -m32
 * regardless (see pc/Makefile's ABI comment) but the types should not
 * depend on that being remembered).
 *
 * Everything else is carried over from the SDK header so its consumers see
 * the same macros (ATTRIBUTE_ALIGN, SDK_WEAK_SYMBOL, SDK_INLINE, ...).
 */

#ifndef NITRO_TYPES_H_
#define NITRO_TYPES_H_

#define SDK_LITTLE_ENDIAN
#define SDK_IS_LITTLE_ENDIAN 1
#define SDK_IS_BIG_ENDIAN 0

#ifndef SDK_ASM

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*
 * The 64-bit pair carries an alignment as well as a width, and on ARM the
 * two build hosts disagree about it. AAPCS aligns a long long to 8; i386
 * aligns it to 4, and so does the DS's compiler. Left alone, every struct
 * with a u64 in it changes shape on armhf, measured, struct{char; u64;}
 * is 12 bytes on i386 and 16 there, the member at offset 4 against 8, and
 * a save written by one build is unreadable by the other.
 *
 * An aligned attribute is normally read as "at least this much", but GCC
 * lets it REDUCE alignment when it is attached to a typedef, which is what
 * makes this a two-line fix rather than a survey. Measured: the same struct
 * is 12 bytes with the member at 4 once these are in place.
 *
 * -fpack-struct=4 reaches the same alignment and is the wrong tool: it also
 * repacks `u32 x : 20` bitfields, which moves nine structures, seven of them
 * save data.
 *
 * wasm32 is the same case as armhf: clang aligns a long long to 8 there and
 * honours the same typedef-level reduction.
 */
#if defined(__arm__) || defined(__wasm__)
#define PC_ALIGN64 __attribute__((aligned(4)))
#else
#define PC_ALIGN64
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t PC_ALIGN64 u64;

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t PC_ALIGN64 s64;

typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;

typedef volatile s8 vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;

typedef float f32;
typedef volatile f32 vf32;

typedef u8 REGType8;
typedef u16 REGType16;
typedef u32 REGType32;
typedef u64 REGType64;

typedef vu8 REGType8v;
typedef vu16 REGType16v;
typedef vu32 REGType32v;
typedef vu64 REGType64v;

#ifndef SDK_BOOL_ALREADY_DEFINED_
#ifndef BOOL
typedef int BOOL;
#endif
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void *)0)
#endif
#endif

/* The SDK guards these behind SDK_CW; GCC accepts the same attribute
 * spelling, so define them unconditionally here. __declspec(weak) is mapped
 * by pc/include/pc_prelude.h. */
#ifndef ATTRIBUTE_ALIGN
#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))
#endif

/* Weak is ELF's. On PE, GCC lowers an attribute-weak DEFINITION to a weak
 * external whose default mingw's ld.bfd does not resolve against a plain
 * call, OS_Terminate was the last undefined of the first true cold
 * Windows link (2026-08-27; the objcopy --weaken encoding resolves fine,
 * this one does not). The SDK marks its terminators weak so a hardware
 * game could override them; nothing in this port does, and on PE every
 * SDK definition is already overridable through the weaken pass. Strong
 * there, weak everywhere else. PC_HIDE_WIN32 is in the test because the
 * Windows build HIDES _WIN32 from game and SDK translation units, the
 * very files this macro appears in, and that hiding flag is therefore
 * the one reliable mark of a PE compile here. */
#if defined(_WIN32) || defined(PC_HIDE_WIN32)
#define SDK_WEAK_SYMBOL
#else
#define SDK_WEAK_SYMBOL __attribute__((weak))
#endif
#define SDK_FORCE_EXPORT

#ifdef __cplusplus
}
#endif

#endif /* SDK_ASM */

#define SDK_INLINE static inline
#define SDK_DECL_INLINE static

#endif
