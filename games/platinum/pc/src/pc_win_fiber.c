/* The one translation unit that includes windows.h, pc_win_fiber.h says
 * why. POSIX builds compile the empty tail. */

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <unistd.h>   /* _exit */

#include "pc_win_fiber.h"

/* CreateFiber's entry is WINAPI and takes LPVOID; the port's is cdecl with
 * its own argument. A heap pair bridges them, and it leaks by design: no
 * fiber in this port is ever deleted while its thread could still run
 * (pc/src/pc_os_fiber.c, fiber_alloc's comment). */
struct pcw_start { void (*fn)(void *); void *arg; };

static VOID CALLBACK pcw_entry(LPVOID p) {
    struct pcw_start s = *(struct pcw_start *)p;

    free(p);
    s.fn(s.arg);
}

void *pcw_self_fiber(void) {
    void *f = GetCurrentFiber();

    /* A thread that is not yet a fiber reads as NULL or the historical
     * 0x1E00 magic, depending on the Windows version. */
    if (f == NULL || f == (void *)0x1E00) f = ConvertThreadToFiber(NULL);
    return f;
}

void *pcw_create_fiber(unsigned stack_size, void (*fn)(void *), void *arg) {
    struct pcw_start *s = (struct pcw_start *)malloc(sizeof *s);

    if (s == NULL) return NULL;
    s->fn = fn;
    s->arg = arg;
    return CreateFiber(stack_size, pcw_entry, s);
}

void pcw_switch_fiber(void *to) {
    SwitchToFiber(to);
}

/*
 * A crash reporter, because Windows' signal(SIGSEGV) hands the handler no
 * context and the ELF symbol machinery is stubbed: the vectored handler sees
 * the exception record first, prints where the fault was and what it
 * touched, and lets the search continue so the CRT's SIGSEGV translation
 * (and pc_state's digest handler behind it) still runs. EIP minus the image
 * base is what `i686-w64-mingw32-nm build/pc-win32/pokeplatinum.exe` turns
 * back into a function name.
 */
#include <stdio.h>

#include "pc_div0.h"

extern int armrec_is_guest_addr(unsigned int addr);   /* armrec_rt.c */

