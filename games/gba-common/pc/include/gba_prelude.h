/*
 * Force-included ahead of every decomp TU of a GBA guest (gbabuild.py).
 * wasi-libc's <limits.h> defines PAGE_SIZE, which pokeemerald's pokedex.c
 * uses as an enum constant; agbcc's libc headers have no such macro.
 */
#ifndef GBA_PRELUDE_H
#define GBA_PRELUDE_H
#include <limits.h>
#undef PAGE_SIZE

#if defined(RUBY) || defined(SAPPHIRE)
/* pokeruby calls some functions with another arity than their definition
 * (K&R declarations; ARM ignores the extra or missing registers). A wasm
 * call must match the callee's type, or wasm-ld routes it to a trapping
 * signature_mismatch stub, so the calls are made to match:
 * GetMonData/GetBoxMonData(mon, field) get a null data pointer, and the
 * parameterless nullsub_11() and gpu_sync_bg_hide() lose their arguments.
 * (The declaration `u32 GetMonData();` keeps its empty list.) */
#define NP_FIRST3(a, b, c, ...) a, b, c
#define GetMonData(...) GetMonData(__VA_OPT__(NP_FIRST3(__VA_ARGS__, 0, 0)))
#define GetBoxMonData(...) GetBoxMonData(__VA_OPT__(NP_FIRST3(__VA_ARGS__, 0, 0)))
#define nullsub_11(...) nullsub_11()
#define gpu_sync_bg_hide(...) gpu_sync_bg_hide()
#endif
#endif
