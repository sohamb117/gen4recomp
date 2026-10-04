/*
 * np_fiber: native stackful coroutines for the guest runtime.
 *
 * Why: the game's OS layer runs many threads, but iOS gives us one process,
 * no JIT and no reason to pay for real threads that would only ever run one
 * at a time anyway. Every guest thread therefore runs on a native fiber, and
 * the runtime switches fibers inside wasm2c imports (fiber_switch, vblank,
 * trap). wasm2c code keeps all of its state either in the instance struct or
 * on the native stack, so swapping native stacks (plus the instance's shadow
 * stack pointer, done by the caller) is a complete context switch.
 *
 * Implementations:
 *   - POSIX arm64 / x86_64: np_fiber_posix.c owns stacks (mmap with a
 *     PROT_NONE guard page below each one); the register switch lives in
 *     np_fiber_arm64.S / np_fiber_x86_64.S and saves exactly the callee-saved
 *     state of the platform ABI.
 *   - Windows: np_fiber_win32.c on top of Win32 Fibers, which already save
 *     the full x64 / arm64 nonvolatile set, TEB stack bounds and SEH chain.
 *
 * All calls for one set of fibers must come from one thread.
 */
#ifndef NP_FIBER_H
#define NP_FIBER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_FIBER_DEFAULT_STACK ((size_t)512 * 1024)

typedef struct np_fiber np_fiber;

/* A fiber body. It must never return: switch away for good instead (the
 * POSIX trampolines execute a trapping instruction if it does). */
typedef void (*np_fiber_fn)(void *arg);

/* Wraps the calling thread's own stack as a fiber so that it can be switched
 * away from and back to. Returns NULL on failure. Undo with
 * np_fiber_leave_thread once every other fiber is destroyed. */
np_fiber *np_fiber_enter_thread(void);
void np_fiber_leave_thread(np_fiber *self);

/* A suspended fiber that will run fn(arg) the first time it is switched to.
 * stack_size 0 means NP_FIBER_DEFAULT_STACK; it is rounded up to whole pages.
 * Returns NULL on failure. */
np_fiber *np_fiber_create(size_t stack_size, np_fiber_fn fn, void *arg);

/* Suspends `from`, which must be the running fiber, and resumes `to`. Returns
 * when something switches back to `from`. */
void np_fiber_switch(np_fiber *from, np_fiber *to);

/* Releases a fiber that is not running. Its stack is freed without being
 * unwound, so it must not own anything that needs cleanup. */
void np_fiber_destroy(np_fiber *fiber);

/*
 * Stack pooling and snapshots (np_core_state_*). A snapshot holds the live
 * bytes of every parked fiber's stack and puts them back at the same
 * addresses, so the runtime keeps each stack mapped for the life of the
 * core and re-arms it instead of destroying and creating.
 */

/* Usable stack bytes of a fiber from np_fiber_create. */
size_t np_fiber_stack_size(const np_fiber *fiber);

/* Re-arms a fiber that is not running so that the next switch to it runs
 * fn(arg) from the top of the stack it already has. Returns 0 or -1. */
int np_fiber_reset(np_fiber *fiber, np_fiber_fn fn, void *arg);

/* The live part of a suspended fiber's stack, [*lo, *hi): every byte a
 * resume can read. Returns -1 where the platform's fibers are opaque
 * (Windows), which makes snapshots unavailable there. */
int np_fiber_live_stack(const np_fiber *fiber, uint8_t **lo, uint8_t **hi);

/* Makes a suspended fiber resume from `lo`, a value np_fiber_live_stack
 * returned for this fiber earlier; the caller puts the bytes [lo, hi) back.
 * Returns -1 if `lo` is not inside the stack. */
int np_fiber_set_live_stack(np_fiber *fiber, uint8_t *lo);

#ifdef __cplusplus
}
#endif

#endif /* NP_FIBER_H */
