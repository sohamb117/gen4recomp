/*
 * Mock guest for the core runtime tests, compiled to wasm32 with wasi-sdk.
 *
 * It does what a real game core does through the same contract
 * (np_guest_abi.h), in miniature, so test_core.c can drive it through
 * np_core.h alone:
 *   - a scheduler on the boot fiber and three worker fibers with their own
 *     malloc'd shadow stacks, switched round-robin several times a frame;
 *     every worker keeps address-taken locals on its shadow stack and checks
 *     them, and its own fiber handle, after every switch;
 *   - a deterministic pattern in two 256x192 screens (the bottom one in DS
 *     VRAM at 0x06000000, the descriptor in DS main RAM at 0x02000000);
 *   - a 440 Hz sine into the audio ring;
 *   - ROM reads checked against the pattern test_core.c serves;
 *   - a 256-byte backup chip: loaded at boot, stored by the guest at frame 3,
 *     published in the descriptor and dirtied at frame 5 for the host's
 *     np_core_save_flush to store;
 *   - input echoed into pixels; special key combos trigger np_host_trap, a
 *     wasm `unreachable` on a worker fiber, and exit(3);
 *   - contract v2: options echoed into status (see publish_status), a
 *     512x384 frame while NP_OPT_RENDER_SCALE is 2, and a datagram to
 *     NP_NET_BROADCAST every frame that a loopback host hands back;
 *   - with NP_MOCK_CONTENT, reads, seeks, stats and listings under the
 *     NP_CONTENT_DIR preopen, and every escape and write refused
 *     (check_content); a GBA slot ROM checked and its save stamped and
 *     stored at frame 4 (check_gba).
 *
 * Status pixels (bottom screen row 0) and the pattern formulas are mirrored
 * in test_core.c; keep the two in sync.
 */
#define _DEFAULT_SOURCE /* clock_gettime, getentropy under -std=c11 */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <wasi/api.h>

#include "np_guest_abi.h"

#define W 256
#define H 192
#define SLICES 4
#define NWORKERS 3
#define SHADOW_STACK (64 * 1024)
#define AUDIO_RATE 32768
#define AUDIO_RING 4096
#define CHIP_SIZE 256
#define GBA_SAVE_SIZE 512

#define KEY_A (1u << 0)
#define KEY_B (1u << 1)
#define KEY_SELECT (1u << 2)
#define KEY_START (1u << 3)
#define KEY_R (1u << 8)
#define KEY_L (1u << 9)
#define KEY_X (1u << 10)
#define KEY_Y (1u << 11)

enum {
    ST_ROM_OK = 1u << 0,
    ST_ENV_OK = 1u << 1,
    ST_CLOCK_OK = 1u << 2,
    ST_LAYOUT_OK = 1u << 3,
    ST_SAVE_LOADED = 1u << 4,
    ST_STORE_FAILED = 1u << 5,
    ST_HOST_FLUSHED = 1u << 6,
    ST_FIBERS_OK = 1u << 7,
    ST_CONTENT_OK = 1u << 8,      /* NP_MOCK_CONTENT: every check_content() case held */
    ST_GBA_ROM_OK = 1u << 9,      /* the GBA slot's ROM read back as test_core serves it */
    ST_GBA_SAVE_LOADED = 1u << 10,
};

#define DESC ((np_frame_desc *)0x02000000u)
#define BOTTOM ((uint32_t *)0x06000000u)

static uint32_t top[W * H];
static uint32_t *audio_ring;
static uint8_t chip[CHIP_SIZE];
static uint8_t gba_save[GBA_SAVE_SIZE];

static uint32_t frame;
static uint32_t seed;
static uint32_t status;
static uint32_t random_word;
static uint32_t save_counter;
static int crash_on_worker; /* set by X+Y: worker 2 executes unreachable */
static uint32_t *big[2];    /* the 512x384 screens of render scale 2 */
static uint32_t net_ok;     /* datagrams received intact */

static uint32_t sched_handle;
static uint32_t switches_this_frame;
static uint32_t expected_turn; /* index of the worker that must run next */

