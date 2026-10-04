/*
 * The WASI preview1 subset a wasi-libc guest needs to boot, print and read
 * runtime content.
 *
 * The cartridge and backup chip come through np_host imports, not files.
 * stdout/stderr are line-buffered into host.log; stdin is always at end of
 * file. Without np_host.content_root that is all: there are no preopened
 * directories (fd_prestat_get answers EBADF at fd 3, which is how wasi-libc
 * learns that), and the directory calls wasi-libc's mkdir/stat/opendir pull
 * into the port's debug paths fail the way any lookup without a preopen
 * does.
 *
 * With a content root, fd 3 is a read-only preopen named NP_CONTENT_DIR
 * ("/content") over that host directory, and the guest can open, read,
 * pread, seek, stat and list what is inside it (mod packages). Containment
 * is checked on every lookup, not trusted to the guest: the guest's path
 * must be relative, without "..", '\\' or ':' components (".", empty and
 * repeated separators are dropped), and the host path it names is resolved
 * by the OS (realpath on POSIX; the final path of the opened handle on
 * Windows) and must still lie inside the resolved root, so a symlink or
 * junction is followed only if it lands inside. Anything that would write
 * (O_CREAT/O_TRUNC/O_EXCL, write or append rights, mkdir) is EROFS.
 * Directory listings leave out "." and ".." and any link that resolves
 * outside. Open content fds are host state: a snapshot load does not
 * change them (the guest's own FILE state rolls back with its memory).
 *
 * Other calls return the errno a real WASI host would (EBADF, ESPIPE,
 * ENOTDIR, ENOTSUP, EFAULT, EINVAL, ...).
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
#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1 /* glibc: realpath, pread, O_CLOEXEC, d_type */
#define _DARWIN_C_SOURCE 1
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

#include "np_imports.h"
#include "np_rt.h"

enum {
    WASI_ESUCCESS = 0,
    WASI_EACCES = 2,
    WASI_EBADF = 8,
    WASI_EFAULT = 21,
    WASI_EINVAL = 28,
    WASI_EIO = 29,
    WASI_EISDIR = 31,
    WASI_ELOOP = 32,
    WASI_EMFILE = 33,
    WASI_ENAMETOOLONG = 37,
    WASI_ENOENT = 44,
    WASI_ENOMEM = 48,
    WASI_ENOTDIR = 54,
    WASI_ENOTSUP = 58,
    WASI_EROFS = 69,
    WASI_ESPIPE = 70,
    WASI_ENOTCAPABLE = 76,
};

enum {
    WASI_CLOCK_REALTIME = 0,
    WASI_CLOCK_MONOTONIC = 1,
    WASI_CLOCK_PROCESS_CPUTIME = 2,
    WASI_CLOCK_THREAD_CPUTIME = 3,
};

enum {
    WASI_FILETYPE_UNKNOWN = 0,
    WASI_FILETYPE_BLOCK_DEVICE = 1,
    WASI_FILETYPE_CHARACTER_DEVICE = 2,
    WASI_FILETYPE_DIRECTORY = 3,
    WASI_FILETYPE_REGULAR_FILE = 4,
};

enum { WASI_WHENCE_SET = 0, WASI_WHENCE_CUR = 1, WASI_WHENCE_END = 2 };
enum { WASI_OFLAGS_CREAT = 1, WASI_OFLAGS_DIRECTORY = 2, WASI_OFLAGS_EXCL = 4, WASI_OFLAGS_TRUNC = 8 };
enum { WASI_FDFLAGS_APPEND = 1, WASI_FDFLAGS_DSYNC = 2, WASI_FDFLAGS_SYNC = 16 };

#define WASI_RIGHT(n) ((uint64_t)1 << (n))
#define WASI_RIGHT_FD_DATASYNC WASI_RIGHT(0)
#define WASI_RIGHT_FD_READ WASI_RIGHT(1)
#define WASI_RIGHT_FD_SEEK WASI_RIGHT(2)
#define WASI_RIGHT_FD_FDSTAT_SET_FLAGS WASI_RIGHT(3)
#define WASI_RIGHT_FD_TELL WASI_RIGHT(5)
#define WASI_RIGHT_FD_WRITE WASI_RIGHT(6)
#define WASI_RIGHT_FD_ADVISE WASI_RIGHT(7)
#define WASI_RIGHT_FD_ALLOCATE WASI_RIGHT(8)
#define WASI_RIGHT_PATH_OPEN WASI_RIGHT(13)
#define WASI_RIGHT_FD_READDIR WASI_RIGHT(14)
#define WASI_RIGHT_PATH_READLINK WASI_RIGHT(15)
#define WASI_RIGHT_PATH_FILESTAT_GET WASI_RIGHT(18)
#define WASI_RIGHT_FD_FILESTAT_GET WASI_RIGHT(21)
#define WASI_RIGHT_FD_FILESTAT_SET_SIZE WASI_RIGHT(22)
#define WASI_RIGHT_POLL_FD_READWRITE WASI_RIGHT(27)
#define WASI_RIGHTS_ALL (WASI_RIGHT(29) - 1)

