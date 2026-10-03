/*
 * PC_SCRIPT_COV=PATH: which field-script bytecode this run actually executed.
 *
 * The field engine dispatches through a table of 840 slots and the frontier
 * engine through 204 more, and "what fraction of the game's scripting has ever
 * run here" was, before this, nobody's number. PC_TRACE_SCRIPT could answer it
 * for one run by printing a line per dispatch, but a run that reaches a battle
 * prints hundreds of thousands of them and two runs cannot be added together.
 *
 * So this counts instead of printing. Every dispatch bumps a counter, and at
 * exit the non-zero ones are written out as text that pc/tests/pc_scrcov.py
 * sums across runs. The cost when the variable is unset is one compare against
 * a static int, which is why the hook can sit in the dispatch loop of both
 * engines permanently rather than behind a build flag.
 *
 * Why counts and not a bitmap. A bitmap is smaller and answers the ratchet
 * question ("did coverage go down") exactly as well. Counts answer one more
 * question for the same four bytes: a command executed once by a station is
 * evidence the station reached it, and a command executed forty thousand times
 * is a loop the corpus is paying for. Both numbers get read by hand when a
 * station is being authored, so the counter stays.
 *
 * The opcode is recorded RAW, before the dispatch loop's own bounds check, so
 * an out-of-table fetch shows up here as a slot past the end rather than being
 * lost with the run that hit it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Slots, not commands: the tables are 840 and 204 today and this file must not
 * be the thing that has to change when pret adds a command. 1024 covers both
 * with room, and anything past it is counted in the overflow row so it is
 * still visible rather than silently dropped.
 */
#define COV_SLOTS 1024

enum { COV_FIELD, COV_FRONTIER, COV_ENGINES };

static const char *const COV_ENGINE_NAME[COV_ENGINES] = { "field", "frontier" };

static unsigned long sCount[COV_ENGINES][COV_SLOTS];
static unsigned long sOverflow[COV_ENGINES];
static const char *sPath;
static int sParsed;
static int sRegistered;

static void pc_scrcov_write(void)
{
    FILE *f;
    int engine, op;

    if (sPath == NULL) return;

    /*
     * Append, not truncate. A sweep and a corpus run both boot the port many
     * times, and pointing every one of them at the same file is the natural
     * way to ask "what did the whole batch cover", the merge tool sums
     * repeated rows, so appending is correct and truncating would keep only
     * whichever process happened to exit last.
     */
    f = fopen(sPath, "a");
    if (f == NULL) {
        fprintf(stderr, "pc-scrcov: cannot write %s\n", sPath);
        return;
    }

    fprintf(f, "# pc-script-coverage 1\n");
    for (engine = 0; engine < COV_ENGINES; engine++) {
        for (op = 0; op < COV_SLOTS; op++) {
            if (sCount[engine][op] == 0) continue;
            fprintf(f, "%s %d %lu\n", COV_ENGINE_NAME[engine], op,
                    sCount[engine][op]);
        }
        if (sOverflow[engine] != 0) {
            fprintf(f, "%s-overflow %d %lu\n", COV_ENGINE_NAME[engine],
                    COV_SLOTS, sOverflow[engine]);
        }
    }
    fclose(f);
}

/*
 * Called once from pc_main.c, and lazily from the first mark if something got
 * there first. Doing it eagerly is what makes a zero-coverage run write an
 * (empty) file instead of no file, and that distinction is the whole reason
 * this is not left lazy: a run that executed no script and a run that died
 * before it could write are different findings, and without the eager init
 * they are the same empty directory.
 */
void pc_scrcov_init(void)
{
    const char *s;

    if (sParsed) return;
    s = getenv("PC_SCRIPT_COV");

    sParsed = 1;
    if (s == NULL || s[0] == '\0') return;
    sPath = s;

    if (!sRegistered) {
        sRegistered = 1;
        /*
         * atexit rather than a call from the shutdown path, because most runs
         * here do not have one: the lab exits from inside a frame callback,
         * the sweep exits when its list runs out, and a trap exits from
         * wherever it fired. atexit covers all three and _exit covers none of
         * them, which is a limitation worth knowing rather than working
         * around; a run killed by a signal contributes nothing, and the
         * merge tool cannot tell that from a run that covered nothing.
         */
        atexit(pc_scrcov_write);
    }
}

void pc_scrcov_mark(int engine, unsigned op)
{
    if (!sParsed) pc_scrcov_init();
    if (sPath == NULL) return;
    if (engine < 0 || engine >= COV_ENGINES) return;

    if (op >= COV_SLOTS) {
        sOverflow[engine]++;
        return;
    }
    sCount[engine][op]++;
}

/*
 * PC_SCRIPT_STALL=1: names the pause callback a field script is parked on, so
 * a cutscene that hangs says what it is waiting for instead of just sitting
 * there. Gated like the coverage counter above, and for the same reason it
 * gives: this runs on every frame a script is parked, which is most of every
 * dialog box in the game, so unset it must cost one compare against a static
 * int. It was ungated, and a player's whole session came back as this.
 *
 * The count is deliberately NOT reset when the callback changes. A parked
 * script alternates between pause callbacks as a matter of course, a message
 * box waits on one, then on the other, frame about, and resetting on that
 * put `n` back to zero on every call, so `n % 600` was true every time and the
 * throttle printed and flushed a line PER FRAME: the opposite of what it was
 * written to do, and invisible until a stall lasted long enough to read. n is
 * parked frames so far, and a hang still reads as the same callback coming
 * round again and again.
 */
void pc_script_stall(const void *fn)
{
    static int on = -1;
    static int n;

    if (on < 0) {
        const char *s = getenv("PC_SCRIPT_STALL");
        on = s != NULL && s[0] != '\0' && s[0] != '0';
    }
    if (!on)
        return;
    if ((n % 600) == 0) {
        fprintf(stderr, "STALL waiting on %p (n=%d)\n", fn, n);
        fflush(stderr);
    }
    n++;
}
