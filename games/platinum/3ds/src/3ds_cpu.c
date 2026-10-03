/*
 * 3ds/src/3ds_cpu.c: one virtual CPU.
 *
 * The DS has two processors and the game's ARM9 runs several threads on one of
 * them. This port has one host thread and it runs all of them.
 * `OS_CreateThread` is not `threadCreate`: it is the SDK's own scheduler,
 * compiled from the same C the cartridge shipped, keeping its queues in plain
 * memory, and a context switch is that scheduler saving registers into an
 * OSContext and restoring another one. Nothing about that needs a second host
 * thread, and giving it one would be strictly worse: the SDK's critical
 * sections are OS_DisableInterrupts, which on this port is a variable, so real
 * concurrency would race everything the DS was allowed to assume was atomic.
 * The ARM7 is not a thread either; it is the responders behind the PXI tags.
 *
 * What this is not saying. libctru runs threads of its own and always has:
 * apt has a service thread, gsp has an event thread, and NDSP has an audio
 * thread. Those are the console's, not the game's, and none of them runs a
 * line of guest C. The model is "no guest code off the main thread", not "one
 * thread in the process".
 *
 * A record is kept of any SDK file that starts a real host thread. There are
 * none: measured over every object in the link, not one references
 * threadCreate, svcCreateThread, pthread_create or thrd_create. The only file
 * in this tree that ever created a host thread is pc/src/pc_win_fiber.c, which
 * is another operating system's file and is not linked here.
 * 3ds/tests/one_cpu.py is that record, kept as a check rather than a sentence,
 * because the interesting case is the one that arrives later.
 *
 * And the trap, which is the half that will matter. A grep over today's
 * objects cannot catch the way this model is actually likely to break, which
 * is a libctru callback: ndspSetCallback runs its function on NDSP's own
 * thread, and a port that answered it by calling into the SDK's sound code
 * would be running guest C on a second CPU with no scheduler and no interrupt
 * flag. So the main thread's id is recorded at start-up and cpu_assert_main()
 * fails loudly anywhere the answer has to be yes.
 *
 * svcGetThreadId rather than threadGetCurrent: libctru's Thread object exists
 * only for threads libctru itself created, and the main thread is not one of
 * them, so threadGetCurrent() answers NULL there and NULL is also what it
 * answers on a thread it does not know.
 */

#ifdef __3DS__
#include <3ds.h>
#endif

#include <stdio.h>

#include "3ds_cpu.h"
#include "3ds_fault.h"

#ifdef __3DS__

static u32 sMainThreadId;
static int sBound;

static u32 this_thread_id(void)
{
    u32 id = 0;

    svcGetThreadId(&id, CUR_THREAD_HANDLE);
    return id;
}

void cpu_bind_main(void)
{
    sMainThreadId = this_thread_id();
    sBound = 1;
}

int cpu_is_main(void)
{
    return sBound && this_thread_id() == sMainThreadId;
}

void cpu_assert_main(const char *what)
{
    char line[64];

    if (cpu_is_main()) {
        return;
    }
    /*
     * Not a return and not a log. Guest code on a second thread has already
     * touched scheduler state that the SDK believes only one CPU can reach,
     * so whatever it corrupted is corrupt by the time anyone could read a
     * warning, and the report names the thread, which is the one fact that
     * says which callback did it.
     */
    snprintf(line, sizeof line, "THREAD %08lX, MAIN IS %08lX",
             (unsigned long)this_thread_id(), (unsigned long)sMainThreadId);
    fault_stop(what != NULL ? what : "GUEST CODE OFF THE MAIN THREAD", line);
}

int cpu_selftest(int *ran)
{
    int n = 0;
    int bad = 0;

    /* Bound, and this is it. The crt calls cpu_bind_main() before anything
     * else; if that ever moved after something that hands work to libctru,
     * this is what would say so. */
    n++;
    if (!sBound) {
        bad++;
    }
    n++;
    if (!cpu_is_main()) {
        bad++;
    }
    /* A thread id of zero would make the compare above pass for every thread
     * in the process, which is the one way this check could be worthless. */
    n++;
    if (sMainThreadId == 0) {
        bad++;
    }

    if (ran != NULL) {
        *ran = n;
    }
    return bad;
}

#else /* !__3DS__: a build machine, where there is no thread id to take */

void cpu_bind_main(void) {}

int cpu_is_main(void)
{
    return 1;
}

void cpu_assert_main(const char *what)
{
    (void)what;
}

int cpu_selftest(int *ran)
{
    if (ran != NULL) {
        *ran = 0;
    }
    return 0;
}

#endif