static LONG CALLBACK pcw_report_exception(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord;

    /* ARM has no divide instruction, so a guest division by zero is answered
     * by the runtime on a cartridge rather than raised. pc_div0 gives that
     * answer and resumes, which is the Linux handler's job too; this
     * handler runs before any SEH, so continuing here is the whole fix. */
    if (r->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        CONTEXT *c = ep->ContextRecord;
        struct pc_x86_regs g;

        g.r[0] = c->Eax; g.r[1] = c->Ecx; g.r[2] = c->Edx; g.r[3] = c->Ebx;
        g.r[4] = c->Esp; g.r[5] = c->Ebp; g.r[6] = c->Esi; g.r[7] = c->Edi;
        g.eip = c->Eip;
        if (pc_div0_fixup(&g)) {
            c->Eax = g.r[0]; c->Ecx = g.r[1]; c->Edx = g.r[2]; c->Ebx = g.r[3];
            c->Esp = g.r[4]; c->Ebp = g.r[5]; c->Esi = g.r[6]; c->Edi = g.r[7];
            c->Eip = g.eip;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
        r->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION ||
        r->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        r->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        fprintf(stderr,
                "pcw: exception %08lX at EIP=%p%s%p\n",
                (unsigned long)r->ExceptionCode, r->ExceptionAddress,
                r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
                    ? (r->ExceptionInformation[0] ? ", writing "
                                                  : ", reading ")
                    : ", info ",
                (void *)(r->NumberParameters > 1
                             ? (uintptr_t)r->ExceptionInformation[1]
                             : 0));
        fflush(stderr);
        /*
         * EIP inside a mapped guest region means control jumped into guest
         * MEMORY, a DS code address called as a function pointer. Nothing
         * can resume that, and letting the search continue leaves the CRT's
         * SEH swallowing it into a silent half-dead stall (measured: the
         * first battle sat frozen on exactly this). Die the way the Linux
         * build does, with the address on record.
         */
        if (armrec_is_guest_addr((unsigned int)(uintptr_t)r->ExceptionAddress)) {
            fprintf(stderr,
                    "pcw: that EIP is a GUEST address, a DS code pointer "
                    "was called. Fatal.\n");
            fflush(stderr);
            _exit(139);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void pcw_install_crash_report(void) {
    AddVectoredExceptionHandler(1, pcw_report_exception);
}

/*
 * The host-spin watchdog, which is pc_irq.c's stall detector's windows
 * TWIN. That detector counts guest time, so the one hang it cannot see is a
 * loop that never advances the clock, a recompiled busy-wait crossing the
 * host boundary each pass, like PMi_WaitBusy spinning for a reply that is
 * not coming. On Linux gdb answers "where is it spinning" in one SIGINT;
 * this exe has no debugger, so the answer has to come from inside. A thread
 * samples the frame counter once a second; fifteen seconds frozen is a
 * verdict no legitimate frame can appeal (a paced frame is 8ms, a loading
 * screen still ticks VBlanks), and the report is the crash reporter's: the
 * main thread suspended, its EIP read back, and the same nm/addr2line
 * arithmetic names the loop. It aborts rather than resumes because a
 * host-spin never exits by itself, and a process that says where it hung
 * beats one that must be killed in silence.
 */
extern unsigned long long pc_irq_frames(void);

static HANDLE pcw_watch_main;   /* the main thread, duplicated at arm time */

static DWORD WINAPI pcw_watchdog(LPVOID arg) {
    unsigned long long last = 0, now;
    int frozen = 0;

    (void)arg;
    for (;;) {
        Sleep(1000);
        now = pc_irq_frames();
        if (now != last) {
            last = now;
            frozen = 0;
            continue;
        }
        if (++frozen < 15) {
            continue;
        }
        if (SuspendThread(pcw_watch_main) != (DWORD)-1) {
            CONTEXT c;
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(pcw_watch_main, &c)) {
                fprintf(stderr,
                        "pcw: the guest frame counter has been frozen at "
                        "%llu for 15 seconds.\n"
                        "  That is a host spin the guest-time stall detector "
                        "cannot see. The main thread is at\n"
                        "  EIP=%p ESP=%p, "
                        "i686-w64-mingw32-addr2line -e pokeplatinum.exe -f -i "
                        "names it.\n",
                        now, (void *)(uintptr_t)c.Eip,
                        (void *)(uintptr_t)c.Esp);
                fflush(stderr);
            }
        }
        abort();
    }
    return 0;
}

void pcw_install_watchdog(void) {
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &pcw_watch_main, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        return;                  /* no watchdog beats no run */
    }
    (void)CreateThread(NULL, 0, pcw_watchdog, NULL, 0, NULL);
}

void pcw_delete_fiber(void *fiber) {
    DeleteFiber(fiber);
}

/*
 * The console, and why this checks before it acts.
 *
 * The exe is linked into the GUI subsystem so that double-clicking it does
 * not flash a console box. That costs nothing when the parent redirected the
 * standard handles; an inherited handle is inherited whatever the
 * subsystem, which is why WSL interop still reads every line the port prints,
 * and everything when it did not, because then the handles are null and
 * the output goes nowhere.
 *
 * So this only fires in the second case. Attaching a console and pointing
 * the C streams at CONOUT$ UNCONDITIONALLY would break the first one: it
 * would move the output off the pipe the harness is reading and onto a
 * console nobody is looking at. Measured before it was written, a GUI
 * subsystem exe run over interop delivers stdout, stderr and its exit code
 * unchanged.
 */
static int pcw_handle_dead(DWORD which) {
    HANDLE h = GetStdHandle(which);

    return h == NULL || h == INVALID_HANDLE_VALUE;
}

void pcw_attach_console(void) {
    if (!pcw_handle_dead(STD_OUTPUT_HANDLE)
        || !pcw_handle_dead(STD_ERROR_HANDLE)) {
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole()) {
        return;
    }
    SetConsoleTitleA("Pokemon Platinum");
    (void)freopen("CONOUT$", "w", stdout);
    (void)freopen("CONOUT$", "w", stderr);
    (void)freopen("CONIN$", "r", stdin);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

#else

/* ISO C dislikes an empty translation unit. */
typedef int pc_win_fiber_is_windows_only;

#endif
