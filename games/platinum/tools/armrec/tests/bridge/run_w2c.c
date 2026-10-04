/*
 * Native driver for the bridge test module after wasm2c (the same way the
 * real runtime executes a guest): the handful of WASI imports a printf-only
 * command uses, then _start. proc_exit ends the process with its status.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bridgetest.h"

struct w2c_wasi__snapshot__preview1 { wasm_rt_memory_t *mem; };

static uint8_t *at(struct w2c_wasi__snapshot__preview1 *w, u32 addr, u32 len) {
    if ((uint64_t)addr + len > w->mem->size) {
        fprintf(stderr, "run_w2c: WASI access 0x%X+%u out of bounds\n", addr, len);
        exit(3);
    }
    return w->mem->data + addr;
}

static u32 ld32(struct w2c_wasi__snapshot__preview1 *w, u32 a) {
    u32 v;
    memcpy(&v, at(w, a, 4), 4);
    return v;
}

u32 w2c_wasi__snapshot__preview1_fd_write(struct w2c_wasi__snapshot__preview1 *w,
                                          u32 fd, u32 iovs, u32 n, u32 nwritten) {
    u32 i, total = 0;
    if (fd != 1 && fd != 2) return 8; /* EBADF */
    for (i = 0; i < n; i++) {
        u32 p = ld32(w, iovs + 8 * i), len = ld32(w, iovs + 8 * i + 4);
        if (len && write((int)fd, at(w, p, len), len) != (ssize_t)len) return 29; /* EIO */
        total += len;
    }
    memcpy(at(w, nwritten, 4), &total, 4);
    return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1 *w, u32 fd) {
    (void)w; (void)fd;
    return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_fdstat_get(struct w2c_wasi__snapshot__preview1 *w,
                                               u32 fd, u32 out) {
    /* A character device, no flags, every right: what a terminal reports. */
    uint8_t st[24] = { 2 };
    if (fd > 2) return 8;
    memset(st + 8, 0xFF, 16);
    memcpy(at(w, out, 24), st, 24);
    return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_seek(struct w2c_wasi__snapshot__preview1 *w,
                                         u32 fd, u64 off, u32 whence, u32 out) {
    (void)w; (void)fd; (void)off; (void)whence; (void)out;
    return 70; /* ESPIPE */
}

u32 w2c_wasi__snapshot__preview1_clock_time_get(struct w2c_wasi__snapshot__preview1 *w,
                                                u32 id, u64 prec, u32 out) {
    struct timespec ts;
    u64 ns;
    (void)id; (void)prec;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ns = (u64)ts.tv_sec * 1000000000u + (u64)ts.tv_nsec;
    memcpy(at(w, out, 8), &ns, 8);
    return 0;
}

void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1 *w, u32 code) {
    (void)w;
    fflush(stdout);
    exit((int)code);
}

int main(void) {
    static w2c_bridgetest inst;
    struct w2c_wasi__snapshot__preview1 wasi;

    wasm_rt_init();
    wasm2c_bridgetest_instantiate(&inst, &wasi);
    wasi.mem = w2c_bridgetest_memory(&inst);
    w2c_bridgetest_0x5Fstart(&inst);
    return 0;
}
