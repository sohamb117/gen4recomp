/*
 * The WASI preview1 subset a wasi-libc guest needs to boot and print.
 *
 * The guest has no filesystem: the cartridge and backup chip come through
 * np_host imports, so only the three standard streams exist and there are no
 * preopened directories (fd_prestat_get answers EBADF at fd 3, which is how
 * wasi-libc learns that). stdout/stderr are line-buffered into host.log;
 * stdin is always at end of file. Calls on anything else return the errno a
 * real WASI host would (EBADF, ESPIPE, ENOTDIR, ENOTSUP, EFAULT, EINVAL); the
 * directory calls (path_*, fd_readdir) exist only because wasi-libc's
 * mkdir/stat/opendir pull them into the port's debug paths, and they fail
 * the same way any lookup without a preopen does.
 *
 * random_get is a fixed-seed xorshift64* stream reset per core, because the
 * port is deterministic and replays must not diverge on it. Clocks are real:
 * realtime and monotonic come from the OS; the CPU-time clocks report
 * monotonic time since np_core_create, since every guest thread is a fiber
 * on the shell's thread and that is the only CPU time the guest consumes.
 *
 * All imports are defined whether or not a module uses them (an unused
 * definition costs nothing; a missing one would fail the link).
 */
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "np_imports.h"
#include "np_rt.h"

enum {
    WASI_ESUCCESS = 0,
    WASI_EBADF = 8,
    WASI_EFAULT = 21,
    WASI_EINVAL = 28,
    WASI_ENOTDIR = 54,
    WASI_ENOTSUP = 58,
    WASI_ESPIPE = 70,
};

enum {
    WASI_CLOCK_REALTIME = 0,
    WASI_CLOCK_MONOTONIC = 1,
    WASI_CLOCK_PROCESS_CPUTIME = 2,
    WASI_CLOCK_THREAD_CPUTIME = 3,
};

#define WASI_FILETYPE_CHARACTER_DEVICE 2
#define WASI_RIGHT_FD_READ ((uint64_t)1 << 1)
#define WASI_RIGHT_FD_WRITE ((uint64_t)1 << 6)

#define NP_RNG_SEED 0x9E3779B97F4A7C15ull

static uint64_t now_ns(uint32_t clock) {
#ifdef _WIN32
    if (clock == WASI_CLOCK_REALTIME) {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        uint64_t t = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull;
        return t * 100;
    }
    LARGE_INTEGER count, freq;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&freq);
    uint64_t q = (uint64_t)count.QuadPart, f = (uint64_t)freq.QuadPart;
    return q / f * 1000000000ull + q % f * 1000000000ull / f;
#else
    struct timespec ts;
    clock_gettime(clock == WASI_CLOCK_REALTIME ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

void np_wasi_init(np_core *c) {
    c->std_open[0] = c->std_open[1] = c->std_open[2] = 1;
    c->out_line[0].len = c->out_line[1].len = 0;
    c->rng_state = NP_RNG_SEED;
    c->clock_origin_ns = now_ns(WASI_CLOCK_MONOTONIC);
}

static void flush_line(np_core *c, np_line_buf *b) {
    b->text[b->len] = 0;
    np_rt_log(c, b->text);
    b->len = 0;
}

void np_wasi_flush(np_core *c) {
    for (int i = 0; i < 2; i++)
        if (c->out_line[i].len) flush_line(c, &c->out_line[i]);
}

static int std_fd(np_core *c, uint32_t fd) {
    return fd < 3 && c->std_open[fd];
}

static void put_u32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}
static void put_u64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, 8);
}

/* ---- args and environment ------------------------------------------- */

uint32_t w2c_wasi__snapshot__preview1_args_sizes_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t argc_out,
                                                     uint32_t buf_size_out) {
    np_core *c = w->core;
    uint8_t *pc = np_rt_guest(c, argc_out, 4), *ps = np_rt_guest(c, buf_size_out, 4);
    if (!pc || !ps) return WASI_EFAULT;
    put_u32(pc, 1);
    put_u32(ps, (uint32_t)strlen(c->mod->name) + 1);
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_args_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t argv,
                                               uint32_t argv_buf) {
    np_core *c = w->core;
    uint32_t n = (uint32_t)strlen(c->mod->name) + 1;
    uint8_t *pv = np_rt_guest(c, argv, 4), *pb = np_rt_guest(c, argv_buf, n);
    if (!pv || !pb) return WASI_EFAULT;
    put_u32(pv, argv_buf);
    memcpy(pb, c->mod->name, n);
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_environ_sizes_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t count_out,
                                                        uint32_t buf_size_out) {
    np_core *c = w->core;
    uint8_t *pc = np_rt_guest(c, count_out, 4), *ps = np_rt_guest(c, buf_size_out, 4);
    if (!pc || !ps) return WASI_EFAULT;
    put_u32(pc, c->env_count);
    put_u32(ps, c->env_bytes);
    return WASI_ESUCCESS;
}