/* What a content file and a content directory grant. Directories advertise
 * every right as inheritable so that wasi-libc, which masks the rights it
 * asks path_open for by them, shows a write intent to path_open, which
 * refuses it with EROFS instead of handing out an fd that fails later. */
#define NP_FILE_RIGHTS                                                                                          \
    (WASI_RIGHT_FD_READ | WASI_RIGHT_FD_SEEK | WASI_RIGHT_FD_TELL | WASI_RIGHT_FD_ADVISE |                       \
     WASI_RIGHT_FD_FILESTAT_GET | WASI_RIGHT_FD_FDSTAT_SET_FLAGS | WASI_RIGHT_POLL_FD_READWRITE)
#define NP_DIR_RIGHTS                                                                                           \
    (WASI_RIGHT_PATH_OPEN | WASI_RIGHT_FD_READDIR | WASI_RIGHT_PATH_READLINK | WASI_RIGHT_PATH_FILESTAT_GET |   \
     WASI_RIGHT_FD_FILESTAT_GET | WASI_RIGHT_FD_FDSTAT_SET_FLAGS)
#define NP_WRITE_RIGHTS                                                                                         \
    (WASI_RIGHT_FD_DATASYNC | WASI_RIGHT_FD_WRITE | WASI_RIGHT_FD_ALLOCATE | WASI_RIGHT_FD_FILESTAT_SET_SIZE)

/* The longest guest path accepted, in bytes. */
#define NP_WASI_PATH_MAX 1024u

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

/* ---- content files: host side ---------------------------------------- */

/* What filestat reports, gathered from either OS. */
typedef struct np_hstat {
    uint64_t dev, ino, nlink, size, atim, mtim, ctim;
    uint8_t type;
} np_hstat;

static np_wasi_file *content_fd(np_core *c, uint32_t fd) {
    if (fd < NP_WASI_FIRST_FD || fd - NP_WASI_FIRST_FD >= NP_WASI_MAX_FILES) return NULL;
    np_wasi_file *f = &c->files[fd - NP_WASI_FIRST_FD];
    return f->kind != NP_WASI_FREE ? f : NULL;
}

/* A nonzero inode substitute for entries whose OS gives none. */
static uint64_t name_ino(const char *name) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (; *name; name++) h = (h ^ (uint8_t)*name) * 0x100000001B3ull;
    return h | 1;
}

#ifdef _WIN32
#define NP_SEP '\\'

static uint32_t os_error(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
        return WASI_ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return WASI_EACCES;
    case ERROR_DIRECTORY: return WASI_ENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE: return WASI_ENAMETOOLONG;
    case ERROR_CANT_RESOLVE_FILENAME: return WASI_ELOOP;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return WASI_ENOMEM;
    default: return WASI_EIO;
    }
}

static wchar_t *to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    return w;
}

static char *to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = malloc((size_t)n);
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) != n) {
        free(s);
        return NULL;
    }
    return s;
}

