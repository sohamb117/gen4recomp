/*
 * The bridge's runtime half: table index -> c2u$ adapter (wasm32, Diamond/
 * Pearl only; Platinum never links this file). See armrec_bridge.h for the
 * model and tools/armrec/gen_bridge.py for the generated table.
 *
 * The table is generated as a list, and turned into a dense array indexed by
 * table index on first use: indices are small (the module's function table
 * has tens of thousands of entries at most) and armrec_dispatch() asks on
 * every indirect branch to C.
 */

#include "armrec_bridge.h"
#include "armrec_rt.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(__wasm__)
#include <pc_wasm.h>
#define bridge_fatalf pc_wasm_fatalf
#else
/* Native builds of the bridge test only. */
#define bridge_fatalf(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr), abort())
#endif

_Static_assert(ARMREC_BRIDGE_VA_STACK_WORDS == ARMREC_EXT_STACK_WORDS,
               "the bridge's variadic ceiling is armrec's stack-word ceiling");

static armrec_bridge_fn *by_index;
static const char **name_by_index;
static uint32_t index_end;

static void bridge_index(void) {
    uint32_t i, top = 0;

    for (i = 0; i < armrec_bridge_table_count; i++) {
        uint32_t idx = (uint32_t)(uintptr_t)armrec_bridge_table[i].fn;
        if (idx >= ARMREC_WASM_FNPTR_END)
            bridge_fatalf("armrec bridge: %s has table index 0x%X, not below "
                          "0x%X; the function-pointer model in armrec_bridge.h "
                          "no longer holds", armrec_bridge_table[i].name, idx,
                          ARMREC_WASM_FNPTR_END);
        if (idx + 1 > top) top = idx + 1;
    }
    by_index = calloc(top ? top : 1, sizeof *by_index);
    name_by_index = calloc(top ? top : 1, sizeof *name_by_index);
    if (by_index == NULL || name_by_index == NULL)
        bridge_fatalf("armrec bridge: out of memory for %u table slots", top);
    for (i = 0; i < armrec_bridge_table_count; i++) {
        uint32_t idx = (uint32_t)(uintptr_t)armrec_bridge_table[i].fn;
        by_index[idx] = armrec_bridge_table[i].adapter;
        name_by_index[idx] = armrec_bridge_table[i].name;
    }
    index_end = top;
}

armrec_bridge_fn armrec_bridge_adapter(uint32_t idx) {
    if (by_index == NULL) bridge_index();
    return idx < index_end ? by_index[idx] : 0;
}

const char *armrec_bridge_name(uint32_t idx) {
    if (by_index == NULL) bridge_index();
    return idx < index_end ? name_by_index[idx] : NULL;
}

uint64_t armrec_bridge_call(uint32_t idx, uint32_t a0, uint32_t a1,
                            uint32_t a2, uint32_t a3) {
    armrec_bridge_fn f = armrec_bridge_adapter(idx);

    if (f == 0) {
        if (idx == 0)
            bridge_fatalf("armrec: call through a null code address");
        bridge_fatalf("armrec: code address 0x%X is a C function pointer "
                      "(wasm table index) with no c2u$ adapter: the bridge "
                      "only knows C functions recompiled code calls or whose "
                      "address C takes (gen_bridge.py)", idx);
    }
    if (armrec_trace)
        fprintf(stderr, "armrec: -> C %s (table index %u)\n",
                armrec_bridge_name(idx), idx);
    return f(a0, a1, a2, a3);
}

void armrec_bridge_unresolved(const char *name) {
    bridge_fatalf("armrec: recompiled code called %s, which no C translation "
                  "unit of this module defines (bridge_report.txt lists every "
                  "such name)", name);
}
