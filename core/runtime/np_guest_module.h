/*
 * The seam between the generic runtime and one wasm2c'd game module.
 *
 * wasm2c bakes the module name into every symbol (w2c_platinum_0x5Fstart,
 * wasm2c_platinum_instantiate, struct w2c_platinum, ...), so the runtime
 * cannot call a module directly without being compiled once per game. Each
 * module instead gets a tiny glue file (np_module_glue.c.in, instantiated by
 * np_add_guest_module in CMake) that exports one np_guest_module describing
 * it, and the executable gets a registry (np_link_guest_modules) naming the
 * modules it contains. The runtime only ever goes through this table.
 */
#ifndef NP_GUEST_MODULE_H
#define NP_GUEST_MODULE_H

#include <stddef.h>
#include <stdint.h>

#include "np_core.h"

struct w2c_np__host;
struct w2c_wasi__snapshot__preview1;

typedef struct np_guest_module {
    const char *name;            /* wasm2c module name, also the guest's argv[0] */
    size_t instance_size;        /* sizeof(w2c_<name>) */
    size_t memory_offset;        /* offsetof(w2c_<name>, w2c_memory) */
    size_t stack_pointer_offset; /* offsetof(w2c_<name>, w2c_0x5F_stack_pointer) */
    void (*instantiate)(void *instance, struct w2c_np__host *host, struct w2c_wasi__snapshot__preview1 *wasi);
    void (*free)(void *instance);
    void (*start)(void *instance);                     /* export _start */
    void (*fiber_entry)(void *instance, uint32_t arg); /* export np_fiber_entry */
} np_guest_module;

/* Defined by the registry source np_link_guest_modules generates for the
 * final executable; NULL entries are games this binary does not contain. */
extern const np_guest_module *const np_guest_registry[NP_GAME_COUNT];

#endif /* NP_GUEST_MODULE_H */
