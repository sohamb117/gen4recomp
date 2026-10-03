/*
 * A digest of the guest's whole state. Ported from pokediamond's pc/src/pc_state.c,
 * where the reasoning below was paid for; the differences are noted where they
 * matter, because carrying a comment that is true next door and false here is
 * worse than having no comment.
 *
 * Why this exists. The frame dumps already compare two runs, but they compare a
 * consequence: a divergence in guest memory is invisible to them until it
 * reaches a pixel, and plenty of state never does. Guest memory is complete in
 * the way a framebuffer is not; it holds every byte a later frame could be
 * computed from, so digesting it is the difference between asserting
 * determinism and measuring it.
 *
 * Why a hash and not a dump. The mapped regions are megabytes, twice per check,
 * compared by a test that has to say *where* they differ. Per-region digests
 * give the region for free and cost eight bytes each. Locating a divergence
 * inside a region is the differential runner's job, against an emulator running
 * the same ROM rather than against yesterday's self.
 *
 * WHY FNV-1a. Eleven lines, no dependency, and it is not a security primitive
 * and does not pretend to be. The alternative in this tree is the SDK's own MD5,
 * which is compiled game code: it would run on the emulated stack and perturb
 * the thing being hashed. A collision costs a test that misses a divergence, not
 * a wrong answer shipped.
 *
 * What is and is not included. Every region armrec_mem_init() maps, read from
 * armrec_region_at() rather than copied into a list here, so a future remap
 * joins the digest by existing. Host memory is deliberately absent: the port's
 * host-side state is scaffolding (the FIFO models, an open FILE *, the viewer's
 * shared page), and the property worth checking is that none of it *leaks* into
 * guest memory; which is exactly what a guest-only digest can see and a
 * whole-process one could not.
 */

#include "pc_state.h"

#include "armrec_rt.h"

#if !defined(__wasm__)
#include <signal.h>
#endif
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */

#define FNV64_OFFSET PC_STATE_FNV64_OFFSET
#define FNV64_PRIME  0x100000001b3ULL

uint64_t pc_state_fnv1a(uint64_t h, const void *buf, uint32_t n) {
    const unsigned char *p = (const unsigned char *)buf;
    uint32_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= FNV64_PRIME;
    }
    return h;
}

#define fnv1a(h, p, n) pc_state_fnv1a((h), (p), (n))

/* ------------------------------------------------------------------ */
/* Host-pointer words, marked by address                              */
/* ------------------------------------------------------------------ */

/*
 * Guest memory holds a small number of host pointers, function
 * pointers the game installed, an FS archive object, an NNS sequence
 * player, the exception vector, a pointer to OSi_ThreadInfo. A relink
 * moves those words and nothing else in the same regions. Folding
 * every word whose VALUE is >= 0x10000000 was measured over 6,000
 * frames and rejected: 76% of changing words sit in that range
 * because graphics, text and compressed data have a high top nibble,
 * so the fold would blind most of the digest's change signal.
 *
 * The port therefore marks the guest word at the WRITE, and the
 * digest skips exactly those addresses. The set is small (a hundred
 * or so words across main RAM, the port window and shared work) and
 * the completeness proof is a link perturbation: any word that moves
 * and is not in this list is a write the hunt missed.
 */

#define HOST_PTR_MIN 0x10000000u

static uint32_t *host_marks;
static uint32_t  host_n;
static uint32_t  host_cap;

/* First index with marks[i] >= addr, or host_n if none. */
static uint32_t host_lb(uint32_t addr)
{
    uint32_t lo = 0, hi = host_n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (host_marks[mid] < addr) lo = mid + 1;
        else                        hi = mid;
    }
    return lo;
}

void pc_state_mark_host_word(uint32_t addr)
{
    uint32_t i;

    addr &= ~3u;
    /* Guest space is everything below the host image. A mark of a
     * host-side field (a static OSThread, a table in .bss) is not a
     * word the digest will ever see. */
    if (addr >= HOST_PTR_MIN) return;

    i = host_lb(addr);
    if (i < host_n && host_marks[i] == addr) return;

    if (host_n == host_cap) {
        uint32_t ncap = host_cap ? host_cap * 2u : 256u;
        uint32_t *n = (uint32_t *)realloc(host_marks, ncap * sizeof *n);
        if (n == NULL) return;          /* digest keeps the words; a
                                         * later perturbation still
                                         * names the miss. */
        host_marks = n;
        host_cap = ncap;
    }
    if (i < host_n) {
        memmove(host_marks + i + 1, host_marks + i,
                (host_n - i) * sizeof *host_marks);
    }
    host_marks[i] = addr;
    host_n++;
}

void pc_state_mark_host_field(const void *field)
{
    if (field == NULL) return;
    pc_state_mark_host_word((uint32_t)(uintptr_t)field);
}

