/*
 * 3ds/src/3ds_perf.h: what a frame costs on this console, and what was in it.
 *
 * A report and not a print, because there is no stdout here that reaches the
 * build machine and the letterbox belongs to the game once it is running. The
 * sound check already solved that by leaving a text file on the SD card; this
 * is the second customer of that channel and is written the same way.
 *
 * One line is a window of PERF_WINDOW frames, closed and appended when it
 * fills: the mean and worst frame interval over the window, how many of its
 * frames drew a polygon and how many polygons the busiest one drew, and a
 * digest of the picture at the end of it. Per-frame lines were the first shape
 * tried and are the wrong one: a boot is tens of thousands of frames, the
 * interesting quantity is a scene, and the card write would then be part of
 * what is being measured.
 *
 * The interval is in ARM11 system ticks, converted to milliseconds only when
 * the line is printed. svcGetSystemTick counts at the CPU's own rate whatever
 * the emulator is doing with wall clock, so the numbers mean the same thing
 * here and on hardware.
 *
 * Deliberately not counted: writing the report and hashing the picture both
 * happen inside a frame and are this file's own cost. Each is timed and
 * subtracted from the interval it landed in, or one frame in every window
 * reads as the worst one.
 *
 * The digest is the PC port's, over the same fixed 256x192 buffers with the
 * same FNV-1a, so a digest here and one in a PC_DUMP_FRAMES manifest are
 * comparable by string compare.
 */

#ifndef POKEPLATINUM_3DS_PERF_H
#define POKEPLATINUM_3DS_PERF_H

#include <stdint.h>

/* Where the console leaves it, beside the sound report. */
#define PERF_PATH "sdmc:/3ds/pokeplatinum/perf-report.txt"

/* Frames per reported window. One second of a console keeping up. */
#define PERF_WINDOW 60u

/*
 * Windows kept, which at 60 frames each is over two hours of a console
 * running at full rate and considerably more of an emulator that is not.
 * When it fills, the last window keeps accumulating and the report says how
 * many were dropped rather than silently wrapping.
 */
#define PERF_MAX_WINDOWS 2048

/*
 * Windows between rewrites of the report. Every one, because the file is what
 * somebody watching a live run is reading: at ten it went two minutes at a
 * time without moving on this emulator, which reads exactly like a console
 * that has stopped. The whole file is rewritten rather than appended so the
 * summary can be at the top, and the write is timed and taken back out of the
 * frame it lands in; what it still costs is real console time, about one
 * card write per second at full rate, and this is the constant to raise if
 * that ever shows up in a hardware number.
 */
#define PERF_WRITE_EVERY 1

/* ARM11 system ticks in one millisecond, times 1000, SYSCLOCK_ARM11 is
 * 268,111,856 Hz, so a millisecond is 268,111.856 ticks and the thousandths
 * are kept rather than rounded away over a 60-frame sum. */
#define PERF_TICKS_PER_MS_X1000 268111856ull

/*
 * Where the frame went. A total frame time says a port is slow and nothing
 * about what to do next, and on this console the plausible answers are far
 * apart: the game's own code, the software renderer that composes the two DS
 * screens, the software rasterizer that draws its polygons, the blit that
 * moves both onto two LCDs of a different shape, or simply waiting for a
 * refresh that arrived long ago. Each is timed where it happens and the game's
 * share is what is left over, so the five always add up to the interval rather
 * than to an unexplained fraction of it.
 *
 * THE 3D span was added last and it matters most. Without it the remainder is
 * labelled "game" and moves with the scene, 4.5 ms on a screen with no
 * polygons, 83 ms on one with 450; which makes the single most important
 * number in the frame budget, what the game's own code costs, unreadable
 * exactly on the frames that decide it.
 */
enum {
    PERF_BLIT = 0,   /* the two surfaces onto the console's framebuffers  */
    PERF_FLUSH,      /* cache flush and buffer swap                       */
    PERF_WAIT,       /* gspWaitForVBlank: idle, and 0 when behind       */
    PERF_GPU2D,      /* the DS's two 2D engines, in software              */
    PERF_GPU3D,      /* the DS's 3D engine: sort, latch and rasterize     */
    PERF_PHASES
};

/* Add `ticks` to a phase of the frame being measured. */
void perf_phase(int phase, uint64_t ticks);

/*
 * One game frame. Called from the frame boundary next to the sound check, so
 * the interval it measures is present to present.
 */
void perf_frame(void);

/*
 * Ticks to leave out of the interval this frame lands in, for work that
 * belongs to an instrument rather than to the game. perf_frame() adds its own
 * report and digest cost through this.
 */
void perf_exclude(uint64_t ticks);

/*
 * The run ended on purpose rather than being killed, which the report says as
 * `closed 1`. It matters to whoever reads the numbers: the report is rewritten
 * at every window boundary, so one from a killed run is missing up to a
 * window's frames and its last line may be a scene that never finished.
 */
void perf_mark_closed(void);

/* Write the report now, whatever the schedule says. Returns 0, or -1 if the
 * card would not take it. */
int perf_write(const char *path);

/* Windows closed so far, and frames seen. */
int perf_windows(void);
unsigned long perf_frames(void);

/* Pure arithmetic, so a build machine can check it: ticks to hundredths of a
 * millisecond. */
unsigned long perf_ticks_to_cms(uint64_t ticks);

/* Host self-test. Returns failures; *ranOut gets the number of checks. */
int perf_selftest(int *ranOut);

#endif /* POKEPLATINUM_3DS_PERF_H */
