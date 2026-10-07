/*
 * The "np_host" wasm imports (np_guest_abi.h) on top of the shell's np_host
 * callbacks.
 *
 * Every guest address an import receives is range-checked against linear
 * memory here, once, so the rest of the runtime can use it directly. A bad
 * pointer, handle or descriptor is a guest bug, so it fails the core with a
 * message naming the import rather than returning an error the port would
 * not check anyway. Cartridge and backup errors are ordinary I/O failures
 * and are returned to the guest.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np_imports.h"
#include "np_rt.h"

static uint8_t *guest_range(np_core *c, uint32_t addr, uint32_t len, const char *what) {
    uint8_t *p = np_rt_guest(c, addr, len);
    if (!p) np_rt_fail(c, "%s: guest range 0x%08x+0x%x is outside linear memory", what, addr, len);
    return p;
}

/* ---- fibers ---------------------------------------------------------- */

uint32_t w2c_np__host_fiber_self(struct w2c_np__host *h) {
    return h->core->current->handle;
}

uint32_t w2c_np__host_fiber_create(struct w2c_np__host *h, uint32_t shadow_stack_top, uint32_t arg) {
    np_core *c = h->core;
    if (shadow_stack_top == 0 || shadow_stack_top > c->memory->size || (shadow_stack_top & 15) != 0)
        np_rt_fail(c, "fiber_create: bad shadow stack top 0x%08x", shadow_stack_top);
    np_rt_fiber *f = np_rt_fiber_new(c, NP_FIBER_DEFAULT_STACK, arg, shadow_stack_top);
    if (!f) np_rt_fail(c, "fiber_create: out of fibers (%d) or stack memory", NP_MAX_FIBERS);
    if (c->trace_fibers) {
        char line[96];
        snprintf(line, sizeof line, "[fiber] create %#x (arg %#x)", f->handle, arg);
        np_rt_log(c, line);
    }
    return f->handle;
}

void w2c_np__host_fiber_switch(struct w2c_np__host *h, uint32_t to) {
    np_core *c = h->core;
    np_rt_fiber *f = np_rt_fiber_lookup(c, to);
    if (!f) np_rt_fail(c, "fiber_switch: no fiber %u", to);
    if (f != c->current) np_rt_switch(c, f);
}

void w2c_np__host_fiber_destroy(struct w2c_np__host *h, uint32_t fiber) {
    np_core *c = h->core;
    np_rt_fiber *f = np_rt_fiber_lookup(c, fiber);
    if (!f) np_rt_fail(c, "fiber_destroy: no fiber %u", fiber);
    if (f == c->current) np_rt_fail(c, "fiber_destroy: fiber %u is running", fiber);
    if (c->trace_fibers) {
        char line[64];
        snprintf(line, sizeof line, "[fiber] destroy %#x", fiber);
        np_rt_log(c, line);
    }
    np_rt_fiber_release(f);
}

/* ---- frames ---------------------------------------------------------- */

static void check_pixels(np_core *c, const np_frame_desc *d, int i) {
    if ((d->screen[i] & 3) != 0 || !np_rt_guest(c, d->screen[i], d->stride * d->height * 4))
        np_rt_fail(c, "vblank: screen[%d] 0x%08x is misaligned or outside memory", i, d->screen[i]);
}

