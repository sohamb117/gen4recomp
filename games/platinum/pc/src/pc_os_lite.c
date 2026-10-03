/* Host implementations of the NitroSDK OS/SVC surface that ships as mwcc
 * `asm` functions (which strip_asm.py removes so the surrounding C compiles)
 * or as BIOS routines that were never in this tree at all.
 *
 * Three kinds of symbol live here:
 *
 *   1. Real implementations, transliterated from the stripped asm bodies in
 *      subprojects/NitroSDK-4.2.30001/libraries/os/src/*.c or from the
 *      documented DS BIOS behavior, each checked against an oracle named in
 *      its comment.
 *   2. mwcc/MSL runtime glue whose semantics the MSL headers pin down.
 *   3. Loud traps: anything whose host meaning does not exist yet (thread
 *      contexts, the IRQ dispatcher, reset) or whose semantics could not be
 *      verified. A trap prints its name and reason to stderr and aborts, so a
 *      run says exactly what to build next instead of misbehaving quietly.
 *
 * Everything is single-threaded guest state: one virtual CPU, so CPSR, the
 * lock-id bitmap and friends are plain process-local variables. */

#include <nitro.h>

#include <stdio.h>
#include <stdlib.h>

static void pc_trap(const char *name, const char *why) __attribute__((noreturn));
static void pc_trap(const char *name, const char *why)
{
    fprintf(stderr, "pc_os_lite: %s: %s\n", name, why);
    abort();
}

/* ------------------------------------------------------------------ */
/* CPSR: interrupt mask and processor mode                            */
/* ------------------------------------------------------------------ */

/* The guest CPSR, process-local because there is exactly one virtual CPU.
 * crt0.c ends its mode setup with `mov r0, #HW_PSR_SYS_MODE; msr cpsr_csfx,
 * r0` (system mode, I and F bits clear) so that is the state NitroMain
 * starts in and the initializer here.
 *
 * Every function below is a transliteration of the corresponding asm body in
 * libraries/os/src/os_system.c. Two properties of those bodies matter enough
 * to keep exactly:
 *
 *   - each returns the PREVIOUS state of the bit(s) it touches, because the
 *     SDK's critical sections are all `prev = OS_DisableInterrupts(); ...;
 *     OS_RestoreInterrupts(prev)` and they nest; a value that does not
 *     round-trip reopens a critical section early.
 *   - OS_RestoreInterrupts ORs the WHOLE argument into the control byte
 *     (`orr r2, r2, r0` before `msr cpsr_c`), not just bit 7. Kept as-is
 *     rather than tidied.
 *
 * `msr cpsr_c` writes only the control byte (mode + I + F), which is what
 * pc_msr_control() reproduces. */

#define PC_PSR_CONTROL_FIELD 0x000000FFu

static u32 pc_cpsr = HW_PSR_SYS_MODE;

static void pc_msr_control(u32 value)
{
    pc_cpsr = (pc_cpsr & ~PC_PSR_CONTROL_FIELD) | (value & PC_PSR_CONTROL_FIELD);
}

OSIntrMode OS_EnableInterrupts(void)
{
    u32 psr = pc_cpsr;
    pc_msr_control(psr & ~(u32)HW_PSR_IRQ_DISABLE);
    return (OSIntrMode)(psr & HW_PSR_IRQ_DISABLE);
}

OSIntrMode OS_DisableInterrupts(void)
{
    u32 psr = pc_cpsr;
    pc_msr_control(psr | HW_PSR_IRQ_DISABLE);
    return (OSIntrMode)(psr & HW_PSR_IRQ_DISABLE);
}

OSIntrMode OS_RestoreInterrupts(OSIntrMode state)
{
    u32 psr = pc_cpsr;
    pc_msr_control((psr & ~(u32)HW_PSR_IRQ_DISABLE) | (u32)state);
    return (OSIntrMode)(psr & HW_PSR_IRQ_DISABLE);
}

