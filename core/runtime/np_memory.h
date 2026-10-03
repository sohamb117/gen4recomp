/*
 * Private interface of np_memory.c (the guest linear memory allocator).
 */
#ifndef NP_MEMORY_H
#define NP_MEMORY_H

#include <stdint.h>

/* Largest reservation for one memory, used when a module declares no
 * maximum (wasm2c then passes 4 GiB, which iOS cannot reserve). */
#define NP_MEMORY_RESERVE_CAP ((uint64_t)512 * 1024 * 1024)

/* Why the last allocation failed, or NULL. Set just before the allocator
 * raises a wasm trap, so the trap handler can report the real cause. */
extern const char *np_memory_failure;

#endif /* NP_MEMORY_H */