void pc_state_scan_mark_host_words(const void *obj, uint32_t n)
{
    const uint32_t *p;
    uint32_t i, words;

    if (obj == NULL || n < 4) return;
    p = (const uint32_t *)obj;
    words = n / 4;
    for (i = 0; i < words; i++) {
        if (p[i] >= HOST_PTR_MIN) {
            pc_state_mark_host_word((uint32_t)(uintptr_t)&p[i]);
        }
    }
}

uint32_t pc_state_host_word_count(void)
{
    return host_n;
}

/*
 * Hash [base, base+size) skipping every marked word. The unmarked
 * runs are fed to FNV as contiguous spans so a region with no marks
 * is byte-identical to the old digest of the same bytes.
 */
static uint64_t digest_span_skipping(uint32_t base, uint32_t size)
{
    uint64_t h = FNV64_OFFSET;
    uint32_t pos = base;
    uint32_t end = base + size;
    uint32_t i = host_lb(base);

    while (i < host_n && host_marks[i] < end) {
        uint32_t m = host_marks[i];
        if (m > pos) {
            h = fnv1a(h, (const unsigned char *)(uintptr_t)pos, m - pos);
        }
        pos = m + 4;
        i++;
    }
    if (pos < end) {
        h = fnv1a(h, (const unsigned char *)(uintptr_t)pos, end - pos);
    }
    return h;
}

/*
 * The stack-argument bracket, and why it is here when this tree does not need it.
 *
 * A decompiled C caller reaching a *recompiled* function leaves sixteen words
 * below the emulated stack pointer for the duration of the call. Next door those
 * words are live at every instant a digest is taken, one of them is a stale host
 * pointer that moves with ASLR, and three unbounded runs gave three digests
 * differing in exactly those sixteen bytes.
 *
 * This tree has no recompiled functions at all; the decompilation is complete,
 * so every call is C calling C, and armrec_stkargs_call() is referenced only
 * by the runtime and its generators, never by anything compiled here. The
 * bracket is therefore a no-op: settle() walks a list that is empty and returns.
 *
 * It is called anyway, for two reasons and not for symmetry. The list becoming
 * non-empty is a thing a future change could do without anyone thinking about
 * this file, and the failure would be a digest that is unstable for a reason
 * nobody would look for here. And the cost of being wrong in the other direction
 * is one increment. Every digest goes through this function so the bracket is in
 * one place; the pair nests, so the whole-state digest below brackets once.
 */
uint64_t pc_state_digest_span(uint32_t base, uint32_t size) {
    uint64_t h;

    if (!armrec_mem_ready) return 0;
    armrec_stkargs_settle();
    h = digest_span_skipping(base, size);
    armrec_stkargs_resume();
    return h;
}

uint64_t pc_state_digest(void) {
    uint64_t total = FNV64_OFFSET;
    int i, n;

    if (!armrec_mem_ready) return 0;
    armrec_stkargs_settle();
    n = armrec_region_count();
    for (i = 0; i < n; i++) {
        uint32_t base, size;
        uint64_t d;
        unsigned char be[8];
        int k;

        if (!armrec_region_at(i, &base, &size, NULL)) continue;
        d = pc_state_digest_span(base, size);
        /* Folded in big-endian byte order rather than as a native uint64_t, so
         * the combined value is a property of the guest's contents and not of
         * the host's byte order. */
        for (k = 0; k < 8; k++) be[k] = (unsigned char)(d >> (56 - 8 * k));
        total = fnv1a(total, be, 8);
    }
    armrec_stkargs_resume();
    return total;
}

