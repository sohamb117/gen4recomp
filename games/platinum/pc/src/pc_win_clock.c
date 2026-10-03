/*
 * clock_gettime() and nanosleep() for the Windows build.
 *
 * Why the port carries its own. Mingw-w64 has both in libwinpthread, and its
 * clock_gettime is written on GetTickCount64, which arrived with Vista. An exe
 * that imports it does not start on Windows XP at all: the loader fails before
 * main(). The port has no other use for winpthread, so the two functions come
 * from here and the Windows build is linked with the win32 thread model, which
 * does not pull that library in.
 *
 * QueryPerformanceCounter rather than GetTickCount: the pacer is a 60 Hz
 * governor and GetTickCount's step is 10-16 ms, which is most of a frame. QPC
 * exists on every Windows since 2000. The frequency is read once, and the
 * epoch is process start, which is all CLOCK_MONOTONIC promises.
 *
 * Whether defining these clashes depends on the mingw vintage. It does not for
 * an archive, where a member is only pulled for a symbol nothing else defines,
 * but a newer mingw-w64's <time.h> pulls in pthread_time.h with both as
 * header-inline definitions. On that vintage this file did not compile at all,
 * the build recorded a skip marker instead of an object, and every exe paced
 * on the 15.6 ms tick. So the two overrides are guarded on the header's own
 * macros below: where the headers bring their own, ours stand down.
 */

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <time.h>

#if !defined(WINPTHREAD_CLOCK_DECL)
int clock_gettime(clockid_t clk, struct timespec *ts)
{
    static LONGLONG freq;
    LARGE_INTEGER now;

    (void)clk;
    if (ts == NULL) {
        return -1;
    }
    if (freq == 0) {
        LARGE_INTEGER f;

        if (!QueryPerformanceFrequency(&f) || f.QuadPart == 0) {
            /* No counter at all: fall back to the tick, wrap and all. A
             * paced run then steps in whole ticks, which is visibly worse
             * and still a running game. */
            DWORD ms = GetTickCount();

            ts->tv_sec = (time_t)(ms / 1000u);
            ts->tv_nsec = (long)((ms % 1000u) * 1000000u);
            return 0;
        }
        freq = f.QuadPart;
    }

    QueryPerformanceCounter(&now);
    ts->tv_sec = (time_t)(now.QuadPart / freq);
    /* The remainder before the multiply, so the product cannot overflow. */
    ts->tv_nsec = (long)(((now.QuadPart % freq) * 1000000000LL) / freq);
    return 0;
}
#endif /* !WINPTHREAD_CLOCK_DECL */

/*
 * Why this is not sleep(), measured on this machine. Sleep's resolution is the
 * system timer tick, and since Windows 10 2004 raising that is per-process,
 * so a process that has not raised it gets 15.6 ms whatever it asks for:
 * Sleep(13) measured 15.84 ms mean and 27.34 ms worst, Sleep(2) measured
 * 15.57 ms. The 60 Hz governor asks for about 13 ms once a frame's work is
 * done, so every frame landed ~3 ms late, the pacer's own deadline test went
 * negative and the high-resolution path dropped a picture to catch up. On
 * screen that is a brief skip every second or so.
 *
 * It was invisible until the renderers got fast. While a frame cost 14 ms the
 * governor only asked for 2 and the overshoot was most of what a frame took
 * anyway; an experiment in August compared total elapsed time over 1800
 * frames, which cannot see per-frame jitter, and concluded the tick did not
 * matter.
 *
 * A waitable timer takes its own resolution instead. The high-resolution flag
 * is Windows 10 1803 and later, and it is reached through GetProcAddress
 * rather than an import because this exe has to load on Windows XP, where
 * neither the flag nor the Ex form exists. There the fallback is
 * timeBeginPeriod(1), which is system-wide on those versions and takes Sleep
 * and an ordinary timer to about a millisecond.
 *
 * Both are loaded by hand and neither is linked. Naming winmm on the link
 * line adds a DLL the loader initialises before main(), and this port needs
 * the low 64 MB of its address space free when it gets there; it maps the
 * DS's memory at the DS's own addresses. Adding that one import was enough to
 * put something at 0x02000000 and the port died on its own main RAM before
 * drawing a frame. Nothing here may grow the load-time DLL set.
 */
