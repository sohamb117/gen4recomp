/*
 * Runtime internals shared by np_core.c, np_imports.c and np_wasi.c.
 *
 * Execution model: np_core_run_frame runs on the shell's thread, which we
 * call the driver fiber. Every guest thread context (including the one that
 * runs _start) is an np_rt_fiber with its own native stack. The driver
 * switches into the guest's parked fiber; the guest runs, switching among its
 * own fibers through the fiber_* imports, until it calls vblank, trap or
 * proc_exit, each of which switches back to the driver. A fiber that traps
 * or exits is never resumed; its stack is simply released on destroy, which
 * is how "unwinding to the runtime" works without setjmp across stacks.
 *
 * Per-fiber guest state that lives outside the native stack is swapped by
 * np_rt_switch: the instance's __stack_pointer global (top of that fiber's
 * shadow stack in linear memory) and wabt's thread-local exception unwind
 * target.
 */
#ifndef NP_RT_H
#define NP_RT_H

#include <stdint.h>

#include "np_core.h"
#include "np_fiber.h"
#include "np_guest_abi.h"
#include "np_guest_module.h"
#include "wasm-rt-exceptions.h"
#include "wasm-rt.h"

#if defined(_MSC_VER)
#define NP_NORETURN __declspec(noreturn)
#else
#define NP_NORETURN __attribute__((noreturn))
#endif

#if defined(__GNUC__)
#define NP_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define NP_PRINTF(fmt, args)
#endif

#define NP_MAX_FIBERS 128
/* The boot fiber runs the game's main thread, which typically owns the
 * deepest call chains; other fibers get NP_FIBER_DEFAULT_STACK. */
#define NP_BOOT_FIBER_STACK ((size_t)1024 * 1024)

typedef struct np_core np_core;

typedef enum np_rt_state {
    NP_RT_READY,   /* instantiated; the boot fiber has not run _start yet */
    NP_RT_RUNNING, /* parked in vblank */
    NP_RT_EXITED,  /* proc_exit or _start returned */
    NP_RT_FAILED,  /* trap, wasm trap or broken contract; see core->error */
} np_rt_state;

typedef struct np_rt_fiber {
    np_core *core;
    np_fiber *native;
    uint32_t handle; /* guest-visible id, 0 while the slot is free */
    uint32_t generation;
    uint32_t arg;       /* np_fiber_entry argument */
    uint32_t shadow_sp; /* saved __stack_pointer while not running */
    WASM_RT_UNWIND_TARGET *unwind;
    /* Catch-all unwind target set up at the base of the fiber, so an
     * exception nothing catches fails the core instead of jumping through a
     * NULL target. */
    WASM_RT_UNWIND_TARGET base_unwind;
} np_rt_fiber;

/* Import-module instances. wasm2c only forward-declares these; their only
 * job is to lead the imports back to the core. */
struct w2c_np__host {
    np_core *core;
};
struct w2c_wasi__snapshot__preview1 {
    np_core *core;
};

typedef struct np_line_buf {
    char text[1024];
    uint32_t len;
} np_line_buf;

/* Content files (np_wasi.c): guest fd NP_WASI_FIRST_FD + i is files[i];
 * files[0] is the NP_CONTENT_DIR preopen when the host gave a content root. */
#define NP_WASI_FIRST_FD 3u
#define NP_WASI_MAX_FILES 64u

enum { NP_WASI_FREE = 0, NP_WASI_REGULAR, NP_WASI_DIRECTORY };

typedef struct np_wasi_file {
    int kind;     /* NP_WASI_* */
    int preopen;  /* the content root itself */
    uint64_t pos; /* fd_read / fd_seek offset of a regular file */
    char *path;   /* resolved host path, inside the content root */
#ifdef _WIN32
    void *handle; /* HANDLE */
#else
    int fd;
#endif
} np_wasi_file;

struct np_core {
    np_game game;
    const np_guest_module *mod;
    np_host host;

    void *instance;           /* w2c_<module>, mod->instance_size bytes */
    wasm_rt_memory_t *memory; /* inside instance */
    uint32_t *stack_pointer;  /* inside instance */
    struct w2c_np__host host_imports;
    struct w2c_wasi__snapshot__preview1 wasi_imports;
    int instantiated;