OSIntrMode OS_DisableInterrupts_IrqAndFiq(void)
{
    u32 psr = pc_cpsr;
    pc_msr_control(psr | HW_PSR_IRQ_FIQ_DISABLE);
    return (OSIntrMode)(psr & HW_PSR_IRQ_FIQ_DISABLE);
}

OSIntrMode OS_RestoreInterrupts_IrqAndFiq(OSIntrMode state)
{
    u32 psr = pc_cpsr;
    pc_msr_control((psr & ~(u32)HW_PSR_IRQ_FIQ_DISABLE) | (u32)state);
    return (OSIntrMode)(psr & HW_PSR_IRQ_FIQ_DISABLE);
}

OSIntrMode_Irq OS_GetCpsrIrq(void)
{
    return (OSIntrMode_Irq)(pc_cpsr & HW_PSR_IRQ_DISABLE);
}

/* Reads the mode field of the same word rather than returning a constant:
 * code asserts on it (e.g. "not in IRQ mode"), and whatever later delivers
 * interrupts will want to flip the field to OS_PROCMODE_IRQ around handlers
 * so those assertions keep their meaning. */
OSProcMode OS_GetProcMode(void)
{
    return (OSProcMode)(pc_cpsr & HW_PSR_CPU_MODE_MASK);
}

/* ------------------------------------------------------------------ */
/* Lock ids                                                           */
/* ------------------------------------------------------------------ */

/* On hardware the free-list bitmap is two words at HW_LOCK_ID_FLAG_MAIN in
 * shared main RAM, initialized by OS_InitLock (os_spinLock.c) to 0xffffffff /
 * 0xffff0000; a set bit means free, giving ids 0x40..0x5f in word 0 and
 * 0x60..0x6f in word 1 (the low half of word 1 stays reserved; 0x70 and up
 * are the debugger and system ids). It lives in shared memory only so the
 * ARM7 can see it; with a single-processor guest and no memory map wired up
 * yet, a process-local pair of words with the same initial value has
 * identical semantics for every allocation this build can make.
 *
 * The allocator asm (os_spinLock.c, `asm s32 OS_GetLockID`) picks the HIGHEST
 * free id in word 0 first because it searches with CLZ; transliterated
 * exactly, including the OS_LOCK_ID_ERROR (-3) return when both words are
 * empty. */

static u32 pc_lockIdFlag[2] = { 0xffffffffu, 0xffff0000u };

static u32 pc_clz32(u32 x)
{
    return x ? (u32)__builtin_clz(x) : 32u;
}

/* ARM register-amount shifts read the low byte of the amount register, and
 * amounts of 32..255 yield zero. OS_ReleaseLockID leans on that rule. */
static u32 pc_lsr(u32 value, u32 amount)
{
    amount &= 0xFFu;
    return (amount >= 32u) ? 0u : (value >> amount);
}

s32 OS_GetLockID(void)
{
    u32 *flags = &pc_lockIdFlag[0];
    u32 base = OS_MAINP_LOCK_ID_START; /* 0x40 */
    u32 bit = pc_clz32(flags[0]);

    if (bit == 32) {
        flags = &pc_lockIdFlag[1];
        base = OS_MAINP_LOCK_ID_START + 32; /* 0x60 */
        bit = pc_clz32(flags[0]);
        if (bit == 32) {
            return OS_LOCK_ID_ERROR;
        }
    }

    flags[0] &= ~(0x80000000u >> bit);
    return (s32)(base + bit);
}

/* The inverse. The asm does not validate: it computes a shift amount from the
 * id and lets the shift-by-32-or-more rule turn an out-of-range id into a
 * no-op, which pc_lsr() reproduces (an id below 0x40 underflows to a large
 * amount and frees nothing). */
void OS_ReleaseLockID(u16 lockID)
{
    u32 *flags;
    u32 bit;

    if (lockID >= OS_MAINP_LOCK_ID_START + 32) {
        flags = &pc_lockIdFlag[1];
        bit = (u32)lockID - (OS_MAINP_LOCK_ID_START + 32);
    } else {
        flags = &pc_lockIdFlag[0];
        bit = (u32)lockID - OS_MAINP_LOCK_ID_START;
    }

    flags[0] |= pc_lsr(0x80000000u, bit);
}