typedef struct worker {
    uint32_t id;
    uint32_t handle;
    uint8_t *shadow;
} worker;
static worker workers[NWORKERS];

static void fail(const char *msg) {
    np_host_trap(msg, (uint32_t)strlen(msg));
}

static uint8_t rom_byte(uint32_t i) {
    return (uint8_t)(i * 7u + (i >> 8) + 3u);
}

static uint32_t top_pixel(uint32_t x, uint32_t y) {
    return ((x + frame) & 0xFFu) << 16 | ((y + seed) & 0xFFu) << 8 | ((x ^ y) & 0xFFu);
}

static uint32_t bottom_pixel(uint32_t x, uint32_t y) {
    return ((x * y + frame) & 0xFFu) << 16 | (x & 0xFFu) << 8 | ((y + frame * 2u) & 0xFFu);
}

static void yield_to_scheduler(void) {
    switches_this_frame++;
    np_host_fiber_switch(sched_handle);
}

/* Keeps `p` opaque to the optimiser so the array really lives in the
 * shadow stack in linear memory. */
__attribute__((noinline)) static void touch(volatile uint32_t *p) {
    (void)p;
}

static void draw_rows(uint32_t *dst, uint32_t (*pixel)(uint32_t, uint32_t), uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = 0; x < W; x++) dst[y * W + x] = pixel(x, y);
}

static void make_audio(uint32_t slice) {
    uint32_t n = (uint32_t)(((uint64_t)AUDIO_RATE * (frame + 1)) / 60 - ((uint64_t)AUDIO_RATE * frame) / 60);
    uint32_t a = n * slice / SLICES, b = n * (slice + 1) / SLICES;
    for (uint32_t i = a; i < b; i++) {
        uint32_t k = DESC->audio_head;
        float phase = (float)(k % AUDIO_RATE) * (440.0f * 2.0f * 3.14159265f / (float)AUDIO_RATE);
        int16_t l = (int16_t)lrintf(sinf(phase) * 12000.0f);
        int16_t r = (int16_t)-l;
        audio_ring[k & (AUDIO_RING - 1)] = (uint16_t)l | (uint32_t)(uint16_t)r << 16;
        DESC->audio_head = k + 1;
    }
}

static void worker_main(worker *w) {
    volatile uint32_t canary[8];
    for (uint32_t i = 0; i < 8; i++) canary[i] = w->id * 0x01010101u + i;
    touch(canary);
    for (;;) {
        for (uint32_t slice = 0; slice < SLICES; slice++) {
            if (expected_turn != w->id) fail("mock: workers ran out of order");
            if (np_host_fiber_self() != w->handle) fail("mock: fiber_self mismatch");
            uint32_t y0 = H * slice / SLICES, y1 = H * (slice + 1) / SLICES;
            switch (w->id) {
            case 0: draw_rows(top, top_pixel, y0, y1); break;
            case 1: draw_rows(BOTTOM, bottom_pixel, y0, y1); break;
            default:
                if (crash_on_worker) __builtin_trap();
                make_audio(slice);
                break;
            }
            expected_turn = (w->id + 1) % NWORKERS;
            yield_to_scheduler();
            for (uint32_t i = 0; i < 8; i++)
                if (canary[i] != w->id * 0x01010101u + i) fail("mock: shadow stack corrupted across a switch");
        }
    }
}

NP_EXPORT(np_fiber_entry) void np_fiber_entry(uint32_t arg) {
    if (arg == 0xFFFFFFFFu) {
        /* The short-lived fiber of frame 2: ping-pong with the scheduler. */
        for (;;) {
            switches_this_frame++;
            np_host_fiber_switch(sched_handle);
        }
    }
    worker_main(&workers[arg]);
}

