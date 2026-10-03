/*
 * 3ds/src/3ds_os_context.c: OSContext switching, the third backend.
 *
 * The SDK's thread machinery, the queues, the priorities and the scheduler, is
 * compiled C in this build and runs as it shipped. Only three primitives were
 * mwcc assembly, and their contract, read from the scheduler's own call sites,
 * is setjmp-shaped:
 *
 *   OS_SaveContext(ctx)  returns FALSE right after saving and "returns" TRUE
 *                        when a later OS_LoadContext resumes ctx.
 *   OS_LoadContext(ctx)  never returns.
 *   OS_InitContext(ctx, pc, sp)  prepares a fresh context; the caller then
 *                        writes ctx->r[0] and ctx->lr into the struct.
 *
 * pc/src/pc_os_context.c answers that with getcontext and makecontext on POSIX
 * and with fibers on Windows. devkitARM has neither, so this is the third
 * backend. OS_SaveContext itself is in 3ds/src/3ds_ctx_switch.s, and that
 * file's head comment is where the reason lives: it is the difference between
 * a switch that works and one that freezes on the first resumption.
 *
 * Execution runs on host stacks, exactly as the PC port's does, and the sp the
 * scheduler passes is never used as a stack. Host code needs far more stack
 * than the 4 to 16 KB a DS thread is given, and the locals are host data
 * anyway. The guest stack still exists and still holds its canaries, so the
 * SDK's own stack checks pass and mean what they meant. What is lost is real
 * overflow detection into those canaries.
 *
 * The PC port covers that loss with host guard pages and this console has
 * none: there is no mprotect here, so an overrun would walk quietly into
 * whatever the heap put below the stack. So every host stack gets a guard word
 * at its lowest address, and every save and every load checks all of them, not
 * just the one being switched to: the stack that overran is the one being
 * switched away from, and by then its guard is the only evidence left.
 *
 * The stacks are not the PC port's 256 KB, because that number is an x86-64
 * host's. This is ARM at -O1 with 32-bit locals, the code that runs on these
 * stacks is the SDK's own thread bodies, and the console has a fixed
 * application region the slab already takes 8 MB of. 128 KB with a guard word
 * that is actually checked is a better trade than 256 KB with nothing watching
 * it, which is why os_context_stack_bytes() and os_context_live() exist.
 *
 * The boot context never gets one. OS_SaveContext on a context that was never
 * initialised just records where it is, and that context goes on running on
 * whatever stack it was already on. Only a context dispatched fresh needs a
 * stack, so only those allocate one.
 */
#include <nitro/os.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "3ds_fault.h"
#include "3ds_os_context.h"

/*
 * The PC port's number, kept, because the two files should run out at the same
 * point: a game that needs a 65th live context is a finding about the game and
 * not about either host.
 */
#define CTX_MAX 64
#define CTX_HOST_STACK (128 * 1024)

/* Not an address and not a plausible piece of guest data, so a stack that
 * overran into it reads differently from one that was never written. */
#define CTX_GUARD 0x3D5CA11Bu

/*
 * r4-r11, sp, lr, then d8-d15. The layout is the assembly's; 8-aligned so the
 * VFP half, which begins 40 bytes in, is 8-aligned too.
 */
#define CTX_WORDS 26

typedef struct Ctx {
    OSContext *key;
    u32 buf[CTX_WORDS] __attribute__((aligned(8)));
    void *hostStack;      /* NULL until this context is dispatched fresh */
    u32 entryPc;
    int fresh;            /* InitContext'd, not yet dispatched */
    int saved;            /* buf holds a save point */
} Ctx;

static Ctx sCtx[CTX_MAX];
static int sLive;

static Ctx *ctx_slot(OSContext *key)
{
    int i;
    int free_i = -1;

    for (i = 0; i < CTX_MAX; i++) {
        if (sCtx[i].key == key) {
            return &sCtx[i];
        }
        if (free_i < 0 && sCtx[i].key == NULL) {
            free_i = i;
        }
    }
    if (free_i < 0) {
        fault_stop("MORE OSCONTEXTS THAN THIS PORT HOLDS",
                   "3ds_os_context.c: the table is 64 deep");
    }
    sCtx[free_i].key = key;
    return &sCtx[free_i];
}