/* The asm body (os_spinLock.c) is two instructions: `ldr r1, =
 * OS_UnlockCartridge; bx r1`. It exists only because the two names differ by
 * capitalisation, and OS_UnlockCartridge itself is ordinary C compiled from
 * the same file, so the faithful implementation is the same tail call, not
 * a stub. A no-op here would strand the cartridge lock word that the real
 * OS_LockCartridge (also compiled C) sets, and the next lock attempt would
 * spin forever. */
s32 OS_UnLockCartridge(u16 lockID)
{
    return OS_UnlockCartridge(lockID);
}

/* ------------------------------------------------------------------ */
/* DTCM                                                               */
/* ------------------------------------------------------------------ */

/* On hardware this reads the DTCM region register (CP15 c9,c1,0) and masks it
 * with HW_C9_TCMR_BASE_MASK (os_tcm.c). crt0 programmed that register with
 * SDK_AUTOLOAD_DTCM_START, a linker-script symbol; this ROM's value is in
 * platinum.us/main.lsf:
 *
 *     Autoload DTCM { Address 0x027E0000 ... }
 *
 * so the identity-mapped guest keeps DTCM at that address and this returns
 * the constant the register would hold. */
#define PC_DTCM_BASE 0x027E0000u

u32 OS_GetDTCMAddress(void)
{
    return PC_DTCM_BASE & HW_C9_TCMR_BASE_MASK;
}

/* ------------------------------------------------------------------ */
/* Busy waits                                                         */
/* ------------------------------------------------------------------ */

/* `subs r0, r0, #4; bcs`, burn the requested cycle count. Callers use it
 * for hardware settle delays (VRAM banking, cartridge timing); with no guest
 * clock to advance yet, completing instantly is the correct host reading of
 * "wait this many cycles", not an approximation of something else. */
void OS_SpinWait(u32 cycle)
{
    (void)cycle;
}

/* The BIOS routine is the same shape (`subs r0, #4` until exhausted), used by
 * the SDK's spinlocks as a polite pause between polls of a lock word the
 * ARM7 might hold. There is no ARM7, so there is nothing to wait for. */
void SVC_WaitByLoop(s32 count)
{
    (void)count;
}

/* ------------------------------------------------------------------ */
/* BIOS: Sqrt                                                         */
/* ------------------------------------------------------------------ */

/* floor(sqrt(v)) of an unsigned 32-bit operand, returned as u16 (the true
 * root of a u32 always fits: sqrt(0xFFFFFFFF) = 65535). Classic binary
 * restoring square root. Checked exhaustively over 0..70000 plus a stride and
 * the edge values {0,1,2,3,4,0xFFFF,0x10000,0xFFFE0001,0xFFFFFFFF} against
 * the definition r*r <= v < (r+1)*(r+1). */
u16 SVC_Sqrt(u32 src)
{
    u32 rem = 0;
    u32 root = 0;
    int i;

    for (i = 0; i < 16; i++) {
        root <<= 1;
        rem = (rem << 2) | (src >> 30);
        src <<= 2;
        if (root < rem) {
            rem -= root | 1;
            root += 2;
        }
    }

    return (u16)(root >> 1);
}

/* ------------------------------------------------------------------ */
/* BIOS: GetCRC16                                                     */
/* ------------------------------------------------------------------ */

/* The DS BIOS CRC: reflected CRC-16 with polynomial 0xA001, initial value in
 * the first argument, no final XOR, data processed low bit first. Validation:
 * over "123456789", start 0 gives 0xBB3D (the CRC-16/ARC check value) and
 * start 0xFFFF gives 0x4B37 (the CRC-16/MODBUS check value), both reproduced
 * by this loop; it was also diffed against the equivalent nibble-table
 * formulation (table 0x0000, 0xCC01, 0xD801, ... 0x4400) for every length
 * 0..257 over a 257-byte pattern at many initial values. */
