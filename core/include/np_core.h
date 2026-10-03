/*
 * np_core: what the shell sees of a game core.
 *
 * A core is one game (Diamond, Pearl or Platinum) compiled to wasm32 and
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

#define NP_CORE_API_VERSION 1

typedef enum np_game {
    NP_GAME_DIAMOND = 0,
    NP_GAME_PEARL = 1,
    NP_GAME_PLATINUM = 2,
    NP_GAME_COUNT
} np_game;

/* DS keypad bits, in the SDK's PAD_* order. */
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
} np_host;

typedef struct np_input {
    uint16_t keys;      /* NP_KEY_* */
    uint8_t touch;      /* 1 while the stylus is down */
    uint8_t lid_closed; /* 1 to close the lid (sleep) */
    uint16_t touch_x;   /* bottom-screen pixel, 0..255 */
    uint16_t touch_y;   /* bottom-screen pixel, 0..191 */
} np_input;

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
