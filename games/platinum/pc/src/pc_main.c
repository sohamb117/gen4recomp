/*
 * Native PC entry point for the pokeplatinum port.
 *
 * The hardware boot path is crt0 -> NitroMain; here it is
 * armrec_mem_init() -> NitroMain(). armrec_mem_init() identity-maps the
 * DS address space into the process (main RAM at 0x02000000, ITCM, DTCM,
 * the IO window, palette, VRAM windows), which is what lets the SDK's
 * compiled C read arena bounds like SDK_MAIN_ARENA_LO as real addresses
 * and lets heap allocations hand out guest pointers. Host text sits at
 * 0x10000000 (see the link flags) precisely so none of those regions
 * collide with the port's own image.
 *
 * The game's statics live in the host image, not in guest RAM, unlike a
 * recompiled tree there is no guest image to load; the compiler placed
 * every global where the host linker wanted it. Only dynamic allocations
 * (arenas/heaps) and hardware-shaped state live at guest addresses.
 *
 * The fault handler exists because the port is expected to die somewhere
 * for a while yet: each wall should name its address and whether it was a
 * guest-region access, not just say "Segmentation fault".
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include "pc_win_fiber.h"   /* crash report + host-spin watchdog */
#elif defined(__wasm__)
/* No signals, no ucontext, no unwinder, no mmap: a wasm trap is reported by
 * the native runtime, and fatal paths here go through pc_wasm_fatal. */
#include <pc_wasm.h>
#else
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#if defined(__BIONIC__)
/* No <execinfo.h> on bionic. glibc's backtrace() is a wrapper over the same
 * unwinder these two headers reach directly, so the walk below is the same
 * walk with the same limits; see print_backtrace. */
#include <dlfcn.h>
#include <unwind.h>
#else
#include <execinfo.h>
#endif
#endif

#include "armrec_rt.h"
#include "pc_args.h"
#include "pc_modfs.h"
#include "pc_prof.h"
#include "pc_diff.h"
#include "pc_div0.h"
#include "pc_state.h"
#include "pc_sym.h"
#include "pc_selftest.h"
#include "pc_view.h"
#include "pc_gpu2d.h"
#include "pc_gpu3d_gl.h"

extern void pc_polydump_init(void);
extern void pc_scrcov_init(void);

#include "pc_bench.h"

extern void NitroMain(void);

/* The GBA slot: ROM window 0x08000000-0x09FFFFFF, SRAM 0x0A000000-0x0A00FFFF.
 * armrec_mem_init() does not map it (no DS region does on that side of
 * 0x08000000), but CTRDG_Init probes it unconditionally at boot. Zero-filled
 * backing is the faithful no-cartridge model: the header, logo and module-ID
 * compares all fail over zeros, so CTRDG reports nothing inserted, which on
 * this machine is the truth. */
