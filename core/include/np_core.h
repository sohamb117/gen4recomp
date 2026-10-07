/*
 * np_core: what the shell sees of a game core.
 *
 * A core is one game (Diamond, Pearl, Platinum, Black, White, or the GBA's
 * Ruby, Sapphire and Emerald) compiled to wasm32 and
 * turned back into C by wasm2c, plus the native runtime that hosts it. The
 * shell never sees wasm, fibers or guest addresses; it drives frames, feeds
 * input, drains audio and supplies the cartridge and backup storage.
 *
 * Threading: every call on one np_core must come from one thread. One core
 * may exist per process at a time (the runtime's fiber state is global).
 */
#ifndef NP_CORE_H
#define NP_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_CORE_API_VERSION 2

typedef enum np_game {
    NP_GAME_DIAMOND = 0,
    NP_GAME_PEARL = 1,
    NP_GAME_PLATINUM = 2,
    NP_GAME_BLACK = 3,
    NP_GAME_WHITE = 4,
    /* 5 and 6 are HeartGold and SoulSilver's. */
    NP_GAME_RUBY = 7,
    NP_GAME_SAPPHIRE = 8,
    NP_GAME_EMERALD = 9,
    NP_GAME_COUNT
} np_game;

/* DS keypad bits, in the SDK's PAD_* order. Bits 0..9 are also the GBA's
 * KEYINPUT bits; a GBA core ignores X and Y. */
enum {
    NP_KEY_A = 1u << 0,
    NP_KEY_B = 1u << 1,
    NP_KEY_SELECT = 1u << 2,
    NP_KEY_START = 1u << 3,
    NP_KEY_RIGHT = 1u << 4,
    NP_KEY_LEFT = 1u << 5,
    NP_KEY_UP = 1u << 6,
    NP_KEY_DOWN = 1u << 7,
    NP_KEY_R = 1u << 8,
    NP_KEY_L = 1u << 9,
    NP_KEY_X = 1u << 10,
    NP_KEY_Y = 1u << 11,
};

#define NP_SCREEN_W 256
#define NP_SCREEN_H 192
#define NP_GBA_SCREEN_W 240
#define NP_GBA_SCREEN_H 160

typedef struct np_host {
    void *user;

    /* The cartridge image, read at random offsets for the whole session.
     * The host owns the file (iOS keeps it behind a security-scoped
     * bookmark). Returns 0 on success, nonzero on a short or failed read. */
    uint32_t rom_size;
    int (*rom_read)(void *user, uint32_t offset, void *dst, uint32_t len);

    /* The backup chip. save_load fills `len` bytes and returns 1, returns 0
     * when there is no save yet (the core then erases the chip to 0xFF), or
     * -1 on error. save_store persists the whole image atomically; the core
     * calls it from np_core_run_frame once writes have gone quiet, and from
     * np_core_save_flush. Returns 0 on success. */
    int (*save_load)(void *user, void *dst, uint32_t len);
    int (*save_store)(void *user, const void *src, uint32_t len);

    /* Wall clock for the RTC, as seconds since 2000-01-01 00:00:00 in the
     * player's local time. NULL keeps the port's deterministic clock
     * (2009-03-22 10:00:00 advancing with frames), which tests rely on. */
    int64_t (*rtc_now)(void *user);

    /* One line of diagnostic text, without a trailing newline. May be NULL. */
    void (*log)(void *user, const char *line);

    /* Local wireless transport (NP_NET_BROADCAST and the datagram semantics
     * are in np_guest_abi.h). All three NULL = no partner in range. net_self
     * returns this station's nonzero id; net_recv returns the length, 0 when
     * nothing waits, -1 on error. Called on the run_frame thread. */
    uint32_t (*net_self)(void *user);
    int (*net_send)(void *user, uint32_t peer, const void *buf, uint32_t len);
    int (*net_recv)(void *user, uint32_t *peer, void *buf, uint32_t cap);

    /* Runtime content (mod packages): a host directory the guest may read
     * but never write, seen as the WASI preopen NP_CONTENT_DIR ("/content",
     * np_guest_abi.h). Lookups are confined to it: '..' components and
     * absolute paths are refused, and a symlink is followed only when it
     * resolves inside the directory. np_core_create copies the string and
     * fails if it is not a readable directory. NULL = no content (the guest
     * sees no preopens). */
    const char *content_root;

    /* The GBA slot (Pal Park). gba_rom_size 0 or gba_rom_read NULL = an
     * empty slot, exactly as without these fields. The guest copies the ROM
     * (at most 32 MiB) into the slot at boot, so changing cartridges needs a
     * new core. The cartridge's backup works like the DS one: gba_save_load
     * fills `len` bytes (the size the guest identifies from the cartridge)
     * and returns 1, returns 0 when there is no save yet (an erased chip,
     * 0xFF), or -1 on error; gba_save_store persists the whole image
     * atomically and returns 0. Both may be NULL (no save, writes lost). */
    uint32_t gba_rom_size;
    int (*gba_rom_read)(void *user, uint32_t offset, void *dst, uint32_t len);
    int (*gba_save_load)(void *user, void *dst, uint32_t len);
    int (*gba_save_store)(void *user, const void *src, uint32_t len);
} np_host;

