/*
 * A fixed pool of worker threads and one operation: split a job into N equal
 * slices, run them, wait. Nothing else, no queue, no futures, no work
 * stealing. The two callers are the 3D rasterizer and the 2D compositor, both
 * of which draw a screen a scanline at a time into disjoint rows, so a slice
 * is a band of scanlines and the answer does not depend on how many bands
 * there are.
 *
 * PC_THREADS=N sets the pool size; 1 runs every slice on the calling thread
 * and creates nothing. The default is the machine's processor count, capped,
 * because a band per hardware thread is already finer than a screen wants.
 *
 * The output must not depend on the slice count. Every caller partitions
 * writes by row, so two runs with different PC_THREADS produce byte-identical
 * frames; pc/tests/run_tests.py holds that.
 */
#ifndef POKEPLATINUM_PC_WORKERS_H
#define POKEPLATINUM_PC_WORKERS_H

/* The pool's ceiling, so a caller can size a per-slice array without
 * allocating. A band per hardware thread is already finer than a screen
 * wants, and past this the barrier costs more than the band saves. The
 * console draws serially, and there a 1 keeps every per-slice array the size
 * it was before this existed; wasm has no threads and draws serially too. */
#if defined(__3DS__) || defined(__wasm__)
#define PC_WORKERS_MAX 1
#else
#define PC_WORKERS_MAX 16
#endif

/* How many slices to cut a job into. 1 means "run it here". */
int pc_workers_slices(void);

/* Run fn(ctx, i, n) for i in [0, n) and return when all of them have. Slice 0
 * runs on the calling thread, so a pool of one thread costs a plain call. */
void pc_workers_run(void (*fn)(void *ctx, int slice, int nslices), void *ctx);

/* The same, with a ceiling on how many slices to cut. A job whose whole cost
 * is a few hundred microseconds is slower split sixteen ways than four: the
 * dispatch is then most of the band. 0 means "as many as the pool has". */
void pc_workers_run_n(void (*fn)(void *ctx, int slice, int nslices), void *ctx,
                      int want);

/* Tell the pool the next job is not imminent, so its threads block instead of
 * spinning for it. Called when a frame's last job is done: without it the
 * workers spin through the gap between frames, which costs a core apiece and
 * is felt hardest on the machines this port is trying to be fast on. */
void pc_workers_park(void);

/* Every slice of the job in flight must reach this before any leaves it.
 * `nslices` is the count the job was handed. A one-slice job returns at once,
 * so a caller writes the same code either way. */
void pc_workers_barrier(int nslices);

#endif
