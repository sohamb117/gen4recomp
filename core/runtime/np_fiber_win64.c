/*
 * Windows x64 fibers with stacks this file owns (GNU-assembler toolchains:
 * mingw, zig cc, clang). MSVC builds use np_fiber_win32.c instead.
 *
 * Why not Win32 Fibers: a suspended Win32 fiber keeps its registers in an
 * opaque kernel32 structure, so its stack bytes alone cannot be captured
 * and put back, and in-session snapshots (np_snapshot.c) need exactly that.
 * Here, as in np_fiber_posix.c, the switch (np_fiber_win64.S) pushes every
 * callee-saved register and the TEB's stack bounds onto the suspended
 * stack, and the fiber record holds only the stack pointer.
 *
 * Each fiber owns one VirtualAlloc region: a PAGE_NOACCESS guard page at the
 * low end, the stack, and the np_fiber record in the top bytes. The stack is
 * committed up front (the OS backs a committed page only when it is first
 * touched, so the default 512 KiB costs what is used), which keeps stack
 * probes from depending on guard-page growth. The TEB bounds a new fiber
 * starts with are its own stack's, so SEH unwinding, __chkstk and longjmp
 * see consistent limits on every fiber.
 */
#if defined(_WIN32)

#include "np_fiber.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

struct np_fiber {
    void *sp;       /* saved stack pointer while suspended */
    void *map_base; /* guard page + stack + this record; NULL for a thread */
    size_t map_size;
};

#define FRAME_BYTES 264 /* np_fiber_win64.S */

extern void np_fiber_asm_switch(void **save_sp, void *new_sp);
extern void np_fiber_asm_start(void);

np_fiber *np_fiber_enter_thread(void) {
    return calloc(1, sizeof(np_fiber));
}

void np_fiber_leave_thread(np_fiber *self) {
    free(self);
}

static size_t page_size(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
}

static uint8_t *stack_low(const np_fiber *f) {
    return (uint8_t *)f->map_base + page_size();
}

/* The frame np_fiber_asm_switch pops on the first switch to `f`. Everything
 * above it up to the record is zero, so a stack walk that reaches
 * np_fiber_asm_start finds a null return address and stops. */
static void *initial_frame(np_fiber *f, np_fiber_fn fn, void *arg) {
    uint8_t *top = (uint8_t *)f;
    uint8_t *frame = top - FRAME_BYTES - 64;
    uint64_t *q = (uint64_t *)frame;
    memset(frame, 0, (size_t)(top - frame));
    const uint32_t mxcsr = 0x1F80; /* all exceptions masked, round to nearest */
    const uint16_t fpucw = 0x027F; /* the x64 Windows default: 53-bit precision */
    memcpy(frame, &mxcsr, 4);
    memcpy(frame + 4, &fpucw, 2);
    q[21] = (uint64_t)(uintptr_t)f->map_base;    /* DeallocationStack */
    q[22] = (uint64_t)(uintptr_t)stack_low(f);   /* StackLimit */
    q[23] = (uint64_t)(uintptr_t)top;            /* StackBase */
    q[26] = (uint64_t)(uintptr_t)arg;            /* r13 */
    q[27] = (uint64_t)(uintptr_t)fn;             /* r12 */
    q[32] = (uint64_t)(uintptr_t)np_fiber_asm_start; /* return address */
    return frame;
}

np_fiber *np_fiber_create(size_t stack_size, np_fiber_fn fn, void *arg) {
    const size_t page = page_size();
    if (stack_size == 0) stack_size = NP_FIBER_DEFAULT_STACK;
    stack_size = (stack_size + page - 1) & ~(page - 1);
    const size_t map_size = page + stack_size;

    uint8_t *base = VirtualAlloc(NULL, map_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) return NULL;
    DWORD old;
    if (!VirtualProtect(base, page, PAGE_NOACCESS, &old)) {
        VirtualFree(base, 0, MEM_RELEASE);
        return NULL;
    }
    uintptr_t rec = ((uintptr_t)(base + map_size) - sizeof(np_fiber)) & ~(uintptr_t)15;
    np_fiber *f = (np_fiber *)rec;
    f->map_base = base;
    f->map_size = map_size;
    f->sp = initial_frame(f, fn, arg);
    return f;
}

void np_fiber_switch(np_fiber *from, np_fiber *to) {
    np_fiber_asm_switch(&from->sp, to->sp);
}

void np_fiber_destroy(np_fiber *fiber) {
    if (fiber && fiber->map_base) VirtualFree(fiber->map_base, 0, MEM_RELEASE);
}

size_t np_fiber_stack_size(const np_fiber *fiber) {
    return (size_t)((const uint8_t *)fiber - stack_low(fiber));
}

int np_fiber_reset(np_fiber *fiber, np_fiber_fn fn, void *arg) {
    if (!fiber->map_base) return -1;
    fiber->sp = initial_frame(fiber, fn, arg);
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

#endif /* _WIN32 */
