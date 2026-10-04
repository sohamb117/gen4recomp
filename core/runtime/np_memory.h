/*
 * Private interface of np_memory.c (the guest linear memory allocator).
 */
#ifndef NP_MEMORY_H
#define NP_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#include "wasm-rt.h"

/* Largest reservation for one memory, used when a module declares no
 * maximum (wasm2c then passes 4 GiB, which iOS cannot reserve). */
#define NP_MEMORY_RESERVE_CAP ((uint64_t)512 * 1024 * 1024)

/* Why the last allocation failed, or NULL. Set just before the allocator
 * raises a wasm trap, so the trap handler can report the real cause. */
extern const char *np_memory_failure;

/* Snapshot support (np_snapshot.c). Granularity is the host page. */
size_t np_memory_os_page(void);

/* Sets map[i] for every host page i of [0, memory->size) that may hold
 * anything but zeros because the guest touched it; untouched pages read as
 * zero and are not worth storing. Returns 0, or -1 when the platform cannot
 * tell (then every page is a candidate). */
int np_memory_touched(const wasm_rt_memory_t *memory, uint8_t *map);

/* Returns [offset, offset + len), host-page aligned and inside the current
 * size, to zero and, where the platform allows, to the untouched state. */
int np_memory_discard(wasm_rt_memory_t *memory, uint64_t offset, uint64_t len);

/* Grows (commit) or shrinks (discard) the memory to exactly `pages`. */
int np_memory_set_pages(wasm_rt_memory_t *memory, uint64_t pages);

#endif /* NP_MEMORY_H */
