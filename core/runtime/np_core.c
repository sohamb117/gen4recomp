/*
 * np_core: the shell-facing API (np_core.h) on top of a wasm2c'd guest.
 *
 * See np_rt.h for the execution model. In short: np_core_create instantiates
 * the module on the guest's boot fiber and parks it; np_core_run_frame
 * writes input into the guest's np_frame_desc, switches into the parked
 * guest fiber and returns when the guest's next vblank (or trap / exit)
 * switches back. Everything the guest hands us is validated in the import
 * that receives it, so the driver side can trust the descriptor afterwards.
 *
 * The runtime keeps one active core in a global because wasm traps reach us
 * through wabt's context-free WASM_RT_TRAP_HANDLER, and wabt's exception
 * unwind target is a thread-local anyway; np_core.h already limits a process
 * to one core at a time.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np_memory.h"
#include "np_rt.h"

static np_core *g_core;
static char g_create_error[256];

static const char *const k_game_names[NP_GAME_COUNT] = {"diamond", "pearl", "platinum"};

/* ---- fibers ---------------------------------------------------------- */

/* Each fiber carries its own wasm exception unwind target, so a longjmp
 * (only wasm_rt_throw can issue one: traps go to WASM_RT_TRAP_HANDLER) always
 * lands on the stack it was thrown on and never crosses fibers. That keeps
 * Windows safe too, where mingw's and MSVC's longjmp run an SEH unwind of the
 * current stack: SwitchToFiber keeps the TEB stack bounds of that stack. */
void np_rt_switch(np_core *c, np_rt_fiber *to) {
    np_rt_fiber *from = c->current;
    from->shadow_sp = *c->stack_pointer;
    from->unwind = wasm_rt_get_unwind_target();
    *c->stack_pointer = to->shadow_sp;
    wasm_rt_set_unwind_target(to->unwind);
    c->current = to;
    np_fiber_switch(from->native, to->native);
}

void np_rt_log(np_core *c, const char *line) {
    if (c->host.log) c->host.log(c->host.user, line);
}

NP_NORETURN static void leave_guest(np_core *c) {
    np_wasi_flush(c);
    if (c->current == &c->driver) {
        /* Guest code only ever runs on guest fibers; reaching this on the
         * driver means the runtime itself is broken. */
        fprintf(stderr, "np_core: fatal outside the guest: %s\n", c->error);
        abort();
    }
    np_rt_switch(c, &c->driver);
    /* The driver never resumes a failed or exited guest. */
    abort();
}

void np_rt_fail(np_core *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->error, sizeof c->error, fmt, ap);
    va_end(ap);
    c->state = NP_RT_FAILED;
    np_rt_log(c, c->error);
    leave_guest(c);
}

void np_rt_exit(np_core *c, int code) {
    c->exit_code = code;
    c->state = NP_RT_EXITED;
    snprintf(c->error, sizeof c->error, "guest exited with status %d", code);
    leave_guest(c);
}

void np_rt_wasm_trap(wasm_rt_trap_t code) {
    np_core *c = g_core;
    if (!c) {
        fprintf(stderr, "np_core: wasm trap with no active core: %s\n", wasm_rt_strerror(code));
        abort();
    }
    const char *why = np_memory_failure ? np_memory_failure : wasm_rt_strerror(code);
    np_memory_failure = NULL;
    np_rt_fail(c, "wasm trap: %s", why);
}

/* Every guest fiber, the boot fiber included, starts here. */
static void fiber_body(void *arg) {
    np_rt_fiber *f = arg;
    np_core *c = f->core;
    if (WASM_RT_SETJMP(f->base_unwind)) np_rt_fail(c, "uncaught wasm exception on fiber %u", f->handle);

    if (f == &c->fibers[0]) {
        /* Instantiation runs guest-side code (data segments, start
         * function) and can trap, so it happens here rather than on the
         * driver. Then park until the first np_core_run_frame. */
        c->mod->instantiate(c->instance, &c->host_imports, &c->wasi_imports);
        c->instantiated = 1;
        np_rt_switch(c, &c->driver);
        c->mod->start(c->instance);
        np_rt_exit(c, 0); /* _start returned: main() returned 0 */
    }
    c->mod->fiber_entry(c->instance, f->arg);
    np_rt_fail(c, "fiber %u returned from np_fiber_entry", f->handle);
}

