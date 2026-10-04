/*
 * Drives the mock guest (mock_guest.c, linked as both "diamond" and
 * "platinum") through np_core.h only, the way the shell will.
 *
 * Checks: module availability and create errors, frame geometry and the
 * exact pixel pattern, input echo, fiber scheduling, ROM reads, both save
 * paths (guest-initiated store with a host failure and retry, and the
 * host-initiated flush of the published chip image), audio contents and
 * ring overflow, WASI stdout and env routing, determinism across two runs,
 * a save round trip, and that guest traps, wasm traps and exit leave the
 * core in a clean terminal state.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np_core.h"
#include "np_guest_abi.h"

#define ROM_SIZE 0x10000u
#define CHIP_SIZE 256u
#define AUDIO_RATE 32768u
#define AUDIO_RING 4096u

/* Mirrors mock_guest.c. */
enum {
    ST_ROM_OK = 1u << 0,
    ST_ENV_OK = 1u << 1,
    ST_CLOCK_OK = 1u << 2,
    ST_LAYOUT_OK = 1u << 3,
    ST_SAVE_LOADED = 1u << 4,
    ST_STORE_FAILED = 1u << 5,
    ST_HOST_FLUSHED = 1u << 6,
    ST_FIBERS_OK = 1u << 7,
};
#define ST_ALWAYS (ST_ROM_OK | ST_ENV_OK | ST_CLOCK_OK | ST_LAYOUT_OK | ST_FIBERS_OK)
#define SEED 7u

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                   \
        }                                                                                                              \
    } while (0)

#define CHECKF(cond, ...)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #cond);                                   \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

/* ---- test host ------------------------------------------------------- */

typedef struct test_host {
    uint8_t rom[ROM_SIZE];
    uint8_t save[CHIP_SIZE];
    uint32_t save_len;
    int fail_next_store;
    int stores;
    char log[64][160];
    int nlog;
    /* loopback transport: what is sent comes back to the sender */
    uint8_t net_buf[8][64];
    uint32_t net_len[8];
    uint32_t net_peer[8];
    int net_count, net_sent;
} test_host;

#define NET_SELF 7u

static uint32_t host_net_self(void *user) {
    (void)user;
    return NET_SELF;
}

static int host_net_send(void *user, uint32_t peer, const void *buf, uint32_t len) {
    test_host *h = user;
    if (peer != NP_NET_BROADCAST || len > sizeof h->net_buf[0] || h->net_count == 8) return -1;
    memcpy(h->net_buf[h->net_count], buf, len);
    h->net_len[h->net_count] = len;
    h->net_peer[h->net_count] = NET_SELF;
    h->net_count++;
    h->net_sent++;
    return 0;
}

static int host_net_recv(void *user, uint32_t *peer, void *buf, uint32_t cap) {
    test_host *h = user;
    if (h->net_count == 0) return 0;
    uint32_t len = h->net_len[0];
    if (len > cap) return -1;
    memcpy(buf, h->net_buf[0], len);
    *peer = h->net_peer[0];
    h->net_count--;
    memmove(h->net_buf[0], h->net_buf[1], sizeof h->net_buf[0] * (size_t)h->net_count);
    memmove(h->net_len, h->net_len + 1, sizeof h->net_len[0] * (size_t)h->net_count);
    memmove(h->net_peer, h->net_peer + 1, sizeof h->net_peer[0] * (size_t)h->net_count);
    return (int)len;
}

static uint8_t rom_byte(uint32_t i) {
    return (uint8_t)(i * 7u + (i >> 8) + 3u);
}

static int host_rom_read(void *user, uint32_t offset, void *dst, uint32_t len) {
    test_host *h = user;
    if ((uint64_t)offset + len > ROM_SIZE) return -1;
    memcpy(dst, h->rom + offset, len);
    return 0;
}

static int host_save_load(void *user, void *dst, uint32_t len) {
    test_host *h = user;
    if (h->save_len == 0) return 0;
    if (len != h->save_len) return -1;
    memcpy(dst, h->save, len);
    return 1;
}

static int host_save_store(void *user, const void *src, uint32_t len) {
    test_host *h = user;
    if (h->fail_next_store) {
        h->fail_next_store = 0;
        return -1;
    }
    if (len > sizeof h->save) return -1;
    memcpy(h->save, src, len);
    h->save_len = len;
    h->stores++;
    return 0;
}

