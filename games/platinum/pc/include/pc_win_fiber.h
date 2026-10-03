/*
 * Windows fibers behind four names with no <windows.h> in sight.
 *
 * The files that switch fibers, pc/src/pc_os_fiber.c, pc/src/pc_arm7.c,
 * also include the guest SDK's headers, and the two worlds do not share a
 * preprocessor: windows.h's own imm.h stops parsing once the guest's macros
 * are in scope. So the one translation unit that includes windows.h is
 * pc/src/pc_win_fiber.c, which includes nothing of the guest, and this is
 * its whole surface. Empty on POSIX; nothing there calls it.
 */

#ifndef POKEDIAMOND_PC_WIN_FIBER_H
#define POKEDIAMOND_PC_WIN_FIBER_H

/* The current thread as a fiber, converting it on first call. */
void *pcw_self_fiber(void);

/* A new fiber that will run fn(arg) when first switched to. NULL on failure. */
void *pcw_create_fiber(unsigned stack_size, void (*fn)(void *), void *arg);

/* Suspend here and run `to`. Returns when something switches back. */
void pcw_switch_fiber(void *to);

/* Free a fiber that is not running. */
void pcw_delete_fiber(void *fiber);

/* Print faulting EIP and address on an unhandled exception, the Windows
 * stand-in for the frame gdb would show. Installed once, early in main. */
void pcw_install_crash_report(void);

/* The stall detector's Windows twin: a thread that watches the guest frame
 * counter and, fifteen frozen seconds later, prints the main thread's EIP
 * and aborts, the host-spin hang made legible on the one platform with no
 * debugger attached. Call from the main thread; it duplicates that thread's
 * handle to read its context. */
void pcw_install_watchdog(void);

/* The escape hatch for the GUI subsystem: give this process somewhere to
 * print. Does nothing at all when stdout and stderr already lead somewhere,
 * which is every run the harness starts; a redirected handle is inherited
 * whatever the subsystem, so reattaching there would take the output AWAY
 * from the pipe the caller is reading. Call it early, from main. */
void pcw_attach_console(void);

#endif /* POKEDIAMOND_PC_WIN_FIBER_H */