np_rt_fiber *np_rt_fiber_new(np_core *c, size_t stack_size, uint32_t arg, uint32_t shadow_sp) {
    for (uint32_t i = 0; i < NP_MAX_FIBERS; i++) {
        np_rt_fiber *f = &c->fibers[i];
        if (f->handle) continue;
        if (f->native) {
            /* A pooled stack: only the boot fiber asks for more than the
             * default and it is created first, so this never skips in
             * practice; a stack is never swapped for a bigger one because a
             * snapshot may still name its addresses. */
            if (np_fiber_stack_size(f->native) < stack_size) continue;
            if (np_fiber_reset(f->native, fiber_body, f) != 0) return NULL;
        } else {
            f->native = np_fiber_create(stack_size, fiber_body, f);
            if (!f->native) return NULL;
        }
        f->core = c;
        f->arg = arg;
        f->shadow_sp = shadow_sp;
        f->unwind = &f->base_unwind;
        f->handle = (f->generation << 8) | (i + 1);
        return f;
    }
    return NULL;
}

np_rt_fiber *np_rt_fiber_lookup(np_core *c, uint32_t handle) {
    uint32_t slot = (handle & 0xFFu) - 1;
    if (handle == 0 || slot >= NP_MAX_FIBERS || c->fibers[slot].handle != handle) return NULL;
    return &c->fibers[slot];
}

void np_rt_fiber_release(np_rt_fiber *f) {
    f->handle = 0;
    f->generation = (f->generation + 1) & 0xFFFFFFu;
}

/* ---- API ------------------------------------------------------------- */

int np_core_available(np_game game) {
    return game >= 0 && game < NP_GAME_COUNT && np_guest_registry[game] != NULL;
}

const char *np_core_create_error(void) {
    return g_create_error;
}

static np_core *create_failed(np_core *c, const char *fmt, ...) NP_PRINTF(2, 3);
static np_core *create_failed(np_core *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_create_error, sizeof g_create_error, fmt, ap);
    va_end(ap);
    if (c) np_core_destroy(c);
    return NULL;
}

static int copy_environment(np_core *c, const char *const *options) {
    size_t count = 0, bytes = 0;
    for (const char *const *o = options; o && *o; o++) {
        count++;
        bytes += strlen(*o) + 1;
    }
    if (bytes > UINT32_MAX / 2) return -1;
    c->env_count = (uint32_t)count;
    c->env_bytes = (uint32_t)bytes;
    if (bytes == 0) return 0;
    c->env_block = malloc(bytes);
    if (!c->env_block) return -1;
    char *p = c->env_block;
    for (const char *const *o = options; *o; o++) {
        size_t n = strlen(*o) + 1;
        memcpy(p, *o, n);
        p += n;
    }
    return 0;
}

