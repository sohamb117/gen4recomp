/*
 * In-session snapshots: np_core_state_size / _save / _load (np_core.h).
 *
 * A snapshot is taken between frames, on the driver, while every guest fiber
 * is parked inside a fiber_switch / vblank import. At that point the whole
 * machine is:
 *
 *   - guest linear memory: the DS map, the C heap, the shadow stacks and
 *     the frame descriptor. Only pages the guest ever touched are visited
 *     (np_memory_touched); of those, all-zero pages cost one map byte.
 *   - the module instance: wasm globals (__stack_pointer and anything the
 *     guest's libc keeps there) and the memory's page count. Function
 *     tables are part of it too and never change after instantiation.
 *   - the runtime: the fiber table (handles, generations, shadow stack
 *     pointers, exception unwind targets), which fiber is parked, the frame
 *     descriptor's address, the audio reader's position, the WASI streams
 *     and RNG, and the guest's last reported status.
 *   - every parked fiber's native stack from its saved stack pointer up:
 *     the wasm2c frames of the guest code it is suspended in, return
 *     addresses into the module included. Stacks are pooled per fiber slot
 *     (np_rt_fiber_new / np_rt_fiber_release) and never unmapped while the
 *     core lives, so those bytes go back to the very addresses they came
 *     from and every pointer in them (into the stack itself, into the
 *     instance, into np_core) stays right.
 *
 * Not part of it: the host's options (a load must not change the volume the
 * player set), the host's pending rejected save copy, and anything outside
 * the process. That is also why a snapshot is only good for the core that
 * made it (snapshot_token) and never goes to disk.
 *
 * Layout (native endianness, 16-byte aligned sections):
 *   snap_header
 *   snap_fiber[NP_MAX_FIBERS]
 *   stack bytes of each live fiber, slot order
 *   instance bytes (the memory descriptor inside is skipped on load)
 *   page map, one byte per host page: PAGE_ABSENT / PAGE_ZERO / PAGE_DATA
 *   the PAGE_DATA pages, in address order
 *
 * Cheap on purpose: no compression. The size is bounded by what the guest
 * has touched (np_core_state_size), the copy runs at memcpy speed, and a
 * caller wanting compact rewind history can diff consecutive snapshots.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np_memory.h"
#include "np_rt.h"

#define SNAP_MAGIC 0x5453504Eu /* 'NPST' */
#define SNAP_VERSION 1u

enum { PAGE_ABSENT = 0, PAGE_ZERO = 1, PAGE_DATA = 2 };

typedef struct snap_header {
    uint32_t magic, version;
    uint64_t token;
    uint64_t total;
    uint64_t mem_pages; /* wasm pages */
    uint64_t os_page;
    uint64_t page_count; /* host pages covering mem_pages */
    uint64_t data_pages;
    uint64_t instance_size;
    uint32_t state, parked;
    uint32_t desc_addr, audio_tail;
    uint64_t rng_state;
    uint32_t status[NP_STAT_COUNT];
    uint8_t std_open[3];
    uint8_t pad[5];
} snap_header;

typedef struct snap_fiber {
    uint32_t handle, generation, arg, shadow_sp;
    uint64_t unwind;     /* WASM_RT_UNWIND_TARGET * */
    uint64_t stack_lo;   /* saved native stack pointer */
    uint64_t stack_len;  /* bytes from stack_lo to the top */
    WASM_RT_UNWIND_TARGET base_unwind;
} snap_fiber;

static size_t align16(size_t n) {
    return (n + 15) & ~(size_t)15;
}

static int fail_soft(np_core *c, const char *fmt, const char *detail) {
    snprintf(c->error, sizeof c->error, fmt, detail);
    return -1;
}

/* Between frames every fiber is parked; nothing else is a valid moment. */
static const char *not_snapshottable(const np_core *c) {
    if (c->current != &c->driver) return "a snapshot can only be taken between frames";
    if (c->state != NP_RT_RUNNING && c->state != NP_RT_READY) return "the core has failed or exited";
    return NULL;
}

static size_t page_count(const np_core *c, size_t os_page) {
    return (size_t)((c->memory->size + os_page - 1) / os_page);
}