static void check_rom(void) {
    uint32_t size = np_host_rom_size();
    uint8_t buf[256];
    if (size < 0x2000) return;
    if (np_host_rom_read(0x1000, buf, sizeof buf) != 0) return;
    for (uint32_t i = 0; i < sizeof buf; i++)
        if (buf[i] != rom_byte(0x1000 + i)) return;
    if (np_host_rom_read(size - 16, buf, 16) != 0) return;
    for (uint32_t i = 0; i < 16; i++)
        if (buf[i] != rom_byte(size - 16 + i)) return;
    if (np_host_rom_read(size - 4, buf, 16) == 0) return; /* must refuse a read past the end */
    status |= ST_ROM_OK;
}

static uint32_t chip_checksum(void) {
    uint32_t sum = save_counter;
    for (uint32_t i = 16; i < CHIP_SIZE; i++) sum += chip[i];
    return sum;
}

static void chip_header(void) {
    memcpy(chip, "NPSV", 4);
    memcpy(chip + 4, &save_counter, 4);
    memcpy(chip + 8, &seed, 4);
    uint32_t sum = chip_checksum();
    memcpy(chip + 12, &sum, 4);
}

static void load_save(void) {
    int32_t r = np_host_save_load(chip, CHIP_SIZE);
    if (r == 1 && memcmp(chip, "NPSV", 4) == 0) {
        uint32_t sum;
        memcpy(&save_counter, chip + 4, 4);
        memcpy(&sum, chip + 12, 4);
        if (sum == chip_checksum()) {
            status |= ST_SAVE_LOADED;
            return;
        }
    }
    save_counter = 0;
    memset(chip, 0xFF, CHIP_SIZE);
}

static void run_workers(void) {
    switches_this_frame = 0;
    expected_turn = 0;
    for (uint32_t slice = 0; slice < SLICES; slice++)
        for (uint32_t i = 0; i < NWORKERS; i++) {
            np_host_fiber_switch(workers[i].handle);
            if (np_host_fiber_self() != sched_handle) fail("mock: scheduler resumed on the wrong fiber");
        }
    if (switches_this_frame != SLICES * NWORKERS || expected_turn != 0) fail("mock: scheduler lost a switch");
}

static void short_lived_fiber(void) {
    uint8_t *stack = malloc(SHADOW_STACK);
    uint32_t h = np_host_fiber_create((uint32_t)(uintptr_t)(stack + SHADOW_STACK), 0xFFFFFFFFu);
    uint32_t before = switches_this_frame;
    for (int i = 0; i < 3; i++) np_host_fiber_switch(h);
    if (switches_this_frame != before + 3) fail("mock: short-lived fiber did not ping-pong");
    np_host_fiber_destroy(h);
    free(stack);
}

/* Loopback check: every frame, one datagram to everyone; with a transport
 * whatever comes back must be one of ours, intact. Without one (net_self
 * 0) sending fails and nothing ever arrives. */
static void exchange_datagrams(void) {
    const uint32_t self = np_host_net_self();
    char msg[48], buf[64];
    int n = snprintf(msg, sizeof msg, "ping %u from %u", (unsigned)frame, (unsigned)self);
    int32_t sent = np_host_net_send(NP_NET_BROADCAST, msg, (uint32_t)n);
    uint32_t peer = 0;
    int32_t r;
    if (!self) {
        if (sent != -1) fail("mock: net_send without a transport succeeded");
        if (np_host_net_recv(&peer, buf, sizeof buf) != 0) fail("mock: net_recv without a transport");
        return;
    }
    if (sent != 0) fail("mock: net_send failed");
    while ((r = np_host_net_recv(&peer, buf, sizeof buf)) > 0) {
        if (r != n || memcmp(buf, msg, (size_t)n) != 0 || peer != self) fail("mock: datagram corrupted");
        net_ok++;
    }
    if (r < 0) fail("mock: net_recv failed");
}

/* What the host asked for, reported back: the real slots get recognisable
 * values and slots 8..15 echo opt[0..7] verbatim. Slots 6 and 7 carry the
 * loopback count and this station's id. */