static int map_agb_slot(void)
{
    void *want = (void *)0x08000000u;
    size_t len = 0x02010000u; /* ROM window + SRAM */
#if defined(_WIN32)
    extern void *pcw_valloc_fixed(void *want, unsigned len); /* armrec_rt.c */
    void *got = pcw_valloc_fixed(want, (unsigned)len);
#elif defined(__wasm__)
    /* Linear memory is the address space: 0x08000000-0x0A00FFFF is already
     * there and zero-initialised, and the C runtime's own data starts at
     * NP_GUEST_C_BASE, above it, so nothing has written to it before this.
     * Zero-fill is the whole of the no-cartridge model, so there is nothing
     * to map. */
    void *got = want;
    (void)len;
#else
    void *got = mmap(want, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#endif
    if (got != want) {
        fprintf(stderr, "pokeplatinum-pc: cannot map the GBA slot window\n");
        return -1;
    }
    return 0;
}

#if !defined(_WIN32) && !defined(__wasm__)

#if defined(__BIONIC__)

/*
 * The same stack walk, without <execinfo.h>.
 *
 * glibc's backtrace() is a thin wrapper over _Unwind_Backtrace and its
 * symbol printer is a thin wrapper over dladdr, both of which bionic has;
 * what it does not have is the two names. So this is not a reduced backtrace,
 * it is the same one written out.
 *
 * It needs -funwind-tables to find any frames at all, which is why
 * Makefile.android passes it; the armhf build learned the same lesson the
 * hard way and prints an empty trace without it.
 */
struct pc_bt_walk {
    void **frame;
    int n;
    int max;
};

static _Unwind_Reason_Code pc_bt_step(struct _Unwind_Context *ctx, void *arg)
{
    struct pc_bt_walk *w = arg;
    _Unwind_Word pc = _Unwind_GetIP(ctx);

    if (pc == 0 || w->n >= w->max) {
        return _URC_END_OF_STACK;
    }
    w->frame[w->n++] = (void *)(unsigned long)pc;
    return _URC_NO_REASON;
}

static void print_backtrace(void)
{
    void *frames[32];
    struct pc_bt_walk w;
    int i;

    w.frame = frames;
    w.n = 0;
    w.max = (int)(sizeof frames / sizeof frames[0]);
    _Unwind_Backtrace(pc_bt_step, &w);

    for (i = 0; i < w.n; i++) {
        Dl_info info;

        /* dladdr is no more signal-safe than the fprintf next to it; a
         * scaffold that is about to _exit gets away with both, and an
         * address with no name on it is what the alternative prints. */
        if (dladdr(frames[i], &info) != 0 && info.dli_fname != NULL) {
            unsigned long off = (unsigned long)((char *)frames[i]
                                                - (char *)info.dli_saddr);

            if (info.dli_sname != NULL) {
                fprintf(stderr, "%s(%s+0x%lx) [%p]\n",
                        info.dli_fname, info.dli_sname, off, frames[i]);
            } else {
                fprintf(stderr, "%s(+0x%lx) [%p]\n", info.dli_fname,
                        (unsigned long)((char *)frames[i]
                                        - (char *)info.dli_fbase),
                        frames[i]);
            }
        } else {
            fprintf(stderr, "[%p]\n", frames[i]);
        }
    }
}

#else

static void print_backtrace(void)
{
    void *frames[32];
    int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, 2);
}

#endif

/* The faulting program counter, wherever this host keeps it. Named "eip" in
 * both handlers' messages because every log, script and note in this port that
 * greps for it was written against the x86 build. */
#if defined(__i386__)
#define PC_FAULT_PC ((unsigned long)uc->uc_mcontext.gregs[REG_EIP])
#elif defined(__arm__)
#define PC_FAULT_PC ((unsigned long)uc->uc_mcontext.arm_pc)
#else
#define PC_FAULT_PC 0UL
#endif

/*
 * The register file, which the report used to leave out.
 *
 * A program counter and a stack are enough when the stack can be walked. On
 * ARM, in this port, it often cannot: a context runs on a fiber stack begun by
 * three instructions with no unwind information, so the unwinder stops at the
 * first frame and the only evidence left is what was in the registers. `lr`
 * in particular says where a bad branch CAME FROM, which is the whole question
 * when a jump lands in data.
 *
 * Written for the two hosts this port builds for. Anywhere else it says
 * nothing rather than something invented.
 */
