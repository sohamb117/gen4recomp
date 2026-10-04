/*
 * Diamond/Pearl's half of the host layer's boot and of the CPU the
 * recompiled code sees.
 *
 * The machine model is Platinum's (games/platinum/pc/src, pc/hw,
 * tools/armrec/armrec_rt.c, pc/wasm/src), compiled for D by pc/mk/host.mk.
 * What Platinum never needed is recompiled ARM code: most of D is assembly
 * that armrec turned into C, which keeps r13 in one global (armrec_sp),
 * expects its assembly data at guest addresses, and reaches the CPU through
 * armrec_swi / armrec_mrc / armrec_mcr / armrec_mrs / armrec_msr. This file
 * is those pieces.
 *
 * pc_dp_boot() runs from pc_main.c right after the guest address space is
 * mapped (armrec_mem_init) and before the ROM header is written or any guest
 * code runs.
 */
#include <nitro.h>
#include <stdio.h>

#include <pc_wasm.h>

#include "armrec_rt.h"

/* Generated: by the bridge (bridge.mk, bridge_externs.c) and by armrec
 * (armrec.mk, armrec_init.c). */
extern void armrec_bind_externs(void);
extern void armrec_init_all(void);

extern void pc_dp_pm_init(void);
extern void pc_dp_nvram_init(void);

/*
 * The launcher thread's stack, where crt0 leaves the system-mode sp before
 * it branches to NitroMain (arm9/lib/NitroSDK/src/crt0.c, `_start`):
 *
 *     sp = SDK_AUTOLOAD_DTCM_START + 0x3fc0 - HW_SVC_STACK_SIZE
 *          - SDK_IRQ_STACKSIZE - 4, then 8-byte aligned
 *
 * which OS_InitThread (OS_thread.c) records as OSi_LauncherThread's
 * stackBottom. The two link values come from the ROM link's xMAP through
 * pc/mk/host.mk; on this port the launcher context's armrec_sp starts here
 * and is saved and restored with every other context's
 * (games/platinum/pc/wasm/src/pc_os_context_wasm.c).
 */
#if !defined(PC_DP_SDK_AUTOLOAD_DTCM_START) || !defined(PC_DP_SDK_IRQ_STACKSIZE)
#error "pc/mk/host.mk passes the DTCM base and IRQ stack size from the xMAP"
#endif
#define PC_DP_HW_SVC_STACK_SIZE 0x40u

static uint32_t launcher_sp(void)
{
    uint32_t sp = (uint32_t)PC_DP_SDK_AUTOLOAD_DTCM_START + 0x3fc0u
                  - PC_DP_HW_SVC_STACK_SIZE - (uint32_t)PC_DP_SDK_IRQ_STACKSIZE
                  - 4u;

    if (sp & 4u) {
        sp -= 4u;
    }
    return sp;
}

void pc_dp_boot(void)
{
    /* The order armrec.mk and bridge.mk give: C addresses the recompiled
     * code holds first, then every recompiled function, overlay slot and
     * assembly data blob of the static module. */
    armrec_bind_externs();
    armrec_init_all();
    armrec_register_decompiled();
    armrec_sp = launcher_sp();

    /* The ARM7 services D's assembly reaches over PXI that the shared layer
     * does not answer (pc/src/pc_dp_pm.c, pc/src/pc_dp_nvram.c). */
    pc_dp_pm_init();
    pc_dp_nvram_init();
}

/* ------------------------------------------------------------------ */
/* CPSR                                                               */
/* ------------------------------------------------------------------ */

/*
 * One CPSR, not two. armrec_rt.c's weak armrec_mrs/armrec_msr keep a word of
 * their own, while the interrupt state everything else reads is
 * pc_os_lite.c's (OS_DisableInterrupts and friends, which replace D's mwcc
 * asm bodies). Recompiled code reading the I bit after C disabled interrupts
 * must see it set, so these go through pc_os_lite.c's functions.
 *
 * Only RUNTIME_f_cmp.s uses mrs/msr in D's assembly today, on the flags
 * field; the flags are kept here as bookkeeping, as armrec_rt.c's default
 * does. A mode change from recompiled code would be a guest exception or
 * IRQ entry, which this port does not model (the SDK's asm that does it is
 * replaced), so it stops the run rather than being half-applied.
 */
#define PC_DP_PSR_FLAGS   0xF8000000u
#define PC_DP_PSR_IF      0x000000C0u
#define PC_DP_PSR_MODE    0x0000001Fu

static uint32_t sPsrFlags;
static uint32_t sSpsr;

uint32_t armrec_mrs(int spsr)
{
    uint32_t irq_fiq;

    if (spsr) {
        return sSpsr;
    }
    irq_fiq = (uint32_t)OS_DisableInterrupts_IrqAndFiq();
    (void)OS_RestoreInterrupts_IrqAndFiq((OSIntrMode)irq_fiq);
    return (sPsrFlags & PC_DP_PSR_FLAGS) | irq_fiq | (uint32_t)OS_GetProcMode();
}