static void host_log(void *user, const char *line) {
    test_host *h = user;
    if (getenv("NP_TEST_VERBOSE")) fprintf(stderr, "  guest: %s\n", line);
    if (h->nlog < 64) snprintf(h->log[h->nlog++], sizeof h->log[0], "%s", line);
}

static int logged(const test_host *h, const char *needle) {
    for (int i = 0; i < h->nlog; i++)
        if (strstr(h->log[i], needle)) return 1;
    return 0;
}

static np_host make_host(test_host *th) {
    for (uint32_t i = 0; i < ROM_SIZE; i++) th->rom[i] = rom_byte(i);
    np_host h = {0};
    h.user = th;
    h.rom_size = ROM_SIZE;
    h.rom_read = host_rom_read;
    h.save_load = host_save_load;
    h.save_store = host_save_store;
    h.log = host_log;
    return h;
}

static const char *const k_options[] = {"NP_MOCK_SEED=7", "PC_TEST=1", NULL};

/* ---- expectations ---------------------------------------------------- */

static uint32_t top_pixel(uint32_t frame, uint32_t x, uint32_t y) {
    return ((x + frame) & 0xFFu) << 16 | ((y + SEED) & 0xFFu) << 8 | ((x ^ y) & 0xFFu);
}

static uint32_t bottom_pixel(uint32_t frame, uint32_t x, uint32_t y) {
    return ((x * y + frame) & 0xFFu) << 16 | (x & 0xFFu) << 8 | ((y + frame * 2u) & 0xFFu);
}

static int16_t sine_sample(uint32_t k) {
    float phase = (float)(k % AUDIO_RATE) * (440.0f * 2.0f * 3.14159265f / (float)AUDIO_RATE);
    return (int16_t)lrintf(sinf(phase) * 12000.0f);
}