static void print_registers(const ucontext_t *uc)
{
#if defined(__arm__)
    const struct sigcontext *m = &uc->uc_mcontext;

    fprintf(stderr,
            "  r0 %08lx r1 %08lx r2 %08lx r3 %08lx\n"
            "  r4 %08lx r5 %08lx r6 %08lx r7 %08lx\n"
            "  r8 %08lx r9 %08lx sl %08lx fp %08lx\n"
            "  ip %08lx sp %08lx lr %08lx pc %08lx cpsr %08lx\n",
            (unsigned long)m->arm_r0, (unsigned long)m->arm_r1,
            (unsigned long)m->arm_r2, (unsigned long)m->arm_r3,
            (unsigned long)m->arm_r4, (unsigned long)m->arm_r5,
            (unsigned long)m->arm_r6, (unsigned long)m->arm_r7,
            (unsigned long)m->arm_r8, (unsigned long)m->arm_r9,
            (unsigned long)m->arm_r10, (unsigned long)m->arm_fp,
            (unsigned long)m->arm_ip, (unsigned long)m->arm_sp,
            (unsigned long)m->arm_lr, (unsigned long)m->arm_pc,
            (unsigned long)m->arm_cpsr);
#elif defined(__i386__)
    fprintf(stderr,
            "  eax %08lx ecx %08lx edx %08lx ebx %08lx\n"
            "  esp %08lx ebp %08lx esi %08lx edi %08lx eip %08lx\n",
            (unsigned long)uc->uc_mcontext.gregs[REG_EAX],
            (unsigned long)uc->uc_mcontext.gregs[REG_ECX],
            (unsigned long)uc->uc_mcontext.gregs[REG_EDX],
            (unsigned long)uc->uc_mcontext.gregs[REG_EBX],
            (unsigned long)uc->uc_mcontext.gregs[REG_ESP],
            (unsigned long)uc->uc_mcontext.gregs[REG_EBP],
            (unsigned long)uc->uc_mcontext.gregs[REG_ESI],
            (unsigned long)uc->uc_mcontext.gregs[REG_EDI],
            (unsigned long)uc->uc_mcontext.gregs[REG_EIP]);
#else
    (void)uc;
#endif
}

static void fault_handler(int sig, siginfo_t *si, void *ucontext)
{
    /* fprintf is not async-signal-safe; a diagnostic scaffold that is
     * about to _exit gets away with it, and the alternative is a wall
     * with no address on it. */
    ucontext_t *uc = ucontext;

    /* A guest division by zero is not a fault on a DS, ARM has no divide
     * instruction and the runtime answers it. pc_div0 gives the answer the
     * cartridge gives and resumes; anything it does not recognise falls
     * through to the report below.
     *
     * x86 only, and not because ARM does not need an answer; it does, glibc
     * raises there too, but because on ARM the answer is given a call
     * earlier, by the --wrap in pc/Makefile.arm. Nothing is left for a signal
     * handler to repair, so a SIGFPE reaching here on ARM is a real one and
     * belongs in the report below. */
#if defined(__i386__)
    if (sig == SIGFPE) {
        static const int kGreg[8] = {
            REG_EAX, REG_ECX, REG_EDX, REG_EBX,
            REG_ESP, REG_EBP, REG_ESI, REG_EDI,
        };
        struct pc_x86_regs g;
        int i;

        for (i = 0; i < 8; i++) {
            g.r[i] = (uint32_t)uc->uc_mcontext.gregs[kGreg[i]];
        }
        g.eip = (uint32_t)uc->uc_mcontext.gregs[REG_EIP];
        if (pc_div0_fixup(&g)) {
            for (i = 0; i < 8; i++) {
                uc->uc_mcontext.gregs[kGreg[i]] = (greg_t)g.r[i];
            }
            uc->uc_mcontext.gregs[REG_EIP] = (greg_t)g.eip;
            return;
        }
    }
#endif
    fprintf(stderr,
            "pokeplatinum-pc: fatal signal %d (%s) at address %p%s, eip=%#lx\n",
            sig, strsignal(sig), si->si_addr,
            armrec_is_guest_addr((uint32_t)(uintptr_t)si->si_addr)
                ? " (inside a mapped guest region)"
                : " (NOT in any mapped guest region)",
            PC_FAULT_PC);
    print_registers(uc);
    print_backtrace();
    fflush(NULL);
    _exit(128 + sig);
}

/* PC_BOOT_WATCHDOG=N: after N seconds, report where the boot is sitting and
 * exit. The port is expected to hang somewhere for a while yet (a compiled
 * wait loop on hardware state nothing here advances); each such wall should
 * name its program counter and stack, not just eat a timeout. */
