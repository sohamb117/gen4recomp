/*
 * 3ds/src/3ds_apt.h: Home, sleep and close, and what this port owes the system
 * when it stops being the foreground application.
 *
 * The system can take the console away at three moments and every one arrives
 * through aptMainLoop(), which 3ds_frame.c calls once per frame. That is the
 * whole reason the frame boundary is a single function: a suspension can only
 * land between two guest frames, never inside one, so nothing here has to be
 * re-entrant and no guest state can be caught half-written.
 *
 *   Home    the Home Menu takes the foreground. aptMainLoop blocks inside
 *           aptJumpToHomeMenu() until the user comes back.
 *   Sleep   the lid closes. aptMainLoop blocks inside aptHandleSleep().
 *   Close   the system wants the application gone. aptMainLoop returns false
 *           and there is a deadline on getting out.
 *
 * Guest time does not advance while we are away, and that is a property of the
 * design rather than something this file arranges. The only guest clock is
 * pc_os_vblank_count, incremented once per delivered VBlank inside OS_Halt(),
 * and nothing in the port reads a host clock to tell the guest what time it
 * is. A frozen guest therefore has a frozen clock for free. What this file
 * adds is the check that says so: the counter is sampled going into a
 * suspension and again coming out, and a difference is a failure with a number
 * on it rather than a slow drift nobody sees.
 *
 * Host time does advance, and has to be taken off the measurement. 3ds_perf.c
 * times present to present, so a Home visit would otherwise be recorded as one
 * frame that took ninety seconds, and the worst-frame column is a maximum, so
 * one of those poisons a window permanently. The suspended interval goes
 * through perf_exclude(), the same door the instruments' own cost goes through.
 *
 * The sound stops at the suspension, not when the queue runs dry. Nothing
 * produces samples while the guest is frozen, so the DSP would play out the
 * one or two buffers already queued on top of the Home Menu and then go
 * silent. Clearing the queue and pausing the channel costs a discontinuity
 * exactly where the player expects one.
 *
 * One teardown, three callers. apt_shutdown() is registered with atexit(), so
 * the guest path's exit(0), a return from main() and the system's own close
 * all reach the same sequence, and it runs before libctru tears down apt and
 * srv underneath it. It is idempotent because more than one can happen.
 */

#ifndef POKEPLATINUM_3DS_APT_H
#define POKEPLATINUM_3DS_APT_H

#include <stdint.h>

/*
 * The bookkeeping, split out from the hook so a build machine can drive it:
 * These two take the host tick and the guest VBlank count rather than reading
 * them, exactly as perf_sample() takes its own clock.
 */
void apt_suspend_at(uint64_t tick, uint32_t vblanks);

/* Host ticks the suspension cost, for perf_exclude(). Zero if no suspension
 * was open, a resume without a suspend is a libctru contract this port does
 * not get to assume. */
uint64_t apt_resume_at(uint64_t tick, uint32_t vblanks);

/* How many times the system took the console away, and how many guest VBlanks
 * passed while it had it. The second must stay zero. */
int apt_suspends(void);
uint32_t apt_guest_frames_lost(void);

/* Host ticks spent suspended over the whole run, which is the quantity that
 * has been taken out of the frame-time report. */
uint64_t apt_suspended_ticks(void);

#ifdef __3DS__
/* The aptHook, and atexit(apt_shutdown). Call once, after gfxInitDefault()
 * and audio_init(); the hook can fire the moment it is installed. */
void apt_install(void);

/* The close sequence: the frame-time report, then the DSP, then the
 * cartridge, then the screens. Idempotent. */
void apt_shutdown(void);

/* True once the system has asked us to go. aptMainLoop() answering false is
 * the same fact; this is the one the hook sees first. */
int apt_closing(void);
#endif

int apt_selftest(int *ranOut);

#endif /* POKEPLATINUM_3DS_APT_H */