void w2c_np__host_vblank(struct w2c_np__host *h, uint32_t desc) {
    np_core *c = h->core;
    if (c->desc_addr && desc != c->desc_addr)
        np_rt_fail(c, "vblank: descriptor moved from 0x%08x to 0x%08x", c->desc_addr, desc);
    if ((desc & 3) != 0) np_rt_fail(c, "vblank: descriptor 0x%08x is misaligned", desc);
    const np_frame_desc *d = (const np_frame_desc *)guest_range(c, desc, sizeof(np_frame_desc), "vblank");
    if (d->magic != NP_FRAME_MAGIC || d->version != NP_GUEST_ABI_VERSION)
        np_rt_fail(c, "vblank: descriptor magic/version 0x%08x/%u, want 0x%08x/%u", d->magic, d->version,
                   NP_FRAME_MAGIC, NP_GUEST_ABI_VERSION);
    /* Any size from the DS's own up to 8x it (render scale 4 of a 342-wide
     * picture is 1368x768); both screens share the geometry. A single
     * screen (screen[1] == 0) is the GBA's 240x160, up to 8x. */
    uint32_t min_w = d->screen[1] ? NP_SCREEN_W : NP_GBA_SCREEN_W;
    uint32_t min_h = d->screen[1] ? NP_SCREEN_H : NP_GBA_SCREEN_H;
    if (d->width < min_w || d->width > NP_SCREEN_W * 8 || d->height < min_h ||
        d->height > NP_SCREEN_H * 8 || d->stride < d->width || d->stride > NP_SCREEN_W * 8)
        np_rt_fail(c, "vblank: bad screen geometry %ux%u stride %u", d->width, d->height, d->stride);
    check_pixels(c, d, 0);
    if (d->screen[1]) check_pixels(c, d, 1);
    if (d->audio_ring_frames) {
        if ((d->audio_ring_frames & (d->audio_ring_frames - 1)) != 0 || d->audio_ring_frames > (1u << 24) ||
            (d->audio_ring & 3) != 0 || !np_rt_guest(c, d->audio_ring, d->audio_ring_frames * 4))
            np_rt_fail(c, "vblank: bad audio ring 0x%08x x %u frames", d->audio_ring, d->audio_ring_frames);
    }
    if (d->save_image && d->save_size && !np_rt_guest(c, d->save_image, d->save_size))
        np_rt_fail(c, "vblank: save image 0x%08x+0x%x is outside memory", d->save_image, d->save_size);

    c->desc_addr = desc;
    c->state = NP_RT_RUNNING;
    c->parked = c->current;
    np_rt_switch(c, &c->driver);
}

/* ---- cartridge and backup ------------------------------------------- */

uint32_t w2c_np__host_rom_size(struct w2c_np__host *h) {
    return h->core->host.rom_size;
}

uint32_t w2c_np__host_rom_read(struct w2c_np__host *h, uint32_t offset, uint32_t dst, uint32_t len) {
    np_core *c = h->core;
    uint8_t *p = guest_range(c, dst, len, "rom_read");
    if ((uint64_t)offset + len > c->host.rom_size) return (uint32_t)-1;
    if (len == 0) return 0;
    return c->host.rom_read(c->host.user, offset, p, len) == 0 ? 0 : (uint32_t)-1;
}

uint32_t w2c_np__host_save_load(struct w2c_np__host *h, uint32_t dst, uint32_t len) {
    np_core *c = h->core;
    uint8_t *p = guest_range(c, dst, len, "save_load");
    if (!c->host.save_load) return 0;
    int r = c->host.save_load(c->host.user, p, len);
    return r > 0 ? 1u : r == 0 ? 0u : (uint32_t)-1;
}

uint32_t w2c_np__host_save_store(struct w2c_np__host *h, uint32_t src, uint32_t len) {
    np_core *c = h->core;
    const uint8_t *p = guest_range(c, src, len, "save_store");
    if (c->host.save_store && c->host.save_store(c->host.user, p, len) == 0) {
        c->save_dirty = 0; /* anything pending is older than this image */
        return 0;
    }
    /* Keep a copy so np_core_save_flush can retry; the guest may consider
     * the chip clean after this call. */
    if (len > c->pending_save_cap) {
        uint8_t *grown = realloc(c->pending_save, len);
        if (!grown) return (uint32_t)-1;
        c->pending_save = grown;
        c->pending_save_cap = len;
    }
    memcpy(c->pending_save, p, len);
    c->pending_save_len = len;
    c->save_dirty = 1;
    return (uint32_t)-1;
}

/* ---- GBA slot -------------------------------------------------------- */

/* An empty slot unless the host gave both a size and a reader; a ROM larger
 * than the slot window is refused at np_core_create. */
uint32_t w2c_np__host_gba_rom_size(struct w2c_np__host *h) {
    np_core *c = h->core;
    return c->host.gba_rom_read ? c->host.gba_rom_size : 0;
}