np_core *np_core_create(np_game game, const np_host *host, const char *const *options) {
    g_create_error[0] = 0;
    if (game < 0 || game >= NP_GAME_COUNT) return create_failed(NULL, "unknown game %d", (int)game);
    if (g_core) return create_failed(NULL, "another np_core is still alive; destroy it first");
    const np_guest_module *mod = np_guest_registry[game];
    if (!mod) return create_failed(NULL, "%s is not built into this binary", k_game_names[game]);
    if (!host || !host->rom_read) return create_failed(NULL, "the host must provide rom_read");

    np_core *c = calloc(1, sizeof *c);
    if (!c) return create_failed(NULL, "out of memory");
    c->game = game;
    c->mod = mod;
    c->host = *host;
    c->host_imports.core = c;
    c->wasi_imports.core = c;
    if (copy_environment(c, options) != 0) return create_failed(c, "out of memory");
    c->instance = calloc(1, mod->instance_size);
    if (!c->instance) return create_failed(c, "out of memory");
    c->memory = (wasm_rt_memory_t *)((uint8_t *)c->instance + mod->memory_offset);
    c->stack_pointer = (uint32_t *)((uint8_t *)c->instance + mod->stack_pointer_offset);
    np_wasi_init(c);
    if (host->content_root && np_wasi_open_content(c, host->content_root) != 0) return create_failed(c, "%s", c->error);
    c->host.content_root = c->content_root; /* the host's string need not outlive this call */
    if (host->gba_rom_read && host->gba_rom_size > NP_GBA_ROM_MAX)
        return create_failed(c, "the GBA ROM is %u bytes; the slot holds at most %u", host->gba_rom_size,
                             NP_GBA_ROM_MAX);
    c->trace_fibers = getenv("NP_TRACE_FIBERS") != NULL;
    c->opts[NP_OPT_BGM_VOLUME] = 256;
    c->opts[NP_OPT_SE_VOLUME] = 256;
    c->opts[NP_OPT_RENDER_SCALE] = 1;
    c->opts[NP_OPT_CAMERA_ZOOM] = 256;
    {
        static uint64_t serial;
        c->snapshot_token = ((uint64_t)(uintptr_t)c << 16) ^ ++serial ^ 0x9E3779B97F4A7C15ull;
    }

    wasm_rt_init();
    c->driver.core = c;
    c->driver.native = np_fiber_enter_thread();
    if (!c->driver.native) return create_failed(c, "could not turn the calling thread into a fiber");
    c->current = &c->driver;
    np_rt_fiber *boot = np_rt_fiber_new(c, NP_BOOT_FIBER_STACK, 0, 0);
    if (!boot) return create_failed(c, "could not allocate the boot fiber stack");

    g_core = c;
    np_rt_switch(c, boot);
    if (c->state == NP_RT_FAILED) return create_failed(c, "%s: instantiation failed: %s", mod->name, c->error);
    c->parked = boot;
    return c;
}

int np_core_run_frame(np_core *c, const np_input *in, np_frame *out) {
    if (out) memset(out, 0, sizeof *out);
    if (c->state == NP_RT_FAILED) return -1;
    if (c->state == NP_RT_EXITED) return 1;

    if (c->desc_addr) {
        np_frame_desc *d = (np_frame_desc *)np_rt_guest(c, c->desc_addr, sizeof *d);
        static const np_input none;
        if (!in) in = &none;
        d->in_keys = in->keys;
        d->in_touch = in->touch ? 1u : 0u;
        d->in_touch_x = in->touch_x;
        d->in_touch_y = in->touch_y;
        d->in_lid = in->lid_closed ? 1u : 0u;
        memcpy(d->opt, c->opts, sizeof d->opt);
    }

    np_rt_switch(c, c->parked);

    if (c->state == NP_RT_FAILED) return -1;
    if (c->state == NP_RT_EXITED) return 1;
    const np_frame_desc *d = (const np_frame_desc *)np_rt_guest(c, c->desc_addr, sizeof *d);
    memcpy(c->status, d->status, sizeof c->status);
    if (out) {
        out->screen[0] = (const uint32_t *)np_rt_guest(c, d->screen[0], 0);
        out->screen[1] = (const uint32_t *)np_rt_guest(c, d->screen[1], 0);
        out->width = d->width;
        out->height = d->height;
        out->stride = d->stride;
        out->number = (uint64_t)d->frame_hi << 32 | d->frame_lo;
    }
    return 0;
}

void np_core_set_option(np_core *c, uint32_t opt, uint32_t value) {
    if (opt < NP_OPT_COUNT) c->opts[opt] = value;
}

uint32_t np_core_get_option(const np_core *c, uint32_t opt) {
    return opt < NP_OPT_COUNT ? c->opts[opt] : 0;
}