void pc_state_report(FILE *out, const char *label) {
    int i, n;

    if (out == NULL) out = stderr;
    if (!armrec_mem_ready) {
        fprintf(out, "pc-state %s UNMAPPED\n", label ? label : "-");
        fflush(out);
        return;
    }
    armrec_stkargs_settle();
    n = armrec_region_count();
    for (i = 0; i < n; i++) {
        uint32_t base, size;
        const char *name;

        if (!armrec_region_at(i, &base, &size, &name)) continue;
        fprintf(out, "pc-state %s %-12s %08X %08X %016llX\n",
                label ? label : "-", name, base, size,
                (unsigned long long)pc_state_digest_span(base, size));
    }
    fprintf(out, "pc-state %s %-12s %8d %8s %016llX\n",
            label ? label : "-", "TOTAL", n, "regions",
            (unsigned long long)pc_state_digest());

    /*
     * PC_DUMP_STATE=dir writes the regions themselves beside the digests.
     * The digest says a region moved; only the bytes say what moved, and the
     * difference between "a host address landed in guest memory" and "the
     * game took a different path" is one diff away once you have both dumps.
     * Off unless asked for: these are megabytes per report.
     */
    {
        const char *dir = getenv("PC_DUMP_STATE");

        if (dir != NULL && dir[0] != '\0') {
            for (i = 0; i < n; i++) {
                uint32_t base, size;
                const char *name;
                char path[512];
                FILE *f;

                char safe[64];
                unsigned k;

                if (!armrec_region_at(i, &base, &size, &name)) continue;
                /* Two of the fourteen region names have a slash in them,
                 * "I/O" and "ARM7 WRAM/IWRAM", and a slash in the middle of
                 * a filename is a directory that does not exist, so fopen
                 * failed and `continue` skipped it in silence. Twelve files
                 * appeared where fourteen were asked for, and the two missing
                 * ones were a region whose digest had MOVED, which is the
                 * only reason anyone dumps state at all. Substitute rather
                 * than rename the regions: the names are printed in the
                 * digest report and in armrec's own tables. */
                for (k = 0; k + 1 < sizeof safe && name != NULL && name[k]; k++) {
                    safe[k] = (name[k] == '/') ? '_' : name[k];
                }
                if (name == NULL || name[0] == '\0') {
                    safe[0] = '-';
                    k = 1;
                }
                safe[k] = '\0';
                snprintf(path, sizeof path, "%s/%s-%08X-%s.bin",
                         dir, label ? label : "-", base, safe);
                f = fopen(path, "wb");
                if (f == NULL) continue;
                fwrite((const void *)(uintptr_t)base, 1, size, f);
                fclose(f);
            }
            /* The marked host-pointer words, one address per line, so a
             * link perturbation can name every word that moved and is
             * not in this list. */
            {
                char path[512];
                FILE *f;
                uint32_t k;

                snprintf(path, sizeof path, "%s/%s-hostptr.txt",
                         dir, label ? label : "-");
                f = fopen(path, "w");
                if (f != NULL) {
                    fprintf(f, "# %u marked guest words\n", host_n);
                    for (k = 0; k < host_n; k++) {
                        fprintf(f, "%08X\n", host_marks[k]);
                    }
                    fclose(f);
                }
            }
        }
    }
    armrec_stkargs_resume();
    fflush(out);
}

/* ------------------------------------------------------------------ */
/* Reporting however the run ends                                      */
/* ------------------------------------------------------------------ */

/*
 * A crashed run's digest is exactly as comparable as a returned one's, so a
 * fatal signal has to be one of the endings this reports from, otherwise the
 * instrument is unavailable in precisely the situation that most wants it.
 *
 * The handler re-installs SIG_DFL and re-raises, so the process still dies of
 * what killed it and the shell's exit code, gdb and the port's own fault handler
 * all see what they saw before. Only reached when the digest was asked for.
 *
 * A list rather than one callback, because there can only be one SIGSEGV handler
 * and each of these re-raises: a second independently installed handler would
 * never run, and the failure would be a report that is silently missing rather
 * than an error.
 */
#define MAX_ENDING 4
static FILE *report_out;
#if defined(__wasm__)
/* wasm has no signals: the only endings are exit (atexit, below) and a trap,
 * which unwinds nothing and runs no handler. */
static int reported;
#else
static volatile sig_atomic_t reported;
#endif
static pc_state_ending_fn ending[MAX_ENDING];
static int nending;

static void report_once(const char *label) {
    int i;
    if (reported) return;
    reported = 1;
    for (i = 0; i < nending; i++)
        ending[i](report_out ? report_out : stderr, label);
}

static void report_at_exit(void) { report_once("exit"); }

#if !defined(__wasm__)
static void report_on_signal(int sig) {
    const char *label = "signal";

    switch (sig) {
    case SIGSEGV: label = "sigsegv"; break;
#if defined(SIGBUS)
    case SIGBUS:  label = "sigbus";  break;
#endif
    case SIGILL:  label = "sigill";  break;
    case SIGFPE:  label = "sigfpe";  break;
    case SIGABRT: label = "sigabrt"; break;
    default: break;
    }
    report_once(label);
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

void pc_state_at_ending(pc_state_ending_fn fn, FILE *out)
{
#if !defined(__wasm__)
    /* SIGBUS is POSIX's; the other four are ISO C's own names and mingw has
     * them, so the Windows build loses only the bus-error label. */
    static const int sigs[] = { SIGSEGV,
#if defined(SIGBUS)
                                SIGBUS,
#endif
                                SIGILL, SIGFPE, SIGABRT };
    size_t i;
#endif
    static int installed;

    if (fn == NULL || nending >= MAX_ENDING) return;
    ending[nending++] = fn;
    report_out = out ? out : stderr;
    if (installed) return;
    installed = 1;
    atexit(report_at_exit);
#if !defined(__wasm__)
    for (i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        signal(sigs[i], report_on_signal);
    }
#endif
}

void pc_state_report_at_exit(FILE *out)
{
    pc_state_at_ending(pc_state_report, out);
}
