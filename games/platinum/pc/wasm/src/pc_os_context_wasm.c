/*
 * OSContext switching on np_host fibers: the wasm32 replacement for
 * pc/src/pc_os_context.c, which Makefile.wasm does not compile (ucontext,
 * mmap and stack-switching asm have no wasm meaning).
 *
 * The same three primitives, the same slot table keyed by OSContext *, but a
 * fiber model instead of a setjmp one. Each context's execution home is a
 * runtime fiber (core/include/np_guest_abi.h); a parked context is a fiber
 * suspended inside np_host_fiber_switch, and resuming it means that call
 * returns.
 *
 *   OS_SaveContext(ctx)  records that ctx's fiber (the caller) is parked
 *                        and resumable, returns FALSE. It never returns TRUE.
 *   OS_LoadContext(ctx)  switches to ctx's fiber, creating it first if ctx is
 *                        fresh. When some later OS_LoadContext switches back
 *                        to the caller's fiber, this RETURNS.
 *   OS_InitContext(ctx)  makes ctx fresh, taking any fiber it had away.
 *
 * Returning from OS_LoadContext is equivalent to OS_SaveContext returning
 * TRUE because the scheduler has nothing after the load
 * (os_thread.c OSi_RescheduleThread: `if (OS_SaveContext(cur)) return;
 * ...; OS_LoadContext(next); }`), so both paths leave OSi_RescheduleThread.
 *
 * A fiber that calls OS_LoadContext without a pending OS_SaveContext of its
 * own context can never run again, because nothing records where it is:
 * a TERMINATED thread rescheduling away (OSi_ExitThread_Destroy), or a
 * running context re-initialised under itself (OSi_ExitThread_ArgSpecified).
 * Such a fiber is handed to whoever runs next, which destroys it and
 * reclaims its shadow stack before doing anything else; a fiber never
 * destroys itself. The boot fiber (handle 1, the one that ran _start) is
 * never destroyed: an abandoned boot fiber is simply left parked.
 *
 * Shadow stacks (the wasm C stack: address-taken locals and aggregates;
 * scalars live on the runtime's native fiber stack) come from a static pool,
 * not malloc: wasi-libc mallocs argv and environ from the same heap, so
 * heap-placed stacks would move with the host's env/argv bytes, and addresses
 * of locals do end up in guest structures. The pool sits in .bss above
 * NP_GUEST_C_BASE, lowest free slot first, so placement depends only on the
 * guest's own thread history. 256 KiB each is the host builds' per-context
 * stack (pc_os_context.c PC_CTX_HOST_STACK), which holds ALL of a thread's
 * locals; a shadow stack holds a subset. There are no guard pages in wasm,
 * so each stack carries a guard word at its lowest address and every save
 * and load checks all of them (the 3DS port's scheme, 3ds_os_context.c).
 *
 * PC_CTX_TRACE=1 logs every init, save, load, create and destroy.
 *
 * Diamond/Pearl (PC_GAME_DP) also run recompiled ARM code, whose r13 is one
 * global, armrec_sp (tools/armrec/armrec_rt.h), pointing into the running
 * thread's own guest stack. It is per thread exactly as r13 is, and it lives
 * where the hardware's OS_SaveContext puts r13, OSContext.sp: saved there by
 * OS_SaveContext, put back by every fiber the moment it regains control
 * (OS_LoadContext returning, or np_fiber_entry for a fresh context).
 * OS_InitContext then has to compute .sp as D's asm body does
 * (arm9/lib/NitroSDK/src/OS_context.c): the thread's system-mode stack sits
 * HW_SVC_STACK_SIZE below the top it was given, 8-byte aligned, and the top
 * itself is the SVC stack (sp_svc). Platinum runs no recompiled code and
 * none of this is compiled for it.
 */
#include <nitro/os.h>

#include <pc_wasm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PC_GAME_DP)
#include "armrec_rt.h"

/* arm9/lib/NitroSDK/include/mmap.h: HW_SVC_STACK_SIZE, the SVC-mode stack
 * OS_InitContext carves off the top of every thread's stack. */
#define PC_DP_SVC_STACK_SIZE 0x40u
#define PC_CTX_SP_SAVE(ctx) ((ctx)->sp = armrec_sp)
#define PC_CTX_SP_LOAD(ctx) (armrec_sp = (ctx)->sp)
#else
#define PC_CTX_SP_SAVE(ctx) ((void)0)
#define PC_CTX_SP_LOAD(ctx) ((void)0)
#endif

