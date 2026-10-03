/*
 * 3ds/tests/perf.c: the frame-time heartbeat, on the host.
 *
 * perf_selftest() covers the arithmetic and the windowing over a made-up
 * clock. What this adds is the report itself: the console's only way of
 * saying what a frame cost is a file on the SD card, so it is written here
 * and read back, and the path is made on the way to it the same way the sound
 * report's is.
 *
 * The suspension bookkeeping is here rather than in a file of its own because
 * it is the same measurement: Home, sleep and close are the one event that
 * puts minutes of host time inside a frame, and what 3ds_apt.c produces is
 * the number handed to perf_exclude() to take it back out again.
 *
 * The hang detector is here for the third half of the same question. All
 * three are about what stops the frames and which stop is legitimate: a slow
 * scene, a suspension, or a port that will never present again.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "3ds_apt.h"
#include "3ds_perf.h"
#include "3ds_watchdog.h"

int main(void)
{
    const char *deep = "build/3ds/perf-dir/perf-report.txt";
    int failed = 0;
    int ran = 0;
    int aptRan = 0;
    int wdRan = 0;
    char line[256];
    int sawHeader = 0;
    FILE *f;

    failed += perf_selftest(&ran);
    failed += apt_selftest(&aptRan);
    failed += watchdog_selftest(&wdRan);
    ran += aptRan + wdRan;
    if (failed != 0) {
        printf("perf: %d of %d checks failed\n", failed, ran);
        return 1;
    }

    /*
     * A directory that is not there, because a console that has only ever run
     * this port out of its own RomFS has never had one made, the same
     * failure the sound report has to survive.
     */
    remove(deep);
    rmdir("build/3ds/perf-dir");
    if (perf_write(deep) != 0) {
        printf("perf: %s was not made on the way to the file\n", deep);
        failed++;
    }
    ran++;

    f = fopen(deep, "r");
    if (f == NULL) {
        printf("perf: no report at %s\n", deep);
        failed++;
    } else {
        while (fgets(line, sizeof line, f) != NULL) {
            if (strncmp(line, "window-frames ", 14) == 0) {
                unsigned n = 0;

                if (sscanf(line, "window-frames %u", &n) == 1
                    && n == PERF_WINDOW) {
                    sawHeader++;
                }
            }
            if (strncmp(line, "frames ", 7) == 0
                || strncmp(line, "windows ", 8) == 0) {
                sawHeader++;
            }
            /* Not yet: nothing has closed this run. The line has to be there
             * and it has to say so, because a reader who sees no line at all
             * cannot tell an old report from a killed run. */
            if (strncmp(line, "closed 0", 8) == 0) {
                sawHeader++;
            }
        }
        fclose(f);
        if (sawHeader != 4) {
            printf("perf: the report is missing its header\n");
            failed++;
        }
        ran += 2;
    }
    remove(deep);
    rmdir("build/3ds/perf-dir");

    /* And the other answer, which is the one the console's teardown writes. */
    perf_mark_closed();
    (void)perf_write(deep);
    f = fopen(deep, "r");
    sawHeader = 0;
    if (f != NULL) {
        while (fgets(line, sizeof line, f) != NULL) {
            if (strncmp(line, "closed 1", 8) == 0) {
                sawHeader++;
            }
        }
        fclose(f);
    }
    if (sawHeader != 1) {
        printf("perf: a closed run does not say so in its report\n");
        failed++;
    }
    ran++;
    remove(deep);
    rmdir("build/3ds/perf-dir");

    if (failed == 0) {
        printf("  %-56s ok\n",
               "frame time, the report, and a suspension left out of it");
        printf("perf: %d checks\n", ran);
    }
    return failed != 0;
}
