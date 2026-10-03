/*
 * PC_PROF=path[:hz], a SIGPROF EIP sampler, for machines where perf and
 * ptrace are locked away (this WSL2's perf is built for a kernel it does not
 * run). Ported from pokediamond's pc/src/pc_main.c, where it earned its keep
 * the same way.
 *
 * Each sample is one little-endian u32 EIP appended to `path`;
 * pc/prof_fold.py resolves them against the binary and folds them into the
 * rasterizer's named cost centres, so `sort | uniq -c` over the resolved
 * samples is a profile. CPU-time driven (ITIMER_PROF): a paced run that
 * sleeps between frames charges only the frames, and the kernel delivers the
 * tick to a thread that is actually running, with the worker pool live the
 * bands are sampled where they burn, which is exactly what a span-level
 * profile of the render wants.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_prof.h"

#if !defined(_WIN32)

#include <stdint.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <ucontext.h>

static int pc_prof_fd = -1;

/*
 * Where the program counter lives in a signal context, which is a question
 * with one answer per architecture and no portable spelling at all.
 *
 * gregs[14] is EIP on i386, glibc's REG_EIP, spelled numerically because
 * that name needs _GNU_SOURCE before the first glibc header and the forced
 * prelude has already been included by then. bionic on 32-bit ARM has no
 * gregs member to spell: its sigcontext is the kernel's, register by named
 * register, and the counter is arm_pc. Reading the wrong one is not a wrong
 * profile, it is a translation unit the compiler refuses; which is how the
 * Android build found this, having been the first non-x86 host to compile
 * the sampler at all.
 *
 * An architecture this does not know samples zero rather than failing to
 * build: a profiler is a diagnostic, and a diagnostic that stops the game
 * being packaged for a device has the cost backwards.
 */
static void pc_prof_handler(int sig, siginfo_t *si, void *uc)
{
#if defined(__i386__)
    uint32_t at = (uint32_t)((ucontext_t *)uc)->uc_mcontext.gregs[14];
#elif defined(__arm__)
    uint32_t at = (uint32_t)((ucontext_t *)uc)->uc_mcontext.arm_pc;
#elif defined(__aarch64__)
    /* Truncated on purpose: the sample file is a stream of u32 and
     * prof_fold.py reads it as one. Harmless for the 32-bit ARM build this
     * exists for, and wrong for a real 64-bit one, widen both ends
     * together on the day there is one. */
    uint32_t at = (uint32_t)((ucontext_t *)uc)->uc_mcontext.pc;
#else
    uint32_t at = 0;
    (void)uc;
#endif
    (void)sig; (void)si;
    (void)!write(pc_prof_fd, &at, 4);
}

void pc_prof_init(void)
{
    const char *spec = getenv("PC_PROF");
    char path[512];
    long hz = 997;
    const char *colon;
    struct sigaction sa;
    struct itimerval it;

    if (spec == NULL || spec[0] == '\0') {
        return;
    }
    colon = strrchr(spec, ':');
    if (colon != NULL && colon[1] >= '0' && colon[1] <= '9') {
        hz = strtol(colon + 1, NULL, 10);
        if (hz < 1) hz = 1;
        if (hz > 10000) hz = 10000;
        (void)snprintf(path, sizeof path, "%.*s", (int)(colon - spec), spec);
    } else {
        (void)snprintf(path, sizeof path, "%s", spec);
    }
    pc_prof_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pc_prof_fd < 0) {
        fprintf(stderr, "pc: PC_PROF %s: cannot open\n", path);
        return;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = pc_prof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = (suseconds_t)(1000000 / hz);
    it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, NULL);
    fprintf(stderr, "pc: profiling to %s at %ld Hz\n", path, hz);
}

/* See pc_prof.h. One 8-byte write, so the handler's own 4-byte appends
 * cannot land inside the marker, O_APPEND writes do not shear. */
void pc_prof_frame(uint32_t work_us)
{
    uint32_t rec[2];

    if (pc_prof_fd < 0) {
        return;
    }
    rec[0] = 0u;
    rec[1] = work_us;
    (void)!write(pc_prof_fd, rec, sizeof rec);
}

#else /* _WIN32 */

/* No ITIMER_PROF on Windows. Saying so beats sampling nothing silently. */
void pc_prof_init(void)
{
    if (getenv("PC_PROF") != NULL) {
        fprintf(stderr, "pc: PC_PROF is Linux-only (SIGPROF); profile the "
                        "Linux build of the same tree\n");
    }
}

void pc_prof_frame(uint32_t work_us)
{
    (void)work_us;
}

#endif
