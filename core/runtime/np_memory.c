/*
 * Guest linear memory: our replacement for wabt's wasm-rt-mem-impl.c.
 *
 * Same API (wasm_rt_allocate_memory / wasm_rt_grow_memory /
 * wasm_rt_free_memory / wasm_rt_memory_is_default32), different policy:
 *
 *   - wabt's guard-page mode reserves 8 GiB per memory so that any
 *     base + u32 address + u32 offset lands in mapped-or-guard space and an
 *     OOB access raises a signal its handler turns into a trap. iOS caps a
 *     process's virtual address space far below that (and we cannot rely on
 *     the com.apple.developer.kernel.extended-virtual-addressing
 *     entitlement), so we reserve exactly the module's declared maximum
 *     (NP_GUEST_MEMORY_BYTES-ish: the guests link with --max-memory=256 MiB).
 *     A module without a declared maximum is capped at NP_MEMORY_RESERVE_CAP.
 *   - The reservation is PROT_NONE / MEM_RESERVE; pages are made accessible
 *     (POSIX mprotect) or committed (Windows VirtualAlloc MEM_COMMIT) as the
 *     memory grows. Anonymous pages are zero-filled on first touch, so the
 *     game's sparse DS address map (main RAM at 0x02000000, VRAM at
 *     0x06000000, C heap above 0x08000000) only costs the pages it uses.
 *   - The base never moves, so host pointers into guest memory stay valid
 *     across memory.grow, which np_core_guest_ptr's contract relies on.
 *
 * TRADEOFF: generated code is built in wabt's guard-page mode with no signal
 * handler (WASM_RT_SKIP_SIGNAL_RECOVERY) and no explicit bounds checks, for
 * speed. An access between the current size and the end of the reservation
 * faults (the process crashes, it does not trap). On 64-bit hosts other than
 * iOS the reservation is extended with PROT_NONE / MEM_RESERVE address space
 * to the full 8 GiB a u32 address plus a u32 offset can reach, so a wild
 * guest pointer past the maximum faults too. Without that trailing guard it
 * landed in whatever host mapping followed the reservation: sometimes a
 * crash, sometimes a silent write into host memory, depending on the run's
 * address-space layout (seen on Diamond: one run in five survived a wild
 * expheap pointer). On iOS, where address space is capped, such an access
 * is still NOT caught. The guest is trusted code we built ourselves, so
 * crashing is accepted. Configure with -DNP_BOUNDS_CHECK=ON to compile the
 * generated code in wabt's bounds-check mode instead: every access is
 * checked against the current size and OOB becomes a clean wasm trap that
 * np_core reports as an error, at a substantial speed cost. Debug with that.
 *
 * Memory operations are not locked: one core runs on one thread.
 */
#include "wasm-rt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "np_memory.h"

/* Address space reserved per memory (see TRADEOFF above): base + u32 + u32
 * stays inside it, so every out-of-range access faults. */
#if UINTPTR_MAX > 0xFFFFFFFFu && !(defined(__APPLE__) && TARGET_OS_IPHONE)
#define NP_MEMORY_GUARD_SPAN (8ull << 30)
#else
#define NP_MEMORY_GUARD_SPAN 0ull
#endif

const char *np_memory_failure;

/* The address space wasm_rt_allocate_memory reserves for a memory of
 * `reserve` bytes at most. */
static uint64_t reserve_span(uint64_t reserve) {
    return reserve && reserve < NP_MEMORY_GUARD_SPAN ? NP_MEMORY_GUARD_SPAN : reserve;
}

