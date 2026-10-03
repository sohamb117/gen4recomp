/*
 * The worker pool, pc/include/pc_workers.h says what it is for.
 *
 * Why it is hand-rolled and not pthreads EVERYWHERE. The Windows build has to
 * start on Windows XP, and mingw's libwinpthread reaches for GetTickCount64
 * and the Vista condition-variable calls; an exe importing those does not
 * load there at all. Win32 events and a critical section are the same program
 * in about the same number of lines and every call in it is Windows 2000 or
 * older. POSIX hosts use pthreads directly for the same reason; it is what
 * is already there.
 *
 * What the threads may touch. A worker runs one function on one slice index
 * and nothing else; there is no queue to race on and no allocation on the
 * path. The renderers hand each slice a disjoint band of scanlines, so the
 * only shared state is read-only for the length of the job. The guest never
 * runs while a job is in flight: both callers are entered from the frame
 * boundary, on the guest fiber, which is stopped inside the call.
 *
 * Why it is a barrier and not a background render. Guest time in this port
 * only moves when the guest yields, so a frame drawn "in the background" has
 * nothing to overlap with; it would just be the same work on another
 * thread, plus a synchronisation problem. Splitting one frame is the whole
 * gain: it is the part that scales with the machine.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pc_workers.h"

#if defined(__3DS__) || defined(__wasm__)

/*
 * The console runs this serially. Its two cores are not symmetric, the second
 * is reserved by the OS unless asked for, and nothing in this port may
 * reference threadCreate; see the 3DS notes. wasm32-wasip1 has no threads at
 * all (its pthread_create fails), so it is the same single slice.
 */
int pc_workers_slices(void) { return 1; }

void pc_workers_run(void (*fn)(void *ctx, int slice, int nslices), void *ctx)
{
    fn(ctx, 0, 1);
}

void pc_workers_run_n(void (*fn)(void *ctx, int slice, int nslices), void *ctx,
                      int want)
{
    (void)want;
    fn(ctx, 0, 1);
}

void pc_workers_barrier(int nslices) { (void)nslices; }

void pc_workers_park(void) { }

#else

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

/*
 * Spinning before sleeping, and why it is the difference between this being
 * worth doing and not. Waking the workers off a condition variable and
 * waiting for the last one to answer costs a few hundred microseconds of
 * scheduler round trips; which is nothing against a 3D frame and is most of
 * a 2D one, where the whole job is about three hundred microseconds. Measured
 * on the walked overworld: the 2D compose is 661 us a frame with the spin and
 * 1011 without. A worker that spins is already running when its slice
 * arrives.
 *
 * Then it yields, and that half is not optional. A pure spin is only free on
 * a machine with a core to spare. Eight of these processes at once, which
 * is what this port's own test suite does, turns fifteen spinning workers
 * apiece into a machine that makes no progress: the run that should take two
 * seconds took sixty-two, and the suite failed two tests that pass alone. So
 * the budget is short, and past it the thread offers the core to whoever else
 * wants it before finally blocking. On an idle machine the pause phase almost
 * always wins and the yields never run.
 */
#define SPIN_PAUSES 2000
#define SPIN_YIELDS 30

#if defined(__i386__) || defined(__x86_64__)
#define cpu_relax() __builtin_ia32_pause()
#elif defined(__arm__)
/* ARM's spelling of the same hint, and the reason the spin budget above is
 * worth anything on a handheld: `yield` is a NOP on a core with nothing to
 * yield to and a real hint on one that is sharing. Without it the pause phase
 * is a bare spin, which is what the comment above measured going badly. */
#define cpu_relax() __asm__ __volatile__("yield" ::: "memory")
#else
#define cpu_relax() ((void)0)
#endif

#if defined(_WIN32)
#define cpu_yield() SwitchToThread()
#else
#include <sched.h>
#define cpu_yield() sched_yield()
#endif

/*
 * Spin for a bounded time on a word changing. Returns 1 if it did; 0 means
 * the caller should block. `stop` is checked through an acquire load so the
 * value the change published is visible when it returns.
 */
#define SPIN_UNTIL(cond)                                       \
    do {                                                       \
        int spin_i;                                            \
        for (spin_i = 0; spin_i < SPIN_PAUSES; spin_i++) {     \
            if (cond) break;                                   \
            cpu_relax();                                       \
        }                                                      \
        if (!(cond)) {                                         \
            for (spin_i = 0; spin_i < SPIN_YIELDS; spin_i++) { \
                if (cond) break;                               \
                cpu_yield();                                   \
            }                                                  \
        }                                                      \
    } while (0)

static int nthreads = -1;           /* -1 until the first call decides */
static int started;

/* The job in flight. Written before the workers are released and read-only
 * until every one of them is done. */
