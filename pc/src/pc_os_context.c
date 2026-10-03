/*
 * OSContext switching on host fibers (ucontext).
 *
 * The SDK's thread machinery, queues, priorities, the scheduler in
 * os_thread.c: is compiled C in this build and runs as-is. Only the
 * three context primitives were mwcc asm, and their contract, read from
 * the scheduler's own call sites, is setjmp-shaped:
 *
 *   OS_SaveContext(ctx)  returns FALSE right after saving, and "returns"
 *                        TRUE when a later OS_LoadContext resumes ctx
 *                        (OSi_RescheduleThread: `if (OS_SaveContext(..))
 *                        return;` is the resumed path).
 *   OS_LoadContext(ctx)  never returns.
 *   OS_InitContext(ctx, pc, sp) prepares a fresh context; the caller then
 *                        writes ctx->r[0] (the argument) and ctx->lr (the
 *                        return-to function, OS_ExitThread) directly into
 *                        the struct, so those are read at first dispatch,
 *                        not at init.
 *
 * Execution runs on malloc'd HOST stacks, not on the guest stack the sp
 * argument names. Host code needs far more stack than the 4-16 KB a DS
 * thread gets, and native locals are host data anyway. The guest stack
 * still exists and keeps its OSi_STACK_CHECKNUM canaries, so the SDK's
 * stack checks pass; what is lost is real overflow detection into those
 * canaries, which host guard pages cover instead. Recorded, not hidden.
 */
#include <nitro/os.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(_WIN32)
#include <setjmp.h>
#include "pc_win_fiber.h"
#else
/*
 * The third implementation, and which host gets it.
 *
 * bionic has no ucontext: <sys/ucontext.h> is there for signal handlers, but
 * getcontext/makecontext/setcontext were never provided on 32-bit Android,
 * and a shared object that references them does not load, because Android's
 * linker binds everything up front.
 *
 * So the save and restore is written out here, and the first thing tried was
 * the shorter answer: _setjmp/_longjmp plus a stack switch. It does not work.
 * glibc redirects longjmp to __longjmp_chk under _FORTIFY_SOURCE, which is on
 * by default on this distribution, and that check exists precisely to refuse a
 * jump to another stack, "longjmp causes uninitialized stack frame", on the
 * first switch. Writing the ten instructions removes the fortify question and
 * bionic's own longjmp from the path at the same time, and what is left is the
 * AAPCS and nothing else.
 *
 * PC_CTX_FIBER=1 forces it on any POSIX host so it can be TESTED where the
 * tests are. That is the point of the switch: bionic is the one host this
 * tree cannot run anything on, so the code that runs there is the code the
 * x86 suite and the qemu-arm suite have both already run.
 */
#if !defined(PC_CTX_FIBER)
#if defined(__BIONIC__)
#define PC_CTX_FIBER 1
#else
#define PC_CTX_FIBER 0
#endif
#endif
#if !PC_CTX_FIBER
#include <ucontext.h>
#endif
#endif

#if !defined(_WIN32) && PC_CTX_FIBER
/*
 * One saved execution point: the callee-saved registers, the stack pointer
 * and the return address, which between them are everything the AAPCS (or
 * cdecl) says a function call preserves. Sized well past what any of the
 * three architectures below writes.
 */
typedef struct PcCtxJmp {
    void *reg[32];
} PcCtxJmp;
#endif

#define PC_CTX_MAX 64
#define PC_CTX_HOST_STACK (256 * 1024)

typedef struct PcCtx {
    OSContext *key;
#if defined(_WIN32)
    void *fiber;     /* the context's execution home; main thread's for the
                        boot context, CreateFiber'd for the rest */
    jmp_buf jb;      /* where OS_SaveContext parked, on that fiber */
#elif PC_CTX_FIBER
    PcCtxJmp jb;     /* where OS_SaveContext parked, on this context's stack */
#else
    ucontext_t uc;
#endif
    void *hostStack;
    u32 entryPc;
    int fresh;   /* InitContext'd, not yet dispatched */
    volatile int resumed;
} PcCtx;

#if defined(_WIN32)
/*
 * Windows fibers have no ucontext shape, SwitchToFiber resumes a fiber
 * wherever it last switched away, which is inside some OS_LoadContext call,
 * so the setjmp half restores the SDK's contract: OS_SaveContext must
 * "return TRUE" at resumption. sCurrent names the context being resumed;
 * the switched-to fiber longjmps to that context's save point.
 */
static PcCtx *sCurrent;

static void ctx_trampoline_win(void *arg);
#endif