/* Refreshes c->page_map with the touched pages; 0 or -1 (out of memory). */
static int map_touched(np_core *c, size_t n) {
    if (c->page_map_len < n) {
        uint8_t *m = realloc(c->page_map, n ? n : 1);
        if (!m) return -1;
        c->page_map = m;
        c->page_map_len = n;
    }
    if (np_memory_touched(c->memory, c->page_map) != 0) memset(c->page_map, 1, n);
    return 0;
}

static int live_stack(const np_rt_fiber *f, uint8_t **lo, uint8_t **hi) {
    return f->handle && f->native ? np_fiber_live_stack(f->native, lo, hi) : -1;
}

/* Everything but the guest pages; `stacks` gets the live stack bytes. */
static size_t fixed_size(const np_core *c, size_t n, int *ok) {
    size_t stacks = 0;
    *ok = 1;
    for (uint32_t i = 0; i < NP_MAX_FIBERS; i++) {
        const np_rt_fiber *f = &c->fibers[i];
        if (!f->handle) continue;
        uint8_t *lo, *hi;
        if (live_stack(f, &lo, &hi) != 0) {
            *ok = 0;
            return 0;
        }
        stacks += (size_t)(hi - lo);
    }
    return align16(sizeof(snap_header)) + align16(sizeof(snap_fiber) * NP_MAX_FIBERS) + align16(stacks) +
           align16(c->mod->instance_size) + align16(n);
}

size_t np_core_state_size(const np_core *cc) {
    np_core *c = (np_core *)cc;
    if (not_snapshottable(c)) return 0;
    const size_t os_page = np_memory_os_page(), n = page_count(c, os_page);
    int ok;
    size_t fixed = fixed_size(c, n, &ok);
    if (!ok || map_touched(c, n) != 0) return 0;
    size_t touched = 0;
    for (size_t i = 0; i < n; i++) touched += c->page_map[i] != 0;
    return fixed + touched * os_page;
}

static int page_is_zero(const uint8_t *p, size_t len) {
    const uint64_t *w = (const uint64_t *)p;
    for (size_t i = 0; i < len / 8; i += 8) {
        if ((w[i] | w[i + 1] | w[i + 2] | w[i + 3] | w[i + 4] | w[i + 5] | w[i + 6] | w[i + 7]) != 0) return 0;
    }
    return 1;
}

int np_core_state_save(np_core *c, void *dst, size_t cap, size_t *written) {
    if (written) *written = 0;
    const char *why = not_snapshottable(c);
    if (why) return fail_soft(c, "state_save: %s", why);
    const size_t os_page = np_memory_os_page(), n = page_count(c, os_page);
    int ok;
    size_t fixed = fixed_size(c, n, &ok);
    if (!ok) return fail_soft(c, "state_save: %s", "fiber stacks cannot be captured on this platform");
    if (map_touched(c, n) != 0) return fail_soft(c, "state_save: %s", "out of memory");
    if (cap < fixed) return fail_soft(c, "state_save: %s", "buffer smaller than np_core_state_size");

    uint8_t *out = dst;
    snap_header *h = (snap_header *)out;
    memset(h, 0, sizeof *h);
    snap_fiber *sf = (snap_fiber *)(out + align16(sizeof *h));
    uint8_t *p = (uint8_t *)sf + align16(sizeof(snap_fiber) * NP_MAX_FIBERS);

    for (uint32_t i = 0; i < NP_MAX_FIBERS; i++) {
        const np_rt_fiber *f = &c->fibers[i];
        snap_fiber *s = &sf[i];
        memset(s, 0, sizeof *s);
        s->handle = f->handle;
        s->generation = f->generation;
        if (!f->handle) continue;
        uint8_t *lo, *hi;
        live_stack(f, &lo, &hi);
        s->arg = f->arg;
        s->shadow_sp = f->shadow_sp;
        s->unwind = (uint64_t)(uintptr_t)f->unwind;
        s->base_unwind = f->base_unwind;
        s->stack_lo = (uint64_t)(uintptr_t)lo;
        s->stack_len = (uint64_t)(hi - lo);
        memcpy(p, lo, (size_t)(hi - lo));
        p += hi - lo;
    }
    p = out + align16((size_t)(p - out));

    memcpy(p, c->instance, c->mod->instance_size);
    p += align16(c->mod->instance_size);

    uint8_t *map = p;
    p += align16(n);
    const uint8_t *mem = c->memory->data;
    uint64_t data_pages = 0;
    for (size_t i = 0; i < n; i++) {
        size_t off = i * os_page, len = os_page;
        if (off + len > c->memory->size) len = (size_t)(c->memory->size - off);
        if (!c->page_map[i] || page_is_zero(mem + off, len)) {
            map[i] = c->page_map[i] ? PAGE_ZERO : PAGE_ABSENT;
            continue;
        }
        if ((size_t)(p - out) + os_page > cap) return fail_soft(c, "state_save: %s", "buffer too small");
        map[i] = PAGE_DATA;
        memcpy(p, mem + off, len);
        p += os_page;
        data_pages++;
    }

    h->magic = SNAP_MAGIC;
    h->version = SNAP_VERSION;
    h->token = c->snapshot_token;
    h->total = (uint64_t)(p - out);
    h->mem_pages = c->memory->pages;
    h->os_page = os_page;
    h->page_count = n;
    h->data_pages = data_pages;
    h->instance_size = c->mod->instance_size;
    h->state = (uint32_t)c->state;
    h->parked = (uint32_t)(c->parked - c->fibers);
    h->desc_addr = c->desc_addr;
    h->audio_tail = c->audio_tail;
    h->rng_state = c->rng_state;
    memcpy(h->status, c->status, sizeof h->status);
    memcpy(h->std_open, c->std_open, sizeof h->std_open);
    if (written) *written = (size_t)(p - out);
    return 0;
}