static void publish_status(void) {
    const uint32_t *opt = DESC->opt;
    DESC->status[NP_STAT_LINK_ACTIVE] = np_host_net_self() != 0;
    DESC->status[NP_STAT_FIELD_READY] = 1;
    DESC->status[NP_STAT_QUICKSAVE_SEQ] = opt[NP_OPT_QUICKSAVE_SEQ];
    DESC->status[NP_STAT_QUICKSAVE_RESULT] = opt[NP_OPT_QUICKSAVE_SEQ] ? NP_QS_SAVED : NP_QS_NONE;
    DESC->status[NP_STAT_MAP_ID] = 400 + frame;
    DESC->status[NP_STAT_IN_BATTLE] = 0;
    DESC->status[6] = net_ok;
    DESC->status[7] = np_host_net_self();
    for (uint32_t i = 0; i < 8; i++) DESC->status[8 + i] = opt[i];
}

/* ---- runtime content (NP_MOCK_CONTENT) ------------------------------ */

/* The tree test_core.c builds under the content root:
 *   hello.txt            "hello, content\n"
 *   sub/data.bin         1000 bytes of content_byte()
 *   sub/many/fNNN        CONTENT_MANY empty files (listings past one buffer)
 * and with NP_MOCK_CONTENT_LINKS also
 *   sub/inner.txt        -> ../hello.txt (stays inside)
 *   escape_file          -> ../outside/secret.txt
 *   escape_dir           -> ../outside
 * next to ../outside/secret.txt. */
#define CONTENT_MANY 300

static uint8_t content_byte(uint32_t i) {
    return (uint8_t)(i * 13u + 5u);
}

static void content_fail(const char *what) {
    char msg[160];
    snprintf(msg, sizeof msg, "mock: content: %s (errno %d)", what, errno);
    fail(msg);
}

/* `call` must fail with `want`. */
#define CONTENT_REFUSED(call, want, what)                \
    do {                                                 \
        errno = 0;                                       \
        if ((call) || errno != (want)) content_fail(what); \
    } while (0)