/* environ_ptrs, not environ: mingw's <stdlib.h> defines environ as a macro. */
uint32_t w2c_wasi__snapshot__preview1_environ_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t environ_ptrs,
                                                  uint32_t environ_buf) {
    np_core *c = w->core;
    uint8_t *pv = np_rt_guest(c, environ_ptrs, c->env_count * 4), *pb = np_rt_guest(c, environ_buf, c->env_bytes);
    if (!pv || !pb) return WASI_EFAULT;
    if (c->env_bytes) memcpy(pb, c->env_block, c->env_bytes);
    uint32_t off = 0;
    for (uint32_t i = 0; i < c->env_count; i++) {
        put_u32(pv + 4 * i, environ_buf + off);
        off += (uint32_t)strlen(c->env_block + off) + 1;
    }
    return WASI_ESUCCESS;
}

/* ---- clocks, randomness, scheduling, exit ---------------------------- */

uint32_t w2c_wasi__snapshot__preview1_clock_res_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t id,
                                                    uint32_t out) {
    uint8_t *p = np_rt_guest(w->core, out, 8);
    if (!p) return WASI_EFAULT;
    if (id > WASI_CLOCK_THREAD_CPUTIME) return WASI_EINVAL;
    put_u64(p, 1);
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_clock_time_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t id,
                                                     uint64_t precision, uint32_t out) {
    (void)precision;
    np_core *c = w->core;
    uint8_t *p = np_rt_guest(c, out, 8);
    if (!p) return WASI_EFAULT;
    switch (id) {
    case WASI_CLOCK_REALTIME:
    case WASI_CLOCK_MONOTONIC:
        put_u64(p, now_ns(id));
        return WASI_ESUCCESS;
    case WASI_CLOCK_PROCESS_CPUTIME:
    case WASI_CLOCK_THREAD_CPUTIME:
        put_u64(p, now_ns(WASI_CLOCK_MONOTONIC) - c->clock_origin_ns);
        return WASI_ESUCCESS;
    default:
        return WASI_EINVAL;
    }
}

uint32_t w2c_wasi__snapshot__preview1_random_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t buf,
                                                 uint32_t len) {
    np_core *c = w->core;
    uint8_t *p = np_rt_guest(c, buf, len);
    if (!p) return WASI_EFAULT;
    uint64_t x = c->rng_state;
    for (uint32_t i = 0; i < len; i += 8) {
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        uint64_t v = x * 0x2545F4914F6CDD1Dull;
        uint32_t n = len - i < 8 ? len - i : 8;
        memcpy(p + i, &v, n);
    }
    c->rng_state = x;
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_sched_yield(struct w2c_wasi__snapshot__preview1 *w) {
    (void)w;
    return WASI_ESUCCESS;
}

void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1 *w, uint32_t code) {
    np_rt_exit(w->core, (int)code);
}

/* ---- file descriptors ------------------------------------------------ */

uint32_t w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd) {
    np_core *c = w->core;
    if (!std_fd(c, fd)) return WASI_EBADF;
    if (fd > 0 && c->out_line[fd - 1].len) flush_line(c, &c->out_line[fd - 1]);
    c->std_open[fd] = 0;
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_fd_fdstat_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd,
                                                    uint32_t out) {
    np_core *c = w->core;
    if (!std_fd(c, fd)) return WASI_EBADF;
    uint8_t *p = np_rt_guest(c, out, 24);
    if (!p) return WASI_EFAULT;
    memset(p, 0, 24);
    p[0] = WASI_FILETYPE_CHARACTER_DEVICE; /* fs_filetype; fs_flags at 2 stay 0 */
    put_u64(p + 8, fd == 0 ? WASI_RIGHT_FD_READ : WASI_RIGHT_FD_WRITE);
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_fd_prestat_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd,
                                                     uint32_t out) {
    (void)w, (void)fd, (void)out;
    return WASI_EBADF; /* no preopened directories */
}

