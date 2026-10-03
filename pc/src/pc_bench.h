/*
 * The per-subsystem timer. Off unless PC_BENCH is set, and the spans cost a
 * branch on a global when it is not; see pc/src/pc_bench.c for what is
 * measured and what lands in "elsewhere".
 */
#ifndef POKEPLATINUM_PC_BENCH_H
#define POKEPLATINUM_PC_BENCH_H

enum {
    PC_BENCH_3D,
    PC_BENCH_2D,
    PC_BENCH_PUBLISH,
    PC_BENCH_AUDIO,
    PC_BENCH_IO,
    PC_BENCH_N
};

extern int pc_bench_on;

void pc_bench_init(void);
unsigned long long pc_bench_enter(void);
void pc_bench_leave(int span, unsigned long long t0);
/* For a caller that timed the work anyway and has the nanoseconds already;
 * the rasterize is timed whether or not --bench asked, because the pacer
 * decides on it. */
void pc_bench_add(int span, unsigned long long ns);
void pc_bench_frame(void);

/* The run's running total inside one span. The report below wants the mean;
 * a caller that wants ONE frame's cost subtracts two of these across it, and
 * the pacer's late-frame autopsy is the caller that does. */
unsigned long long pc_bench_span_ns(int span);

/* The two the call sites use: a span is an enter/leave pair around the work,
 * and both compile to a predictable branch when the instrument is off. */
#define PC_BENCH_BEGIN(var) \
    unsigned long long var = pc_bench_on ? pc_bench_enter() : 0ull
#define PC_BENCH_END(span, var) \
    do { if (pc_bench_on) pc_bench_leave((span), (var)); } while (0)

#endif