uint32_t w2c_np__host_gba_rom_read(struct w2c_np__host *h, uint32_t offset, uint32_t dst, uint32_t len) {
    np_core *c = h->core;
    uint8_t *p = guest_range(c, dst, len, "gba_rom_read");
    if (!c->host.gba_rom_read || (uint64_t)offset + len > c->host.gba_rom_size) return (uint32_t)-1;
    if (len == 0) return 0;
    return c->host.gba_rom_read(c->host.user, offset, p, len) == 0 ? 0 : (uint32_t)-1;
}

uint32_t w2c_np__host_gba_save_load(struct w2c_np__host *h, uint32_t dst, uint32_t len) {
    np_core *c = h->core;
    uint8_t *p = guest_range(c, dst, len, "gba_save_load");
    if (!c->host.gba_save_load) return 0;
    int r = c->host.gba_save_load(c->host.user, p, len);
    return r > 0 ? 1u : r == 0 ? 0u : (uint32_t)-1;
}

uint32_t w2c_np__host_gba_save_store(struct w2c_np__host *h, uint32_t src, uint32_t len) {
    np_core *c = h->core;
    const uint8_t *p = guest_range(c, src, len, "gba_save_store");
    if (!c->host.gba_save_store) return (uint32_t)-1;
    return c->host.gba_save_store(c->host.user, p, len) == 0 ? 0 : (uint32_t)-1;
}

uint64_t w2c_np__host_rtc_now(struct w2c_np__host *h) {
    np_core *c = h->core;
    return c->host.rtc_now ? (uint64_t)c->host.rtc_now(c->host.user) : (uint64_t)(int64_t)-1;
}

/* ---- local wireless -------------------------------------------------- */

uint32_t w2c_np__host_net_self(struct w2c_np__host *h) {
    np_core *c = h->core;
    return c->host.net_self ? c->host.net_self(c->host.user) : 0;
}

uint32_t w2c_np__host_net_send(struct w2c_np__host *h, uint32_t peer, uint32_t buf, uint32_t len) {
    np_core *c = h->core;
    const uint8_t *p = guest_range(c, buf, len, "net_send");
    if (!c->host.net_send) return (uint32_t)-1;
    return c->host.net_send(c->host.user, peer, p, len) == 0 ? 0 : (uint32_t)-1;
}

uint32_t w2c_np__host_net_recv(struct w2c_np__host *h, uint32_t peer_out, uint32_t buf, uint32_t cap) {
    np_core *c = h->core;
    uint8_t *pp = guest_range(c, peer_out, 4, "net_recv");
    uint8_t *p = guest_range(c, buf, cap, "net_recv");
    if (!c->host.net_recv) return 0;
    uint32_t peer = 0;
    int n = c->host.net_recv(c->host.user, &peer, p, cap);
    if (n < 0) return (uint32_t)-1;
    if ((uint32_t)n > cap) np_rt_fail(c, "net_recv: the host returned %d bytes into a %u-byte buffer", n, cap);
    memcpy(pp, &peer, 4);
    return (uint32_t)n;
}

/* ---- diagnostics ----------------------------------------------------- */

void w2c_np__host_log(struct w2c_np__host *h, uint32_t text, uint32_t len) {
    np_core *c = h->core;
    const char *s = (const char *)guest_range(c, text, len, "log");
    char line[1024];
    uint32_t n = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (s[i] == '\n' || n == sizeof line - 1) {
            line[n] = 0;
            np_rt_log(c, line);
            n = 0;
            if (s[i] == '\n') continue;
        }
        line[n++] = s[i];
    }
    if (n) {
        line[n] = 0;
        np_rt_log(c, line);
    }
}

void w2c_np__host_trap(struct w2c_np__host *h, uint32_t text, uint32_t len) {
    np_core *c = h->core;
    const char *s = (const char *)np_rt_guest(c, text, len);
    if (!s) np_rt_fail(c, "guest trap (message outside memory)");
    np_rt_fail(c, "guest trap: %.*s", (int)(len > 400 ? 400 : len), s);
}