uint32_t w2c_wasi__snapshot__preview1_fd_prestat_dir_name(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd,
                                                          uint32_t path, uint32_t path_len) {
    (void)w, (void)fd, (void)path, (void)path_len;
    return WASI_EBADF;
}

uint32_t w2c_wasi__snapshot__preview1_fd_seek(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint64_t offset,
                                              uint32_t whence, uint32_t newoffset_out) {
    (void)offset, (void)whence, (void)newoffset_out;
    return std_fd(w->core, fd) ? WASI_ESPIPE : WASI_EBADF;
}

uint32_t w2c_wasi__snapshot__preview1_path_open(struct w2c_wasi__snapshot__preview1 *w, uint32_t dirfd,
                                                uint32_t dirflags, uint32_t path, uint32_t path_len, uint32_t oflags,
                                                uint64_t rights_base, uint64_t rights_inheriting, uint32_t fdflags,
                                                uint32_t fd_out) {
    (void)dirflags, (void)path, (void)path_len, (void)oflags, (void)rights_base, (void)rights_inheriting;
    (void)fdflags, (void)fd_out;
    return std_fd(w->core, dirfd) ? WASI_ENOTDIR : WASI_EBADF;
}

uint32_t w2c_wasi__snapshot__preview1_path_create_directory(struct w2c_wasi__snapshot__preview1 *w, uint32_t dirfd,
                                                            uint32_t path, uint32_t path_len) {
    (void)path, (void)path_len;
    return std_fd(w->core, dirfd) ? WASI_ENOTDIR : WASI_EBADF;
}

uint32_t w2c_wasi__snapshot__preview1_path_filestat_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t dirfd,
                                                        uint32_t flags, uint32_t path, uint32_t path_len,
                                                        uint32_t out) {
    (void)flags, (void)path, (void)path_len, (void)out;
    return std_fd(w->core, dirfd) ? WASI_ENOTDIR : WASI_EBADF;
}

uint32_t w2c_wasi__snapshot__preview1_fd_readdir(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t buf,
                                                 uint32_t buf_len, uint64_t cookie, uint32_t bufused_out) {
    (void)buf, (void)buf_len, (void)cookie, (void)bufused_out;
    return std_fd(w->core, fd) ? WASI_ENOTDIR : WASI_EBADF;
}

/* The standard streams are character devices with no settable flags. */
uint32_t w2c_wasi__snapshot__preview1_fd_fdstat_set_flags(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd,
                                                          uint32_t flags) {
    if (!std_fd(w->core, fd)) return WASI_EBADF;
    return flags == 0 ? WASI_ESUCCESS : WASI_ENOTSUP;
}

/* Validates an iovec array; returns its host address or NULL. */
static const uint8_t *iovecs(np_core *c, uint32_t iovs, uint32_t n) {
    if (n > (1u << 20)) return NULL;
    return np_rt_guest(c, iovs, n * 8);
}

uint32_t w2c_wasi__snapshot__preview1_fd_read(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t iovs,
                                              uint32_t iovs_len, uint32_t nread_out) {
    np_core *c = w->core;
    if (fd != 0 || !std_fd(c, fd)) return WASI_EBADF;
    uint8_t *pn = np_rt_guest(c, nread_out, 4);
    if (!iovecs(c, iovs, iovs_len) || !pn) return WASI_EFAULT;
    put_u32(pn, 0); /* stdin is always at end of file */
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_fd_write(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t iovs,
                                               uint32_t iovs_len, uint32_t nwritten_out) {
    np_core *c = w->core;
    if (fd == 0 || !std_fd(c, fd)) return WASI_EBADF;
    const uint8_t *iov = iovecs(c, iovs, iovs_len);
    uint8_t *pn = np_rt_guest(c, nwritten_out, 4);
    if (!iov || !pn) return WASI_EFAULT;
    np_line_buf *b = &c->out_line[fd - 1];
    uint32_t total = 0;
    for (uint32_t i = 0; i < iovs_len; i++) {
        uint32_t base, len;
        memcpy(&base, iov + 8 * i, 4);
        memcpy(&len, iov + 8 * i + 4, 4);
        const uint8_t *s = np_rt_guest(c, base, len);
        if (!s) return WASI_EFAULT;
        for (uint32_t j = 0; j < len; j++) {
            if (s[j] == '\n') {
                flush_line(c, b);
                continue;
            }
            b->text[b->len++] = (char)s[j];
            if (b->len == sizeof b->text - 1) flush_line(c, b);
        }
        total += len;
    }
    put_u32(pn, total);
    return WASI_ESUCCESS;
}