int np_core_state_load(np_core *c, const void *src, size_t len) {
    const uint8_t *in = src;
    if (c->current != &c->driver) return fail_soft(c, "state_load: %s", "only between frames");
    if (!in || len < sizeof(snap_header)) return fail_soft(c, "state_load: %s", "truncated snapshot");
    snap_header h;
    memcpy(&h, in, sizeof h);
    const size_t os_page = np_memory_os_page();
    if (h.magic != SNAP_MAGIC || h.version != SNAP_VERSION) return fail_soft(c, "state_load: %s", "not a snapshot");
    if (h.token != c->snapshot_token) return fail_soft(c, "state_load: %s", "the snapshot belongs to another core");
    if (h.total != len || h.os_page != os_page || h.instance_size != c->mod->instance_size ||
        h.mem_pages > c->memory->max_pages ||
        h.page_count != (h.mem_pages * c->memory->page_size + os_page - 1) / os_page ||
        (h.state != NP_RT_RUNNING && h.state != NP_RT_READY) || h.parked >= NP_MAX_FIBERS)
        return fail_soft(c, "state_load: %s", "inconsistent snapshot header");

    /* Validate everything before changing anything, so a refused load
     * leaves the core as it was. */
    const snap_fiber *sf = (const snap_fiber *)(in + align16(sizeof h));
    size_t off = align16(sizeof h) + align16(sizeof(snap_fiber) * NP_MAX_FIBERS);
    if (off > len) return fail_soft(c, "state_load: %s", "truncated snapshot");
    size_t stacks = 0;
    for (uint32_t i = 0; i < NP_MAX_FIBERS; i++) {
        const snap_fiber *s = &sf[i];
        if (!s->handle) continue;
        np_rt_fiber *f = &c->fibers[i];
        uint8_t *lo, *hi;
        if ((s->handle & 0xFFu) != i + 1 || !f->native || np_fiber_live_stack(f->native, &lo, &hi) != 0 ||
            s->stack_lo + s->stack_len != (uint64_t)(uintptr_t)hi ||
            s->stack_len > np_fiber_stack_size(f->native))
            return fail_soft(c, "state_load: %s", "fiber table does not match this core's stacks");
        stacks += (size_t)s->stack_len;
    }
    if (!sf[h.parked].handle) return fail_soft(c, "state_load: %s", "parked fiber is not live");
    const size_t stacks_off = off;
    off += align16(stacks);
    const size_t inst_off = off;
    off += align16((size_t)h.instance_size);
    const size_t map_off = off;
    off += align16((size_t)h.page_count);
    if (off > len) return fail_soft(c, "state_load: %s", "truncated snapshot");
    const uint8_t *map = in + map_off;
    uint64_t data_pages = 0;
    for (size_t i = 0; i < h.page_count; i++) {
        if (map[i] > PAGE_DATA) return fail_soft(c, "state_load: %s", "bad page map");
        data_pages += map[i] == PAGE_DATA;
    }
    if (data_pages != h.data_pages || off + data_pages * os_page != len)
        return fail_soft(c, "state_load: %s", "page data does not match the map");

    /* Guest memory. */
    if (np_memory_set_pages(c->memory, h.mem_pages) != 0) {
        c->state = NP_RT_FAILED;
        return fail_soft(c, "state_load: %s", "could not resize guest memory");
    }
    const size_t n = (size_t)h.page_count;
    if (map_touched(c, n) != 0) {
        c->state = NP_RT_FAILED;
        return fail_soft(c, "state_load: %s", "out of memory");
    }
    uint8_t *mem = c->memory->data;
    const uint8_t *data = in + off;
    for (size_t i = 0; i < n;) {
        size_t plen = os_page;
        if (i * os_page + plen > c->memory->size) plen = (size_t)(c->memory->size - i * os_page);
        if (map[i] == PAGE_DATA) {
            memcpy(mem + i * os_page, data, plen);
            data += os_page;
            i++;
            continue;
        }
        /* A run of pages the snapshot holds as zero: give back the ones
         * the guest has touched since, in one call per run. */
        size_t j = i;
        while (j < n && map[j] != PAGE_DATA) j++;
        size_t k = i;
        while (k < j) {
            while (k < j && !c->page_map[k]) k++;
            size_t e = k;
            while (e < j && c->page_map[e]) e++;
            if (e > k) {
                uint64_t a = (uint64_t)k * os_page, b = (uint64_t)e * os_page;
                if (b > c->memory->size) b = c->memory->size;
                if (np_memory_discard(c->memory, a, b - a) != 0) memset(mem + a, 0, (size_t)(b - a));
            }
            k = e;
        }
        i = j;
    }

    /* Instance: everything but the memory descriptor, whose base pointer
     * and reservation are this process's and whose page count is set. */
    {
        uint8_t *inst = c->instance;
        const uint8_t *s = in + inst_off;
        const size_t mo = c->mod->memory_offset, ml = sizeof(wasm_rt_memory_t);
        memcpy(inst, s, mo);
        memcpy(inst + mo + ml, s + mo + ml, (size_t)h.instance_size - mo - ml);
    }

    /* Fibers: live ones get their stacks back; the rest are free slots
     * whose pooled stacks wait for the next fiber_create. */
    const uint8_t *st = in + stacks_off;
    for (uint32_t i = 0; i < NP_MAX_FIBERS; i++) {
        const snap_fiber *s = &sf[i];
        np_rt_fiber *f = &c->fibers[i];
        f->handle = s->handle;
        f->generation = s->generation;
        if (!s->handle) continue;
        f->core = c;
        f->arg = s->arg;
        f->shadow_sp = s->shadow_sp;
        f->unwind = (WASM_RT_UNWIND_TARGET *)(uintptr_t)s->unwind;
        f->base_unwind = s->base_unwind;
        uint8_t *lo = (uint8_t *)(uintptr_t)s->stack_lo;
        np_fiber_set_live_stack(f->native, lo);
        memcpy(lo, st, (size_t)s->stack_len);
        st += s->stack_len;
    }

    c->state = (np_rt_state)h.state;
    c->parked = &c->fibers[h.parked];
    c->desc_addr = h.desc_addr;
    c->audio_tail = h.audio_tail;
    c->rng_state = h.rng_state;
    memcpy(c->status, h.status, sizeof c->status);
    /* The one option that is an edge rather than a setting: a quick save
     * request the restored guest never saw would fire on the next frame, so
     * the request counter goes back to what that guest last handled. */
    c->opts[NP_OPT_QUICKSAVE_SEQ] = c->status[NP_STAT_QUICKSAVE_SEQ];
    memcpy(c->std_open, h.std_open, sizeof c->std_open);
    c->error[0] = 0;
    return 0;
}