#if !defined(_WIN32)
/*
 * A context's stack, with a wall at the bottom of it.
 *
 * These used to be malloc'd. A malloc'd stack that overflows does not fault:
 * It walks quietly down into whatever the allocator put below it, and what
 * comes back is a context resuming onto a return address somebody else wrote.
 * That is not a theory; it is what an Android boot looked like, a jump into
 * .bss with no explanation anywhere near the code that caused it.
 *
 * So the stack is its own mapping with a PROT_NONE page under it. An overflow
 * is now a SIGSEGV at a page boundary one page below a stack, which the fault
 * handler reports with an address that says exactly that, and nothing else in
 * the process is touched.
 */
static void *ctx_stack_alloc(size_t size)
{
    long pagesz = sysconf(_SC_PAGESIZE);
    size_t guard = (pagesz > 0) ? (size_t)pagesz : 4096u;
    char *p;

    p = mmap(NULL, size + guard, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    /* The stack grows down, so the wall goes at the low end. */
    if (mprotect(p, guard, PROT_NONE) != 0) {
        /* Not fatal: a stack with no wall is what this always used to be. */
        fprintf(stderr, "pc_os_context: no guard page (%s)\n", strerror(errno));
    }
    return p + guard;
}
#endif

static PcCtx sCtxTable[PC_CTX_MAX];

static PcCtx *ctx_slot(OSContext *key)
{
    int i, free_i = -1;
    for (i = 0; i < PC_CTX_MAX; i++) {
        if (sCtxTable[i].key == key) {
            return &sCtxTable[i];
        }
        if (free_i < 0 && sCtxTable[i].key == NULL) {
            free_i = i;
        }
    }
    if (free_i < 0) {
        fprintf(stderr, "pc_os_context: more than %d live OSContexts\n",
                PC_CTX_MAX);
        abort();
    }
    sCtxTable[free_i].key = key;
    return &sCtxTable[free_i];
}

void OS_InitContext(OSContext *context, u32 newpc, u32 newsp)
{
    PcCtx *e = ctx_slot(context);
    e->entryPc = newpc;
    e->fresh = 1;
    e->resumed = 0;
    /* The struct's own fields are documentation here (the scheduler never
     * reads pc/sp back), but keep them true. */
    context->pc_plus4 = newpc + 4;
    context->sp = newsp;
    /* pc_plus4 is the thread entry, a host function. Marked so the
     * digest skips it when the OSThread itself lives in guest memory.
     * Today's four live threads are host-static and this is a no-op
     * for them (the address is above 0x10000000). */
    {
        extern void pc_state_mark_host_field(const void *field);
        pc_state_mark_host_field(&context->pc_plus4);
    }
}

/* First dispatch of a fresh context: call the entry with the argument the
 * creator wrote into ctx->r[0]; a return goes to whatever it wrote in
 * ctx->lr (OS_ExitThread), exactly as the asm's register state would. */
static void ctx_trampoline(int ctx_as_int)
{
    OSContext *context = (OSContext *)ctx_as_int;
    PcCtx *e = ctx_slot(context);
    void (*entry)(u32) = (void (*)(u32))e->entryPc;
    entry(context->r[0]);
    if (context->lr != 0) {
        ((void (*)(void))context->lr)();
    }
    fprintf(stderr,
            "pc_os_context: context %p entry returned with lr=0\n",
            (void *)context);
    abort();
}

#if defined(_WIN32)

static void ctx_trampoline_win(void *arg)
{
    ctx_trampoline((int)(intptr_t)arg);
}

BOOL OS_SaveContext(OSContext *context)
{
    PcCtx *e = ctx_slot(context);

    e->fresh = 0;
    /* This context's execution home is whatever fiber it runs on now,
     * the main thread's for the boot context (converted on first ask). */
    e->fiber = pcw_self_fiber();
    if (setjmp(e->jb)) {
        return TRUE;                    /* resumed by OS_LoadContext */
    }
    return FALSE;
}

void OS_LoadContext(OSContext *context)
{
    PcCtx *e = ctx_slot(context);

    if (e->fresh) {
        e->fresh = 0;
        e->fiber = pcw_create_fiber(PC_CTX_HOST_STACK, ctx_trampoline_win,
                                    (void *)context);
        if (e->fiber == NULL) {
            fprintf(stderr, "pc_os_context: CreateFiber failed\n");
            abort();
        }
    }
    sCurrent = e;
    if (e->fiber == pcw_self_fiber()) {
        /* Loading the context we are already on: no fiber switch will run,
         * so jump straight to its save point. */
        longjmp(e->jb, 1);
    }
    pcw_switch_fiber(e->fiber);
    /*
     * Control comes back here when SOMEONE resumes the fiber this call
     * switched away from; that is, when this fiber's own context is
     * loaded again. The contract says that resumption appears as
     * OS_SaveContext returning TRUE, and sCurrent says which context the
     * resumer meant.
     */
    longjmp(sCurrent->jb, 1);
}

#elif PC_CTX_FIBER

/*
 * The three primitives, and every one of them is the ABI written out.
 *
 *   pc_ctx_save(b)   parks here. Returns 0 now and 1 when pc_ctx_load(b)
 *                    resumes it, setjmp's contract, which is also
 *                    OS_SaveContext's.
 *   pc_ctx_load(b)   resumes a parked point and never returns.
 *   pc_ctx_start(..) begins fn(arg) on a stack of its own and never returns,
 *                    which is the one thing a save/restore pair cannot do and
 *                    the only reason makecontext existed here.
 *
 * Naked, so no prologue is emitted onto a stack that is about to be replaced,
 * and basic asm only, which is all a naked function may contain. The floating
 * point registers are saved on ARM because the AAPCS makes d8-d15 callee-saved
 * and the compiler will hold live values in them across this call.
 *
 * No signal mask is saved. A DS context does not have one, and saving it would
 * put a sigprocmask syscall on every thread switch.
 */
#if defined(__arm__)
/*
 * TYPED `OSContext *` and it is not one. A musttail call has to match its
 * caller's signature exactly, OS_SaveContext takes an OSContext *, and this
 * function reads r0 as nothing but an address. The cast is at the one call
 * site and says the same thing.
 */
__attribute__((naked))
static BOOL pc_ctx_save(OSContext *b)
{
    __asm__("mov    r1, sp\n\t"
            "stmia  r0!, {r1, r4-r11, lr}\n\t"
#if defined(__ARM_FP)
            "vstmia r0!, {d8-d15}\n\t"
#endif
            "mov    r0, #0\n\t"
            "bx     lr");
}
__attribute__((naked, noreturn))
static void pc_ctx_load(PcCtxJmp *b)
{
    __asm__("ldmia  r0!, {r1, r4-r11, lr}\n\t"
#if defined(__ARM_FP)
            "vldmia r0!, {d8-d15}\n\t"
#endif
            "mov    sp, r1\n\t"
            "mov    r0, #1\n\t"
            "bx     lr");
}
__attribute__((naked, noreturn))
static void pc_ctx_start(void *sp, void (*fn)(int), int arg)
{
    __asm__("mov sp, r0\n\t"
            "mov r0, r2\n\t"
            "bx  r1");
}
#elif defined(__i386__)
__attribute__((naked))
static int pc_ctx_save(PcCtxJmp *b)
{
    __asm__("movl 4(%esp), %eax\n\t"
            "movl (%esp), %ecx\n\t"      /* the return address */
            "movl %ecx, 0(%eax)\n\t"
            "leal 4(%esp), %ecx\n\t"     /* sp as of that return */
            "movl %ecx, 4(%eax)\n\t"
            "movl %ebx, 8(%eax)\n\t"
            "movl %esi, 12(%eax)\n\t"
            "movl %edi, 16(%eax)\n\t"
            "movl %ebp, 20(%eax)\n\t"
            "xorl %eax, %eax\n\t"
            "ret");
}
__attribute__((naked, noreturn))
static void pc_ctx_load(PcCtxJmp *b)
{
    __asm__("movl 4(%esp), %eax\n\t"
            "movl 8(%eax), %ebx\n\t"
            "movl 12(%eax), %esi\n\t"
            "movl 16(%eax), %edi\n\t"
            "movl 20(%eax), %ebp\n\t"
            "movl 4(%eax), %esp\n\t"
            "movl 0(%eax), %ecx\n\t"
            "movl $1, %eax\n\t"
            "jmp *%ecx");
}
__attribute__((naked, noreturn))
static void pc_ctx_start(void *sp, void (*fn)(int), int arg)
{
    /* cdecl puts the arguments on the stack this is about to replace, so
     * read all three out first. At a call, esp must be 16-byte aligned. */
    __asm__("movl 4(%esp), %eax\n\t"
            "movl 8(%esp), %ecx\n\t"
            "movl 12(%esp), %edx\n\t"
            "movl %eax, %esp\n\t"
            "andl $-16, %esp\n\t"
            "subl $12, %esp\n\t"
            "pushl %edx\n\t"
            "call *%ecx");
}
#else
#error "pc_os_context: PC_CTX_FIBER has no context switch for this architecture"
#endif

/*
 * PC_CTX_TRACE=1: every switch, as it happens.
 *
 * A context switch that goes wrong leaves a program counter in the middle of
 * nowhere and a stack that no longer says where it came from, so the report
 * afterwards cannot name the switch that did it. This is the log that can:
 * which slot, which OSContext, the stack pointer being parked or restored, and
 * the return address it will resume to. Off by default and one branch when it
 * is off.
 */
static int ctx_trace = -1;

static int ctx_tracing(void)
{
    if (ctx_trace < 0) {
        const char *e = getenv("PC_CTX_TRACE");

        ctx_trace = (e != NULL && *e != '\0' && *e != '0') ? 1 : 0;
    }
    return ctx_trace;
}

static void ctx_say(const char *what, PcCtx *e, OSContext *key)
{
    if (!ctx_tracing()) {
        return;
    }
    fprintf(stderr, "pc_ctx: %-6s slot %2d ctx %p sp %08lx lr %08lx stack %p\n",
            what, (int)(e - sCtxTable), (void *)key,
            (unsigned long)(uintptr_t)e->jb.reg[0],
            (unsigned long)(uintptr_t)e->jb.reg[9], e->hostStack);
}

BOOL OS_SaveContext(OSContext *context)
{
    PcCtx *e = ctx_slot(context);

    e->fresh = 0;
    /*
     * A tail call, and it is the whole correctness of this function.
     *
     * pc_ctx_save parks the stack pointer it is called with. Called normally,
     * that is a pointer into this function's own frame, and this function
     * then returns FALSE, the frame is popped, and the scheduler calls
     * OS_LoadContext, whose frame lands on the same memory. When the context
     * is resumed later, the epilogue here pops a return address out of
     * somebody else's frame.
     *
     * That has always been true, of the ucontext implementation below as much
     * as of this one, and it survived on x86 and on armhf by luck: gcc gives
     * this function an 8-byte frame and OS_LoadContext a 16-byte one, so the
     * word the epilogue pops into pc happens to be OS_LoadContext's own return
     * address, which goes to the same place. clang lays the two frames out
     * differently and the same code jumps to zero on the first thread switch,
     * found on an ARM handheld, where it was a boot that hung with no message.
     *
     * Tail-calling removes the frame from the picture: pc_ctx_save parks the
     * SCHEDULER's stack pointer and the scheduler's return address, and the
     * scheduler's frame is live for exactly as long as the context is parked,
     * which is the invariant that was missing. Resuming returns 1 straight to
     * the scheduler, which is what TRUE is.
     *
     * gcc has no musttail before 15, so it keeps the arrangement it has been
     * running for two phases; PC_CTX_TRACE traces the load side, which says
     * everything the save side used to.
     */
#if defined(__clang__)
    __attribute__((musttail)) return pc_ctx_save((OSContext *)&e->jb);
#else
    return pc_ctx_save((OSContext *)&e->jb) ? TRUE : FALSE;
#endif
}

void OS_LoadContext(OSContext *context)
{
    PcCtx *e = ctx_slot(context);

    if (e->fresh) {
        char *top;

        e->fresh = 0;
        if (!e->hostStack) {
            e->hostStack = ctx_stack_alloc(PC_CTX_HOST_STACK);
            if (!e->hostStack) {
                fprintf(stderr, "pc_os_context: host stack alloc failed\n");
                abort();
            }
        }
        /* Grows down from the top, 16-aligned, which satisfies every ABI
         * pc_ctx_start knows about. */
        top = (char *)e->hostStack + PC_CTX_HOST_STACK;
        top = (char *)((uintptr_t)top & ~(uintptr_t)15);
        if (ctx_tracing()) {
            fprintf(stderr, "pc_ctx: start  slot %2d ctx %p pc %08lx top %p\n",
                    (int)(e - sCtxTable), (void *)context,
                    (unsigned long)e->entryPc, (void *)top);
        }
        pc_ctx_start(top, ctx_trampoline, (int)(intptr_t)context);
    }
    ctx_say("load", e, context);
    pc_ctx_load(&e->jb);
}

#else /* POSIX: ucontext */

BOOL OS_SaveContext(OSContext *context)
{
    PcCtx *e = ctx_slot(context);
    e->fresh = 0;
    e->resumed = 0;
    getcontext(&e->uc);
    if (e->resumed) {
        return TRUE;
    }
    e->resumed = 1;
    return FALSE;
}

void OS_LoadContext(OSContext *context)
{
    PcCtx *e = ctx_slot(context);
    if (e->fresh) {
        e->fresh = 0;
        if (!e->hostStack) {
            e->hostStack = ctx_stack_alloc(PC_CTX_HOST_STACK);
            if (!e->hostStack) {
                fprintf(stderr, "pc_os_context: host stack alloc failed\n");
                abort();
            }
        }
        getcontext(&e->uc);
        e->uc.uc_stack.ss_sp = e->hostStack;
        e->uc.uc_stack.ss_size = PC_CTX_HOST_STACK;
        e->uc.uc_link = NULL;
        makecontext(&e->uc, (void (*)(void))ctx_trampoline, 1,
                    (int)context);
    }
    setcontext(&e->uc);
    fprintf(stderr, "pc_os_context: setcontext returned\n");
    abort();
}

#endif