/* pc_os_context.c's limit: a 65th live context is a finding about the game. */
#define PC_CTX_MAX 64
#define PC_CTX_SHADOW_STACK (256 * 1024)
/* Live fibers at once, boot fiber excluded. The game, SDK, NitroSystem,
 * NitroWiFi and DWC create about a dozen threads between them. */
#define PC_CTX_SHADOW_POOL 32
#define PC_CTX_GUARD 0x3D5CA11Bu
#define PC_FIBER_BOOT 1u

typedef struct PcCtx {
    OSContext *key;
    uint32_t fiber;  /* execution home, 0 = none yet */
    int shadow;      /* shadow pool index + 1, 0 = none (boot fiber) */
    u32 entryPc;
    int fresh;       /* InitContext'd, not yet dispatched */
    int saved;       /* fiber parked by OS_SaveContext, not yet resumed */
} PcCtx;

typedef struct PcFiber {
    uint32_t fiber;
    int shadow;
} PcFiber;

static PcCtx sCtxTable[PC_CTX_MAX];

static unsigned char sShadowPool[PC_CTX_SHADOW_POOL][PC_CTX_SHADOW_STACK]
    __attribute__((aligned(16)));
static unsigned char sShadowUsed[PC_CTX_SHADOW_POOL];

/* The running fiber whose context OS_InitContext re-initialised under it;
 * the OS_LoadContext that follows abandons it. */
static PcFiber sOrphan;
/* An abandoned fiber, destroyed by the next fiber to run. */
static PcFiber sZombie;

static int ctx_trace = -1;

static int ctx_tracing(void)
{
    if (ctx_trace < 0) {
        const char *e = getenv("PC_CTX_TRACE");

        ctx_trace = (e != NULL && *e != '\0' && *e != '0') ? 1 : 0;
    }
    return ctx_trace;
}

static void ctx_say(const char *what, const PcCtx *e, uint32_t fiber)
{
    if (!ctx_tracing()) {
        return;
    }
    fprintf(stderr, "pc_ctx: %-7s slot %2d ctx %p fiber %u shadow %d\n", what,
            e ? (int)(e - sCtxTable) : -1, e ? (void *)e->key : NULL,
            (unsigned)fiber, e ? e->shadow - 1 : -1);
}

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
        pc_wasm_fatalf("pc_os_context: more than %d live OSContexts", PC_CTX_MAX);
    }
    sCtxTable[free_i].key = key;
    return &sCtxTable[free_i];
}

static PcCtx *ctx_by_fiber(uint32_t fiber)
{
    int i;

    for (i = 0; i < PC_CTX_MAX; i++) {
        if (sCtxTable[i].key != NULL && sCtxTable[i].fiber == fiber) {
            return &sCtxTable[i];
        }
    }
    return NULL;
}

static int shadow_alloc(void)
{
    int i;

    for (i = 0; i < PC_CTX_SHADOW_POOL; i++) {
        if (!sShadowUsed[i]) {
            sShadowUsed[i] = 1;
            *(volatile u32 *)sShadowPool[i] = PC_CTX_GUARD;
            return i + 1;
        }
    }
    pc_wasm_fatalf("pc_os_context: more than %d live thread fibers "
                   "(PC_CTX_SHADOW_POOL)", PC_CTX_SHADOW_POOL);
}

static void shadow_free(int shadow)
{
    if (shadow != 0) {
        sShadowUsed[shadow - 1] = 0;
    }
}

static void ctx_check_guards(void)
{
    int i;

    for (i = 0; i < PC_CTX_SHADOW_POOL; i++) {
        if (sShadowUsed[i] && *(volatile u32 *)sShadowPool[i] != PC_CTX_GUARD) {
            pc_wasm_fatalf("pc_os_context: shadow stack %d (%p, %u bytes) "
                           "overflowed", i, (void *)sShadowPool[i],
                           (unsigned)PC_CTX_SHADOW_STACK);
        }
    }
}

/* Discard a fiber that is not running. */
static void fiber_discard(PcFiber f)
{
    if (f.fiber == PC_FIBER_BOOT) {
        ctx_say("park", NULL, f.fiber);   /* left suspended for good */
        return;
    }
    ctx_say("destroy", NULL, f.fiber);
    np_host_fiber_destroy(f.fiber);
    shadow_free(f.shadow);
}