static void check_content(void) {
    const int links = getenv("NP_MOCK_CONTENT_LINKS") != NULL;
    char buf[64];
    struct stat st;

    FILE *f = fopen(NP_CONTENT_DIR "/hello.txt", "r");
    if (!f) content_fail("fopen hello.txt");
    if (fread(buf, 1, sizeof buf, f) != 15 || memcmp(buf, "hello, content\n", 15) != 0)
        content_fail("hello.txt contents");
    if (fseek(f, 7, SEEK_SET) != 0 || ftell(f) != 7 || fgetc(f) != 'c') content_fail("fseek/ftell");
    fclose(f);

    int fd = open(NP_CONTENT_DIR "/sub/data.bin", O_RDONLY);
    if (fd < 0) content_fail("open data.bin");
    uint8_t b[64];
    if (pread(fd, b, sizeof b, 500) != (ssize_t)sizeof b) content_fail("pread");
    for (uint32_t i = 0; i < sizeof b; i++)
        if (b[i] != content_byte(500 + i)) content_fail("pread contents");
    if (lseek(fd, 0, SEEK_CUR) != 0) content_fail("pread moved the offset");
    if (lseek(fd, 0, SEEK_END) != 1000 || lseek(fd, -10, SEEK_CUR) != 990) content_fail("lseek");
    if (read(fd, b, sizeof b) != 10 || b[9] != content_byte(999) || read(fd, b, sizeof b) != 0)
        content_fail("read to the end");
    if (lseek(fd, -1, SEEK_SET) != -1) content_fail("lseek before the start");
    if (fstat(fd, &st) != 0 || st.st_size != 1000 || !S_ISREG(st.st_mode)) content_fail("fstat");
    if (write(fd, b, 1) != -1) content_fail("write to a content file");
    close(fd);

    if (stat(NP_CONTENT_DIR "/sub", &st) != 0 || !S_ISDIR(st.st_mode)) content_fail("stat sub");
    if (stat(NP_CONTENT_DIR "/./sub//data.bin", &st) != 0 || st.st_size != 1000) content_fail("stat . and //");
    CONTENT_REFUSED(stat(NP_CONTENT_DIR "/missing", &st) == 0, ENOENT, "stat missing");
    CONTENT_REFUSED(opendir(NP_CONTENT_DIR "/hello.txt") != NULL, ENOTDIR, "opendir on a file");

    /* The root lists hello.txt and sub, never a link that escapes. */
    DIR *d = opendir(NP_CONTENT_DIR);
    if (!d) content_fail("opendir root");
    int hello = 0, sub = 0, other = 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        if (strcmp(e->d_name, "hello.txt") == 0) hello += e->d_type == DT_REG;
        else if (strcmp(e->d_name, "sub") == 0) sub += e->d_type == DT_DIR;
        else other++;
    }
    closedir(d);
    if (hello != 1 || sub != 1 || other != 0) content_fail("root listing");

    /* Past one fd_readdir buffer: every name once. */
    static uint8_t seen[CONTENT_MANY];
    int count = 0;
    d = opendir(NP_CONTENT_DIR "/sub/many");
    if (!d) content_fail("opendir many");
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        int n = atoi(e->d_name + 1);
        if (e->d_name[0] != 'f' || n < 0 || n >= CONTENT_MANY || seen[n]++) content_fail("many listing");
        count++;
    }
    closedir(d);
    if (count != CONTENT_MANY) content_fail("many count");

    /* Escapes. libc hands ".." through to path_open, which refuses it. */
    CONTENT_REFUSED(open(NP_CONTENT_DIR "/../outside/secret.txt", O_RDONLY) >= 0, ENOTCAPABLE, "open ../");
    CONTENT_REFUSED(open(NP_CONTENT_DIR "/sub/../../outside/secret.txt", O_RDONLY) >= 0, ENOTCAPABLE, "open sub/../../");
    CONTENT_REFUSED(stat(NP_CONTENT_DIR "/sub/..", &st) == 0, ENOTCAPABLE, "stat sub/..");
    {
        __wasi_fd_t nfd;
        if (__wasi_path_open(3, 0, "/etc/hosts", 0, __WASI_RIGHTS_FD_READ, 0, 0, &nfd) != __WASI_ERRNO_NOTCAPABLE)
            content_fail("absolute path_open");
        if (__wasi_path_open(3, 0, "sub\\data.bin", 0, __WASI_RIGHTS_FD_READ, 0, 0, &nfd) != __WASI_ERRNO_NOTCAPABLE)
            content_fail("backslash path_open");
        if (__wasi_path_open(3, 0, "hello.txt:x", 0, __WASI_RIGHTS_FD_READ, 0, 0, &nfd) != __WASI_ERRNO_NOTCAPABLE)
            content_fail("colon path_open");
    }
    if (links) {
        CONTENT_REFUSED(open(NP_CONTENT_DIR "/escape_file", O_RDONLY) >= 0, ENOTCAPABLE, "open escape_file");
        CONTENT_REFUSED(stat(NP_CONTENT_DIR "/escape_dir/secret.txt", &st) == 0, ENOTCAPABLE, "stat escape_dir/");
        CONTENT_REFUSED(opendir(NP_CONTENT_DIR "/escape_dir") != NULL, ENOTCAPABLE, "opendir escape_dir");
        f = fopen(NP_CONTENT_DIR "/sub/inner.txt", "r");
        if (!f || fread(buf, 1, sizeof buf, f) != 15 || memcmp(buf, "hello, content\n", 15) != 0)
            content_fail("a link inside the root");
        fclose(f);
    }

    /* Read-only. */
    CONTENT_REFUSED(fopen(NP_CONTENT_DIR "/new.txt", "w") != NULL, EROFS, "fopen w");
    CONTENT_REFUSED(open(NP_CONTENT_DIR "/hello.txt", O_WRONLY) >= 0, EROFS, "open O_WRONLY");
    CONTENT_REFUSED(open(NP_CONTENT_DIR "/hello.txt", O_RDWR) >= 0, EROFS, "open O_RDWR");
    CONTENT_REFUSED(mkdir(NP_CONTENT_DIR "/newdir", 0777) == 0, EROFS, "mkdir");

    status |= ST_CONTENT_OK;
    printf("mock: content ok\n");
}

