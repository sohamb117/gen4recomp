/*
 * 3ds/src/3ds_fault.c: what this console says when it faults.
 *
 * None of the PC port's answer ports. There, a fatal signal is caught with
 * sigaction, the faulting address comes out of siginfo_t, the stack comes out
 * of backtrace(), and SIGFPE is fixed up through ucontext because an x86
 * integer divide by zero traps where the guest's ARM946E-S absorbs it. This
 * console has no signals, no backtrace, no ucontext, and no SIGFPE problem at
 * all, because the ARM11 absorbs a divide by zero exactly as the ARM946E-S
 * does. What it has instead is threadOnException(), which hands a handler the
 * abort type, the fault address, the fault status register and a register
 * dump.
 *
 * The one thing the report must say, and the reason this file exists rather
 * than a call to ERRF_ThrowResult: a fault address here is a host address, and
 * the question anybody debugging this port will ask is which guest address it
 * was. armrec_guest_addr() answers it for anything in the slab, and a null
 * dereference with a small offset is the signature of a translation that
 * returned NULL and was used anyway. So the report names the guest address
 * when there is one and says "NULL+off" when there is not, and those two lines
 * are worth more than a backtrace would be.
 *
 * The handler draws and then leaves. It cannot return, so it presents the
 * report on both screens and waits for Start, then ends the process. Drawing
 * from a fault handler is not free of risk, but a black screen is not a report
 * and this console has no stderr.
 *
 * Whether it fires at all is not this file's to promise, and on the emulator
 * it does not. libctru documents that CPU exceptions reach this mechanism only
 * when UNITINFO was non-zero at kernel start. Measured on one emulator build: a
 * read of address 0 returns zeros and the frame counter keeps climbing, no
 * abort and no handler. That means a NULL from the translator is silent there
 * and loud only on hardware.
 *
 * So the report is split from the delivery. fault_report() is reached by the
 * crt's own probe as well as by the handler, and everything after delivery is
 * observable anywhere; delivery itself is a hardware claim and stays one.
 */

/*
 * libctru only for the half that talks to the console, the way 3ds_view.c
 * splits its blit from its geometry: the description below is pure C over the
 * translator, so 3ds/tests/fault_report.c compiles this file on a build
 * machine (where __3DS__ is not defined) and checks every case it can
 * produce.
 */
#ifdef __3DS__
#include <3ds.h>
#endif

#include <stdio.h>
#include <string.h>

#include "3ds_fault.h"
#include "3ds_guest.h"
#include "3ds_guest_map.h"
#include "3ds_view.h"

/*
 * A null dereference is a translation that answered NULL and was used, and the
 * offset is the field that was reached for. Anything within a page of zero is
 * that rather than a wild pointer; 4 KB is the granularity of every row in the
 * map, so nothing legitimate is that low.
 */
#define FAULT_NULL_PAGE 0x1000u

/*
 * uintptr_t and not u32, which is a concession to the build machine and costs
 * the console nothing: a 3DS pointer *is* 32 bits, so the ARM's fault address
 * widens exactly, while truncating a host pointer on an LP64 test machine
 * would put every address somewhere else. The same lesson armrec_guest_addr()
 * already learned.
 */
int fault_describe(uintptr_t addr, char *buf, int n)
{
    const uint8_t *vram;
    uint32_t guest;

    if (buf == NULL || n <= 0) {
        return 0;
    }

    if (addr < FAULT_NULL_PAGE) {
        return snprintf(buf, (size_t)n, "NULL+%lX", (unsigned long)addr);
    }

    guest = armrec_guest_addr((const void *)addr);
    if (guest != 0) {
        return snprintf(buf, (size_t)n, "GUEST %08X", (unsigned)guest);
    }

    /* The one row the translator cannot name backwards: which guest address a
     * byte of the bank store answers to is VRAMCNT's business and changes. The
     * offset into the store is still the useful half. */
    vram = (const uint8_t *)guest_region_base(GUEST_R_VRAM);
    if (vram != NULL && addr >= (uintptr_t)vram
        && addr - (uintptr_t)vram < (uintptr_t)GUEST_BACK_VRAM) {
        return snprintf(buf, (size_t)n, "VRAM+%lX",
                        (unsigned long)(addr - (uintptr_t)vram));
    }

    return snprintf(buf, (size_t)n, "HOST %08lX", (unsigned long)addr);
}

const char *fault_type_name(int type)
{
    /* ERRF_ExceptionType, written out because this half of the file is
     * compiled without libctru too. The asserts below are the check. */
    switch (type) {
    case FAULT_PREFETCH_ABORT:
        return "PREFETCH";
    case FAULT_DATA_ABORT:
        return "DATA";
    case FAULT_UNDEFINED:
        return "UNDEF";
    case FAULT_VFP:
        return "VFP";
    default:
        return "?";
    }
}

