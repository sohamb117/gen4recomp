/*
 * wasm32 override for include/nitro/types.h (pokediamond's, shared by the
 * ARM9 and ARM7 trees there).
 *
 * The only change is the alignment of the 64-bit pair. mwcc aligns a long
 * long to 4; wasm32 clang aligns it to 8, so every struct holding a u64/s64
 * would change shape (struct{u8; u64;} is 12 bytes for mwcc, 16 for wasm32)
 * and guest memory shared with the recompiled assembly would be misread.
 * An aligned attribute on a TYPEDEF may reduce alignment (clang honours it,
 * as GCC does), which is the same two-line fix games/platinum/pc/include/
 * nitro/types.h uses (read its comment). -fpack-struct=4 would also repack
 * bitfields and is the wrong tool.
 *
 * How it is reached: include/global.h and friends spell it
 * `#include "nitro/types.h"`, and a quote include searches the includer's
 * own directory first, so an -I shadow can never win for headers that live
 * in include/. pc/include/pc_prelude.h therefore includes THIS file first,
 * and it defines the original's guard, POKEDIAMOND_TYPES_H, so every later
 * include of the original is empty.
 *
 * u32/s32 stay `unsigned long`/`signed long` as in the original: wasm32 is
 * ILP32, so they are 32 bits, and keeping the spelling keeps every
 * prototype the decomp wrote type-identical.
 */

#ifndef POKEDIAMOND_TYPES_H
#define POKEDIAMOND_TYPES_H

#define PC_ALIGN64 __attribute__((aligned(4)))

typedef unsigned char u8;
typedef unsigned short int u16;
typedef unsigned long u32;

typedef signed char s8;
typedef signed short int s16;
typedef signed long s32;

typedef unsigned long long int PC_ALIGN64 u64;
typedef signed long long int PC_ALIGN64 s64;

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

typedef int BOOL;
#define TRUE 1
#define FALSE 0

#ifndef NULL
#ifdef  __cplusplus
#define NULL 0
#else  // __cplusplus
#define NULL ((void *)0)
#endif // __cplusplus
#endif

#define SDK_FORCE_EXPORT __declspec(force_export)

#endif //POKEDIAMOND_TYPES_H