    np_rt_state state;
    np_rt_fiber driver;              /* the shell's thread */
    np_rt_fiber fibers[NP_MAX_FIBERS]; /* [0] is the boot fiber, handle 1 */
    np_rt_fiber *current;
    np_rt_fiber *parked; /* where run_frame resumes the guest */

    uint32_t desc_addr; /* np_frame_desc, known from the first vblank */
    uint32_t audio_tail;

    /* contract v2: written into the descriptor before every frame / copied
     * out of it after every frame */
    uint32_t opts[NP_OPT_COUNT];
    uint32_t status[NP_STAT_COUNT];

    /* Snapshots (np_snapshot.c): this core's identity, so a state from
     * another core is refused, and a scratch map of touched pages. */
    uint64_t snapshot_token;
    uint8_t *page_map;
    size_t page_map_len;
    int trace_fibers; /* NP_TRACE_FIBERS in the host's environment */

    /* "KEY=VALUE\0" strings back to back, as WASI environ_get wants them. */
    char *env_block;
    uint32_t env_count, env_bytes;

    /* WASI */
    uint8_t std_open[3];
    np_line_buf out_line[2]; /* fd 1 and fd 2, flushed to host.log per line */
    uint64_t rng_state;
    uint64_t clock_origin_ns;
    char *content_root;                       /* resolved, or NULL */
    np_wasi_file files[NP_WASI_MAX_FILES];
    int exit_code;

    /* A save the host failed to store, retried by np_core_save_flush. */
    uint8_t *pending_save;
    uint32_t pending_save_len, pending_save_cap;
    int save_dirty;

    char error[512];
};

/* Host pointer for guest [addr, addr + len), or NULL if it leaves memory. */
static inline uint8_t *np_rt_guest(np_core *c, uint32_t addr, uint32_t len) {
    if ((uint64_t)addr + len > c->memory->size) return NULL;
    return c->memory->data + addr;
}

/* Switches from the running fiber to `to`, swapping the shadow stack pointer
 * and the exception unwind target. Returns when switched back. */
void np_rt_switch(np_core *c, np_rt_fiber *to);

/* Marks the core failed with a message and switches to the driver for good.
 * Only valid on a guest fiber. */
NP_NORETURN void np_rt_fail(np_core *c, const char *fmt, ...) NP_PRINTF(2, 3);

/* Marks the core exited and switches to the driver for good. */
NP_NORETURN void np_rt_exit(np_core *c, int code);

/* Flushes partial stdout/stderr lines to host.log. */
void np_wasi_flush(np_core *c);
/* Resets WASI state (std fds, line buffers, RNG seed, clock origin). */
void np_wasi_init(np_core *c);
/* Opens `root` as the NP_CONTENT_DIR preopen; 0, or -1 with c->error set. */
int np_wasi_open_content(np_core *c, const char *root);
/* Closes every content fd and the preopen (np_core_destroy). */
void np_wasi_close_files(np_core *c);

/* Guest fiber slots. new returns NULL when out of slots or stack memory;
 * lookup returns NULL for a stale or invalid handle; release retires the
 * handle of a fiber that is not running. Native stacks are pooled per slot
 * and only freed by np_core_destroy, so a snapshot's stack bytes always go
 * back to the addresses they came from. */
np_rt_fiber *np_rt_fiber_new(np_core *c, size_t stack_size, uint32_t arg, uint32_t shadow_sp);
np_rt_fiber *np_rt_fiber_lookup(np_core *c, uint32_t handle);
void np_rt_fiber_release(np_rt_fiber *f);

/* One line to host.log, if any. */
void np_rt_log(np_core *c, const char *line);

/* WASM_RT_TRAP_HANDLER: wasm traps (unreachable, divide by zero, OOB in
 * bounds-check builds, bad call_indirect) land here instead of longjmp. */
NP_NORETURN void np_rt_wasm_trap(wasm_rt_trap_t code);

#endif /* NP_RT_H */
