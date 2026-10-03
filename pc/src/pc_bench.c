/*
 * Where a frame's time goes, per subsystem, measured rather than guessed.
 *
 * `--bench` accumulates wall time inside four named spans and prints the
 * total, the share and the implied frames per second at exit. It exists
 * because the alternative is arguing about which part of the frame is
 * expensive, and because "the rasterizer is the cost" is the sort of claim
 * that ages badly; this port's plan carried a 40 fps overworld figure that
 * turned out to be seven times off.
 *
 * What it costs. Two clock_gettime calls per span per frame, eight a frame:
 * against a frame that is milliseconds. Measured at 3,000 frames: within
 * the run-to-run noise of the same script. It is off unless asked for, and
 * the spans compile to nothing when it is off but a branch on a global.
 *
 * What it does not measure. Anything outside the four spans lands in
 * "elsewhere", which is the guest's own code, the game logic, the SDK, the
 * decompressors, and that is the useful residual rather than a gap: if
 * elsewhere dominates, the rasterizer is not the thing to optimize.
 *
 * CLOCK_MONOTONIC and not CLOCK_PROCESS_CPUTIME_ID, deliberately: a paced run
 * sleeps inside no span, and what a player feels is wall time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "pc_bench.h"

int pc_bench_on;

static const char *const kNames[PC_BENCH_N] = {
    "3D rasterize", "2D compose", "publish", "audio mix", "card read"
};

static unsigned long long span_ns[PC_BENCH_N];
static unsigned long long span_hits[PC_BENCH_N];
static unsigned long long run_start_ns;
static unsigned long long frames;

/*
 * PC_BENCH_FROM=N: throw away everything before the Nth frame and start the
 * clock there. A run's early frames are the boot and the intro, which draw
 * almost nothing; averaging them in makes an optimisation on the overworld
 * look smaller than it is, and the alternative; two runs and a subtraction:
 * puts the difference of two large numbers where a measurement should be.
 */
static unsigned long long bench_from;
static unsigned long long seen;

static unsigned long long now_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull
           + (unsigned long long)t.tv_nsec;
}

unsigned long long pc_bench_enter(void)
{
    return now_ns();
}

void pc_bench_leave(int span, unsigned long long t0)
{
    if (span < 0 || span >= PC_BENCH_N) {
        return;
    }
    span_ns[span] += now_ns() - t0;
    span_hits[span]++;
}

void pc_bench_add(int span, unsigned long long ns)
{
    if (span < 0 || span >= PC_BENCH_N) {
        return;
    }
    span_ns[span] += ns;
    span_hits[span]++;
}

unsigned long long pc_bench_span_ns(int span)
{
    if (span < 0 || span >= PC_BENCH_N) {
        return 0ull;
    }
    return span_ns[span];
}

void pc_bench_frame(void)
{
    if (++seen <= bench_from) {
        int i;

        for (i = 0; i < PC_BENCH_N; i++) {
            span_ns[i] = 0;
            span_hits[i] = 0;
        }
        frames = 0;
        run_start_ns = now_ns();
        return;
    }
    frames++;
}

static void pc_bench_report(void)
{
    unsigned long long total = now_ns() - run_start_ns, accounted = 0;
    int i;

    if (frames == 0) {
        return;
    }
    if (bench_from != 0) {
        fprintf(stderr, "\npc-bench: from frame %llu\n", bench_from);
    }
    fprintf(stderr, "\npc-bench: %llu frames in %.2f s, %.1f fps unpaced\n",
            frames, total / 1e9, frames / (total / 1e9));
    fprintf(stderr, "pc-bench: %-14s %10s %8s %7s %12s %9s\n",
            "span", "ms total", "us/frame", "share", "alone it is",
            "calls/fr");
    for (i = 0; i < PC_BENCH_N; i++) {
        accounted += span_ns[i];
        /* calls/frame, because a span's mean hides how it is spent: the
         * card read is a handful of milliseconds either way, but one read a
         * frame and two hundred want different answers. */
        fprintf(stderr, "pc-bench: %-14s %10.1f %8.1f %6.1f%% %9.0f fps %9.1f\n",
                kNames[i], span_ns[i] / 1e6,
                span_ns[i] / 1e3 / (double)frames,
                100.0 * span_ns[i] / (double)total,
                span_ns[i] > 0 ? frames / (span_ns[i] / 1e9) : 0.0,
                span_hits[i] / (double)frames);
    }
    fprintf(stderr, "pc-bench: %-14s %10.1f %8.1f %6.1f%%\n",
            "elsewhere", (total - accounted) / 1e6,
            (total - accounted) / 1e3 / (double)frames,
            100.0 * (total - accounted) / (double)total);
    fflush(stderr);
}

void pc_bench_init(void)
{
    if (getenv("PC_BENCH") == NULL) {
        return;
    }
    pc_bench_on = 1;
    {
        const char *from = getenv("PC_BENCH_FROM");
        bench_from = from != NULL ? strtoull(from, NULL, 0) : 0;
    }
    run_start_ns = now_ns();
    atexit(pc_bench_report);
}