static void watchdog_handler(int sig, siginfo_t *si, void *ucontext)
{
    ucontext_t *uc = ucontext;
    (void)sig;
    (void)si;
    fprintf(stderr,
            "pokeplatinum-pc: boot watchdog fired; eip=%#lx, stack:\n",
            PC_FAULT_PC);
    print_registers(uc);
    print_backtrace();
    fflush(NULL);
    _exit(3);
}

static void install_fault_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

#endif /* !_WIN32 && !__wasm__ */

int main(int argc, char **argv)
{
    int i;

    /* Trap messages must land even when the port dies mid-line. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

#if defined(_WIN32)
    /*
     * PC_CONSOLE is read before the argument layer rather than after it,
     * because the argument layer is itself something that prints, --help
     * and every rejection message go out of pc_args_apply. A raw scan for
     * the three spellings the input has is the price of `--console --help`
     * working from a double-click. pc_args_apply still puts the variable in
     * the environment afterwards; nothing downstream cares that it was read
     * twice.
     */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--console") == 0
            || strncmp(argv[i], "--console=", 10) == 0
            || strncmp(argv[i], "PC_CONSOLE=", 11) == 0) {
            break;
        }
    }
    if (i < argc || getenv("PC_CONSOLE") != NULL) {
        pcw_attach_console();
    }
#endif

    /*
     * Flags and KEY=VALUE arguments both become environment variables before
     * anything reads one. The port's whole configuration surface is PC_*
     * variables and that does not change: pc_args_apply
     * translates each flag into its variable, so `--frames 900` and
     * `PC_FRAMES=900` are one input with two spellings rather than two inputs
     * to keep in step. KEY=VALUE stays because WSL interop does not forward
     * arbitrary environment into a Windows process, so
     *   pokeplatinum.exe PC_ROM=build/rom/pokeplatinum.us.nds PC_VIEW=pplat
     * is how the Windows build is driven. See pc/src/pc_args.c.
     */
    i = pc_args_apply(argc, argv);
    if (i != 0) {
        return i > 0 ? 0 : 2;
    }

    /*
     * Which mods this binary was built with (pc/mods/README.md). Said out
     * loud at boot because a modded binary is deliberately not the port the
     * tests and the pinned numbers measure, and a session log that does not
     * say so would let the two be confused after the fact. The manifest is
     * generated by the build; a vanilla binary carries the empty string.
     */
    {
        extern const char pc_mods_manifest[];

        if (pc_mods_manifest[0] != '\0') {
            fprintf(stderr, "pc-mods: %s\n", pc_mods_manifest);
        }
    }
    pc_modfs_boot();
    /* Where to read the symbol table from if /proc is unavailable. Harmless
     * when --watch is never given: the table is loaded lazily, when something
     * asks for a name. */
    pc_sym_set_exe(argv[0]);

#if defined(_WIN32)
    /* The vectored handler prints the faulting EIP and address; nm or
     * addr2line on the exe (image base 0x10000000) names the function. */
    pcw_install_crash_report();
    if (getenv("PC_BOOT_WATCHDOG")) {
        pcw_install_watchdog();
    }
#elif defined(__wasm__)
    /* No signals and no timer: a wasm fault is a trap the native runtime
     * reports, and a hang is the runtime's to time out. */
    if (getenv("PC_BOOT_WATCHDOG")) {
        fprintf(stderr, "pokeplatinum-pc: PC_BOOT_WATCHDOG has no timer in"
                        " wasm; ignored\n");
    }
#else
    install_fault_handlers();

    if (getenv("PC_BOOT_WATCHDOG")) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = watchdog_handler;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGALRM, &sa, NULL);
        alarm((unsigned)atoi(getenv("PC_BOOT_WATCHDOG")));
    }
