/*
 * 3ds/src/3ds_replay.h: scripted input, so a run on this console is the same
 * run twice.
 *
 * The emulator harness cannot substitute. 3ds/tests/azahar_shot.sh drives
 * buttons with xdotool on a wall clock, and the emulated console runs at
 * whatever rate the host manages, measured between five and sixty frames a
 * second over one boot. So "press A twelve seconds in" lands on a different
 * guest frame every time, and a scene deeper than the title screen cannot be
 * reached that way at all.
 *
 * The PC port already solved this: pc/src/pc_input.c reads a frame-addressed,
 * level-held script from PC_INPUT, and pc/replays/ holds the assets, including
 * a recorded session that goes from a cold boot to a save file. That file is
 * frame-addressed, not time-addressed, so it means the same thing on a console
 * running at five frames a second as on a desktop running at two hundred. This
 * file is the reader for it.
 *
 * Same format and the same files, deliberately: the scripts under pc/replays
 * are copied to the SD card unchanged and nothing here parses a dialect.
 *
 *   # comment / blank lines ignored; frames must not decrease
 *   392 keys START            hold START from frame 392
 *   398 keys none             release everything at frame 398
 *   709 keys A B              chords are one line
 *   520 touch 128 96          pen down at (128,96) on the lower screen
 *   540 release               pen up
 *
 * The script comes from sdmc:/3ds/pokeplatinum/input.txt, read once at input
 * init. No file means the console's own buttons drive the game, which is the
 * normal case and costs one failed fopen.
 *
 * The script wins while it is loaded. Merging the player's buttons in would
 * make a measured run depend on whether anything was resting on the console.
 *
 * No libctru and no port state, so 3ds/tests/replay.c runs the parser and the
 * frame walk on a build machine.
 */

#ifndef POKEPLATINUM_3DS_REPLAY_H
#define POKEPLATINUM_3DS_REPLAY_H

#include <stdint.h>

/* Where the console looks for one. */
#define REPLAY_PATH "sdmc:/3ds/pokeplatinum/input.txt"

/*
 * The longest script this will hold, in events rather than frames, one
 * event is one line, and a line is a *change*, so the recorded new-game
 * session is 27,168 frames in 1,391 of them. Static rather than grown: the
 * budget is 96 KB of a 64 MB console and a heap failure at input init would
 * be the least debuggable place on the boot to have one.
 */
#define REPLAY_MAX 8192

/* What the keypad and the pen are holding at a given frame. `keys` is the
 * PAD_Read-positive mask pc/src/pc_input.c publishes, not the active-low
 * register value. */
typedef struct ReplayState {
    uint16_t keys;
    uint16_t x, y;
    int touching;
} ReplayState;

/* One parsed line. */
typedef struct ReplayEvent {
    uint32_t frame;
    uint16_t keys;
    uint16_t x, y;
    uint8_t kind;
} ReplayEvent;

enum {
    REPLAY_KEYS = 0,
    REPLAY_TOUCH = 1,
    REPLAY_RELEASE = 2
};

/*
 * Parse one line into `out`. 1 if it produced an event, 0 if the line was
 * blank or a comment, -1 if it could not be read.
 */
int replay_parse_line(const char *line, ReplayEvent *out);

/* The PAD_Read bit a button name stands for, or 0 if it is not one. "none"
 * is a name here and answers 0 like an unknown word, because a line that
 * lists it adds nothing. */
uint16_t replay_button_mask(const char *name);

/*
 * Read `path`. Returns the number of events loaded, or 0 for "no script";
 * which is also what a missing file gives, so a console with no card set up
 * simply plays normally. A file that exists and will not parse loads nothing
 * and says so on stderr rather than replaying half a session.
 */
int replay_init(const char *path);

/* Non-zero once a script is loaded. */
int replay_active(void);

/* Events loaded, and how many have been consumed. */
int replay_events(void);
int replay_position(void);

/*
 * Advance to `frame` and report what is held there. 0 when no script is
 * loaded, in which case `out` is untouched and the caller should read the
 * console's own buttons.
 */
int replay_frame(uint64_t frame, ReplayState *out);

/* Forget everything. For the tests; the console loads once. */
void replay_reset(void);

/* Host self-test. Returns failures; *ranOut gets the number of checks. */
int replay_selftest(int *ranOut);

#endif /* POKEPLATINUM_3DS_REPLAY_H */