/* ---- GBA slot ------------------------------------------------------- */

static uint8_t gba_byte(uint32_t i) {
    return (uint8_t)(i * 5u + 1u + (i >> 9));
}

static void check_gba(void) {
    const uint32_t size = np_host_gba_rom_size();
    uint8_t buf[256];
    if (size == 0) return;
    if (np_host_gba_rom_read(0, buf, sizeof buf) != 0) return;
    for (uint32_t i = 0; i < sizeof buf; i++)
        if (buf[i] != gba_byte(i)) return;
    if (np_host_gba_rom_read(size - 16, buf, 16) != 0 || buf[15] != gba_byte(size - 1)) return;
    if (np_host_gba_rom_read(size - 4, buf, 16) == 0) return; /* past the end */
    status |= ST_GBA_ROM_OK;
    const int32_t r = np_host_gba_save_load(gba_save, GBA_SAVE_SIZE);
    if (r == 1) status |= ST_GBA_SAVE_LOADED;
    else memset(gba_save, 0xFF, GBA_SAVE_SIZE);
}

int main(int argc, char **argv) {
    volatile uint32_t local = 0;
    touch(&local);

    const char *s = getenv("NP_MOCK_SEED");
    if (s) {
        seed = (uint32_t)atoi(s);
        status |= ST_ENV_OK;
    }
    printf("mock: boot argv0=%s argc=%d seed=%u\n", argc > 0 ? argv[0] : "?", argc, (unsigned)seed);
    static const char direct[] = "mock: direct log\nmock: second line";
    np_host_log(direct, (uint32_t)(sizeof direct - 1));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if ((t1.tv_sec > t0.tv_sec || (t1.tv_sec == t0.tv_sec && t1.tv_nsec >= t0.tv_nsec)) && time(NULL) > 1600000000)
        status |= ST_CLOCK_OK;
    if (getentropy(&random_word, sizeof random_word) != 0) fail("mock: getentropy failed");

    /* No filesystem outside NP_CONTENT_DIR: the directory calls the port's
     * debug paths make must fail cleanly, and the streams accept no flags.
     * This also links in the same WASI imports the real games declare. */
    struct stat st;
    DIR *dir = opendir(".");
    if (dir && readdir(dir)) fail("mock: readdir found a filesystem");
    if (mkdir("np_mock", 0777) == 0 || stat("np_mock", &st) == 0 || dir)
        fail("mock: filesystem calls unexpectedly succeeded");
    if (fcntl(1, F_SETFL, O_NONBLOCK) != -1 || fcntl(1, F_SETFL, 0) != 0) fail("mock: fd_fdstat_set_flags");
    if (getenv("NP_MOCK_CONTENT")) check_content();
    check_gba();

    audio_ring = calloc(AUDIO_RING, 4);
    if ((uintptr_t)&local >= NP_GUEST_C_BASE && (uintptr_t)top >= NP_GUEST_C_BASE &&
        (uintptr_t)audio_ring >= NP_GUEST_C_BASE)
        status |= ST_LAYOUT_OK;

    check_rom();
    load_save();

    sched_handle = np_host_fiber_self();
    if (sched_handle != 1) fail("mock: boot fiber is not handle 1");
    for (uint32_t i = 0; i < NWORKERS; i++) {
        workers[i].id = i;
        workers[i].shadow = malloc(SHADOW_STACK);
        workers[i].handle = np_host_fiber_create((uint32_t)(uintptr_t)(workers[i].shadow + SHADOW_STACK), i);
    }
    big[0] = malloc(4u * W * H * 4);
    big[1] = malloc(4u * W * H * 4);
    if (!big[0] || !big[1]) fail("mock: out of memory");

    memset(DESC, 0, sizeof *DESC);
    DESC->magic = NP_FRAME_MAGIC;
    DESC->version = NP_GUEST_ABI_VERSION;
    DESC->screen[0] = (uint32_t)(uintptr_t)top;
    DESC->screen[1] = (uint32_t)(uintptr_t)BOTTOM;
    DESC->width = W;
    DESC->height = H;
    DESC->stride = W;
    DESC->audio_ring = (uint32_t)(uintptr_t)audio_ring;
    DESC->audio_ring_frames = AUDIO_RING;
    DESC->audio_rate = AUDIO_RATE;

    for (frame = 0;; frame++) {
        const uint32_t keys = DESC->in_keys;
        if (keys == (KEY_START | KEY_SELECT)) {
            for (uint32_t i = 0; i < NWORKERS; i++) np_host_fiber_destroy(workers[i].handle);
            printf("mock: exiting at frame %u\n", (unsigned)frame);
            exit(3);
        }
        if (keys == (KEY_L | KEY_R)) {
            char msg[64];
            snprintf(msg, sizeof msg, "mock: trap requested at frame %u", (unsigned)frame);
            np_host_trap(msg, (uint32_t)strlen(msg));
        }
        crash_on_worker = keys == (KEY_X | KEY_Y);
        if (keys == (KEY_A | KEY_B)) {
            /* Out of bounds of linear memory: only exercised by the test in
             * NP_BOUNDS_CHECK builds, where it must become a wasm trap. */
            status |= *(volatile uint32_t *)(uintptr_t)0xFFFFFFF0u;
        }

        run_workers();
        status |= ST_FIBERS_OK;
        exchange_datagrams();
        if (frame == 2) short_lived_fiber();

        if (frame == 3) {
            save_counter++;
            chip_header();
            if (np_host_save_store(chip, CHIP_SIZE) != 0) status |= ST_STORE_FAILED;
        }
        if (frame == 4 && (status & ST_GBA_ROM_OK)) {
            gba_save[0]++;
            memcpy(gba_save + 1, "GBA", 3);
            if (np_host_gba_save_store(gba_save, GBA_SAVE_SIZE) != 0) fail("mock: gba_save_store failed");
        }
        if (frame == 5) {
            for (uint32_t i = 16; i < CHIP_SIZE; i++) chip[i] = (uint8_t)(i + frame);
            chip_header();
            DESC->save_image = (uint32_t)(uintptr_t)chip;
            DESC->save_size = CHIP_SIZE;
            DESC->save_dirty = 1;
        }
        if (frame > 5 && DESC->save_dirty == 0) status |= ST_HOST_FLUSHED;

        /* Input echo and status, over the pattern. */
        top[0] = keys;
        top[1] = DESC->in_touch << 16 | (DESC->in_touch_x & 0xFFu) << 8 | (DESC->in_touch_y & 0xFFu);
        top[2] = DESC->in_lid;
        BOTTOM[0] = status;
        BOTTOM[1] = save_counter;
        BOTTOM[2] = seed;
        BOTTOM[3] = random_word & 0xFFFFFFu;
        BOTTOM[4] = switches_this_frame;

        publish_status();
        if (DESC->opt[NP_OPT_RENDER_SCALE] == 2) {
            /* A frame at twice the size: each screen a constant derived
             * from the frame number, which test_core.c checks. */
            for (uint32_t i = 0; i < 4u * W * H; i++) {
                big[0][i] = 0x00100000u | frame;
                big[1][i] = 0x00200000u | frame;
            }
            DESC->screen[0] = (uint32_t)(uintptr_t)big[0];
            DESC->screen[1] = (uint32_t)(uintptr_t)big[1];
            DESC->width = DESC->stride = 2 * W;
            DESC->height = 2 * H;
        } else {
            DESC->screen[0] = (uint32_t)(uintptr_t)top;
            DESC->screen[1] = (uint32_t)(uintptr_t)BOTTOM;
            DESC->width = DESC->stride = W;
            DESC->height = H;
        }

        DESC->frame_lo = frame;
        DESC->frame_hi = 0;
        np_host_vblank(DESC);
    }
}