static uint32_t audio_produced(uint32_t frames) {
    return (uint32_t)(((uint64_t)AUDIO_RATE * frames) / 60);
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

static np_input input_for(uint32_t frame) {
    np_input in = {0};
    in.keys = (uint16_t)(1u << (frame % 8)); /* single keys only: combos trigger trap/exit */
    in.touch = (uint8_t)(frame & 1);
    in.touch_x = (uint16_t)(frame * 10 % 256);
    in.touch_y = (uint16_t)(frame * 5 % 192);
    in.lid_closed = (uint8_t)(frame == 4);
    return in;
}

static void check_frame(const np_frame *f, uint32_t k, const np_input *in, int input_applied) {
    CHECK(f->width == NP_SCREEN_W && f->height == NP_SCREEN_H && f->stride == NP_SCREEN_W);
    CHECKF(f->number == k, "number %llu want %u", (unsigned long long)f->number, k);
    CHECK(f->screen[0] && f->screen[1]);
    if (!f->screen[0] || !f->screen[1]) return;
    int bad = 0;
    for (uint32_t y = 0; y < NP_SCREEN_H; y++)
        for (uint32_t x = 0; x < NP_SCREEN_W; x++) {
            if (!(y == 0 && x < 3) && f->screen[0][y * f->stride + x] != top_pixel(k, x, y)) bad++;
            if (!(y == 0 && x < 5) && f->screen[1][y * f->stride + x] != bottom_pixel(k, x, y)) bad++;
        }
    CHECKF(bad == 0, "frame %u: %d pixels differ from the pattern", k, bad);
    if (input_applied) {
        CHECKF(f->screen[0][0] == in->keys, "frame %u keys echo %#x want %#x", k, f->screen[0][0], in->keys);
        uint32_t touch = (uint32_t)in->touch << 16 | (uint32_t)(in->touch_x & 0xFF) << 8 | (in->touch_y & 0xFF);
        CHECKF(f->screen[0][1] == touch, "frame %u touch echo %#x want %#x", k, f->screen[0][1], touch);
        CHECK(f->screen[0][2] == in->lid_closed);
    }
    const uint32_t status = f->screen[1][0];
    CHECKF((status & ST_ALWAYS) == ST_ALWAYS, "frame %u status %#x", k, status);
    CHECK(f->screen[1][2] == SEED);
    CHECKF(f->screen[1][4] == (k == 2 ? 15u : 12u), "frame %u switches %u", k, f->screen[1][4]);
}

/* ---- scenarios ------------------------------------------------------- */

#define SCRIPT_FRAMES 20u

/* The main scripted session. Returns a hash of everything observable. */
static uint64_t run_script(test_host *th, int check_extras) {
    memset(th, 0, sizeof *th);
    np_host host = make_host(th);
    th->fail_next_store = 1; /* the guest's frame-3 store fails once */

    np_core *c = np_core_create(NP_GAME_DIAMOND, &host, k_options);
    CHECKF(c != NULL, "create: %s", np_core_create_error());
    if (!c) return 0;
    if (check_extras) {
        CHECK(np_core_create(NP_GAME_PLATINUM, &host, k_options) == NULL);
        CHECK(strstr(np_core_create_error(), "still alive") != NULL);
    }
    CHECK(np_core_audio_rate(c) == 0);

    uint64_t hash = 0xCBF29CE484222325ull;
    uint32_t audio_read_total = 0;
    static int16_t audio[2 * 8192];
    for (uint32_t k = 0; k < SCRIPT_FRAMES; k++) {
        np_input in = input_for(k);
        np_frame f;
        int rc = np_core_run_frame(c, &in, &f);
        CHECKF(rc == 0, "frame %u rc %d: %s", k, rc, np_core_last_error(c));
        if (rc != 0) break;
        check_frame(&f, k, &in, k > 0);
        const uint32_t status = f.screen[1][0];
        CHECKF(!!(status & ST_STORE_FAILED) == (k >= 3), "frame %u status %#x", k, status);
        CHECKF(!!(status & ST_HOST_FLUSHED) == (k >= 6), "frame %u status %#x", k, status);
        CHECK(!(status & ST_SAVE_LOADED));
        CHECK(f.screen[1][1] == (k >= 3 ? 1u : 0u));
        hash = fnv(hash, f.screen[0], NP_SCREEN_W * NP_SCREEN_H * 4);
        hash = fnv(hash, f.screen[1], NP_SCREEN_W * NP_SCREEN_H * 4);
        CHECK(np_core_audio_rate(c) == AUDIO_RATE);

        if (k == 4) {
            /* The guest's store failed (host refused); flush retries the copy. */
            CHECK(th->stores == 0);
            CHECK(np_core_save_flush(c) == 0);
            CHECK(th->stores == 1 && th->save_len == CHIP_SIZE);
            uint32_t counter;
            memcpy(&counter, th->save + 4, 4);
            CHECK(memcmp(th->save, "NPSV", 4) == 0 && counter == 1);
            CHECK(np_core_save_flush(c) == 0 && th->stores == 1); /* nothing pending */
        }
        if (k == 5) {
            /* The guest published a dirty chip image; the host stores it. */
            CHECK(np_core_save_flush(c) == 0);
            CHECK(th->stores == 2);
            int ok = 1;
            for (uint32_t i = 16; i < CHIP_SIZE; i++) ok &= th->save[i] == (uint8_t)(i + 5);
            CHECK(ok);
            np_frame_desc d;
            memcpy(&d, np_core_guest_ptr(c, 0x02000000u, sizeof d), sizeof d);
            CHECK(d.magic == NP_FRAME_MAGIC && d.save_dirty == 0 && d.save_size == CHIP_SIZE);
        }

        if (k < 6) {
            /* Drain every frame and compare against the sine. */
            size_t n = np_core_audio_read(c, audio, 8192);
            CHECKF(audio_read_total + n == audio_produced(k + 1), "frame %u audio %zu", k, n);
            int bad = 0;
            for (size_t i = 0; i < n; i++) {
                int16_t want = sine_sample(audio_read_total + (uint32_t)i);
                if (abs(audio[2 * i] - want) > 1 || audio[2 * i + 1] != -audio[2 * i]) bad++;
            }
            CHECKF(bad == 0, "frame %u: %d audio samples wrong", k, bad);
            hash = fnv(hash, audio, n * 4);
            audio_read_total += (uint32_t)n;
        }
    }

    /* Frames 6..19 were not drained: only the newest ring's worth is left. */
    size_t n = np_core_audio_read(c, audio, 8192);
    CHECKF(n == AUDIO_RING, "overflow read %zu", n);
    uint32_t first = audio_produced(SCRIPT_FRAMES) - AUDIO_RING;
    int bad = 0;
    for (size_t i = 0; i < n; i++)
        if (abs(audio[2 * i] - sine_sample(first + (uint32_t)i)) > 1) bad++;
    CHECKF(bad == 0, "%d audio samples wrong after overflow", bad);
    CHECK(np_core_audio_read(c, audio, 8192) == 0);
    hash = fnv(hash, audio, n * 4);

    if (check_extras) {
        const uint8_t *d = np_core_guest_ptr(c, 0x02000000u, sizeof(np_frame_desc));
        CHECK(d != NULL);
        CHECK(np_core_guest_ptr(c, NP_GUEST_MEMORY_BYTES - 4, 4) != NULL);
        CHECK(np_core_guest_ptr(c, NP_GUEST_MEMORY_BYTES - 4, 8) == NULL);
        CHECK(np_core_guest_ptr(c, 0xFFFFFFFFu, 2) == NULL);
        CHECK(logged(th, "mock: boot argv0=diamond argc=1 seed=7"));
        CHECK(logged(th, "mock: direct log"));
        CHECK(logged(th, "mock: second line"));
    }
    np_core_destroy(c);
    return hash;
}

/* Boots, runs to frame `at`, then sends `keys`; returns the core. */
static np_core *run_until(test_host *th, np_host *host, np_game game, uint32_t at, uint16_t keys, int *rc) {
    memset(th, 0, sizeof *th);
    *host = make_host(th);
    np_core *c = np_core_create(game, host, k_options);
    CHECKF(c != NULL, "create: %s", np_core_create_error());
    if (!c) return NULL;
    np_frame f;
    for (uint32_t k = 0; k < at; k++) {
        np_input in = input_for(k);
        CHECK(np_core_run_frame(c, &in, &f) == 0);
    }
    np_input in = {0};
    in.keys = keys;
    *rc = np_core_run_frame(c, &in, &f);
    CHECK(f.screen[0] == NULL); /* no frame on a terminal return */
    return c;
}

static void check_dead(np_core *c, int want_rc) {
    np_frame f;
    int16_t audio[64];
    CHECK(np_core_run_frame(c, NULL, &f) == want_rc);
    CHECK(np_core_run_frame(c, NULL, &f) == want_rc);
    CHECK(np_core_audio_read(c, audio, 32) == 0);
    CHECK(np_core_save_flush(c) == 0);
}

/* ---- contract v2 ----------------------------------------------------- */

static np_core *boot(test_host *th, np_host *host, int with_net) {
    memset(th, 0, sizeof *th);
    *host = make_host(th);
    if (with_net) {
        host->net_self = host_net_self;
        host->net_send = host_net_send;
        host->net_recv = host_net_recv;
    }
    np_core *c = np_core_create(NP_GAME_DIAMOND, host, k_options);
    CHECKF(c != NULL, "create: %s", np_core_create_error());
    return c;
}

/* Options reach the guest's descriptor before the frame they were set for;
 * the status the guest publishes with that frame comes back. */
static void test_v2_options(test_host *th) {
    np_host host;
    np_core *c = boot(th, &host, 0);
    if (!c) return;
    CHECK(np_core_get_option(c, NP_OPT_BGM_VOLUME) == 256 && np_core_get_option(c, NP_OPT_SE_VOLUME) == 256);
    CHECK(np_core_get_option(c, NP_OPT_RENDER_SCALE) == 1 && np_core_get_option(c, NP_OPT_CAMERA_ZOOM) == 256);
    CHECK(np_core_get_option(c, NP_OPT_WIDESCREEN) == 0 && np_core_get_option(c, NP_OPT_RULES) == 0);
    CHECK(np_core_get_option(c, NP_OPT_COUNT) == 0 && np_core_status(c, NP_STAT_COUNT) == 0);
    CHECK(np_core_status(c, NP_STAT_MAP_ID) == 0); /* nothing reported yet */

    np_frame f;
    np_input in = input_for(0);
    CHECK(np_core_run_frame(c, &in, &f) == 0);
    check_frame(&f, 0, &in, 0);
    CHECK(np_core_status(c, NP_STAT_MAP_ID) == 400 && np_core_status(c, NP_STAT_FIELD_READY) == 1);
    /* Frame 0 ran before the descriptor was known: no options yet. */
    CHECK(np_core_status(c, 8 + NP_OPT_BGM_VOLUME) == 0);

    np_core_set_option(c, NP_OPT_QUICKSAVE_SEQ, 5);
    np_core_set_option(c, NP_OPT_CAMERA_TILT, (uint32_t)-32);
    np_core_set_option(c, NP_OPT_BGM_VOLUME, 100);
    np_core_set_option(c, NP_OPT_COUNT + 3, 9); /* ignored */
    CHECK(np_core_get_option(c, NP_OPT_BGM_VOLUME) == 100);
    in = input_for(1);
    CHECK(np_core_run_frame(c, &in, &f) == 0);
    check_frame(&f, 1, &in, 1);
    CHECK(np_core_status(c, NP_STAT_QUICKSAVE_SEQ) == 5 && np_core_status(c, NP_STAT_QUICKSAVE_RESULT) == NP_QS_SAVED);
    CHECK(np_core_status(c, NP_STAT_MAP_ID) == 401);
    CHECK(np_core_status(c, 8 + NP_OPT_BGM_VOLUME) == 100 && np_core_status(c, 8 + NP_OPT_SE_VOLUME) == 256);
    CHECK(np_core_status(c, 8 + NP_OPT_CAMERA_TILT) == (uint32_t)-32);
    CHECK(np_core_status(c, 8 + NP_OPT_CAMERA_ZOOM) == 256 && np_core_status(c, 8 + NP_OPT_RENDER_SCALE) == 1);
    CHECK(np_core_status(c, NP_STAT_LINK_ACTIVE) == 0 && np_core_status(c, 6) == 0);

    /* Frame size follows the guest from one frame to the next. */
    np_core_set_option(c, NP_OPT_RENDER_SCALE, 2);
    in = input_for(2);
    CHECK(np_core_run_frame(c, &in, &f) == 0);
    CHECKF(f.width == 512 && f.height == 384 && f.stride == 512, "scaled frame %ux%u/%u", f.width, f.height,
           f.stride);
    if (f.screen[0] && f.screen[1] && f.width == 512) {
        int bad = 0;
        for (uint32_t i = 0; i < 512u * 384u; i++)
            bad += f.screen[0][i] != (0x00100000u | 2u) || f.screen[1][i] != (0x00200000u | 2u);
        CHECKF(bad == 0, "%d scaled pixels wrong", bad);
    }
    np_core_set_option(c, NP_OPT_RENDER_SCALE, 1);
    in = input_for(3);
    CHECK(np_core_run_frame(c, &in, &f) == 0);
    check_frame(&f, 3, &in, 1);
    np_core_destroy(c);
}

/* A loopback transport: every datagram the guest sends comes back intact. */
static void test_v2_net(test_host *th) {
    np_host host;
    np_core *c = boot(th, &host, 1);
    if (!c) return;
    np_frame f;
    for (uint32_t k = 0; k < 6; k++) {
        np_input in = input_for(k);
        CHECK(np_core_run_frame(c, &in, &f) == 0);
        check_frame(&f, k, &in, k > 0);
        CHECKF(np_core_status(c, 5) == k + 1, "frame %u: %u datagrams back", k, np_core_status(c, 5));
    }
    CHECK(th->net_sent == 6 && th->net_count == 0);
    CHECK(np_core_status(c, NP_STAT_LINK_ACTIVE) == 1 && np_core_status(c, 6) == NET_SELF);
    np_core_destroy(c);
}

/* ---- snapshots ------------------------------------------------------- */

static uint64_t run_hashed(np_core *c, uint32_t from, uint32_t count) {
    uint64_t hash = 0xCBF29CE484222325ull;
    static int16_t audio[2 * 8192];
    for (uint32_t k = from; k < from + count; k++) {
        np_input in = input_for(k);
        np_frame f;
        int rc = np_core_run_frame(c, &in, &f);
        CHECKF(rc == 0, "frame %u rc %d: %s", k, rc, np_core_last_error(c));
        if (rc != 0) return 0;
        check_frame(&f, k, &in, 1);
        hash = fnv(hash, f.screen[0], NP_SCREEN_W * NP_SCREEN_H * 4);
        hash = fnv(hash, f.screen[1], NP_SCREEN_W * NP_SCREEN_H * 4);
        size_t n = np_core_audio_read(c, audio, 8192);
        CHECKF(n == audio_produced(k + 1) - audio_produced(k), "frame %u audio %zu", k, n);
        hash = fnv(hash, audio, n * 4);
        for (uint32_t s = 0; s < NP_STAT_COUNT; s++) {
            uint32_t v = np_core_status(c, s);
            hash = fnv(hash, &v, 4);
        }
    }
    return hash;
}

static void *snapshot(np_core *c, size_t *len) {
    size_t cap = np_core_state_size(c);
    CHECK(cap > 0);
    void *buf = malloc(cap ? cap : 1);
    *len = 0;
    CHECKF(np_core_state_save(c, buf, cap, len) == 0, "save: %s", np_core_last_error(c));
    CHECK(*len > 0 && *len <= cap);
    return buf;
}

/*
 * Save, run, load, run again: the same frames, audio and status. The
 * windows straddle frame 2, where the guest creates, ping-pongs with and
 * destroys a fiber, so a load has to bring back a fiber table (and stack
 * pool) from before and after that.
 */
static void test_snapshots(test_host *th) {
    np_host host;
    np_core *c = boot(th, &host, 0);
    if (!c) return;
    np_frame f;
    np_input in = input_for(0);
    CHECK(np_core_run_frame(c, &in, &f) == 0);
    int16_t audio[2 * 4096];
    while (np_core_audio_read(c, audio, 4096) > 0) {}

    size_t len1;
    void *s1 = snapshot(c, &len1); /* after frame 0 */
    uint64_t a = run_hashed(c, 1, 6);
    size_t len2;
    void *s2 = snapshot(c, &len2); /* after frame 6, past the fiber's life */
    uint64_t a2 = run_hashed(c, 7, 4);

    CHECKF(np_core_state_load(c, s1, len1) == 0, "load: %s", np_core_last_error(c));
    uint64_t b = run_hashed(c, 1, 6);
    CHECKF(a == b && a != 0, "frames 1-6 after load %016llx, first run %016llx", (unsigned long long)b,
           (unsigned long long)a);
    uint64_t b2 = run_hashed(c, 7, 4);
    CHECK(a2 == b2 && a2 != 0);

    /* Back past the fiber's destruction and forward again, twice. */
    for (int i = 0; i < 2; i++) {
        CHECK(np_core_state_load(c, s2, len2) == 0);
        CHECK(run_hashed(c, 7, 4) == a2);
        CHECK(np_core_state_load(c, s1, len1) == 0);
        CHECK(run_hashed(c, 1, 6) == a);
    }

    /* Refusals leave the core running. */
    uint8_t *bad = malloc(len1);
    memcpy(bad, s1, len1);
    bad[0] ^= 0xFF;
    CHECK(np_core_state_load(c, bad, len1) == -1);
    CHECK(np_core_state_load(c, s1, len1 - 16) == -1);
    CHECK(np_core_state_load(c, s1, 8) == -1);
    free(bad);
    size_t w = 0;
    void *tiny = malloc(64);
    CHECK(np_core_state_save(c, tiny, 64, &w) == -1 && w == 0);
    free(tiny);
    CHECK(run_hashed(c, 7, 4) == a2);

    /* A load revives a core a guest trap killed. */
    CHECK(np_core_state_load(c, s1, len1) == 0);
    in = input_for(1);
    in.keys = NP_KEY_L | NP_KEY_R;
    CHECK(np_core_run_frame(c, &in, &f) == -1);
    CHECK(np_core_state_size(c) == 0);
    CHECKF(np_core_state_load(c, s1, len1) == 0, "revive: %s", np_core_last_error(c));
    CHECK(run_hashed(c, 1, 6) == a);

    printf("snapshot: %zu bytes after frame 0, %zu after frame 6 (bound %zu)\n", len1, len2, np_core_state_size(c));
    np_core_destroy(c);

    /* Another core (even at the same address) refuses this core's state. */
    c = boot(th, &host, 0);
    if (c) {
        in = input_for(0);
        CHECK(np_core_run_frame(c, &in, &f) == 0);
        CHECK(np_core_state_load(c, s1, len1) == -1);
        CHECK(strstr(np_core_last_error(c), "another core") != NULL);
        while (np_core_audio_read(c, audio, 4096) > 0) {}
        CHECK(run_hashed(c, 1, 6) == a);
        np_core_destroy(c);
    }
    free(s1);
    free(s2);
}

int main(void) {
    static test_host th, th2;
    np_host host;
    int rc;

    /* Availability. */
    CHECK(np_core_available(NP_GAME_DIAMOND));
    CHECK(np_core_available(NP_GAME_PLATINUM));
    CHECK(!np_core_available(NP_GAME_PEARL));
    CHECK(!np_core_available(NP_GAME_COUNT));
    host = make_host(&th);
    CHECK(np_core_create(NP_GAME_PEARL, &host, NULL) == NULL);
    CHECK(strstr(np_core_create_error(), "pearl") != NULL);

    /* Determinism: two identical sessions observe identical output. */
    uint64_t h1 = run_script(&th, 1);
    uint64_t h2 = run_script(&th2, 0);
    CHECKF(h1 == h2 && h1 != 0, "hashes %016llx %016llx", (unsigned long long)h1, (unsigned long long)h2);
    CHECK(th.save_len == th2.save_len && memcmp(th.save, th2.save, CHIP_SIZE) == 0);
    printf("deterministic session hash %016llx\n", (unsigned long long)h1);

    /* Save round trip: boot with the chip the previous session stored. */
    {
        uint8_t saved[CHIP_SIZE];
        memcpy(saved, th2.save, CHIP_SIZE);
        memset(&th, 0, sizeof th);
        host = make_host(&th);
        memcpy(th.save, saved, CHIP_SIZE);
        th.save_len = CHIP_SIZE;
        np_core *c = np_core_create(NP_GAME_DIAMOND, &host, k_options);
        CHECK(c != NULL);
        np_frame f;
        for (uint32_t k = 0; c && k < 5; k++) {
            np_input in = input_for(k);
            CHECK(np_core_run_frame(c, &in, &f) == 0);
            CHECK(f.screen[1][0] & ST_SAVE_LOADED);
            CHECK(f.screen[1][1] == (k >= 3 ? 2u : 1u));
        }
        uint32_t counter;
        memcpy(&counter, th.save + 4, 4);
        CHECK(th.stores == 1 && counter == 2);
        np_core_destroy(c);
    }

    /* Guest trap: message reported, later calls fail cleanly. */
    {
        np_core *c = run_until(&th, &host, NP_GAME_DIAMOND, 2, NP_KEY_L | NP_KEY_R, &rc);
        CHECK(rc == -1);
        CHECKF(c && strstr(np_core_last_error(c), "guest trap: mock: trap requested at frame 2"), "%s",
               c ? np_core_last_error(c) : "");
        if (c) check_dead(c, -1);
        np_core_destroy(c);
    }

    /* Wasm trap (unreachable) on a worker fiber. */
    {
        np_core *c = run_until(&th, &host, NP_GAME_DIAMOND, 3, NP_KEY_X | NP_KEY_Y, &rc);
        CHECK(rc == -1);
        CHECKF(c && strstr(np_core_last_error(c), "wasm trap: Unreachable"), "%s", c ? np_core_last_error(c) : "");
        if (c) check_dead(c, -1);
        np_core_destroy(c);
    }

#if defined(NP_BOUNDS_CHECK)
    /* Bounds-check builds turn an access outside linear memory into a
     * reported wasm trap (default builds have no check; see np_memory.c). */
    {
        np_core *c = run_until(&th, &host, NP_GAME_DIAMOND, 1, NP_KEY_A | NP_KEY_B, &rc);
        CHECK(rc == -1);
        CHECKF(c && strstr(np_core_last_error(c), "wasm trap: Out-of-bounds"), "%s", c ? np_core_last_error(c) : "");
        if (c) check_dead(c, -1);
        np_core_destroy(c);
    }
#endif

    /* Exit through proc_exit, on the second module. */
    {
        np_core *c = run_until(&th, &host, NP_GAME_PLATINUM, 2, NP_KEY_START | NP_KEY_SELECT, &rc);
        CHECK(rc == 1);
        CHECKF(c && strstr(np_core_last_error(c), "status 3"), "%s", c ? np_core_last_error(c) : "");
        CHECK(logged(&th, "mock: boot argv0=platinum"));
        CHECK(logged(&th, "mock: exiting at frame 2"));
        if (c) check_dead(c, 1);
        np_core_destroy(c);
    }

    test_v2_options(&th);
    test_v2_net(&th);
    test_snapshots(&th);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all core runtime checks passed\n");
    return 0;
}