u16 SVC_GetCRC16(u32 start, const void *datap, u32 size)
{
    const u8 *p = (const u8 *)datap;
    u32 crc = start & 0xFFFFu;
    u32 i;
    int b;

    for (i = 0; i < size; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xA001u) : (crc >> 1);
        }
    }

    return (u16)crc;
}

/* ------------------------------------------------------------------ */
/* BIOS: CpuSet / CpuFastSet                                          */
/* ------------------------------------------------------------------ */

/* The control word is DMA-register-shaped; the SDK builds it from the MI_DMA_*
 * macros, whose values pin the layout:
 *
 *   bits 0-20  transfer count, in UNITS (halfwords or words), mask 0x1FFFFF
 *   bit 24     source fixed (MI_DMA_SRC_FIX = 2 << 23) -> fill, else copy
 *   bit 26     unit size (MI_DMA_32BIT_BUS = 1 << 26) -> words, else halfwords
 *
 * A fill reads the source unit once and stores it count times; a copy
 * advances both pointers.
 *
 * A count of zero traps rather than guessing: the BIOS loop tests AFTER its
 * first transfer (subs/bgt), so zero on hardware transfers something, how
 * much has not been verified here, and no SDK caller passes zero. */
#define PC_CPUSET_COUNT_MASK 0x001FFFFFu
#define PC_CPUSET_SRC_FIXED  (1u << 24)
#define PC_CPUSET_UNIT_32BIT (1u << 26)

void SVC_CpuSet(const void *srcp, void *destp, u32 dmaCntData)
{
    u32 count = dmaCntData & PC_CPUSET_COUNT_MASK;
    int fixed = (dmaCntData & PC_CPUSET_SRC_FIXED) != 0;
    u32 i;

    if (count == 0) {
        pc_trap("SVC_CpuSet",
                "count 0: the BIOS transfer loop is post-tested, so hardware "
                "would not transfer nothing, unverified, refusing to guess");
    }

    if (dmaCntData & PC_CPUSET_UNIT_32BIT) {
        const u32 *src = (const u32 *)srcp;
        u32 *dst = (u32 *)destp;
        if (fixed) {
            u32 fill = *src;
            for (i = 0; i < count; i++) {
                dst[i] = fill;
            }
        } else {
            for (i = 0; i < count; i++) {
                dst[i] = src[i];
            }
        }
    } else {
        const u16 *src = (const u16 *)srcp;
        u16 *dst = (u16 *)destp;
        if (fixed) {
            u16 fill = *src;
            for (i = 0; i < count; i++) {
                dst[i] = fill;
            }
        } else {
            for (i = 0; i < count; i++) {
                dst[i] = src[i];
            }
        }
    }
}

/* CpuFastSet: always words, and the BIOS moves them with 8-register
 * ldmia/stmia bursts in a post-tested loop, so a count that is not a multiple
 * of 8 is rounded UP, up to 7 words beyond the requested count are read and
 * written, exactly as on hardware. The SDK relies on the rounding being
 * harmless rather than avoiding it: os_china.c calls
 * SVC_CpuClearFast(0, HW_DB_BG_PLTT, 0x04), count 1, and hardware clears 8
 * words of the (much larger) palette. With identity-mapped guest memory the
 * overrun lands on the same guest bytes it would on hardware.
 *
 * Count zero traps for the same post-tested-loop reason as SVC_CpuSet. */
void SVC_CpuSetFast(const void *srcp, void *destp, u32 dmaCntData)
{
    u32 count = dmaCntData & PC_CPUSET_COUNT_MASK;
    const u32 *src = (const u32 *)srcp;
    u32 *dst = (u32 *)destp;
    u32 i;

    if (count == 0) {
        pc_trap("SVC_CpuSetFast",
                "count 0: the BIOS burst loop is post-tested, so hardware "
                "would not transfer nothing, unverified, refusing to guess");
    }

    count = (count + 7u) & ~7u;

    if (dmaCntData & PC_CPUSET_SRC_FIXED) {
        u32 fill = *src;
        for (i = 0; i < count; i++) {
            dst[i] = fill;
        }
    } else {
        for (i = 0; i < count; i++) {
            dst[i] = src[i];
        }
    }
}