/* First thing every fiber does when it (re)gains control. */
static void fiber_reap(void)
{
    if (sZombie.fiber != 0) {
        PcFiber z = sZombie;

        sZombie.fiber = 0;
        sZombie.shadow = 0;
        fiber_discard(z);
    }
}

void OS_InitContext(OSContext *context, u32 newpc, u32 newsp)
{
    PcCtx *e = ctx_slot(context);

    if (e->fiber != 0) {
        uint32_t self = np_host_fiber_self();

        if (e->fiber == self) {
            /* The running context, re-initialised under itself
             * (OSi_ExitThread_ArgSpecified); the OS_LoadContext that
             * follows starts a fresh fiber and abandons this one. */
            if (sOrphan.fiber != 0) {
                pc_wasm_fatalf("pc_os_context: context %p re-initialised on "
                               "fiber %u while fiber %u is still orphaned",
                               (void *)context, (unsigned)self,
                               (unsigned)sOrphan.fiber);
            }
            sOrphan.fiber = self;
            sOrphan.shadow = e->shadow;
            ctx_say("orphan", e, self);
        } else {
            /* Another thread's parked context (re-created after
             * OS_DestroyThread, or OS_KillThread of another thread): its
             * fiber can never be resumed now. */
            PcFiber old;

            if (!e->saved) {
                pc_wasm_fatalf("pc_os_context: context %p re-initialised "
                               "while its fiber %u is neither running nor "
                               "parked", (void *)context, (unsigned)e->fiber);
            }
            old.fiber = e->fiber;
            old.shadow = e->shadow;
            fiber_discard(old);
        }
        e->fiber = 0;
        e->shadow = 0;
    }
    e->entryPc = newpc;
    e->fresh = 1;
    e->saved = 0;
    /* The struct's own fields are documentation here (the scheduler never
     * reads pc/sp back), but keep them true. */
    context->pc_plus4 = newpc + 4;
#if defined(PC_GAME_DP)
    /* D's asm body, field for field: on D the fields are not documentation,
     * .sp is the recompiled code's r13 when this context first runs. */
    {
        u32 sp = newsp - PC_DP_SVC_STACK_SIZE;
        int i;

        if (sp & 4u) {
            sp -= 4u;
        }
        context->sp = sp;
        context->sp_svc = newsp;
        context->cpsr = (newpc & 1u) ? 0x3Fu : 0x1Fu;   /* SYS, Thumb bit */
        for (i = 0; i < 13; i++) {
            context->r[i] = 0;
        }
        context->lr = 0;
    }
#else
    context->sp = newsp;
#endif
    /* pc_plus4 is the thread entry, a host function. Marked so the digest
     * skips it when the OSThread itself lives in guest memory. */
    {
        extern void pc_state_mark_host_field(const void *field);
        pc_state_mark_host_field(&context->pc_plus4);
    }
    ctx_say("init", e, 0);
}

BOOL OS_SaveContext(OSContext *context)
{
    PcCtx *e = ctx_slot(context);
    uint32_t self = np_host_fiber_self();

    ctx_check_guards();
    if (e->fresh) {
        pc_wasm_fatalf("pc_os_context: saving context %p, which was "
                       "initialised but never dispatched", (void *)context);
    }
    if (e->fiber == 0) {
        /* The boot context (the launcher thread) is the one context that
         * runs without being dispatched: it adopts the fiber that ran
         * _start. Every other fiber is created owned. */
        PcCtx *owner = ctx_by_fiber(self);

        if (self != PC_FIBER_BOOT || owner != NULL || sOrphan.fiber == self) {
            pc_wasm_fatalf("pc_os_context: saving context %p from fiber %u, "
                           "which already belongs to %p", (void *)context,
                           (unsigned)self,
                           owner ? (void *)owner->key : NULL);
        }
        e->fiber = self;
        e->shadow = 0;
    } else if (e->fiber != self) {
        pc_wasm_fatalf("pc_os_context: saving context %p from fiber %u; its "
                       "fiber is %u", (void *)context, (unsigned)self,
                       (unsigned)e->fiber);
    }
    e->saved = 1;
    PC_CTX_SP_SAVE(context);
    ctx_say("save", e, self);
    return FALSE;
}