static void *os_reserve(uint64_t size) {
    if (size > (uint64_t)SIZE_MAX) return NULL;
#ifdef _WIN32
    return VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
#else
    void *p = mmap(NULL, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static int os_commit(uint8_t *addr, uint64_t size) {
    if (size == 0) return 0;
#ifdef _WIN32
    return VirtualAlloc(addr, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE) == addr ? 0 : -1;
#else
    return mprotect(addr, (size_t)size, PROT_READ | PROT_WRITE);
#endif
}

static void os_release(uint8_t *addr, uint64_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(addr, 0, MEM_RELEASE);
#else
    munmap(addr, (size_t)size);
#endif
}

/* Instantiation has no error return. Record why and raise a wasm trap: the
 * runtime instantiates on a guest fiber with WASM_RT_TRAP_HANDLER set, so
 * the trap fails np_core_create cleanly instead of crashing the app. */
static void fail(const char *why) {
    np_memory_failure = why;
    wasm_rt_trap(WASM_RT_TRAP_OOB);
}

bool wasm_rt_memory_is_default32(const wasm_rt_memory_t *memory) {
    return memory->page_size == WASM_DEFAULT_PAGE_SIZE && !memory->is64;
}

void wasm_rt_allocate_memory(wasm_rt_memory_t *memory, uint64_t initial_pages, uint64_t max_pages, bool is64,
                             uint32_t page_size) {
    memory->data = NULL;
    memory->data_end = NULL;
    memory->page_size = page_size;
    memory->is64 = is64;
    memory->pages = 0;
    memory->size = 0;

    const uint64_t cap_pages = NP_MEMORY_RESERVE_CAP / page_size;
    if (max_pages > cap_pages) max_pages = cap_pages;
    memory->max_pages = max_pages;
    if (initial_pages > max_pages) fail("guest memory: initial size exceeds the reservation cap");

    const uint64_t reserve = max_pages * page_size;
    const uint64_t initial = initial_pages * page_size;
    uint8_t *base = reserve ? os_reserve(reserve_span(reserve)) : NULL;
    if (reserve && !base) fail("guest memory: could not reserve address space");
    if (os_commit(base, initial) != 0) {
        os_release(base, reserve_span(reserve));
        fail("guest memory: could not commit the initial pages");
    }
    memory->data = base;
    memory->data_end = base + reserve;
    memory->pages = initial_pages;
    memory->size = initial;
}

uint64_t wasm_rt_grow_memory(wasm_rt_memory_t *memory, uint64_t delta) {
    const uint64_t old_pages = memory->pages;
    if (delta == 0) return old_pages;
    if (delta > memory->max_pages - old_pages) return (uint64_t)-1;
    const uint64_t new_pages = old_pages + delta;
    if (os_commit(memory->data + memory->size, delta * memory->page_size) != 0) return (uint64_t)-1;
    memory->pages = new_pages;
    memory->size = new_pages * memory->page_size;
    return old_pages;
}

void wasm_rt_free_memory(wasm_rt_memory_t *memory) {
    if (memory->data) os_release(memory->data, reserve_span((uint64_t)(memory->data_end - memory->data)));
    memory->data = NULL;
    memory->data_end = NULL;
    memory->pages = 0;
    memory->size = 0;
}

/* ---- snapshot support ------------------------------------------------ */

size_t np_memory_os_page(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (size_t)p : 4096;
#endif
}

/*
 * Which pages the guest ever touched. On Apple systems mincore() reports a
 * disposition for every page of an anonymous mapping that has a backing
 * page: resident (MINCORE_INCORE), or compressed / swapped
 * (MINCORE_PAGED_OUT), so a zero disposition is a page that was never
 * touched (or was discarded) and reads as zeros. Linux's mincore only
 * reports residency, which would miss swapped pages, and Windows has no
 * equivalent, so elsewhere every page is a candidate and the caller's zero
 * check does the work.
 */
int np_memory_touched(const wasm_rt_memory_t *memory, uint8_t *map) {
#if defined(__APPLE__)
    const size_t page = np_memory_os_page();
    const size_t n = (size_t)((memory->size + page - 1) / page);
    if (n == 0) return 0;
    if (mincore(memory->data, (size_t)memory->size, (char *)map) != 0) return -1;
    const unsigned char backed = MINCORE_INCORE | MINCORE_PAGED_OUT | MINCORE_MODIFIED | MINCORE_REFERENCED;
    for (size_t i = 0; i < n; i++) map[i] = (map[i] & backed) != 0;
    return 0;
#else
    (void)memory;
    (void)map;
    return -1;
#endif
}

int np_memory_discard(wasm_rt_memory_t *memory, uint64_t offset, uint64_t len) {
    if (len == 0) return 0;
    if (offset + len > memory->size) return -1;
    uint8_t *p = memory->data + offset;
#ifdef _WIN32
    if (!VirtualFree(p, (SIZE_T)len, MEM_DECOMMIT)) return -1;
    return os_commit(p, len);
#else
    /* A fresh anonymous mapping over the range: zero, and untouched again
     * as far as np_memory_touched is concerned. Same address, same
     * protection, still inside the reservation. */
    void *r = mmap(p, (size_t)len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return r == (void *)p ? 0 : -1;
#endif
}

int np_memory_set_pages(wasm_rt_memory_t *memory, uint64_t pages) {
    if (pages > memory->max_pages) return -1;
    if (pages > memory->pages) return wasm_rt_grow_memory(memory, pages - memory->pages) == (uint64_t)-1 ? -1 : 0;
    if (pages < memory->pages) {
        const uint64_t keep = pages * memory->page_size;
        /* Zero what is given up: a later memory.grow must see zeros. The
         * pages stay accessible, which is harmless (the guest bounds its
         * own accesses by memory.size). */
        if (np_memory_discard(memory, keep, memory->size - keep) != 0) return -1;
        memory->pages = pages;
        memory->size = keep;
    }
    return 0;
}