/* ------------------------------------------------------------------ */
/* BIOS: UnpackBits                                                   */
/* ------------------------------------------------------------------ */

/* BitUnPack: widen packed source units into larger destination units.
 * MIUnpackBitsParam (nitro/mi/uncompress.h):
 *
 *   srcNum          source length in BYTES
 *   srcBitNum       bits per source unit  (1, 2, 4 or 8)
 *   destBitNum      bits per dest unit    (1, 2, 4, 8, 16 or 32)
 *   destOffset      value added to each NONZERO unit after widening
 *   destOffset0_on  if set, the offset is added to zero units too
 *
 * Source bytes are consumed low bits first; widened units are packed into a
 * 32-bit accumulator low bits first and the accumulator is stored each time
 * it fills, so the destination must be word-aligned and is written in whole
 * words only. The offset is added before packing and is NOT masked to
 * destBitNum, an offset that overflows the unit bleeds into its neighbor,
 * as on hardware.
 *
 * Worked example, srcNum=2 src={0x21,0x43} srcBitNum=4 destBitNum=8
 * destOffset=0x30 destOffset0_on=0: units 1,2,3,4 widen to 0x31,0x32,0x33,
 * 0x34 and pack into the single word 0x34333231, the bytes "1234" in
 * memory order.
 *
 * Parameter combinations outside the BIOS's contract trap: unit widths not in
 * the sets above, a source unit wider than the destination unit, and a total
 * output that is not a whole number of words (every full-word store has an
 * unambiguous meaning; whether the BIOS flushes or discards a partial final
 * word has not been verified here, and the SDK's callers, graphics
 * unpacking, always produce word-multiple output). */
void SVC_UnpackBits(const void *srcp, void *destp, const MIUnpackBitsParam *paramp)
{
    const u8 *src = (const u8 *)srcp;
    u32 *dst = (u32 *)destp;
    u32 srcBits = paramp->srcBitNum;
    u32 destBits = paramp->destBitNum;
    u32 offset = paramp->destOffset;
    int offsetZero = paramp->destOffset0_on != 0;
    u32 srcMask;
    u32 acc = 0;
    u32 accBits = 0;
    u32 i;
    u32 bit;

    if (srcBits != 1 && srcBits != 2 && srcBits != 4 && srcBits != 8) {
        pc_trap("SVC_UnpackBits", "srcBitNum not 1/2/4/8, outside the BIOS contract");
    }
    if (destBits != 1 && destBits != 2 && destBits != 4 && destBits != 8
        && destBits != 16 && destBits != 32) {
        pc_trap("SVC_UnpackBits", "destBitNum not 1/2/4/8/16/32, outside the BIOS contract");
    }
    if (destBits < srcBits) {
        pc_trap("SVC_UnpackBits", "destBitNum < srcBitNum, BitUnPack only widens");
    }

    srcMask = (1u << srcBits) - 1u;

    for (i = 0; i < paramp->srcNum; i++) {
        u32 byte = src[i];
        for (bit = 0; bit < 8; bit += srcBits) {
            u32 unit = (byte >> bit) & srcMask;
            if (unit != 0 || offsetZero) {
                unit += offset;
            }
            acc |= unit << accBits;
            accBits += destBits;
            if (accBits >= 32) { /* destBits divides 32, so this is exact */
                *dst++ = acc;
                acc = 0;
                accBits = 0;
            }
        }
    }

    if (accBits != 0) {
        pc_trap("SVC_UnpackBits",
                "output is not a whole number of words; whether the BIOS "
                "stores or discards a partial final word is unverified");
    }
}

/* ------------------------------------------------------------------ */
/* mwcc / MSL runtime glue                                            */
/* ------------------------------------------------------------------ */

/* mwcc's alloca. g2d_CharCanvas.c calls it with no declaration in scope, so
 * the object carries an undefined reference to a plain function symbol, and
 * a function symbol CANNOT implement alloca: the memory must live in the
 * caller's frame, which a callee cannot grow (forwarding to __builtin_alloca
 * here would allocate in this frame and hand back a dangling pointer at
 * return). Until the build maps __alloca onto the compiler builtin at the
 * call site (a prelude macro), any call is unservable, so it traps. */
