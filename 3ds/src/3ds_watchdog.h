/*
 * 3ds/src/3ds_watchdog.h: a hang says where it hung. 9.7.
 *
 * Silent hangs are not a port. A desktop that stops can be attached to; this
 * console cannot, and what a player or a bisect sees is two screens holding
 * the last frame forever. The PC port answers that with PC_BOOT_WATCHDOG and
 * the Windows build with a sampling thread; this is the same idea on the one
 * clock this console will give it.
 *
 * It does not get a thread, and that is not a stylistic choice. Every guest
 * thread here runs on one host thread, the SDK's scheduler switches contexts
 * and its critical sections are a variable, so 3ds/tests/one_cpu.py fails the
 * build if anything in the link so much as references threadCreate. What it
 * uses instead is libctru's GSP event thread, which gfxInitDefault() has
 * already started and which is woken by the console's own VBlank interrupt
 * sixty times a second whatever the main thread is doing. That thread is
 * created at one priority above main, so a main thread spinning in a compiled
 * wait loop does not starve it.
 *
 * What counts as alive is a presented frame: frame_count() in 3ds_frame.c,
 * which both the diagnostic loop and the guest's OS_Halt go through. A guest
 * VBlank without a present is not a frame, and a present without a guest
 * VBlank is the loader still holding the console; both are covered because
 * both end at the same function.
 *
 * What it prints in place of a program counter. This port compiles the game
 * to native ARM11 code, so there is no guest PC to read and no debug SVC that
 * would let one thread read another's registers anyway. The breadcrumb is the
 * substitute: watchdog_where() is a single pointer store at the handful of
 * places a frame passes through, and the report names the last one. "3d
 * rasterizer" and "diagnostic loop" are different bugs.
 */

#ifndef POKEPLATINUM_3DS_WATCHDOG_H
#define POKEPLATINUM_3DS_WATCHDOG_H

/* Console VBlanks with no presented frame before it is a hang. Twenty seconds:
 * The slowest scene this port has measured is 311 ms a frame, which is three
 * presents a second, and no legitimate frame can appeal sixty times that. */
#define WATCHDOG_VBLANKS 1200u

/* Where the report goes. Read by 3ds/tests/run.sh, which produces a hang on
 * purpose and expects to find one. */
#define WATCHDOG_PATH "sdmc:/3ds/pokeplatinum/hang-report.txt"

#ifdef __3DS__
/*
 * Register the VBlank callback and start watching. `vblanks` is 0 for
 * WATCHDOG_VBLANKS; the probe passes a short one so a check does not wait
 * twenty seconds for it. Call after gfxInitDefault(): there is no GSP event
 * thread before it.
 */
void watchdog_arm(unsigned vblanks);

/* Stop watching. The teardown does this, because frames stop there on
 * purpose. */
void watchdog_disarm(void);

/* A suspension is not a hang: no frames are presented while the Home Menu has
 * the console, and that can last minutes. 3ds_apt.c pairs these. */
void watchdog_pause(void);
void watchdog_resume(void);
#endif

/*
 * The last place a frame was seen. A string literal, stored as a pointer and
 * never copied; the caller's storage has to outlive the run, which is what a
 * literal does.
 */
void watchdog_where(const char *what);
const char *watchdog_last_where(void);

/*
 * The decision, split out so a build machine drives it: one console VBlank has
 * passed and `frames` presents have happened in total. Returns non-zero the
 * one time it decides the port has hung.
 */
int watchdog_tick_at(unsigned long frames);

/* How many consecutive VBlanks have passed with no presented frame. */
unsigned watchdog_quiet(void);

int watchdog_selftest(int *ranOut);

#endif /* POKEPLATINUM_3DS_WATCHDOG_H */
