/*
 * 3ds/tests/replay.c: scripted input, on the host.
 *
 * replay_selftest() covers the parser and the frame walk over planted lines.
 * What this adds is the two things a planted line cannot say:
 *
 *   * a script is read from a FILE, and a file that is not there is the
 *     ordinary console rather than an error;
 *   * the format is the PC port's, checked against the PC port's own assets
 *     rather than against a fixture written to match. If pc/replays stops
 *     loading here, the two ports have drifted apart and the claim that this
 *     console replays a recorded session is no longer true.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "3ds_replay.h"

static int load_asset(const char *path, int *events)
{
    FILE *f = fopen(path, "r");

    if (f == NULL) {
        return 0;                      /* not in this checkout */
    }
    fclose(f);
    *events = replay_init(path);
    return 1;
}

int main(void)
{
    static const char *const assets[] = {
        "pc/replays/new-game.txt",
        "pc/replays/lab-continue.txt",
        "pc/replays/lab-menu-party.txt",
        "pc/replays/lab-poketch-calc.txt",
    };
    const char *tmp = "build/3ds/replay-script.txt";
    int failed = 0;
    int ran = 0;
    size_t i;
    FILE *f;

    failed += replay_selftest(&ran);
    if (failed != 0) {
        printf("replay: %d of %d checks failed\n", failed, ran);
        return 1;
    }

    /* No file is not a failure: it is what an ordinary boot finds. */
    remove(tmp);
    if (replay_init(tmp) != 0 || replay_active() != 0) {
        printf("replay: a missing script loaded something\n");
        failed++;
    }
    ran += 2;

    /* A real one, through the file reader. */
    f = fopen(tmp, "w");
    if (f == NULL) {
        printf("replay: cannot write %s\n", tmp);
        return 1;
    }
    fputs("# a script\n"
          "0 keys none\n"
          "10 keys START\n"
          "16 keys none\n"
          "40 touch 200 100\n"
          "45 release\n", f);
    fclose(f);

    if (replay_init(tmp) != 5) {
        printf("replay: %s loaded %d events, wanted 5\n", tmp,
               replay_events());
        failed++;
    } else {
        ReplayState st;

        memset(&st, 0, sizeof st);
        replay_frame(12, &st);
        if (st.keys != 0x0008) {
            printf("replay: START is not held at frame 12\n");
            failed++;
        }
        replay_frame(42, &st);
        if (!st.touching || st.x != 200 || st.y != 100) {
            printf("replay: the pen is not down at frame 42\n");
            failed++;
        }
        replay_frame(50, &st);
        if (st.touching) {
            printf("replay: the pen is still down at frame 50\n");
            failed++;
        }
    }
    ran += 4;

    /* Out-of-order frames refuse the whole file rather than a prefix. */
    f = fopen(tmp, "w");
    fputs("10 keys A\n5 keys B\n", f);
    fclose(f);
    if (replay_init(tmp) != 0 || replay_active() != 0) {
        printf("replay: a script that goes backwards was accepted\n");
        failed++;
    }
    ran++;

    /* One bad line does the same. */
    f = fopen(tmp, "w");
    fputs("10 keys A\n20 keys WOBBLE\n", f);
    fclose(f);
    if (replay_init(tmp) != 0) {
        printf("replay: a script with a bad button was accepted\n");
        failed++;
    }
    ran++;
    remove(tmp);

    /* And the PC port's own scripts, which is the format check. */
    for (i = 0; i < sizeof assets / sizeof *assets; i++) {
        int events = 0;

        if (!load_asset(assets[i], &events)) {
            continue;
        }
        ran++;
        if (events <= 0) {
            printf("replay: %s did not load\n", assets[i]);
            failed++;
        }
    }

    replay_reset();
    if (failed == 0) {
        printf("  %-56s ok\n", "scripted input, and the PC port's scripts");
        printf("replay: %d checks\n", ran);
    }
    return failed != 0;
}
