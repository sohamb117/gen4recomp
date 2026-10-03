/*
 * The contract between a guest core (compiled for wasm32) and the native
 * runtime that hosts it after wasm2c.
 *
 * Both sides include this header: the guest with __wasm__ defined, where the
 * imports below are real wasm imports from module "np_host"; the runtime
 * natively, where only the shared struct and constants are visible.
 *
 * Guest memory is wasm linear memory. A guest address is a byte offset into
 * it, and DS regions sit at their hardware addresses (main RAM at
 * 0x02000000, I/O at 0x04000000, VRAM windows at 0x06000000, ...), so the
 * game's pointers are DS pointers. The C runtime's statics, heap and shadow
 * stacks are linked above NP_GUEST_C_BASE, clear of every DS region.
 */
#ifndef NP_GUEST_ABI_H
#define NP_GUEST_ABI_H

#include <stdint.h>

#define NP_GUEST_ABI_VERSION 1

/* Everything below this address belongs to the DS memory map, including the
 * GBA slot (ROM 0x08000000-0x09FFFFFF, SRAM 0x0A000000-0x0A00FFFF), which
 * CTRDG probes at boot and Pal Park reads through. */
#define NP_GUEST_C_BASE 0x0B000000u
/* Linear memory the guest is linked with (pages are committed lazily). */
#define NP_GUEST_MEMORY_BYTES 0x10000000u

#define NP_FRAME_MAGIC 0x4E504652u /* 'NPFR' */

/*
 * One per guest, in guest memory. The guest fills the "guest -> host" half
 * before every np_host_vblank() and reads the "host -> guest" half after it
 * returns; the runtime does the opposite while the guest is parked. All
 * fields are little-endian u32 so the struct means the same on both sides.
 */
typedef struct np_frame_desc {
    uint32_t magic;   /* NP_FRAME_MAGIC */
    uint32_t version; /* NP_GUEST_ABI_VERSION */

    /* guest -> host */
    uint32_t screen[2]; /* guest addresses of 0x00RRGGBB pixels; [0] top */
    uint32_t width, height, stride;
    uint32_t frame_lo, frame_hi;  /* VBlanks since boot */
    uint32_t audio_ring;          /* guest address of u32 stereo frames, L in low half */
    uint32_t audio_ring_frames;   /* ring capacity, a power of two */
    uint32_t audio_head;          /* frames ever produced (wraps mod 2^32) */
    uint32_t audio_rate;          /* Hz */
    uint32_t save_size;           /* backup chip bytes, 0 until identified */
    uint32_t save_image;          /* guest address of the chip image, 0 until identified */
    uint32_t save_dirty;          /* 1 while the image differs from what was last stored;
                                     while the guest is parked the host may store
                                     save_image itself and then write 0 here */

    /* host -> guest */
    uint32_t in_keys;     /* NP_KEY_* from np_core.h */
    uint32_t in_touch;    /* 1 while down */
    uint32_t in_touch_x;  /* 0..255 */
    uint32_t in_touch_y;  /* 0..191 */
    uint32_t in_lid;      /* 1 = closed */
    uint32_t in_quit;     /* 1 = flush and exit at the next safe point */
    uint32_t reserved[8];
} np_frame_desc;

#if defined(__wasm__)

#define NP_IMPORT(name) __attribute__((import_module("np_host"), import_name(#name)))
#define NP_EXPORT(name) __attribute__((export_name(#name)))

/*
 * Fibers. Every guest thread context runs on a host fiber with its own native
 * stack; the guest supplies the top of a shadow stack (the region compiled C
 * uses for address-taken locals) and the runtime swaps __stack_pointer with
 * it. A new fiber starts by calling the guest export np_fiber_entry(arg).
 * Handles are nonzero; the fiber that ran _start is handle 1.
 */
NP_IMPORT(fiber_self) uint32_t np_host_fiber_self(void);
NP_IMPORT(fiber_create) uint32_t np_host_fiber_create(uint32_t shadow_stack_top, uint32_t arg);
NP_IMPORT(fiber_switch) void np_host_fiber_switch(uint32_t to);
NP_IMPORT(fiber_destroy) void np_host_fiber_destroy(uint32_t fiber);

/* The frame is finished: park until the host wants the next one. The
 * descriptor's address is fixed for the life of the guest. */
NP_IMPORT(vblank) void np_host_vblank(np_frame_desc *desc);

/* Cartridge reads; 0 on success. */
NP_IMPORT(rom_size) uint32_t np_host_rom_size(void);
NP_IMPORT(rom_read) int32_t np_host_rom_read(uint32_t offset, void *dst, uint32_t len);

/* Backup chip: load returns 1 loaded, 0 no save, -1 error; store returns 0. */
NP_IMPORT(save_load) int32_t np_host_save_load(void *dst, uint32_t len);
NP_IMPORT(save_store) int32_t np_host_save_store(const void *src, uint32_t len);

/* Seconds since 2000-01-01 00:00:00 local time, or -1 to keep the port's
 * deterministic clock. */
NP_IMPORT(rtc_now) int64_t np_host_rtc_now(void);

NP_IMPORT(log) void np_host_log(const char *text, uint32_t len);
/* Fatal: the runtime records the message and unwinds out of the guest. */
NP_IMPORT(trap) __attribute__((noreturn)) void np_host_trap(const char *text, uint32_t len);

/* Guest export the runtime calls on a fresh fiber. */
void np_fiber_entry(uint32_t arg);

#endif /* __wasm__ */

#endif /* NP_GUEST_ABI_H */
