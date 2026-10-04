/*
 * POSIX fibers (macOS, iOS, Linux, Android) for arm64 and x86_64.
 *
 * Each fiber owns one anonymous mapping: a PROT_NONE guard page at the low
 * end (stacks grow down, so an overflow faults instead of silently eating
 * the neighbouring mapping), the stack, and the np_fiber record itself in
 * the top bytes so creating a fiber is a single mmap. Pages are only backed
 * when touched, so the default 512 KiB costs a few KiB of RSS per fiber.
 *
 * The register switch is in assembly (np_fiber_<arch>.S) because it has to
 * swap the stack pointer, which C cannot express. A new fiber gets a
 * hand-built frame that looks as if it had called np_fiber_asm_switch, whose
 * return address is np_fiber_asm_start; the trampoline moves (fn, arg) out of
 * callee-saved registers and calls fn. The frame layouts here must match the
 * .S files exactly.
 */
#if !defined(_WIN32)

#include "np_fiber.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

struct np_fiber {
    void *sp;       /* saved stack pointer while suspended */
    void *map_base; /* guard page + stack + this record; NULL for a thread */
    size_t map_size;
};

/* Saves the callee-saved state on the current stack, stores the stack
 * pointer to *save_sp, then restores the state saved at new_sp. */
extern void np_fiber_asm_switch(void **save_sp, void *new_sp);
/* First return target of a fresh fiber; see the .S files. */
extern void np_fiber_asm_start(void);

np_fiber *np_fiber_enter_thread(void) {
    return calloc(1, sizeof(np_fiber));
}

void np_fiber_leave_thread(np_fiber *self) {
    free(self);
}

static size_t page_size(void) {
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (size_t)p : 4096;
}

/* Builds the frame np_fiber_asm_switch pops on the first switch to `top`'s
 * fiber and returns its stack pointer. `top` is 16-byte aligned. */
static void *initial_frame(uint8_t *top, np_fiber_fn fn, void *arg) {
#if defined(__aarch64__)
    /* 0xb0 bytes: x19..x30 at 0x00..0x58, d8..d15 at 0x60..0x98. */
    uint64_t *f = (uint64_t *)(top - 0xb0);
    memset(f, 0, 0xb0);
    f[0] = (uint64_t)(uintptr_t)arg;                 /* x19 */
    f[1] = (uint64_t)(uintptr_t)fn;                  /* x20 */
    f[11] = (uint64_t)(uintptr_t)np_fiber_asm_start; /* x30: return address */
    return f;
#elif defined(__x86_64__)
    /* 64 bytes: control words, r15, r14, r13, r12, rbx, rbp, return
     * address. After the final `ret` rsp == top, which is 16-byte aligned,
     * so the trampoline's `call` sees the alignment the ABI requires. */
    uint64_t *f = (uint64_t *)(top - 64);
    memset(f, 0, 64);
    uint32_t mxcsr = 0x1F80; /* all exceptions masked, round to nearest */
    uint16_t fpucw = 0x037F; /* same for x87, 64-bit precision */
    memcpy((uint8_t *)f, &mxcsr, 4);
    memcpy((uint8_t *)f + 4, &fpucw, 2);
    f[3] = (uint64_t)(uintptr_t)arg;                 /* r13 */
    f[4] = (uint64_t)(uintptr_t)fn;                  /* r12 */
    f[7] = (uint64_t)(uintptr_t)np_fiber_asm_start;  /* return address */
    return f;
#else
#error "np_fiber: unsupported architecture (need arm64 or x86_64)"
#endif
}

np_fiber *np_fiber_create(size_t stack_size, np_fiber_fn fn, void *arg) {
    const size_t page = page_size();
    if (stack_size == 0) stack_size = NP_FIBER_DEFAULT_STACK;
    stack_size = (stack_size + page - 1) & ~(page - 1);
    const size_t map_size = page + stack_size;

    uint8_t *base = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;
    if (mprotect(base, page, PROT_NONE) != 0) {
        munmap(base, map_size);
        return NULL;
    }

    uintptr_t rec = ((uintptr_t)(base + map_size) - sizeof(np_fiber)) & ~(uintptr_t)15;
    np_fiber *f = (np_fiber *)rec;
    f->map_base = base;
    f->map_size = map_size;
    f->sp = initial_frame((uint8_t *)rec, fn, arg);
    return f;
}

void np_fiber_switch(np_fiber *from, np_fiber *to) {
    np_fiber_asm_switch(&from->sp, to->sp);
}

void np_fiber_destroy(np_fiber *fiber) {
    if (fiber && fiber->map_base) munmap(fiber->map_base, fiber->map_size);
}

/* The usable stack runs from just above the guard page up to the record. */
static uint8_t *stack_low(const np_fiber *f) {
    return (uint8_t *)f->map_base + page_size();
}

size_t np_fiber_stack_size(const np_fiber *fiber) {
    return (size_t)((const uint8_t *)fiber - stack_low(fiber));
}

int np_fiber_reset(np_fiber *fiber, np_fiber_fn fn, void *arg) {
    if (!fiber->map_base) return -1;
    fiber->sp = initial_frame((uint8_t *)fiber, fn, arg);
    return 0;
}

int np_fiber_live_stack(const np_fiber *fiber, uint8_t **lo, uint8_t **hi) {
    if (!fiber->map_base) return -1;
    *lo = fiber->sp;
    *hi = (uint8_t *)fiber;
    return 0;
}

int np_fiber_set_live_stack(np_fiber *fiber, uint8_t *lo) {
    if (!fiber->map_base || lo < stack_low(fiber) || lo >= (uint8_t *)fiber || ((uintptr_t)lo & 7) != 0) return -1;
    fiber->sp = lo;
    return 0;
}

#endif /* !_WIN32 */