/* Opens `path` (UTF-8) for reading, following links; directories too. */
static HANDLE open_handle(const char *path, DWORD access) {
    wchar_t *w = to_wide(path);
    if (!w) {
        SetLastError(ERROR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    return h;
}

/* Where an open handle really is, after every link and junction. */
static char *final_path(HANDLE h) {
    DWORD n = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (!w) return NULL;
    DWORD got = GetFinalPathNameByHandleW(h, w, n, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    char *s = got > 0 && got < n ? to_utf8(w) : NULL;
    free(w);
    return s;
}

static uint64_t filetime_ns(FILETIME t) {
    uint64_t v = (uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime;
    return v < 116444736000000000ull ? 0 : (v - 116444736000000000ull) * 100;
}

static uint32_t handle_stat(HANDLE h, np_hstat *st) {
    BY_HANDLE_FILE_INFORMATION fi;
    if (!GetFileInformationByHandle(h, &fi)) return os_error(GetLastError());
    st->dev = fi.dwVolumeSerialNumber;
    st->ino = (uint64_t)fi.nFileIndexHigh << 32 | fi.nFileIndexLow;
    st->nlink = fi.nNumberOfLinks;
    st->type = (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? WASI_FILETYPE_DIRECTORY : WASI_FILETYPE_REGULAR_FILE;
    st->size = st->type == WASI_FILETYPE_DIRECTORY ? 0 : (uint64_t)fi.nFileSizeHigh << 32 | fi.nFileSizeLow;
    st->atim = filetime_ns(fi.ftLastAccessTime);
    st->mtim = filetime_ns(fi.ftLastWriteTime);
    st->ctim = filetime_ns(fi.ftLastWriteTime);
    return WASI_ESUCCESS;
}
#else
#define NP_SEP '/'

static uint32_t os_error(int e) {
    switch (e) {
    case ENOENT: return WASI_ENOENT;
    case ENOTDIR: return WASI_ENOTDIR;
    case EACCES:
    case EPERM:
        return WASI_EACCES;
    case ELOOP: return WASI_ELOOP;
    case ENAMETOOLONG: return WASI_ENAMETOOLONG;
    case EISDIR: return WASI_EISDIR;
    case EMFILE:
    case ENFILE:
        return WASI_EMFILE;
    case ENOMEM: return WASI_ENOMEM;
    default: return WASI_EIO;
    }
}

#if defined(__APPLE__)
#define NP_ST_NS(st, x) ((uint64_t)(st).st_##x##timespec.tv_sec * 1000000000ull + (uint64_t)(st).st_##x##timespec.tv_nsec)
#else
#define NP_ST_NS(st, x) ((uint64_t)(st).st_##x##tim.tv_sec * 1000000000ull + (uint64_t)(st).st_##x##tim.tv_nsec)
#endif

static void from_stat(const struct stat *s, np_hstat *st) {
    st->dev = (uint64_t)s->st_dev;
    st->ino = (uint64_t)s->st_ino;
    st->nlink = (uint64_t)s->st_nlink;
    st->size = (uint64_t)s->st_size;
    st->type = S_ISDIR(s->st_mode)   ? WASI_FILETYPE_DIRECTORY
               : S_ISREG(s->st_mode) ? WASI_FILETYPE_REGULAR_FILE
               : S_ISCHR(s->st_mode) ? WASI_FILETYPE_CHARACTER_DEVICE
               : S_ISBLK(s->st_mode) ? WASI_FILETYPE_BLOCK_DEVICE
                                     : WASI_FILETYPE_UNKNOWN;
    st->atim = NP_ST_NS(*s, a);
    st->mtim = NP_ST_NS(*s, m);
    st->ctim = NP_ST_NS(*s, c);
}
#endif

/* Whether resolved host path `p` is the content root or below it. */
static int inside_root(const np_core *c, const char *p) {
    size_t n = strlen(c->content_root);
    if (strncmp(p, c->content_root, n) != 0) return 0;
    return p[n] == 0 || p[n] == NP_SEP || (n > 0 && c->content_root[n - 1] == NP_SEP);
}

/*
 * The guest's path, checked and normalised into `out` ("a/b", or "" for the
 * directory itself): relative, no NUL, no "..", no '\\' or ':' (separators
 * and drive or stream syntax on Windows, refused everywhere so a package
 * behaves the same on every host); "." and empty components dropped.
 */
static uint32_t guest_relpath(np_core *c, uint32_t path, uint32_t len, char *out, size_t cap) {
    if (len >= NP_WASI_PATH_MAX) return WASI_ENAMETOOLONG;
    const char *s = (const char *)np_rt_guest(c, path, len);
    if (!s) return WASI_EFAULT;
    if (len > 0 && s[0] == '/') return WASI_ENOTCAPABLE;
    size_t n = 0;
    for (uint32_t i = 0; i < len;) {
        uint32_t j = i;
        while (j < len && s[j] != '/') j++;
        const char *comp = s + i;
        const uint32_t clen = j - i;
        i = j + 1;
        if (clen == 0 || (clen == 1 && comp[0] == '.')) continue;
        if (clen == 2 && comp[0] == '.' && comp[1] == '.') return WASI_ENOTCAPABLE;
        for (uint32_t k = 0; k < clen; k++) {
            if (comp[k] == 0) return WASI_EINVAL;
            if (comp[k] == '\\' || comp[k] == ':') return WASI_ENOTCAPABLE;
        }
        if (n + clen + 2 > cap) return WASI_ENAMETOOLONG;
        if (n) out[n++] = NP_SEP;
        memcpy(out + n, comp, clen);
        n += clen;
    }
    out[n] = 0;
    return WASI_ESUCCESS;
}

/* dir/rel as the OS resolves it, if that is inside the root: 0 and a
 * malloc'd *out, or a WASI errno (ENOTCAPABLE for an escape). */
static uint32_t resolve(np_core *c, const char *dir, const char *rel, char **out) {
    size_t n = strlen(dir) + strlen(rel) + 2;
    char *joined = malloc(n);
    if (!joined) return WASI_ENOMEM;
    snprintf(joined, n, rel[0] ? "%s%c%s" : "%s", dir, NP_SEP, rel);
#ifdef _WIN32
    HANDLE h = open_handle(joined, FILE_READ_ATTRIBUTES);
    free(joined);
    if (h == INVALID_HANDLE_VALUE) return os_error(GetLastError());
    char *real = final_path(h);
    CloseHandle(h);
    if (!real) return WASI_EIO;
#else
    char *real = realpath(joined, NULL);
    const int e = errno;
    free(joined);
    if (!real) return os_error(e);
#endif
    if (!inside_root(c, real)) {
        free(real);
        return WASI_ENOTCAPABLE;
    }
    *out = real;
    return WASI_ESUCCESS;
}

static uint32_t path_stat(const char *real, np_hstat *st) {
#ifdef _WIN32
    HANDLE h = open_handle(real, FILE_READ_ATTRIBUTES);
    if (h == INVALID_HANDLE_VALUE) return os_error(GetLastError());
    uint32_t r = handle_stat(h, st);
    CloseHandle(h);
    return r;
#else
    struct stat s;
    if (stat(real, &s) != 0) return os_error(errno);
    from_stat(&s, st);
    return WASI_ESUCCESS;
#endif
}

static uint32_t file_stat(const np_wasi_file *f, np_hstat *st) {
#ifdef _WIN32
    return handle_stat((HANDLE)f->handle, st);
#else
    struct stat s;
    if (fstat(f->fd, &s) != 0) return os_error(errno);
    from_stat(&s, st);
    return WASI_ESUCCESS;
#endif
}

/* Opens resolved path `real` into `f`; takes ownership of `real`. */
static uint32_t file_open(char *real, np_wasi_file *f) {
    np_hstat st;
#ifdef _WIN32
    HANDLE h = open_handle(real, GENERIC_READ);
    if (h == INVALID_HANDLE_VALUE) {
        free(real);
        return os_error(GetLastError());
    }
    f->handle = h;
#else
    int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        free(real);
        return os_error(errno);
    }
    f->fd = fd;
#endif
    f->path = real;
    f->pos = 0;
    f->preopen = 0;
    uint32_t r = file_stat(f, &st);
    if (r == WASI_ESUCCESS && st.type != WASI_FILETYPE_DIRECTORY && st.type != WASI_FILETYPE_REGULAR_FILE)
        r = WASI_EACCES; /* devices, sockets, fifos: not content */
    f->kind = st.type == WASI_FILETYPE_DIRECTORY ? NP_WASI_DIRECTORY : NP_WASI_REGULAR;
    if (r != WASI_ESUCCESS) {
        np_wasi_file dead = *f;
        memset(f, 0, sizeof *f);
#ifdef _WIN32
        CloseHandle((HANDLE)dead.handle);
#else
        close(dead.fd);
#endif
        free(dead.path);
    }
    return r;
}

static void file_close(np_wasi_file *f) {
#ifdef _WIN32
    CloseHandle((HANDLE)f->handle);
#else
    close(f->fd);
#endif
    free(f->path);
    memset(f, 0, sizeof *f);
}

/* Reads at `offset`; returns bytes read (short only at end of file) or -1. */
static int64_t file_pread(np_wasi_file *f, uint8_t *dst, uint32_t len, uint64_t offset) {
    uint32_t done = 0;
    while (done < len) {
#ifdef _WIN32
        OVERLAPPED ov = {0};
        const uint64_t at = offset + done;
        ov.Offset = (DWORD)at;
        ov.OffsetHigh = (DWORD)(at >> 32);
        DWORD got = 0;
        if (!ReadFile((HANDLE)f->handle, dst + done, len - done, &got, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return -1;
        }
#else
        ssize_t got = pread(f->fd, dst + done, len - done, (off_t)(offset + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
#endif
        if (got == 0) break;
        done += (uint32_t)got;
    }
    return done;
}

/* The type a listing reports for `name` in directory `dir`, following a
 * link; 0 to leave the entry out (a link that escapes, or vanished). */
static int entry_type(np_core *c, const char *dir, const char *name, int is_dir, int is_reg, uint8_t *type) {
    if (is_dir || is_reg) {
        *type = is_dir ? WASI_FILETYPE_DIRECTORY : WASI_FILETYPE_REGULAR_FILE;
        return 1;
    }
    char *real = NULL;
    np_hstat st;
    if (resolve(c, dir, name, &real) != WASI_ESUCCESS) return 0;
    const uint32_t r = path_stat(real, &st);
    free(real);
    if (r != WASI_ESUCCESS) return 0;
    *type = st.type;
    return 1;
}

/* Appends one dirent (24-byte header, then the name, both possibly cut
 * short by the end of the buffer, as WASI allows). */
static void put_dirent(uint8_t *buf, uint32_t cap, uint32_t *used, uint64_t next, uint64_t ino, const char *name,
                       uint8_t type) {
    uint8_t head[24] = {0};
    const uint32_t namlen = (uint32_t)strlen(name);
    put_u64(head, next);
    put_u64(head + 8, ino);
    put_u32(head + 16, namlen);
    head[20] = type;
    uint32_t n = cap - *used < 24 ? cap - *used : 24;
    memcpy(buf + *used, head, n);
    *used += n;
    n = cap - *used < namlen ? cap - *used : namlen;
    memcpy(buf + *used, name, n);
    *used += n;
}

/* Entries from index `cookie` on ("." and ".." are not listed); the cookie
 * of an entry is its index + 1. */
static uint32_t dir_list(np_core *c, const np_wasi_file *d, uint8_t *buf, uint32_t cap, uint64_t cookie,
                         uint32_t *used) {
    uint64_t index = 0;
    uint8_t type;
    *used = 0;
#ifdef _WIN32
    size_t n = strlen(d->path) + 3;
    char *pattern = malloc(n);
    if (!pattern) return WASI_ENOMEM;
    snprintf(pattern, n, "%s\\*", d->path);
    wchar_t *wp = to_wide(pattern);
    free(pattern);
    if (!wp) return WASI_ENOMEM;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(wp, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND ? WASI_ESUCCESS : os_error(e);
    }
    do {
        char *name = to_utf8(fd.cFileName);
        if (!name) continue;
        const int link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        const int is_dir = !link && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && entry_type(c, d->path, name, is_dir, !link && !is_dir, &type)) {
            if (index++ >= cookie) put_dirent(buf, cap, used, index, name_ino(name), name, type);
        }
        free(name);
    } while (*used < cap && FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *dir = opendir(d->path);
    if (!dir) return os_error(errno);
    struct dirent *e;
    while (*used < cap && (e = readdir(dir)) != NULL) {
        const char *name = e->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (!entry_type(c, d->path, name, e->d_type == DT_DIR, e->d_type == DT_REG, &type)) continue;
        if (index++ < cookie) continue;
        put_dirent(buf, cap, used, index, e->d_ino ? (uint64_t)e->d_ino : name_ino(name), name, type);
    }
    closedir(dir);
#endif
    return WASI_ESUCCESS;
}

int np_wasi_open_content(np_core *c, const char *root) {
    np_wasi_file *f = &c->files[0];
    char *real = NULL;
#ifdef _WIN32
    HANDLE h = open_handle(root, FILE_READ_ATTRIBUTES);
    if (h != INVALID_HANDLE_VALUE) {
        real = final_path(h);
        CloseHandle(h);
    }
#else
    real = realpath(root, NULL);
#endif
    if (!real) {
        snprintf(c->error, sizeof c->error, "content_root %s: not found", root);
        return -1;
    }
    c->content_root = malloc(strlen(real) + 1);
    if (!c->content_root) {
        free(real);
        snprintf(c->error, sizeof c->error, "out of memory");
        return -1;
    }
    strcpy(c->content_root, real);
    if (file_open(real, f) != WASI_ESUCCESS || f->kind != NP_WASI_DIRECTORY) {
        if (f->kind) file_close(f);
        snprintf(c->error, sizeof c->error, "content_root %s: not a readable directory", root);
        return -1;
    }
    f->preopen = 1;
    return 0;
}

void np_wasi_close_files(np_core *c) {
    for (uint32_t i = 0; i < NP_WASI_MAX_FILES; i++)
        if (c->files[i].kind != NP_WASI_FREE) file_close(&c->files[i]);
    free(c->content_root);
    c->content_root = NULL;
}

/* ---- file descriptors ------------------------------------------------ */

uint32_t w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd) {
    np_core *c = w->core;
    np_wasi_file *f = content_fd(c, fd);
    if (f) {
        file_close(f);
        return WASI_ESUCCESS;
    }
    if (!std_fd(c, fd)) return WASI_EBADF;
    if (fd > 0 && c->out_line[fd - 1].len) flush_line(c, &c->out_line[fd - 1]);
    c->std_open[fd] = 0;
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_fd_fdstat_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd,
                                                    uint32_t out) {
    np_core *c = w->core;
    np_wasi_file *f = content_fd(c, fd);
    if (!f && !std_fd(c, fd)) return WASI_EBADF;
    uint8_t *p = np_rt_guest(c, out, 24);
    if (!p) return WASI_EFAULT;
    memset(p, 0, 24); /* fs_flags at 2 stay 0 */
    if (!f) {
        p[0] = WASI_FILETYPE_CHARACTER_DEVICE;
        put_u64(p + 8, fd == 0 ? WASI_RIGHT_FD_READ : WASI_RIGHT_FD_WRITE);
    } else if (f->kind == NP_WASI_DIRECTORY) {
        p[0] = WASI_FILETYPE_DIRECTORY;
        put_u64(p + 8, NP_DIR_RIGHTS);
        put_u64(p + 16, WASI_RIGHTS_ALL);
    } else {
        p[0] = WASI_FILETYPE_REGULAR_FILE;
        put_u64(p + 8, NP_FILE_RIGHTS);
    }
    return WASI_ESUCCESS;
}

static uint32_t put_filestat(np_core *c, uint32_t out, const np_hstat *st) {
    uint8_t *p = np_rt_guest(c, out, 64);
    if (!p) return WASI_EFAULT;
    memset(p, 0, 64);
    put_u64(p, st->dev);
    put_u64(p + 8, st->ino);
    p[16] = st->type;
    put_u64(p + 24, st->nlink);
    put_u64(p + 32, st->size);
    put_u64(p + 40, st->atim);
    put_u64(p + 48, st->mtim);
    put_u64(p + 56, st->ctim);
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_fd_filestat_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd,
                                                      uint32_t out) {
    np_core *c = w->core;
    np_wasi_file *f = content_fd(c, fd);
    np_hstat st = {0};
    if (f) {
        uint32_t r = file_stat(f, &st);
        if (r != WASI_ESUCCESS) return r;
    } else if (std_fd(c, fd)) {
        st.type = WASI_FILETYPE_CHARACTER_DEVICE;
        st.nlink = 1;
    } else {
        return WASI_EBADF;
    }
    return put_filestat(c, out, &st);
}

uint32_t w2c_wasi__snapshot__preview1_fd_prestat_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd,
                                                     uint32_t out) {
    np_core *c = w->core;
    np_wasi_file *f = content_fd(c, fd);
    if (!f || !f->preopen) return WASI_EBADF; /* the only preopen is the content root */
    uint8_t *p = np_rt_guest(c, out, 8);
    if (!p) return WASI_EFAULT;
    memset(p, 0, 8);
    p[0] = 0; /* preopentype dir */
    put_u32(p + 4, (uint32_t)strlen(NP_CONTENT_DIR));
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_fd_prestat_dir_name(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd,
                                                          uint32_t path, uint32_t path_len) {
    np_core *c = w->core;
    np_wasi_file *f = content_fd(c, fd);
    if (!f || !f->preopen) return WASI_EBADF;
    const uint32_t n = (uint32_t)strlen(NP_CONTENT_DIR);
    if (path_len < n) return WASI_EINVAL;
    uint8_t *p = np_rt_guest(c, path, n);
    if (!p) return WASI_EFAULT;
    memcpy(p, NP_CONTENT_DIR, n);
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_fd_seek(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint64_t offset,
                                              uint32_t whence, uint32_t newoffset_out) {
    np_core *c = w->core;
    np_wasi_file *f = content_fd(c, fd);
    if (!f) return std_fd(c, fd) ? WASI_ESPIPE : WASI_EBADF;
    if (f->kind != NP_WASI_REGULAR) return WASI_EBADF;
    uint8_t *p = np_rt_guest(c, newoffset_out, 8);
    if (!p) return WASI_EFAULT;
    int64_t base;
    if (whence == WASI_WHENCE_SET) {
        base = 0;
    } else if (whence == WASI_WHENCE_CUR) {
        base = (int64_t)f->pos;
    } else if (whence == WASI_WHENCE_END) {
        np_hstat st;
        uint32_t r = file_stat(f, &st);
        if (r != WASI_ESUCCESS) return r;
        base = (int64_t)st.size;
    } else {
        return WASI_EINVAL;
    }
    const int64_t delta = (int64_t)offset;
    if ((delta < 0 && base + delta < 0) || (delta > 0 && base > INT64_MAX - delta)) return WASI_EINVAL;
    f->pos = (uint64_t)(base + delta);
    put_u64(p, f->pos);
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_fd_tell(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd,
                                              uint32_t offset_out) {
    np_core *c = w->core;
    np_wasi_file *f = content_fd(c, fd);
    if (!f) return std_fd(c, fd) ? WASI_ESPIPE : WASI_EBADF;
    if (f->kind != NP_WASI_REGULAR) return WASI_EBADF;
    uint8_t *p = np_rt_guest(c, offset_out, 8);
    if (!p) return WASI_EFAULT;
    put_u64(p, f->pos);
    return WASI_ESUCCESS;
}

/* The directory fd a path call starts from, or the errno for `dirfd`. */
static uint32_t lookup_dir(np_core *c, uint32_t dirfd, np_wasi_file **out) {
    np_wasi_file *d = content_fd(c, dirfd);
    if (!d) return std_fd(c, dirfd) ? WASI_ENOTDIR : WASI_EBADF;
    if (d->kind != NP_WASI_DIRECTORY) return WASI_ENOTDIR;
    *out = d;
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_path_open(struct w2c_wasi__snapshot__preview1 *w, uint32_t dirfd,
                                                uint32_t dirflags, uint32_t path, uint32_t path_len, uint32_t oflags,
                                                uint64_t rights_base, uint64_t rights_inheriting, uint32_t fdflags,
                                                uint32_t fd_out) {
    np_core *c = w->core;
    (void)dirflags, (void)rights_inheriting; /* links are followed, inside the root only */
    np_wasi_file *d;
    uint32_t r = lookup_dir(c, dirfd, &d);
    if (r != WASI_ESUCCESS) return r;
    uint8_t *po = np_rt_guest(c, fd_out, 4);
    if (!po) return WASI_EFAULT;
    char rel[NP_WASI_PATH_MAX + 2];
    r = guest_relpath(c, path, path_len, rel, sizeof rel);
    if (r != WASI_ESUCCESS) return r;
    if ((oflags & (WASI_OFLAGS_CREAT | WASI_OFLAGS_EXCL | WASI_OFLAGS_TRUNC)) || (rights_base & NP_WRITE_RIGHTS) ||
        (fdflags & (WASI_FDFLAGS_APPEND | WASI_FDFLAGS_DSYNC | WASI_FDFLAGS_SYNC)))
        return WASI_EROFS;
    uint32_t slot = 1;
    while (slot < NP_WASI_MAX_FILES && c->files[slot].kind != NP_WASI_FREE) slot++;
    if (slot == NP_WASI_MAX_FILES) return WASI_EMFILE;
    char *real;
    r = resolve(c, d->path, rel, &real);
    if (r != WASI_ESUCCESS) return r;
    np_wasi_file *f = &c->files[slot];
    r = file_open(real, f);
    if (r != WASI_ESUCCESS) return r;
    if ((oflags & WASI_OFLAGS_DIRECTORY) && f->kind != NP_WASI_DIRECTORY) {
        file_close(f);
        return WASI_ENOTDIR;
    }
    put_u32(po, NP_WASI_FIRST_FD + slot);
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_path_create_directory(struct w2c_wasi__snapshot__preview1 *w, uint32_t dirfd,
                                                            uint32_t path, uint32_t path_len) {
    np_core *c = w->core;
    np_wasi_file *d;
    (void)path, (void)path_len;
    uint32_t r = lookup_dir(c, dirfd, &d);
    return r != WASI_ESUCCESS ? r : WASI_EROFS;
}

uint32_t w2c_wasi__snapshot__preview1_path_filestat_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t dirfd,
                                                        uint32_t flags, uint32_t path, uint32_t path_len,
                                                        uint32_t out) {
    np_core *c = w->core;
    (void)flags; /* links are followed, inside the root only */
    np_wasi_file *d;
    uint32_t r = lookup_dir(c, dirfd, &d);
    if (r != WASI_ESUCCESS) return r;
    char rel[NP_WASI_PATH_MAX + 2];
    r = guest_relpath(c, path, path_len, rel, sizeof rel);
    if (r != WASI_ESUCCESS) return r;
    char *real;
    r = resolve(c, d->path, rel, &real);
    if (r != WASI_ESUCCESS) return r;
    np_hstat st;
    r = path_stat(real, &st);
    free(real);
    return r != WASI_ESUCCESS ? r : put_filestat(c, out, &st);
}

uint32_t w2c_wasi__snapshot__preview1_fd_readdir(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t buf,
                                                 uint32_t buf_len, uint64_t cookie, uint32_t bufused_out) {
    np_core *c = w->core;
    np_wasi_file *d;
    uint32_t r = lookup_dir(c, fd, &d);
    if (r != WASI_ESUCCESS) return r;
    uint8_t *p = np_rt_guest(c, buf, buf_len);
    uint8_t *pu = np_rt_guest(c, bufused_out, 4);
    if (!p || !pu) return WASI_EFAULT;
    uint32_t used = 0;
    r = dir_list(c, d, p, buf_len, cookie, &used);
    if (r != WASI_ESUCCESS) return r;
    put_u32(pu, used);
    return WASI_ESUCCESS;
}

/* The standard streams and content fds take no flags. */
uint32_t w2c_wasi__snapshot__preview1_fd_fdstat_set_flags(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd,
                                                          uint32_t flags) {
    if (!std_fd(w->core, fd) && !content_fd(w->core, fd)) return WASI_EBADF;
    return flags == 0 ? WASI_ESUCCESS : WASI_ENOTSUP;
}

/* Validates an iovec array; returns its host address or NULL. */
static const uint8_t *iovecs(np_core *c, uint32_t iovs, uint32_t n) {
    if (n > (1u << 20)) return NULL;
    return np_rt_guest(c, iovs, n * 8);
}

/* Scatter-reads a content file at `offset`; *total = bytes read. */
static uint32_t read_iovecs(np_core *c, np_wasi_file *f, uint32_t iovs, uint32_t iovs_len, uint64_t offset,
                            uint32_t *total) {
    const uint8_t *iov = iovecs(c, iovs, iovs_len);
    if (!iov) return WASI_EFAULT;
    if (f->kind == NP_WASI_DIRECTORY) return WASI_EISDIR;
    *total = 0;
    for (uint32_t i = 0; i < iovs_len; i++) {
        uint32_t base, len;
        memcpy(&base, iov + 8 * i, 4);
        memcpy(&len, iov + 8 * i + 4, 4);
        uint8_t *dst = np_rt_guest(c, base, len);
        if (!dst) return WASI_EFAULT;
        if (len > UINT32_MAX - *total) len = UINT32_MAX - *total;
        const int64_t got = file_pread(f, dst, len, offset + *total);
        if (got < 0) return WASI_EIO;
        *total += (uint32_t)got;
        if ((uint32_t)got < len) break;
    }
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_fd_read(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t iovs,
                                              uint32_t iovs_len, uint32_t nread_out) {
    np_core *c = w->core;
    np_wasi_file *f = content_fd(c, fd);
    uint8_t *pn = np_rt_guest(c, nread_out, 4);
    if (f) {
        if (!pn) return WASI_EFAULT;
        uint32_t total = 0, r = read_iovecs(c, f, iovs, iovs_len, f->pos, &total);
        if (r != WASI_ESUCCESS) return r;
        f->pos += total;
        put_u32(pn, total);
        return WASI_ESUCCESS;
    }
    if (fd != 0 || !std_fd(c, fd)) return WASI_EBADF;
    if (!iovecs(c, iovs, iovs_len) || !pn) return WASI_EFAULT;
    put_u32(pn, 0); /* stdin is always at end of file */
    return WASI_ESUCCESS;
}

uint32_t w2c_wasi__snapshot__preview1_fd_pread(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t iovs,
                                               uint32_t iovs_len, uint64_t offset, uint32_t nread_out) {
    np_core *c = w->core;
    np_wasi_file *f = content_fd(c, fd);
    if (!f) return std_fd(c, fd) ? WASI_ESPIPE : WASI_EBADF;
    uint8_t *pn = np_rt_guest(c, nread_out, 4);
    if (!pn) return WASI_EFAULT;
    uint32_t total = 0, r = read_iovecs(c, f, iovs, iovs_len, offset, &total);
    if (r != WASI_ESUCCESS) return r;
    put_u32(pn, total);
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