static void (*job_fn)(void *ctx, int slice, int nslices);
static void *job_ctx;
static int job_slices;

static unsigned generation;         /* bumped once per job */
/*
 * "The next job is not imminent, sleep rather than spin." Set by the caller
 * when it has finished the last job of a frame. Without it every worker spins
 * its whole budget after every job, which on a paced run is the twelve
 * milliseconds between frames and on a machine running several of these at
 * once is the difference between a game and a stall.
 */
static int park;
static int outstanding;             /* slices still running */
static int barrier_at;              /* slices arrived at the barrier */
static unsigned barrier_gen;

static int worker_index[PC_WORKERS_MAX];

#if defined(_WIN32)

static HANDLE go[PC_WORKERS_MAX];   /* "there is a job": the fallback sleep */
static HANDLE done_ev;

static void sleep_for_job(int me, unsigned seen)
{
    (void)seen;
    WaitForSingleObject(go[me], INFINITE);
}
static void wake_workers(int n)
{
    int i;

    /*
     * The completion event is reset before anyone can set it. It is
     * auto-reset, and the main thread often finds the job already done while
     * spinning and never waits on it; which leaves it SIGNALLED. Without
     * this the next job's wait returns immediately on that stale signal and
     * the main thread reads a frame the workers are still drawing. It showed
     * as one wrong frame in a thousand on Windows and never on POSIX, whose
     * wait re-tests the count; both ends are fixed rather than the one that
     * was caught.
     */
    ResetEvent(done_ev);
    for (i = 0; i < n - 1; i++) SetEvent(go[i]);
}

static void signal_done(void) { SetEvent(done_ev); }

static void wait_done(void)
{
    while (__atomic_load_n(&outstanding, __ATOMIC_ACQUIRE) > 0)
        WaitForSingleObject(done_ev, INFINITE);
}

static DWORD WINAPI worker_main(LPVOID arg);