#ifdef __3DS__
_Static_assert(FAULT_PREFETCH_ABORT == ERRF_EXCEPTION_PREFETCH_ABORT, "");
_Static_assert(FAULT_DATA_ABORT == ERRF_EXCEPTION_DATA_ABORT, "");
_Static_assert(FAULT_UNDEFINED == ERRF_EXCEPTION_UNDEFINED, "");
_Static_assert(FAULT_VFP == ERRF_EXCEPTION_VFP, "");
#endif

#ifdef __3DS__

/*
 * The handler's own stack. It cannot run on the faulting one: an abort taken
 * on a stack overflow would fault again on the first push and the report would
 * be lost. 4 KB is snprintf and a blit.
 */
static u8 sFaultStack[0x1000] __attribute__((aligned(8)));
static ERRF_ExceptionData sFaultData;

/* The two lines, kept where a later frame can find them and where the
 * handler does not have to build a stack frame for them. */
static char sLineTop[64];
static char sLineBottom[64];

/* The picture the fault screen shows behind the text: whatever the port last
 * presented, if it left one here. NULL is a black screen, which is honest. */
static const uint32_t *sTopSurface;
static const uint32_t *sBottomSurface;

void fault_set_surfaces(const uint32_t *top, const uint32_t *bottom)
{
    sTopSurface = top;
    sBottomSurface = bottom;
}

/*
 * The report is its own function, and that is what makes any of this
 * checkable. Whether a CPU abort is *delivered* to this handler is a property
 * of the kernel the console booted with, and it is not delivered on the
 * emulator this port is developed against: Azahar 2126.0 answers an unmapped
 * read with zeros and runs on, measured with a read of address 0, which left
 * the frame counter climbing. So delivery is a hardware claim and stays one.
 * Everything after delivery, the description, the two lines, drawing them
 * from the handler's own stack, and leaving on Start, is reached by the
 * probe in the crt as well, and that is observable anywhere.
 */
/*
 * No aptMainLoop() here: that asks the applet manager to co-operate, and after
 * a real abort the process is in a state it does not know about. Draw, wait
 * for Start, leave.
 */
static void fault_show(void)
{
    for (;;) {
        hidScanInput();
        if (hidKeysDown() & (KEY_START | KEY_SELECT)) {
            break;
        }
        view_present(sTopSurface, sBottomSurface, sLineTop, sLineBottom);
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    gfxExit();
    svcExitProcess();
}

void fault_report(int type, uint32_t addr, uint32_t fsr, uint32_t pc,
                  uint32_t lr, uint32_t sp)
{
    char where[32];

    fault_describe(addr, where, (int)sizeof where);

    snprintf(sLineTop, sizeof sLineTop, "FAULT %s AT %s",
             fault_type_name(type), where);
    snprintf(sLineBottom, sizeof sLineBottom, "PC %08X LR %08X SP %08X FSR %08X",
             (unsigned)pc, (unsigned)lr, (unsigned)sp, (unsigned)fsr);

    fault_show();
}

/*
 * A failure the port found itself. The lines are the caller's because the
 * useful half is what it knows, which model, and what it was asked for,
 * and nothing about a CPU register would add to that.
 */
void fault_stop(const char *top, const char *bottom)
{
    snprintf(sLineTop, sizeof sLineTop, "%s", top != NULL ? top : "TRAP");
    snprintf(sLineBottom, sizeof sLineBottom, "%s", bottom != NULL ? bottom : "");

    fault_show();
}

static void fault_handler(ERRF_ExceptionInfo *excep, CpuRegisters *regs)
{
    fault_report((int)excep->type, excep->far, excep->fsr, regs->pc, regs->lr,
                 regs->sp);
    svcExitProcess();
}

void fault_install(void)
{
    threadOnException(fault_handler, sFaultStack + sizeof sFaultStack,
                      &sFaultData);
}

const char *fault_last_line(int which)
{
    return which == 0 ? sLineTop : sLineBottom;
}

#else /* !__3DS__ */

/*
 * The build machine's half of the same thing. There is no screen to leave the
 * report on, so it goes where a host program's last words go. The tests that
 * compile this file do not expect to reach it; a run that does has found
 * something, and it should say what.
 */
#include <stdlib.h>

void fault_stop(const char *top, const char *bottom)
{
    fprintf(stderr, "%s\n%s\n", top != NULL ? top : "TRAP",
            bottom != NULL ? bottom : "");
    abort();
}

#endif /* __3DS__ */