void armrec_msr(int spsr, uint32_t mask, uint32_t v)
{
    if (spsr) {
        sSpsr = (sSpsr & ~mask) | (v & mask);
        return;
    }
    if (mask & PC_DP_PSR_FLAGS) {
        sPsrFlags = (sPsrFlags & ~(mask & PC_DP_PSR_FLAGS)) | (v & mask & PC_DP_PSR_FLAGS);
    }
    if (mask & 0xFFu) {
        if ((v & PC_DP_PSR_MODE) != (uint32_t)OS_GetProcMode()) {
            pc_wasm_fatalf("armrec_msr: recompiled code switched the CPU mode "
                           "to %#x (from %#x); exception and IRQ entry are "
                           "not modeled", (unsigned)(v & PC_DP_PSR_MODE),
                           (unsigned)OS_GetProcMode());
        }
        (void)OS_RestoreInterrupts_IrqAndFiq((OSIntrMode)(v & PC_DP_PSR_IF));
    }
}

/* ------------------------------------------------------------------ */
/* CP15                                                               */
/* ------------------------------------------------------------------ */

/*
 * The ARM946E-S system control coprocessor. Every NitroSDK function that
 * touches it is mwcc asm the host replaces (OS_cache.c, OS_tcm.c,
 * OS_protectionRegion.c, OS_terminate_proc.c) or never runs (crt0's
 * init_cp15), so recompiled code reaching CP15 is the protection unit's
 * enable/disable (OS_protectionUnit.c) and nothing else today. The model is
 * the register file: a write is kept, a read returns what was written or
 * the value crt0 would have programmed. Cache and write-buffer operations
 * (c7) have nothing to act on; c7,c0,4 is wait-for-interrupt, which is
 * OS_Halt's job.
 */
#define PC_DP_CP15_REGS 32

static struct {
    uint32_t key;
    uint32_t value;
} sCp15[PC_DP_CP15_REGS];
static int sCp15N;

static uint32_t cp15_key(uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2)
{
    return (op1 << 12) | (crn << 8) | (crm << 4) | op2;
}

static uint32_t cp15_reset_value(uint32_t key)
{
    switch (key) {
    case 0x000:                              /* c0,c0,0: main ID */
        return 0x41059461u;                  /* ARM946E-S */
    case 0x910:                              /* c9,c1,0: DTCM region */
        return (uint32_t)PC_DP_SDK_AUTOLOAD_DTCM_START | 0x0Au;   /* 16 KB */
    case 0x911:                              /* c9,c1,1: ITCM region */
        return 0x0000000Cu;                  /* base 0, 32 KB */
    default:
        return 0;
    }
}

uint32_t armrec_mrc(uint32_t cp, uint32_t op1, uint32_t crn, uint32_t crm,
                    uint32_t op2)
{
    uint32_t key;
    int i;

    if (cp != 15) {
        pc_wasm_fatalf("armrec_mrc: coprocessor p%u does not exist on the "
                       "ARM9", (unsigned)cp);
    }
    key = cp15_key(op1, crn, crm, op2);
    for (i = 0; i < sCp15N; i++) {
        if (sCp15[i].key == key) {
            return sCp15[i].value;
        }
    }
    return cp15_reset_value(key);
}

void armrec_mcr(uint32_t cp, uint32_t op1, uint32_t crn, uint32_t crm,
                uint32_t op2, uint32_t v)
{
    uint32_t key;
    int i;

    if (cp != 15) {
        pc_wasm_fatalf("armrec_mcr: coprocessor p%u does not exist on the "
                       "ARM9", (unsigned)cp);
    }
    if (crn == 7) {
        if (crm == 0 && op2 == 4) {
            OS_Halt();
        }
        return;                              /* cache maintenance */
    }
    key = cp15_key(op1, crn, crm, op2);
    for (i = 0; i < sCp15N; i++) {
        if (sCp15[i].key == key) {
            sCp15[i].value = v;
            return;
        }
    }
    if (sCp15N == PC_DP_CP15_REGS) {
        pc_wasm_fatalf("armrec_mcr: more than %d distinct CP15 registers "
                       "written", PC_DP_CP15_REGS);
    }
    sCp15[sCp15N].key = key;
    sCp15[sCp15N].value = v;
    sCp15N++;
}

/* ------------------------------------------------------------------ */
/* SWI                                                                */
/* ------------------------------------------------------------------ */

/*
 * BIOS calls. The SVC_* thunks (arm9/lib/syscall) are not recompiled
 * (pc/mk/armrec.mk): pc_os_lite.c defines the ones D's link references in
 * C, so a recompiled `bl SVC_x` is an ordinary boundary call and never gets
 * here. What remains is inline `swi` in assembly, and in D's arm9 that is
 * RUNTIME_ARM_semihosted_console_io.s's `swi 0x123456`, ARM semihosting:
 * a debugger hook that a retail console (no debugger attached) does not
 * answer. Its callers are the MSL console writers; answering "nothing
 * written, no error" (0) is what they see on hardware with nothing listening.
 * Anything else is a BIOS call this port has not been shown to need.
 */
#define PC_DP_SWI_SEMIHOST 0x123456u

uint32_t armrec_swi(uint32_t num, uint32_t r0, uint32_t r1, uint32_t r2,
                    uint32_t r3)
{
    (void)r1; (void)r2; (void)r3;
    switch (num) {
    case PC_DP_SWI_SEMIHOST:
        (void)r0;
        return 0;
    case 0x03:                               /* WaitByLoop */
        SVC_WaitByLoop((s32)r0);
        return r0;
    case 0x06:                               /* Halt */
        OS_Halt();
        return r0;
    default:
        pc_wasm_fatalf("armrec_swi: BIOS call %#x from recompiled code is "
                       "not modeled", (unsigned)num);
    }
}