/* The prelude's macro would rewrite the NAME below into __builtin_alloca and
 * this would become a definition of a compiler builtin, which gcc allows
 * with a warning and clang refuses outright. The trap symbol is the point of
 * this function, so the macro is put aside for it. */
#undef __alloca

void *__alloca(u32 size)
{
    (void)size;
    pc_trap("__alloca",
            "alloca cannot be a function symbol (the allocation must live in "
            "the caller's frame); map __alloca to __builtin_alloca at the "
            "call site instead");
}

/* MSL's assertion sink; the signature is pinned by the one in-tree caller,
 * NitroDWC's gsAssert.h: (condition, file, function, line). Print everything
 * it gives us, then abort; an assertion failure must never continue. */
void __msl_assertion_failed(const char *condition, const char *filename,
                            const char *funcname, int lineno)
{
    fprintf(stderr, "pc_os_lite: assertion failed: %s:%d (%s): %s\n",
            filename ? filename : "<null>",
            lineno,
            funcname ? funcname : "<null>",
            condition ? condition : "<null>");
    abort();
}

/* Head of the mwcc static-destructor chain. __register_global_object links
 * C++ objects onto it and fs_overlay.c walks it when an overlay unloads
 * (mw_dtor.h / fs_overlay.c). Nothing in a C-only link ever registers one,
 * so the empty chain (a NULL head that every walk skips) is the correct
 * value, not a stub. */
struct MWiDestructorChain *__global_destructor_chain = NULL;

/* ------------------------------------------------------------------ */
/* CLZ                                                                */
/* ------------------------------------------------------------------ */

/* Despite the name this is count-LEADING-zeros: the stripped asm body in
 * os_vramExclusive.c is exactly `clz r0, r0; bx lr`, and its callers read it
 * that way (`31 - OsCountZeroBits(map)` is the index of the highest set bit).
 * With source that short there is nothing to guess. */
u32 OsCountZeroBits(u32 bitmap)
{
    return pc_clz32(bitmap);
}

/* ------------------------------------------------------------------ */
/* Loud traps                                                         */
/* ------------------------------------------------------------------ */

/* Thread context switching. OSContext save/load/init is the heart of the
 * cooperative scheduler and needs a real host mechanism (fibers or
 * equivalent); faking any one of the three corrupts control flow, so all
 * three trap until that layer exists. */



/* The IRQ entry stub the vector jumps to. Nothing delivers interrupts yet;
 * when something does, it will own this. */
void OS_IrqHandler(void)
{
    pc_trap("OS_IrqHandler", "no interrupt delivery exists yet");
}

/* Alarm callback trampoline (os_alarm.c): reached only from the timer
 * interrupt, which does not exist yet. */
void OSi_AlarmHandler(void *arg)
{
    (void)arg;
    pc_trap("OSi_AlarmHandler", "alarms fire from the timer IRQ, which does not exist yet");
}

/* CPU exception vector targets (os_exception.c). A host fault becomes a
 * signal, not a guest exception; if the guest ever *installs and reaches*
 * these, that is real information. */
void OSi_ExceptionHandler(void)
{
    pc_trap("OSi_ExceptionHandler", "guest CPU exceptions have no host delivery path");
}

void OSi_DebuggerExceptionHook(void)
{
    pc_trap("OSi_DebuggerExceptionHook", "guest CPU exceptions have no host delivery path");
}

/* Spins on REG_VCOUNT during OS_Init (os_init.c). Trapping is deliberate: a
 * This trapped at first so that a caller needing real display timing would
 * name itself; the caller that showed up is boot-time init synchronizing to
 * a frame boundary before display setup. With no display yet, every IO read
 * of REG_VCOUNT returns 0; the port IS at VCount 0 by every observation
 * the guest can make, so returning immediately is consistent, not a lie.
 * When a video/pacing layer arrives and VCOUNT starts advancing, it owns
 * this wait. */