static void ctx_check_guards(void)
{
    int i;

    for (i = 0; i < CTX_MAX; i++) {
        if (sCtx[i].hostStack == NULL) {
            continue;
        }
        if (*(volatile u32 *)sCtx[i].hostStack != CTX_GUARD) {
            char line[64];

            snprintf(line, sizeof line, "OSCONTEXT %p, %lu BYTE STACK",
                     (void *)sCtx[i].key, os_context_stack_bytes());
            fault_stop("HOST STACK OVERFLOWED", line);
        }
    }
}

/*
 * Called from OS_SaveContext, in assembly, before it saves. Everything that
 * needs C happens here; what the assembly does afterwards is nine stores.
 */
u32 *ctx_begin_save(OSContext *context)
{
    Ctx *e = ctx_slot(context);

    ctx_check_guards();
    e->fresh = 0;
    e->saved = 1;
    return e->buf;
}

void OS_InitContext(OSContext *context, u32 newpc, u32 newsp)
{
    Ctx *e = ctx_slot(context);

    e->entryPc = newpc;
    e->fresh = 1;
    e->saved = 0;

    /* The struct's own fields are documentation here, the scheduler never
     * reads them back, but they are kept true. */
    context->pc_plus4 = newpc + 4;
    context->sp = newsp;

    /* pc_plus4 is a host function address. Marked so the digest skips it when
     * the OSThread this context belongs to lives in guest memory; a
     * host-static OSThread makes this a no-op, which is what every thread that
     * exists today is. */
    {
        extern void pc_state_mark_host_field(const void *field);

        pc_state_mark_host_field(&context->pc_plus4);
    }
}

/*
 * First dispatch: call the entry with the argument the creator wrote into
 * ctx->r[0], and when it returns go to whatever it wrote into ctx->lr,
 * exactly what the register state the mwcc assembly set up would have done.
 */
static void ctx_trampoline(unsigned long context_as_ulong)
{
    OSContext *context = (OSContext *)context_as_ulong;
    void (*entry)(u32) = (void (*)(u32))(unsigned long)ctx_slot(context)->entryPc;

    entry(context->r[0]);

    if (context->lr != 0) {
        ((void (*)(void))(unsigned long)context->lr)();
    }
    fault_stop("THREAD ENTRY RETURNED WITH NO LR",
               "3ds_os_context.c: nothing wrote OS_ExitThread into it");
}

void OS_LoadContext(OSContext *context)
{
    Ctx *e = ctx_slot(context);

    ctx_check_guards();

    if (e->fresh) {
        unsigned char *stack;

        e->fresh = 0;
        if (e->hostStack == NULL) {
            e->hostStack = malloc(CTX_HOST_STACK);
            if (e->hostStack == NULL) {
                fault_stop("NO HEAP FOR AN OSCONTEXT STACK",
                           "3ds_os_context.c: 128 KB per dispatched context");
            }
            *(u32 *)e->hostStack = CTX_GUARD;
            sLive++;
        }
        stack = (unsigned char *)e->hostStack;
        /* The top of the block, which malloc already made 8-aligned as AAPCS
         * wants at a public interface. It grows down from here towards the
         * guard word. */
        ctx_start(stack + CTX_HOST_STACK, (unsigned long)context,
                  ctx_trampoline);
        /* ctx_start does not return. */
    }

    if (!e->saved) {
        fault_stop("OSCONTEXT LOADED WITH NO SAVE POINT",
                   "3ds_os_context.c: never Init'd and never Saved");
    }
    ctx_resume(e->buf);
    /* ctx_resume does not return either: sp belongs to the other context. */
}

unsigned long os_context_stack_bytes(void)
{
    return CTX_HOST_STACK;
}

int os_context_live(void)
{
    return sLive;
}

