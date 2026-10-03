/*
 * Windows fibers on top of Win32 Fibers.
 *
 * Win32 fibers already do everything a hand-written switch would have to:
 * they save the x64 (or arm64) nonvolatile registers including xmm6-xmm15,
 * swap the TEB stack base/limit that the kernel and SEH unwinding consult,
 * and give each stack a guard page with on-demand commit. Hand-rolling that
 * in assembly would only add ways to break longjmp and structured
 * exceptions, so this file is a thin adapter.
 *
 * CreateFiberEx reserves stack_size and commits a single page; the stack
 * grows through the system guard page, so the 512 KiB default is lazily
 * backed like the POSIX version.
 */
#if defined(_WIN32)

#include "np_fiber.h"

#include <stdlib.h>
#include <windows.h>

struct np_fiber {
    LPVOID handle;
    np_fiber_fn fn;
    void *arg;
    BOOL converted; /* enter_thread converted the thread; leave undoes it */
};

np_fiber *np_fiber_enter_thread(void) {
    np_fiber *f = calloc(1, sizeof(np_fiber));
    if (!f) return NULL;
    if (IsThreadAFiber()) {
        f->handle = GetCurrentFiber();
    } else {
        f->handle = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
        f->converted = TRUE;
    }
    if (!f->handle) {
        free(f);
        return NULL;
    }
    return f;
}

void np_fiber_leave_thread(np_fiber *self) {
    if (!self) return;
    if (self->converted) ConvertFiberToThread();
    free(self);
}

static VOID CALLBACK fiber_main(LPVOID param) {
    np_fiber *f = param;
    f->fn(f->arg);
    /* The body must never return: returning from a fiber procedure exits
     * the whole thread. Fail loudly instead. */
    abort();
}

np_fiber *np_fiber_create(size_t stack_size, np_fiber_fn fn, void *arg) {
    np_fiber *f = calloc(1, sizeof(np_fiber));
    if (!f) return NULL;
    if (stack_size == 0) stack_size = NP_FIBER_DEFAULT_STACK;
    f->fn = fn;
    f->arg = arg;
    f->handle = CreateFiberEx(0, stack_size, FIBER_FLAG_FLOAT_SWITCH, fiber_main, f);
    if (!f->handle) {
        free(f);
        return NULL;
    }
    return f;
}

void np_fiber_switch(np_fiber *from, np_fiber *to) {
    (void)from;
    SwitchToFiber(to->handle);
}

void np_fiber_destroy(np_fiber *fiber) {
    if (!fiber) return;
    DeleteFiber(fiber->handle);
    free(fiber);
}

#endif /* _WIN32 */