void OSi_WaitVCount0(void)
{
}

/* "Return to the DS menu." A PC port must answer this differently, restart
 * the process, or exit, and that is a frontend decision, not something to
 * guess here. */
void OS_ResetSystem(u32 parameter)
{
    (void)parameter;
    pc_trap("OS_ResetSystem", "soft reset has no host meaning yet; decide restart-vs-exit in the frontend");
}

/* Wait-for-interrupt. Correct once an IRQ/pacing layer can wake it; until
 * then a caller reaching it would spin forever on hardware and must be seen,
 * not skipped. */
/* Wait-for-interrupt, and the port's whole interrupt model with it.
 *
 * One source is modeled: VBlank. OS_Halt is only reached from waits (the
 * idle thread; OS_WaitIrq's loop), so "halt until the next interrupt"
 * becomes "the next VBlank is now": raise the IF bit and call the
 * handler the game registered in the OS's own table (OS_SetIrqFunction /
 * OS_GetIrqFunction are compiled C operating on plain memory). The
 * handler is the game's real VBlankIntr; it acks, sets the IRQ check
 * flag, wakes threads, so everything downstream is the SDK's own
 * machinery, not a model of it.
 *
 * If VBlank delivery cannot ever wake this halt (IME clear, or the
 * VBlank bit not enabled in IE), the halt would spin forever on hardware
 * too, trap, do not spin silently.
 *
 * Known modeling gap, recorded: REG_IF is write-1-to-clear silicon, and
 * the IO window is plain RAM, so the handler's ack-write SETS the bit
 * here instead of clearing it. Nothing reads IF yet; whatever first does
 * gets this comment. */
/* The port's frame clock: one tick per delivered VBlank. Global because
 * pc_video_frame_end() numbers dump frames with it. */
u32 pc_os_vblank_count;

/* The Windows host-spin watchdog (pc/src/pc_win_fiber.c) samples this to
 * ask "is the program advancing"; named for its diamond counterpart. */
unsigned long long pc_irq_frames(void) { return pc_os_vblank_count; }