void OS_LoadContext(OSContext *context)
{
    PcCtx *t = ctx_slot(context);
    uint32_t self = np_host_fiber_self();
    PcCtx *me = ctx_by_fiber(self);
    int keep = me != NULL && me->saved;

    ctx_check_guards();

    if (t->fresh) {
        int shadow = shadow_alloc();
        uint32_t top = (uint32_t)(uintptr_t)(sShadowPool[shadow - 1] + PC_CTX_SHADOW_STACK);
        uint32_t f = np_host_fiber_create(top, (uint32_t)(uintptr_t)context);

        if (f == 0) {
            pc_wasm_fatalf("pc_os_context: np_host_fiber_create failed for "
                           "context %p", (void *)context);
        }
        t->fiber = f;
        t->shadow = shadow;
        t->fresh = 0;
        ctx_say("create", t, f);
    } else if (t->fiber == 0) {
        pc_wasm_fatalf("pc_os_context: loading context %p, which was never "
                       "initialised or saved", (void *)context);
    } else if (t->fiber == self) {
        pc_wasm_fatalf("pc_os_context: loading context %p, which is running "
                       "on this fiber (%u)", (void *)context, (unsigned)self);
    } else if (!t->saved) {
        pc_wasm_fatalf("pc_os_context: loading context %p, whose fiber %u is "
                       "not parked", (void *)context, (unsigned)t->fiber);
    }
    t->saved = 0;

    if (!keep) {
        /* No pending save: nothing can ever switch back to this fiber. */
        PcFiber dead;

        if (me != NULL) {
            dead.fiber = me->fiber;
            dead.shadow = me->shadow;
            ctx_say("abandon", me, self);
            memset(me, 0, sizeof *me);
        } else if (sOrphan.fiber == self) {
            dead = sOrphan;
            sOrphan.fiber = 0;
            sOrphan.shadow = 0;
            ctx_say("abandon", NULL, self);
        } else if (self == PC_FIBER_BOOT) {
            /* Boot fiber that never saved a context. */
            dead.fiber = self;
            dead.shadow = 0;
        } else {
            pc_wasm_fatalf("pc_os_context: loading context %p from unknown "
                           "fiber %u", (void *)context, (unsigned)self);
        }
        if (dead.fiber == PC_FIBER_BOOT) {
            ctx_say("park", NULL, self);
        } else {
            if (sZombie.fiber != 0) {
                pc_wasm_fatalf("pc_os_context: fiber %u abandoned while fiber "
                               "%u is still unreaped", (unsigned)self,
                               (unsigned)sZombie.fiber);
            }
            sZombie = dead;
        }
    }

    ctx_say("load", t, t->fiber);
    np_host_fiber_switch(t->fiber);

    /* Resumed: some OS_LoadContext named this fiber's context. */
    if (!keep) {
        pc_wasm_fatalf("pc_os_context: abandoned fiber %u was resumed",
                       (unsigned)self);
    }
    fiber_reap();
    PC_CTX_SP_LOAD(me->key);
}

/* First dispatch of a fresh context, on its own fiber: call the entry with
 * the argument the creator wrote into ctx->r[0]; a return goes to whatever
 * it wrote in ctx->lr (OS_ExitThread), exactly as the asm's register state
 * would. Neither may return here. */
NP_EXPORT(np_fiber_entry) void np_fiber_entry(uint32_t arg)
{
    OSContext *context = (OSContext *)(uintptr_t)arg;
    PcCtx *e;
    void (*entry)(u32);

    fiber_reap();
    e = ctx_slot(context);
    if (e->fiber != np_host_fiber_self()) {
        pc_wasm_fatalf("pc_os_context: fiber %u started for context %p, "
                       "which belongs to fiber %u", (unsigned)np_host_fiber_self(),
                       (void *)context, (unsigned)e->fiber);
    }
    entry = (void (*)(u32))(uintptr_t)e->entryPc;
    PC_CTX_SP_LOAD(context);
    entry(context->r[0]);
    if (context->lr != 0) {
        ((void (*)(void))(uintptr_t)context->lr)();
    }
    pc_wasm_fatalf("pc_os_context: context %p entry returned with lr=%#x",
                   (void *)context, (unsigned)context->lr);
}