uint32_t np_core_status(const np_core *c, uint32_t status) {
    return status < NP_STAT_COUNT ? c->status[status] : 0;
}

uint32_t np_core_audio_rate(const np_core *c) {
    if (!c->desc_addr) return 0;
    np_core *mc = (np_core *)c;
    return ((const np_frame_desc *)np_rt_guest(mc, c->desc_addr, sizeof(np_frame_desc)))->audio_rate;
}

size_t np_core_audio_read(np_core *c, int16_t *stereo, size_t max_frames) {
    if (c->state != NP_RT_RUNNING || !c->desc_addr) return 0;
    const np_frame_desc *d = (const np_frame_desc *)np_rt_guest(c, c->desc_addr, sizeof *d);
    const uint32_t cap = d->audio_ring_frames;
    if (cap == 0) return 0;
    uint32_t avail = d->audio_head - c->audio_tail;
    if (avail > cap) { /* the ring lapped the reader: drop the oldest */
        c->audio_tail = d->audio_head - cap;
        avail = cap;
    }
    size_t n = avail < max_frames ? avail : max_frames;
    const uint8_t *ring = np_rt_guest(c, d->audio_ring, 0);
    for (size_t i = 0; i < n; i++) {
        uint32_t v;
        memcpy(&v, ring + (size_t)((c->audio_tail + (uint32_t)i) & (cap - 1)) * 4, 4);
        stereo[2 * i] = (int16_t)(v & 0xFFFFu);
        stereo[2 * i + 1] = (int16_t)(v >> 16);
    }
    c->audio_tail += (uint32_t)n;
    return n;
}

/*
 * Two sources of unsaved data: the chip image the guest publishes in its
 * descriptor (only trusted while the guest is parked in vblank, never after
 * a failure, when it may be half-written), and a copy of a guest-initiated
 * save_store the host rejected. The guest image, when dirty, is newer than
 * any rejected copy, so storing it supersedes the copy.
 */
int np_core_save_flush(np_core *c) {
    int rc = 0;
    if (c->state == NP_RT_RUNNING && c->desc_addr) {
        np_frame_desc *d = (np_frame_desc *)np_rt_guest(c, c->desc_addr, sizeof *d);
        if (d->save_dirty && d->save_image && d->save_size) {
            const uint8_t *image = np_rt_guest(c, d->save_image, d->save_size);
            if (image && c->host.save_store && c->host.save_store(c->host.user, image, d->save_size) == 0) {
                d->save_dirty = 0;
                c->save_dirty = 0;
            } else {
                rc = -1;
            }
        }
    }
    if (c->save_dirty) {
        if (c->host.save_store && c->host.save_store(c->host.user, c->pending_save, c->pending_save_len) == 0)
            c->save_dirty = 0;
        else
            rc = -1;
    }
    return rc;
}

uint8_t *np_core_guest_ptr(np_core *c, uint32_t guest_addr, uint32_t len) {
    if (!c->instantiated) return NULL;
    return np_rt_guest(c, guest_addr, len);
}

const char *np_core_last_error(const np_core *c) {
    return c->error;
}

void np_core_destroy(np_core *c) {
    if (!c) return;
    if (c->driver.native) np_wasi_flush(c);
    for (uint32_t i = 0; i < NP_MAX_FIBERS; i++)
        if (c->fibers[i].native) np_fiber_destroy(c->fibers[i].native);
    /* A trap during instantiation can leave memory allocated before
     * `instantiated` is set; the module's free tolerates the zeroed rest. */
    if (c->instance && (c->instantiated || c->memory->data)) c->mod->free(c->instance);
    if (c->driver.native) {
        wasm_rt_free();
        np_fiber_leave_thread(c->driver.native);
    }
    if (g_core == c) g_core = NULL;
    np_wasi_close_files(c);
    free(c->instance);
    free(c->env_block);
    free(c->pending_save);
    free(c->page_map);
    free(c);
}
