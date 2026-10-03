/*
 * 3ds/src/3ds_os_context.h: the SDK scheduler's three context primitives.
 *
 * The primitives themselves are the SDK's own names and are declared by
 * <nitro/os.h>; this header is only what the port adds around them. See
 * 3ds_os_context.c for the model and for what this backend does differently
 * from pc/src/pc_os_context.c, which is not linked on this console.
 */

#ifndef POKEPLATINUM_3DS_OS_CONTEXT_H
#define POKEPLATINUM_3DS_OS_CONTEXT_H

/*
 * 3ds/src/3ds_ctx_switch.s. `ctx_start` begins a context that has never run,
 * on a fresh host stack; `ctx_resume` puts a saved one back and returns TRUE
 * from the OS_SaveContext that saved it. Neither returns to its caller.
 * OS_SaveContext lives in that file too and is declared by <nitro/os.h>.
 */
void ctx_start(void *sp_top, unsigned long context, void (*trampoline)(unsigned long));
void ctx_resume(unsigned int *buf);

/*
 * Console self-test: two contexts, a ping-pong between them, the argument and
 * the return address the scheduler writes into the struct, and the stack
 * canaries. Returns failures and sets *ran. It really switches stacks, so it
 * runs on the console and not on a build machine.
 */
int os_context_selftest(int *ran);

/* Which check went first, or -1. */
int os_context_first_failure(void);

/* Host stack bytes handed to each dispatched context, and how many have one.
 * The budget is where these become more than a number. */
unsigned long os_context_stack_bytes(void);
int os_context_live(void);

#endif /* POKEPLATINUM_3DS_OS_CONTEXT_H */