void OS_Halt(void)
{
    OSIrqFunction fn;

    /* Scripted input applies before the VBlank is delivered, so the frame
     * whose logic follows it samples the new level, the ordering the
     * ARM7's own refresh gives a console. */
    {
        extern void pc_input_frame(unsigned long long frame);
        pc_input_frame(pc_os_vblank_count);
    }

    /* The save chip, which writes its image out once a save's burst of card
     * commands has settled rather than on every one of them. Here because it
     * is the port's one guest-frame boundary and both hosts reach it. */
    {
        extern void pc_card_step(void);
        pc_card_step();
    }

    if (!(reg_OS_IME & 1) || !(reg_OS_IE & OS_IE_V_BLANK)) {
        pc_trap("OS_Halt",
                "halted with VBlank delivery disabled (IME/IE); no modeled "
                "interrupt source could ever wake this");
    }

    reg_OS_IF = OS_IE_V_BLANK;
    fn = OS_GetIrqFunction(OS_IE_V_BLANK);
    if (fn == NULL) {
        pc_trap("OS_Halt", "VBlank enabled but no handler is registered");
    }
    {
        /* PC_TRACE_FRAMES=1 heartbeats every 60 deliveries so a live run
         * and a silent hang are distinguishable from the outside. */
        static int sTrace = -1;
        u32 sVBlanks;
        pc_os_vblank_count++;
        sVBlanks = pc_os_vblank_count;
        if (sTrace < 0) {
            extern char *getenv(const char *);
            sTrace = getenv("PC_TRACE_FRAMES") != NULL;
        }
        if (sTrace && (sVBlanks % 60) == 0) {
            fprintf(stderr, "pc: vblank %u\n", (unsigned)sVBlanks);
        }
        /* A display-state pulse at frame 300: is the game actually
         * producing something a renderer would show? Sums, not dumps,
         * enough to tell "real graphics data" from "still zeros". */
        if (sTrace && (sVBlanks % 300) == 0) {
            extern u32 pc_gpu3d_num_polygons(void);
            extern u32 pc_gpu3d_soft_polygons_drawn(void);
            const u16 *pal = (const u16 *)0x05000000;
            const u32 *vram = (const u32 *)0x06000000;
            u32 pal_sum = 0, vram_sum = 0, i;
            for (i = 0; i < 0x200; i++) {
                pal_sum += pal[i];
            }
            for (i = 0; i < 0x20000 / 4; i++) {
                vram_sum += vram[i];
            }
            fprintf(stderr,
                    "pc: display probe: DISPCNT=%08x/%08x "
                    "palsum=%08x vramsum(A)=%08x poly=%u drawn=%u\n",
                    *(volatile u32 *)0x04000000,
                    *(volatile u32 *)0x04001000,
                    (unsigned)pal_sum, (unsigned)vram_sum,
                    (unsigned)pc_gpu3d_num_polygons(),
                    (unsigned)pc_gpu3d_soft_polygons_drawn());
        }
    }
    fn();

    /* The half of the hardware dispatcher the handler cannot do for
     * itself: OS_IrqHandler's epilogue wakes OSi_IrqThreadQueue so
     * OS_WaitIrq sleepers re-check their flag. Without this, the main
     * thread sleeps forever while the idle thread delivers VBlanks
     * nobody hears, measured on the boot's first frame wait. */
    {
        extern OSThreadQueue OSi_IrqThreadQueue;
        OS_WakeupThread(&OSi_IrqThreadQueue);
    }

    /* The sound driver's 192 Hz cadence and the mixer, batched here,
     * before the render/dump so a run that ends at this frame boundary
     * has this frame's audio already produced. 560,190 is one frame of
     * the 33,513,982 Hz guest clock at 59.8261 frames a second; the SPU
     * makes one sample per 1024 of those (pc_audio.c). */
    {
        extern void pc_arm7snd_frame(void);
        extern void pc_audio_advance(unsigned long long cycles);
        pc_arm7snd_frame();
        pc_audio_advance(560190u);
    }

    /* The frame boundary, the renderer. VBlank is when a hardware frame has
     * finished scanning out, so this is where the renderer reads the
     * registers/VRAM/OAM as the game left them and the dump consumes the
     * surfaces. After the handler and the wakeup: the handler is part of
     * the frame it ends (it acks and requeues work exactly as on
     * hardware), the render reads the settled state. */
    {
        extern void pc_video_frame_end(unsigned long long frame);
        extern void pc_bgm_mute_apply(void);   /* PC_MUTE_BGM: SE only */
        extern u32 pc_os_vblank_count;

        /* Before the render/handler: pause the BGM players if asked, in the
         * same main-thread context the game's own sound calls run in. */
        pc_bgm_mute_apply();

        pc_video_frame_end(pc_os_vblank_count);

        /* The differential checkpoint, AFTER the render: the trace compares
         * the picture as well as memory, and a checkpoint taken before the
         * renderer ran would digest the previous frame's surfaces against the
         * console's current ones. Silent and near-free when no trace is open. */
        {
            extern void pc_diff_frame(unsigned long long frame);
            pc_diff_frame(pc_os_vblank_count);
        }
    }
}

/* The ARM946E-S protection unit. The host has no MPU, so there is nothing
 * to program. These trapped at first, on the reasoning that a caller which
 * RELIES on protection semantics (fault on access, permission flips around
 * DMA) must not be silently no-op'd, and the trap did its job: the caller
 * that showed up is OS_Init's boot-time region setup, which configures the
 * MPU as defense-in-depth and depends on nothing it does. No-ops are the
 * faithful port for that caller. If some later caller genuinely needs
 * fault-on-access, no mechanism here will provide it silently, that
 * would have to be built on mprotect, deliberately. */
void OS_SetProtectionRegion1(u32 param)
{
    (void)param;
}

void OS_SetProtectionRegion2(u32 param)
{
    (void)param;
}

void OS_SetDPermissionsForProtectionRegion(u32 setMask, u32 flags)
{
    (void)setMask; (void)flags;
}
