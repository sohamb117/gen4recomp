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

#define NP_GUEST_ABI_VERSION 2

/* Everything below this address belongs to the DS memory map, including the
 * GBA slot (ROM 0x08000000-0x09FFFFFF, SRAM 0x0A000000-0x0A00FFFF), which
 * CTRDG probes at boot and Pal Park reads through. */
#define NP_GUEST_C_BASE 0x0B000000u
/* Linear memory the guest is linked with (pages are committed lazily). */
#define NP_GUEST_MEMORY_BYTES 0x10000000u

/* Where the host's runtime content directory (np_host.content_root), if
 * any, appears: a read-only WASI preopen (fd 3) under this name. */
#define NP_CONTENT_DIR "/content"
/* The largest GBA ROM the slot holds (0x08000000-0x09FFFFFF). */
#define NP_GBA_ROM_MAX 0x02000000u

#define NP_FRAME_MAGIC 0x4E504652u /* 'NPFR' */

/*
 * Host -> guest settings, indexes into np_frame_desc.opt[]. The runtime
 * writes every slot before each frame; the guest reads them after
 * np_host_vblank returns and must treat values it does not implement as
 * no-ops. Adding an index never changes the struct layout.
 */
enum np_opt {
    NP_OPT_BGM_VOLUME = 0,   /* 0..256, 256 = as the cartridge mixes it */
    NP_OPT_SE_VOLUME = 1,    /* 0..256: sound effects and cries */
    NP_OPT_RENDER_SCALE = 2, /* 1..4: internal 3D resolution multiplier */
    NP_OPT_WIDESCREEN = 3,   /* 0 or 1: widen the 3D field of view (screens become wider) */
    NP_OPT_CAMERA_ZOOM = 4,  /* field camera distance, 256 = original, larger = farther */
    NP_OPT_CAMERA_TILT = 5,  /* field camera pitch offset, signed, 1/16 degree units */
    NP_OPT_QUICKSAVE_SEQ = 6, /* the host increments it to request one in-game save */
    NP_OPT_RULES = 7,        /* NP_RULE_* bits: opt-in fixes of documented cartridge bugs */
    NP_OPT_TEXT_INSTANT = 8, /* 0 or 1: print message boxes at once (QoL) */
    NP_OPT_COUNT = 32
};

/*
 * Guest -> host state, indexes into np_frame_desc.status[]. Zero means
 * "not reported" for every slot, so an older guest reads as idle.
 */
enum np_status {
    NP_STAT_LINK_ACTIVE = 0,     /* 1 while a wireless session runs: the host must run at 1x */
    NP_STAT_FIELD_READY = 1,     /* 1 while the player can act in the field (quick save allowed) */
    NP_STAT_QUICKSAVE_SEQ = 2,   /* last NP_OPT_QUICKSAVE_SEQ the guest handled */
    NP_STAT_QUICKSAVE_RESULT = 3, /* NP_QS_* for that request */
    NP_STAT_MAP_ID = 4,          /* current field map header id, for diagnostics and mods */
    NP_STAT_IN_BATTLE = 5,       /* 1 from a battle's intro until the fade back to the field
                                    (wild, trainer, link, facility), 0 otherwise */
    NP_STAT_E2E = 6,             /* guest address of the test probe block (np_e2e.h), 0 unless
                                    the guest runs with PC_E2E=1 */
    NP_STAT_COUNT = 16
};

enum { NP_QS_NONE = 0, NP_QS_SAVED = 1, NP_QS_REFUSED = 2, NP_QS_FAILED = 3 };

/* NP_OPT_RULES bits. 0 is the faithful cartridge; each bit fixes one
 * documented bug (see the decomps' docs/bugs_and_glitches.md). The guest
 * ignores bits it does not implement. Link play forces 0 on both sides. */
enum { NP_RULE_FIX_BUGS = 1u << 0 };

/* Peer id meaning "every station on the local link". */
#define NP_NET_BROADCAST 0xFFFFFFFFu

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
    uint32_t screen[2]; /* guest addresses of 0x00RRGGBB pixels; [0] top;
                           [1] = 0 for a single-screen (GBA) guest */
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

    uint32_t status[NP_STAT_COUNT]; /* enum np_status */

    /* host -> guest */
    uint32_t in_keys;     /* NP_KEY_* from np_core.h */
    uint32_t in_touch;    /* 1 while down */
    uint32_t in_touch_x;  /* 0..255 */
    uint32_t in_touch_y;  /* 0..191 */
    uint32_t in_lid;      /* 1 = closed */
    uint32_t in_quit;     /* 1 = flush and exit at the next safe point */
    uint32_t opt[NP_OPT_COUNT]; /* enum np_opt */
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

/*
 * Local wireless transport, used by the ARM7 wireless (WM) model. Datagrams:
 * unordered, may be lost; the guest's protocol handles it. net_self returns
 * this station's nonzero id, or 0 when the host has networking off (then the
 * guest behaves as a DS with no partner in range). send returns 0 or -1;
 * recv returns the datagram length, 0 when nothing is waiting, -1 on error.
 */
NP_IMPORT(net_self) uint32_t np_host_net_self(void);
NP_IMPORT(net_send) int32_t np_host_net_send(uint32_t peer, const void *buf, uint32_t len);
NP_IMPORT(net_recv) int32_t np_host_net_recv(uint32_t *peer, void *buf, uint32_t cap);

/*
 * The GBA slot. gba_rom_size is 0 for an empty slot; otherwise the guest
 * reads the ROM (0 on success) into the slot window. The backup image:
 * load returns 1 loaded, 0 no save (erased), -1 error; store returns 0.
 */
NP_IMPORT(gba_rom_size) uint32_t np_host_gba_rom_size(void);
NP_IMPORT(gba_rom_read) int32_t np_host_gba_rom_read(uint32_t offset, void *dst, uint32_t len);
NP_IMPORT(gba_save_load) int32_t np_host_gba_save_load(void *dst, uint32_t len);
NP_IMPORT(gba_save_store) int32_t np_host_gba_save_store(const void *src, uint32_t len);

/* Guest export the runtime calls on a fresh fiber. */
void np_fiber_entry(uint32_t arg);

#endif /* __wasm__ */

#endif /* NP_GUEST_ABI_H */