static int workers_start(int n)
{
    int i;

    done_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (done_ev == NULL) return 1;

    for (i = 0; i < n - 1; i++) {
        HANDLE h;

        go[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (go[i] == NULL) return i + 1;
        worker_index[i] = i;
        h = CreateThread(NULL, 0, worker_main, &worker_index[i], 0, NULL);
        if (h == NULL) return i + 1;
        CloseHandle(h);
    }
    return n;
}

static int host_cpus(void)
{
    SYSTEM_INFO si;

    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

#else

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  go = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  done = PTHREAD_COND_INITIALIZER;

static void sleep_for_job(int me, unsigned seen)
{
    (void)me;
    pthread_mutex_lock(&lock);
    while (__atomic_load_n(&generation, __ATOMIC_ACQUIRE) == seen)
        pthread_cond_wait(&go, &lock);
    pthread_mutex_unlock(&lock);
}

static void wake_workers(int n)
{
    (void)n;
    pthread_mutex_lock(&lock);
    pthread_cond_broadcast(&go);
    pthread_mutex_unlock(&lock);
}

static void signal_done(void)
{
    pthread_mutex_lock(&lock);
    pthread_cond_signal(&done);
    pthread_mutex_unlock(&lock);
}

static void wait_done(void)
{
    pthread_mutex_lock(&lock);
    while (__atomic_load_n(&outstanding, __ATOMIC_ACQUIRE) > 0)
        pthread_cond_wait(&done, &lock);
    pthread_mutex_unlock(&lock);
}

static void *worker_main(void *arg);

static int workers_start(int n)
{
    int i;

    for (i = 0; i < n - 1; i++) {
        pthread_t th;

        worker_index[i] = i;
        if (pthread_create(&th, NULL, worker_main, &worker_index[i]) != 0)
            return i + 1;
        pthread_detach(th);
    }
    return n;
}

static int host_cpus(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    return n > 0 ? (int)n : 1;
}

#endif

/* The worker body, one for both platforms. `seen` is the last job this thread
 * ran; the generation counter moving is the job arriving. */
static void worker_body(int me)
{
    unsigned seen = 0;

    for (;;) {
        if (!__atomic_load_n(&park, __ATOMIC_ACQUIRE))
            SPIN_UNTIL(__atomic_load_n(&generation, __ATOMIC_ACQUIRE) != seen);
        if (__atomic_load_n(&generation, __ATOMIC_ACQUIRE) == seen)
            sleep_for_job(me, seen);
        if (__atomic_load_n(&generation, __ATOMIC_ACQUIRE) == seen) continue;
        seen = __atomic_load_n(&generation, __ATOMIC_ACQUIRE);

        /* A job may ask for fewer slices than the pool has. The workers past
         * that go back to waiting and do not count towards it. */
        if (me + 1 >= job_slices) continue;

        job_fn(job_ctx, me + 1, job_slices);

        if (__atomic_sub_fetch(&outstanding, 1, __ATOMIC_ACQ_REL) <= 0)
            signal_done();
    }
}

#if defined(_WIN32)
static DWORD WINAPI worker_main(LPVOID arg) { worker_body(*(int *)arg); return 0; }
#else
static void *worker_main(void *arg) { worker_body(*(int *)arg); return NULL; }
#endif

static void workers_init(void)
{
    const char *env = getenv("PC_THREADS");
    int want;

    if (env != NULL && env[0] != '\0') {
        want = atoi(env);
        if (want < 1) want = 1;
    } else {
        /*
         * Not every hardware thread, and not every core either.
         *
         * The ceiling of eight is the desktop measurement: on a 12-core,
         * 24-thread machine over a walked overworld the render is fastest at
         * eight to ten and gets slower above twelve, because the bands are
         * then short enough that the barrier costs more than the band saves.
         *
         * Subtracting two is the handheld measurement. On an 8-core device the
         * old arithmetic asked for 8, which is main plus seven workers on eight
         * cores and nothing left for anyone else. Inside an Android app that is
         * the surface thread, the audio callback and the GPU driver's own
         * threads, and they do not get to wait.
         *
         * Measured on an eight-core handheld, same build, same replay, with
         * PC_THREADS the only difference:
         *
         *   8 (the old default)  52.3-58.0 fps, 108-175 of 300 frames late,
         *                        33,117 audio frames padded, 54 rebuffers
         *   6                    60.0 fps, 0-15 late, 0 padded, 0 rebuffers
         *   4                    60.0 fps, 0-7 late,  0 padded, 0 rebuffers
         *   2                    60.0 fps, 0-7 late,  0 padded, 0 rebuffers
         *
         * Eight is the only broken setting. This is not "fewer threads are
         * faster", it is a pool that starves the machine it runs on.
         *
         * The desktop ceiling does not move: a 24-thread host asks for 22 and
         * is still capped to 8. Only a machine with ten cores or fewer sees any
         * change.
         */
        want = host_cpus() - 2;
        if (want > 8) want = 8;
        if (want < 1) want = 1;
    }
    if (want > PC_WORKERS_MAX) want = PC_WORKERS_MAX;

    nthreads = want > 1 ? workers_start(want) : 1;
    if (nthreads < 1) nthreads = 1;
    started = 1;
}

int pc_workers_slices(void)
{
    if (!started) workers_init();
    return nthreads;
}

void pc_workers_run_n(void (*fn)(void *ctx, int slice, int nslices), void *ctx,
                      int want)
{
    int n = pc_workers_slices();

    if (want > 0 && want < n) n = want;
    if (n <= 1) {
        fn(ctx, 0, 1);
        return;
    }

    job_fn = fn;
    job_ctx = ctx;
    job_slices = n;
    __atomic_store_n(&park, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&outstanding, n - 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&generation, 1, __ATOMIC_ACQ_REL);
    wake_workers(n);

    fn(ctx, 0, n);

    SPIN_UNTIL(__atomic_load_n(&outstanding, __ATOMIC_ACQUIRE) <= 0);
    if (__atomic_load_n(&outstanding, __ATOMIC_ACQUIRE) > 0) wait_done();
}

void pc_workers_run(void (*fn)(void *ctx, int slice, int nslices), void *ctx)
{
    pc_workers_run_n(fn, ctx, 0);
}

void pc_workers_park(void)
{
    __atomic_store_n(&park, 1, __ATOMIC_RELEASE);
}

/*
 * A barrier inside one job, so a two-phase pass costs one dispatch instead of
 * two. Spun rather than blocked: what it waits for is the slowest band of the
 * same job, which is tens of microseconds out of a millisecond, and a thread
 * that blocks here would have to be woken again immediately.
 */
void pc_workers_barrier(int nslices)
{
    unsigned g;

    if (nslices <= 1) return;

    g = __atomic_load_n(&barrier_gen, __ATOMIC_ACQUIRE);
    if (__atomic_add_fetch(&barrier_at, 1, __ATOMIC_ACQ_REL) == nslices) {
        __atomic_store_n(&barrier_at, 0, __ATOMIC_RELEASE);
        __atomic_add_fetch(&barrier_gen, 1, __ATOMIC_ACQ_REL);
        return;
    }
    /* Bounded the same way, and then it keeps yielding: there is nothing to
     * block on here and the wait is for the slowest band of the same job. */
    SPIN_UNTIL(__atomic_load_n(&barrier_gen, __ATOMIC_ACQUIRE) != g);
    while (__atomic_load_n(&barrier_gen, __ATOMIC_ACQUIRE) == g) cpu_yield();
}

#endif  /* !__3DS__ */