typedef struct np_input {
    uint16_t keys;      /* NP_KEY_* */
    uint8_t touch;      /* 1 while the stylus is down */
    uint8_t lid_closed; /* 1 to close the lid (sleep) */
    uint16_t touch_x;   /* bottom-screen pixel, 0..255 */
    uint16_t touch_y;   /* bottom-screen pixel, 0..191 */
} np_input;

/* width/height are 256x192 by default and change with NP_OPT_RENDER_SCALE
 * and NP_OPT_WIDESCREEN; the shell must accept a new size on any frame.
 * Touch coordinates stay in DS pixels (0..255, 0..191) at every size.
 * A GBA core has one screen: screen[1] is NULL and the frame is
 * NP_GBA_SCREEN_W x NP_GBA_SCREEN_H (times the render scale). */
typedef struct np_frame {
    /* [0] is the top screen, [1] the bottom. Pixels are 0x00RRGGBB, row
     * major, `stride` pixels apart. Valid until the next np_core_run_frame. */
    const uint32_t *screen[2];
    uint32_t width, height, stride;
    uint64_t number; /* VBlanks since boot */
} np_frame;

typedef struct np_core np_core;

/*
 * `options` is a NULL-terminated list of "KEY=VALUE" strings handed to the
 * core's environment (the port reads PC_* settings there). May be NULL.
 * Returns NULL on failure; np_core_create_error() then says why.
 */
np_core *np_core_create(np_game game, const np_host *host, const char *const *options);
const char *np_core_create_error(void);

/* Whether this binary contains `game`'s core. */
int np_core_available(np_game game);

/*
 * Run the guest until its next VBlank. `in` is the input sampled for that
 * frame. On return `out` describes the finished frame. Returns 0, 1 once the
 * guest has exited, or -1 after a fatal guest error (np_core_last_error).
 */
int np_core_run_frame(np_core *core, const np_input *in, np_frame *out);

/* Audio produced so far: interleaved signed 16-bit stereo at
 * np_core_audio_rate() Hz, which is 0 until the first frame has run (the
 * guest announces it). Returns frames copied (each frame = 2 samples).
 * Unread audio older than the core's ring (about one second) is dropped. */
uint32_t np_core_audio_rate(const np_core *core);
size_t np_core_audio_read(np_core *core, int16_t *stereo, size_t max_frames);

/* Persist the backup chip now if it changed (app backgrounding, quit). */
int np_core_save_flush(np_core *core);

/*
 * Settings and state shared with the guest; ids are enum np_opt / enum
 * np_status from np_guest_abi.h. Options take effect from the next
 * np_core_run_frame. Defaults: volumes 256, render scale 1, camera zoom 256,
 * everything else 0. Status reads the value the guest reported with its
 * last frame (0 before the first).
 */
void np_core_set_option(np_core *core, uint32_t opt, uint32_t value);
uint32_t np_core_get_option(const np_core *core, uint32_t opt);
uint32_t np_core_status(const np_core *core, uint32_t status);

/*
 * In-session snapshots (checkpoints, quick states, rewind). A snapshot is
 * the whole machine between two frames: guest memory, runtime state and the
 * native stacks of parked guest threads. It is only valid for the same
 * np_core in the same process (native stacks hold code addresses), so it is
 * never written to disk; the cartridge save is the persistent state.
 * state_size is an upper bound for a state_save made before the next
 * np_core_run_frame (it grows as the guest touches more memory), or 0 when
 * no snapshot can be taken (MSVC-built Windows cores; a failed or exited core). Both
 * return 0 on success. A refused state_load (not this core's snapshot,
 * truncated, or taken before a soft reset: NP_STAT_RESETS changed since)
 * returns -1 and leaves the core as it was; one that fails
 * halfway leaves the core failed (np_core_last_error). Loading also
 * revives a failed or exited core, and reverts the guest's backup chip
 * image with the rest of memory (the host's stored save is untouched).
 * Options are the host's and survive a load, except NP_OPT_QUICKSAVE_SEQ,
 * a request counter, which is set to the restored NP_STAT_QUICKSAVE_SEQ so
 * a load never fires a quick save.
 */
size_t np_core_state_size(const np_core *core);
int np_core_state_save(np_core *core, void *dst, size_t cap, size_t *written);
int np_core_state_load(np_core *core, const void *src, size_t len);

/* A host pointer to `len` bytes of guest memory at `guest_addr`, or NULL if
 * the range is not guest memory. For the save editor, mods and diagnostics;
 * valid until np_core_destroy. */
uint8_t *np_core_guest_ptr(np_core *core, uint32_t guest_addr, uint32_t len);

const char *np_core_last_error(const np_core *core);
void np_core_destroy(np_core *core);

#ifdef __cplusplus
}
#endif

#endif /* NP_CORE_H */