#define PC_TIMER_HIGH_RESOLUTION 0x00000002u
#define PC_TIMER_ALL_ACCESS      0x1F0003u

typedef HANDLE (WINAPI *pc_create_timer_ex)(LPSECURITY_ATTRIBUTES, LPCWSTR,
                                            DWORD, DWORD);
typedef unsigned (WINAPI *pc_time_period)(unsigned);

/* One handle, created once. The governor is the only caller and it runs on
 * the guest's own thread; a second caller would want its own. */
static HANDLE sleep_timer(void)
{
    static HANDLE timer;
    static int tried;

    if (!tried) {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        pc_create_timer_ex ex = NULL;

        tried = 1;
        if (k32 != NULL) {
            ex = (pc_create_timer_ex)(void (*)(void))
                 GetProcAddress(k32, "CreateWaitableTimerExW");
        }
        if (ex != NULL) {
            timer = ex(NULL, NULL, PC_TIMER_HIGH_RESOLUTION,
                       PC_TIMER_ALL_ACCESS);
        }
        if (timer == NULL) {
            /* No high-resolution timer: raise the tick instead, and take an
             * ordinary timer, which then follows it. winmm is loaded here
             * rather than linked; see the note above. */
            HMODULE mm = LoadLibraryA("winmm.dll");

            if (mm != NULL) {
                pc_time_period beg = (pc_time_period)(void (*)(void))
                                     GetProcAddress(mm, "timeBeginPeriod");

                if (beg != NULL) beg(1);
            }
            timer = CreateWaitableTimerA(NULL, FALSE, NULL);
        }
    }
    return timer;
}

/*
 * The named entry, and the only one the pacer may use. The nanosleep
 * override below is a symbol-interposition bet, and on 2026-08-27 it LOST:
 * The VPS's newer mingw-w64 ships a winpthreads whose own strong nanosleep
 * won the link, the shipped exe's pacer slept on the 15.6 ms tick, and the
 * live build skipped a third of its pictures with `slip 23 ms` written in
 * every trace row while a locally built exe of the SAME source was clean,
 * two toolchain vintages, two link winners, one binary honest and one not.
 * A named function cannot lose a race, so pc_view.c calls this directly and
 * the override stays only as a belt for whatever else sleeps.
 */
void pc_sleep_ns(long long ns64)
{
    LONGLONG ns = (LONGLONG)ns64;
    HANDLE timer;

    if (ns <= 0) {
        return;
    }

    timer = sleep_timer();
    if (timer != NULL) {
        LARGE_INTEGER due;

        /* Negative is relative, in 100 ns units. Never zero: that would arm
         * the timer to fire immediately and spin the governor. */
        due.QuadPart = -(ns / 100LL);
        if (due.QuadPart == 0) due.QuadPart = -1;
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }

    /* Rounded up: Sleep(0) yields the rest of the timeslice rather than
     * pausing, so a sub-millisecond request would busy-spin the pacer. */
    Sleep((DWORD)((ns + 999999LL) / 1000000LL));
}

#if !defined(WINPTHREAD_NANOSLEEP_DECL)
int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (rem != NULL) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    if (req == NULL || req->tv_sec < 0 || req->tv_nsec < 0) {
        return -1;
    }
    pc_sleep_ns((long long)req->tv_sec * 1000000000LL + req->tv_nsec);
    return 0;
}
#endif /* !WINPTHREAD_NANOSLEEP_DECL */

#endif

/* ISO C dislikes an empty translation unit. */
typedef int pc_win_clock_is_windows_only;