#endif

    if (armrec_mem_init() != 0) {
#if defined(__wasm__)
        pc_wasm_fatalf("pokeplatinum-pc: guest memory init failed: %s",
                       armrec_mem_strerror());
#else
        fprintf(stderr, "pokeplatinum-pc: guest memory init failed: %s\n",
                armrec_mem_strerror());
        return 1;
#endif
    }
    if (map_agb_slot() != 0) {
        return 1;
    }
    {
        extern int pc_rom_init(void);
        if (pc_rom_init() != 0) {
            return 1;
        }
    }

    /* The input layer: idles REG_KEYINPUT (0x03FF, active-low, and a
     * zeroed register reads as the L+R+Start+Select reset combo held) AND
     * HW_BUTTON_XY_BUF (0x2C00, same story for X/Y/DEBUG, which read as
     * held from a zeroed buf), then loads the PC_INPUT script if one is
     * named. OS_Halt applies script lines at each frame boundary. */
    {
        extern int pc_input_init(void);
        if (!pc_input_init()) {
            return 1;
        }
    }

    /* The window channel: PC_VIEW=<shm-name> publishes every frame for the
     * native SDL2 viewer (build/pc/pcview) and accepts its live input.
     * Unset, the seam costs nothing. */
    {
        extern int pc_view_init(void);
        if (pc_view_init() != 0) {
            return 1;
        }
    }

    /* The RTC: a deterministic clock behind PXI tag 5 (fixed epoch +
     * frame-counter advance; PC_RTC overrides the epoch). Before
     * NitroMain so RTC_Init's first read finds the responder. */
    {
        extern int pc_rtc_init(void);
        if (pc_rtc_init() != 0) {
            return 1;
        }
    }

    /* The WVR radio behind PXI tag 15: answers STARTUP/TERMINATE with
     * success, which is what the console's own radio does. Without it the
     * game waits forever mid-CONNECTING and draws the network icon over
     * unloaded tiles in every battle. */
    {
        extern int pc_wvr_init(void);
        if (pc_wvr_init() != 0) {
            return 1;
        }
    }

    /* The silent sound driver behind PXI tag 7: consumes and acknowledges
     * every command list (the title screen hangs in SND_WaitForCommandProc
     * without the acknowledgement), plays nothing until the sound work. */
    {
        extern int pc_snd_init(void);
        if (pc_snd_init() != 0) {
            return 1;
        }
    }

    /* The renderer: the video path. Both engines install unconditionally and
     * after armrec_mem_init(), because they read registers out of guest
     * memory (POWCNT gates the 3D engine) and a renderer that only ran
     * when someone was watching would make PC_DUMP_FRAMES change the run.
     * The 2D installer before pc_video_open_dump(), which stamps the
     * renderer's name into the manifest header the moment it opens. */
    {
        extern void pc_gpu2d_install(void);
        extern void pc_gpu3d_install(void);
        extern void pc_gpu3d_soft_survey_init(void);
        extern int pc_video_open_dump(const char *dir);
        extern void pc_video_set_frame_limit(unsigned long long frames);
        const char *dump = getenv("PC_DUMP_FRAMES");
        const char *frames = getenv("PC_FRAMES");

        pc_gpu2d_install();
        pc_gpu3d_install();
        pc_polydump_init();
        pc_bench_init();
        pc_prof_init();
        pc_gpu3d_soft_survey_init();
        pc_scrcov_init();
        if (dump != NULL && !pc_video_open_dump(dump)) {
            return 1;
        }
        if (frames != NULL) {
            pc_video_set_frame_limit(strtoull(frames, NULL, 0));
        }
    }

    /*
     * Internal resolution. Before the widescreen block, because the
     * rasterizer sizes its buffers from the width and the scale together and
     * the width is set below; and refused beside the same three instruments
     * as widescreen, for a sharper reason. The port's own 256x192 surfaces
     * are a point-sample of the high-resolution render at anything above 1,
     * so a frame dump taken here is NOT the native picture those instruments
     * compare; it is a different sampling of a different rasterization.
     */
    {
        const char *spec = getenv("PC_HD3D");

        if (spec != NULL && spec[0] != '\0') {
            char *end = NULL;
            long n = strtol(spec, &end, 10);

            if (end == spec || (end != NULL && *end != '\0')
                || n < 1 || n > (long)PC_VIEW_HD_MAX) {
                fprintf(stderr, "%s: --hd3d wants 1..%u; got %s\n",
                        argv[0], PC_VIEW_HD_MAX, spec);
                return 2;
            }
            if (n > 1) {
                if (getenv("PC_DIFF_TRACE") != NULL
                    || getenv("PC_DIFF_SPANS") != NULL
                    || getenv("PC_DUMP_FRAMES") != NULL) {
                    fprintf(stderr,
                            "%s: --hd3d rasterizes the 3D layer at a"
                            " different resolution, so the surfaces this port"
                            " composes are a point-sample of it and not the"
                            " native picture --diff-trace, --diff-spans and"
                            " --dump-frames compare.\n", argv[0]);
                    return 2;
                }
                pc_view_set_hd((int)n);
            }
        }
    }

    /*
     * Which producer draws the 3D layer. Selected once,
     * here, after the resolution is fixed and before any guest code runs;
     * a refused gl stays soft and says so, a wrong value is a usage error
     * the same way --hd3d's is.
     */
    if (pc_gpu3d_gl_select() < 0) {
        fprintf(stderr, "%s: --gpu3d wants soft or gl; got %s\n",
                argv[0], getenv("PC_GPU3D"));
        return 2;
    }

    /* ...and the instrument that arbitrates producers,
     * armed after the choice so its report can name what it judged. */
    {
        extern void pc_gpu3d_soft_diff_init(void);

        pc_gpu3d_soft_diff_init();
    }

    /*
     * ARM THE 2D engine's high-resolution recording without changing the
     * RESOLUTION, which exists so the recording can be shown to disturb
     * nothing.
     *
     * At --hd3d 2 and above the 2D engine lays the 3D layer down at every
     * column the window admits, a phantom where the rasterizer drew
     * nothing, and takes the phantoms back out before the blend unit runs.
     * That is a real edit to the composed line, and "it puts everything back"
     * is a claim rather than an observation. With this set the recording runs
     * over a native render, so the picture the port publishes must be
     * byte-for-byte the picture it publishes without it; --dump-frames is the
     * instrument that says so, and it is not refused here because nothing
     * about the render changed.
     */
    if (getenv("PC_HD3D_RECORD") != NULL) {
        pc_gpu2d_set_hd(2);
    }

    /*
     * Widescreen. The port owns the projection, so PC_ASPECT widens the 3D
     * field of view rather than stretching the picture: `auto` follows the
     * window the viewer reports and re-tunes as it is resized, a ratio or a
     * column count fixes it, and 4:3 is the console's own.
     *
     * Refused beside the differential trace and the frame dumps, and the
     * reason is one sentence: a wider frustum is what the box test culls
     * against, so a run can legitimately keep an object a native run
     * dropped. That is the feature working. It also means a run rendered
     * wide is not the run those two instruments exist to compare, and an
     * instrument quietly measuring something else is worse than one that
     * refuses to start.
     */
    {
        const char *spec = getenv("PC_ASPECT");

        if (spec != NULL && spec[0] != '\0') {
            int want;

            if (strcmp(spec, "auto") == 0) {
                want = PC_VIEW_ASPECT_AUTO;
            } else if (strcmp(spec, "off") == 0 || strcmp(spec, "native") == 0) {
                want = PC_VIEW_W;
            } else if (strchr(spec, ':') != NULL) {
                char *end = NULL;
                unsigned long n = strtoul(spec, &end, 10), d = 0;

                /* The whole string or nothing: "16:9:2" parsed as 16:9 by
                 * reading only as far as it understood, which is how a
                 * typo becomes a silently different picture. */
                if (end != NULL && *end == ':') {
                    d = strtoul(end + 1, &end, 10);
                }
                if (n == 0 || d == 0 || end == NULL || *end != '\0') {
                    fprintf(stderr, "%s: --aspect %s is not a ratio like"
                                    " 16:9\n", argv[0], spec);
                    return 2;
                }
                want = pc_view_aspect_width((unsigned)n, (unsigned)d);
            } else {
                char *end = NULL;
                long cols = strtol(spec, &end, 10);

                if (end == spec || (end != NULL && *end != '\0')
                    || cols < PC_VIEW_W || cols > (long)PC_VIEW_WIDE_MAX) {
                    fprintf(stderr,
                            "%s: --aspect wants auto, off, a ratio like 16:9,"
                            " or %u..%u columns; got %s\n",
                            argv[0], PC_VIEW_W, PC_VIEW_WIDE_MAX, spec);
                    return 2;
                }
                want = (int)cols;
            }

            if (want != PC_VIEW_W) {
                if (getenv("PC_DIFF_TRACE") != NULL
                    || getenv("PC_DIFF_SPANS") != NULL
                    || getenv("PC_DUMP_FRAMES") != NULL) {
                    fprintf(stderr,
                            "%s: --aspect widens the frustum the box test"
                            " culls against, so it cannot run under"
                            " --diff-trace, --diff-spans or --dump-frames;"
                            " those compare a native picture.\n", argv[0]);
                    return 2;
                }
            }
            pc_view_set_aspect(want);
        }
    }

    /* The state digest, if it was asked for: every mapped guest region hashed
     * here, after the maps and the subsystem inits, before a single
     * instruction of the game, and again however the run ends, fatal signal
     * included. Two runs must agree, and so must a run with the host perturbed;
     * a divergence in guest memory that has not reached a pixel is invisible to
     * the frame dumps and visible here. */
    /* The vector suites, if asked for: after the maps because MI's decompressor
     * vectors write through guest addresses, and before the game because a run
     * that has just proved its own crypto has nothing further to say. */
    if (getenv("PC_SELFTEST") != NULL) {
        return pc_selftest_run(stderr) ? 0 : 1;
    }

    if (getenv("PC_STATE_DIGEST") != NULL) {
        pc_state_report(stderr, "start");
        pc_state_report_at_exit(stderr);
    }

    /*
     * The differential runner. --diff-spans is a question about the build,
     * the memory map, for the oracle to read the same addresses, and is
     * answered here rather than after a boot, because no frame moves a region.
     * --diff-trace opens the trace and takes its `boot` checkpoint at this
     * point, which is the game's entry and NOT where the oracle's `boot` is;
     * pcdiff.py knows that and reports the offset.
     */
    {
        const char *spans = getenv("PC_DIFF_SPANS");
        const char *dtrace = getenv("PC_DIFF_TRACE");

        if (spans != NULL && *spans != '\0') {
            return pc_diff_write_spans(spans) ? 0 : 1;
        }
        if (dtrace != NULL && *dtrace != '\0') {
            const char *every = getenv("PC_DIFF_EVERY");

            pc_diff_open(dtrace, every ? (unsigned)strtoul(every, NULL, 0) : 1);
            pc_state_at_ending(pc_diff_ending, NULL);
        }
    }

    /*
     * --watch: one comma-separated list in PC_WATCH, so several watches remain
     * one input rather than PC_WATCH_1..N. Registered here, after the maps and
     * the subsystem inits, because a guest watch resolved earlier would read an
     * unmapped page, and reported at the ending through the same hook the
     * digest uses, since there can only be one set of signal handlers.
     */
    {
        const char *spec = getenv("PC_WATCH");

        if (spec != NULL && *spec != '\0') {
            char *list = strdup(spec);
            char *p = list;

            while (p != NULL && *p != '\0') {
                char *comma = strchr(p, ',');

                if (comma != NULL) *comma = '\0';
                if (*p != '\0' && !pc_sym_add_watch(p, stderr)) {
                    free(list);
                    return 2;
                }
                p = (comma != NULL) ? comma + 1 : NULL;
            }
            free(list);
        }
        if (pc_sym_watch_count() > 0) {
            pc_state_at_ending(pc_sym_report, stderr);
            pc_sym_report_start(stderr);
        }
    }

    NitroMain();

    /* NitroMain's main loop never returns on hardware; reaching here is
     * itself a fact worth printing. */
    fprintf(stderr, "pokeplatinum-pc: NitroMain returned\n");
    return 0;
}
