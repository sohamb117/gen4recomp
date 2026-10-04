/*
 * The typed C <-> recompiled bridge on wasm32 (Diamond/Pearl only).
 *
 * irbridge.py rewrites every C translation unit's IR so that no call crosses
 * between a C prototype and armrec's uniform
 * uint64_t f(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) without a
 * generated adapter in between; gen_bridge.py generates the adapters from the
 * per-TU .sigs records. This header is the runtime half they share
 * (armrec_bridge_wasm.c), and it deliberately declares nothing but the
 * bridge's own names: the generated files declare every C callee under an
 * asm label, and must not collide with a header's prototype of the same
 * symbol.
 *
 * The value model, from the design contract: on wasm a C function pointer is
 * a table index, always < ARMREC_WASM_FNPTR_END, and every guest code address
 * is >= 0x01FF8000. So a 32-bit "code address" says which it is by value:
 *   < ARMREC_WASM_FNPTR_END  a compiled C function; armrec_bridge_call()
 *                            calls it through its c2u$ adapter
 *   otherwise                a guest address; armrec_dispatch()
 *
 * Marshalling (both directions; mwcc's AAPCS): the arguments are a flat
 * sequence of 32-bit words, i64/double two words low first with no
 * even-register alignment, a by-value aggregate its words, a hidden result
 * pointer one word (r0), except that a result of 4 bytes or fewer comes back
 * in r0 instead. Words 0..3 travel in r0..r3; word 4+k is at
 * armrec_sp + 4*k when the callee starts, exactly where ARMREC_STACK_ARGS
 * (armrec_rt.h) reads them.
 */

#ifndef ARMREC_BRIDGE_H
#define ARMREC_BRIDGE_H

#include <stdint.h>

#define ARMREC_WASM_FNPTR_END 0x00100000u

/* The ceiling of stack words a variadic C callee is handed; the same value as
 * armrec_rt.h's ARMREC_EXT_STACK_WORDS (static-asserted there). */
#define ARMREC_BRIDGE_VA_STACK_WORDS 16

typedef uint64_t (*armrec_bridge_fn)(uint32_t, uint32_t, uint32_t, uint32_t);

/* One per compiled C function recompiled code can reach: its table index (as
 * a function pointer), the generated adapter that calls it with its real
 * prototype, and its name for diagnostics. Generated (bridge_adapters.c). */
struct armrec_bridge_ent {
    void *fn;
    armrec_bridge_fn adapter;
    const char *name;
};
extern const struct armrec_bridge_ent armrec_bridge_table[];
extern const uint32_t armrec_bridge_table_count;

/* Call the C function whose table index is `idx` with the guest's r0..r3
 * (and its stack words at armrec_sp). Fatal, by index, if the bridge has no
 * adapter for it. */
uint64_t armrec_bridge_call(uint32_t idx, uint32_t a0, uint32_t a1,
                            uint32_t a2, uint32_t a3);

/* The adapter for a table index, or 0. */
armrec_bridge_fn armrec_bridge_adapter(uint32_t idx);

/* The name of a table index's function, or NULL. */
const char *armrec_bridge_name(uint32_t idx);

/* A trap adapter's body: recompiled code called a name no C defines. */
void armrec_bridge_unresolved(const char *name) __attribute__((noreturn));

/* Generated (bridge_externs.c): every armrec_ext_<name> for the extern names
 * recompiled code reaches, functions as their table index, data as its
 * address. */
void armrec_bind_externs(void);

#endif /* ARMREC_BRIDGE_H */
