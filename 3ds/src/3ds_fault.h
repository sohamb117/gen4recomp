/*
 * 3ds/src/3ds_fault.h: the fault report.
 *
 * No signals on this console and no backtrace: a CPU abort arrives at a
 * handler installed with threadOnException(), which cannot return. See
 * 3ds_fault.c for what the report says and why the guest address is the half
 * that matters.
 */

#ifndef POKEPLATINUM_3DS_FAULT_H
#define POKEPLATINUM_3DS_FAULT_H

#include <stdint.h>

/*
 * Install the handler on the current thread. Called once, from the crt, after
 * the graphics are up, the handler draws.
 */
void fault_install(void);

/*
 * What a faulting address *is*, in this port's terms, into `buf`. Returns what
 * snprintf returns.
 *
 *   "GUEST 02003F00"  the address is in the slab and translates back
 *   "VRAM+1234"       in the bank store, which no fixed guest address names
 *   "NULL+14"         a translation answered NULL and was used anyway
 *   "HOST 0010A2B4"   the port's own memory: not guest state
 *
 * Pure C over the translator, so 3ds/tests/fault_report.c checks every case on
 * a build machine.
 */
int fault_describe(uintptr_t addr, char *buf, int n);

/* ERRF_ExceptionType, spelled where a file without libctru can still use it.
 * 3ds_fault.c asserts the four against libctru's own when it has them. */
#define FAULT_PREFETCH_ABORT 0
#define FAULT_DATA_ABORT     1
#define FAULT_UNDEFINED      2
#define FAULT_VFP            3

/* "DATA", "PREFETCH", "UNDEF", "VFP", one of the four, named. */
const char *fault_type_name(int type);

/*
 * The picture the fault screen draws behind its two lines. The crt hands over
 * its surfaces so a crash still shows the last frame; NULL for a black screen.
 */
void fault_set_surfaces(const uint32_t *top, const uint32_t *bottom);

/*
 * Show a fault report and end the process: two lines over the last picture,
 * until Start or Select. Does not return.
 *
 * The handler calls this with what the kernel gave it. The crt's probe calls
 * it directly, because whether an abort is *delivered* is a property of the
 * console; the emulator this port is developed against answers an unmapped
 * read with zeros and carries on, while everything after delivery can be
 * seen anywhere.
 */
void fault_report(int type, uint32_t addr, uint32_t fsr, uint32_t pc,
                  uint32_t lr, uint32_t sp);

/* The report, after a fault: 0 is the top line, anything else the bottom.
 * Empty strings before one. */
const char *fault_last_line(int which);

/*
 * The same screen, for a failure the port detects itself rather than one the
 * CPU delivers, armrec_trap()'s fatal path. Two lines the caller writes,
 * over the last picture, until Start or Select. Does not return.
 *
 * A definition exists on a build machine too, where it prints to stderr and
 * aborts: the models this port compiles on the host reach it through
 * armrec_trap(), and a test that hits one should die with the message rather
 * than fail to link.
 */
void fault_stop(const char *top, const char *bottom);

#endif /* POKEPLATINUM_3DS_FAULT_H */