/* ------------------------------------------------------------------ */
/* The self-test                                                       */
/* ------------------------------------------------------------------ */
/*
 * It really switches stacks and it is really assembly, so there is no host
 * twin: a build machine would be running x86 against ARM. What it checks is
 * the contract above in the order the scheduler exercises it, a fresh
 * dispatch with an argument, a switch back, a switch in again, and the lr path
 * when the entry returns. The evidence is a sequence of marks, because "it
 * came back" is not the claim; "it came back HERE, after THAT" is. The first
 * version of the switch passed nothing at all here and froze the console,
 * which is the whole reason this test exists in the binary rather than in a
 * comment.
 *
 * Everything it remembers is static. A local written after an OS_SaveContext
 * in the same function may not survive the resumed path, and that is exactly
 * this function's shape.
 */
#define SEQ_MAX 8

static OSContext sMainCtx;
static OSContext sThreadCtx;
static int sSeq[SEQ_MAX];
static int sSeqN;
static u32 sArgSeen;
static int sFirstFail = -1;
static int *sRanOut;
static int sRan;
static int sBad;

static void note(int mark)
{
    if (sSeqN < SEQ_MAX) {
        sSeq[sSeqN++] = mark;
    }
}

static void check(int ok, int which)
{
    sRan++;
    if (!ok) {
        sBad++;
        if (sFirstFail < 0) {
            sFirstFail = which;
        }
    }
}

/* Reached only through ctx->lr, when the entry below returns. */
static void thread_exit(void)
{
    note(5);
    OS_LoadContext(&sMainCtx);
    note(99);   /* OS_LoadContext returned, which it may not do */
}

static void thread_entry(u32 arg)
{
    sArgSeen = arg;
    note(2);

    /* Hand the CPU back, the way a yielding thread does. */
    if (!OS_SaveContext(&sThreadCtx)) {
        OS_LoadContext(&sMainCtx);
    }

    /* Resumed. Returning from here goes to ctx->lr. */
    note(4);
}

int os_context_selftest(int *ran)
{
    static const int kWant[6] = { 1, 2, 3, 4, 5, 6 };
    int i;

    sRanOut = ran;
    sSeqN = 0;
    sRan = 0;
    sBad = 0;
    sFirstFail = -1;
    sArgSeen = 0;
    memset(&sMainCtx, 0, sizeof sMainCtx);
    memset(&sThreadCtx, 0, sizeof sThreadCtx);

    note(1);

    /* What the scheduler does: init, then write the argument and the
     * return-to function into the struct itself. */
    OS_InitContext(&sThreadCtx, (u32)(unsigned long)thread_entry, 0x02300000u);
    sThreadCtx.r[0] = 0xC0FFEE01u;
    sThreadCtx.lr = (u32)(unsigned long)thread_exit;

    /* Out to the fresh context, which runs on its own stack. */
    if (!OS_SaveContext(&sMainCtx)) {
        OS_LoadContext(&sThreadCtx);
    }
    note(3);

    /* And back in, to the thread's own save point. */
    if (!OS_SaveContext(&sMainCtx)) {
        OS_LoadContext(&sThreadCtx);
    }
    note(6);

    /* Nothing below this line is resumed into, so locals are locals again. */
    check(sSeqN == 6, 1);
    for (i = 0; i < 6 && i < sSeqN; i++) {
        check(sSeq[i] == kWant[i], 2 + i);
    }
    check(sArgSeen == 0xC0FFEE01u, 8);
    check(sThreadCtx.pc_plus4 == (u32)(unsigned long)thread_entry + 4, 9);
    check(os_context_live() >= 1, 10);
    check(ctx_slot(&sThreadCtx)->hostStack != NULL, 11);
    check(*(u32 *)ctx_slot(&sThreadCtx)->hostStack == CTX_GUARD, 12);
    /* The boot side never needed a stack of its own. */
    check(ctx_slot(&sMainCtx)->hostStack == NULL, 13);

    if (sRanOut != NULL) {
        *sRanOut = sRan;
    }
    return sBad;
}

int os_context_first_failure(void)
{
    return sFirstFail;
}
