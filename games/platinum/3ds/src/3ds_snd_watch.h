/*
 * 3ds/src/3ds_snd_watch.h: the silent-channel check, over a real boot.
 *
 * This is the one bug this port is most likely to ship without noticing. A DS
 * channel is pointed at a wave by SOUNDxSAD, which carries bits 26 to 2 of an
 * address. Every wave this game plays lives in the sound heap, the sound heap
 * is inside SoundSystem, and SoundSystem is moved into the port window
 * precisely so its addresses survive that field. If any of that stops being
 * true the game does not crash and does not complain: it goes quiet, or it
 * plays noise.
 *
 * 3ds_snd_addr.c states the rule and proves it over addresses the self-test
 * makes up. This file is the other half: the rule checked against the
 * addresses the game actually keys channels with, which only a boot can
 * produce. pc/hw/pc_spu.c already records every distinct source address and
 * length a channel was keyed on. What is added here is the classification the
 * 3DS needs and PC does not: a truncated host pointer on this console can land
 * inside a real guest region, so "the SPU could read it" is not enough. The
 * address has to be in the window.
 *
 * The verdict is three-valued, because two of them are not failures of the
 * same kind. NONE means the boot never keyed a channel, which is a fact about
 * the run and not about the port. FAIL means it did and an address was wrong.
 * Only PASS says the path from the sound heap to the SPU holds.
 *
 * How it leaves the console: it writes a small text file to the SD card, one
 * fopen per report, the verdict written last, so a run killed mid-write
 * produces a file with no verdict rather than one that says PASS.
 *
 * No libctru here, so 3ds/tests/snd_watch.c runs the whole of it on the host
 * over a planted log.
 */

#ifndef POKEPLATINUM_3DS_SND_WATCH_H
#define POKEPLATINUM_3DS_SND_WATCH_H

#include <stdint.h>

/* Where the console leaves the report. The directory is the one the cartridge
 * is read from when a skinny 3dsx falls back to the SD card, so a
 * console set up to run this port already has it. */
#define SND_WATCH_PATH "sdmc:/3ds/pokeplatinum/snd-report.txt"

enum {
    SND_WATCH_NONE = 0,  /* no channel was keyed on: no evidence either way */
    SND_WATCH_PASS = 1,  /* every keyed address was inside the port window */
    SND_WATCH_FAIL = 2   /* at least one was not */
};

/*
 * Take in whatever the SPU has logged since the last call, and rewrite the
 * report if anything moved. Called once per game frame from the frame
 * boundary; cheap when nothing has changed, which is almost every frame.
 */
void snd_watch_step(void);

/* The tallies, over distinct (address, length) pairs the SPU was keyed with. */
unsigned snd_watch_keyons(void);
unsigned snd_watch_window(void);
unsigned snd_watch_stray(void);
unsigned snd_watch_lost(void);

/* The first address of each kind, and 0 if there was none. A count says
 * something is wrong; the address says what object to go and move. */
uint32_t snd_watch_first_window(void);
uint32_t snd_watch_first_bad(void);

int snd_watch_verdict(void);

/* Write the report to `path`, creating its directory if that is what is
 * missing. Returns 0 on success. Separate from the step so the host test can
 * read one back. */
int snd_watch_write(const char *path);

/* Where snd_watch_step() writes. SND_WATCH_PATH until something says
 * otherwise, which on this console nothing does; the host test points it at a
 * file it can read. */
void snd_watch_set_path(const char *path);

/* Forget everything. For the self-test, and for a run that wants a fresh
 * measurement. */
void snd_watch_reset(void);

/*
 * The rule, checked over a planted log: a window address passes, an address
 * that translates outside the window fails even though the SPU could read it,
 * an address that translates to nothing fails, and a real host pointer put
 * through SOUNDxSAD's own mask fails whichever of those two it lands as.
 * Returns failures, fills *ran.
 */
int snd_watch_selftest(int *ran);

#endif /* POKEPLATINUM_3DS_SND_WATCH_H */
