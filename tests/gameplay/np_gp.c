/*
 * np_gp: the gameplay-test driver for tests/gameplay.
 *
 * np_headless with what long scripted and randomized runs need on top:
 * schedules of any length, frame lists to dump, seeded random input for
 * soaks, and a watchdog that turns traps, hangs and audio stalls into one
 * line each that names the frame (and seed) that reproduces them. (Core
 * snapshots cannot cross processes, they hold this process's fiber stack
 * addresses, so a scenario starts from a save, never from a snapshot.)
 *
 *   np_gp <rom> [options]
 *     --game NAME         diamond, pearl, platinum, black, white, heartgold,
 *                         soulsilver, ruby, sapphire or emerald (default platinum)
 *     --frames N          run until frame N (absolute; counts from 0 at boot)
 *     --save FILE         backup chip file (loaded if present, written on store)
 *     --schedule FILE     shell press schedule (shell/README.md), repeatable
 *     --dump DIR          PPM dumps: DIR/frame_NNNNNN.ppm (NNNNNN = run frame)
 *     --dump-at F[,F..]   dump after these frames, repeatable
 *     --dump-every N      dump every N frames from --dump-from F
 *     -o [F:]NAME=VALUE   np_core option (as np_headless)
 *     -e KEY=VALUE        guest environment; PC_* host variables pass through
 *     --random SEED       seeded random input from --random-from F (default:
 *                         the first frame run) instead of the schedule
 *     --hang-sec S        a frame that takes longer than S seconds is a hang
 *                         (default 30)
 *     --time-from F       frames/second measured from frame F (default first)
 *     --peek F:ADDR:LEN   after frame F, print LEN bytes of guest memory at
 *                         ADDR as "peek F ADDR: <u32 words>", repeatable
 *                         (addresses: games/<game>/build/pc-wasm/<game>.map)
 *     --serve 1           no frame count or schedule loop: commands on stdin
 *                         drive the run (tests/e2e's bots; see serve() below)
 *
 * Prints status changes ("[status] frame K: map_id A -> B"), then a summary:
 *   frames N  hash H  ms/frame M  fps X  audio A  stalls S  audio-stalls T
 *   static-max L@F
 * Defects print one line each, prefixed DEFECT, and the exit status is 1:
 *   DEFECT trap frame K seed S: <the core's error>
 *   DEFECT hang frame K seed S: run_frame did not return in Ts
 *   DEFECT stall frame K seed S: the VBlank counter did not advance (N)
 *   DEFECT audio-stall frame K seed S: no audio for N frames
 * static-max is the longest run of frames with identical screens (a soft
 * lock shows up there; it is reported, not judged).
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "np_core.h"
#include "np_e2e.h"
#include "np_guest_abi.h"

extern char **environ;

#define MAX_SCHED 16384
#define MAX_LIST 4096
#define MAX_SETS 64
#define MAX_OPTIONS 128
#define AUDIO_STALL_FRAMES 30

typedef struct sched_step {
    int64_t frame;
    int n, every, count, tap;
    uint16_t keys, x, y;
} sched_step;

static sched_step g_sched[MAX_SCHED];
static int g_nsched;

static uint16_t parse_key_names(const char *v) {
    static const char *const names[12] = {"a", "b", "select", "start", "right", "left",
                                          "up", "down", "r", "l", "x", "y"};
    uint16_t keys = 0;
    while (*v) {
        size_t n = strcspn(v, "+");
        for (int i = 0; i < 12; i++)
            if (strlen(names[i]) == n && strncmp(v, names[i], n) == 0) keys |= (uint16_t)(1u << i);
        v += n;
        if (*v == '+') v++;
    }
    return keys;
}

/* The same grammar as np_headless's load_schedule; "+D" is relative to the
 * previous step of the same file. Frames count from BASE (0 for
 * --schedule; the serve loop's `sched` loads a file at the current frame). */
static int load_schedule(const char *path, int64_t base) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char line[512];
    int64_t prev = base;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *save = NULL;
        for (char *step = strtok_r(line, ";\n", &save); step; step = strtok_r(NULL, ";\n", &save)) {
            while (*step == ' ' || *step == '\t' || *step == '\r') step++;
            if (!*step) continue;
            if (g_nsched == MAX_SCHED) {
                fclose(f);
                fprintf(stderr, "np_gp: more than %d schedule steps\n", MAX_SCHED);
                return -1;
            }
            int rel = *step == '+';
            char k[64] = {0};
            int frame = 0, v[5] = {0};
            int got = sscanf(step + rel, "%d:%63[a-zA-Z+]:%d:%d:%d:%d:%d", &frame, k, &v[0], &v[1], &v[2], &v[3],
                             &v[4]) - 2;
            if (got < 0) {
                fclose(f);
                fprintf(stderr, "np_gp: bad schedule step \"%s\" in %s\n", step, path);
                return -1;
            }
            sched_step *p = &g_sched[g_nsched];
            memset(p, 0, sizeof *p);
            p->n = 6;
            p->count = 1;
            p->frame = rel ? prev + frame : base + frame;
            prev = p->frame;
            int *rest = v;
            if (strcmp(k, "tap") == 0) {
                if (got < 2) {
                    fclose(f);
                    return -1;
                }
                p->tap = 1;
                p->x = (uint16_t)v[0];
                p->y = (uint16_t)v[1];
                rest += 2;
                got -= 2;
            } else {
                p->keys = parse_key_names(k);
            }
            if (got >= 1 && rest[0] >= 1) p->n = rest[0];
            if (got >= 3) {
                p->every = rest[1];
                p->count = rest[2];
            }
            g_nsched++;
        }
    }
    fclose(f);
    return 0;
}

static void schedule_input(int64_t k, np_input *in) {
    for (int i = 0; i < g_nsched; i++) {
        const sched_step *p = &g_sched[i];
        int64_t off = k - p->frame;
        if (off < 0) continue;
        if (p->every > 0) {
            if (off / p->every >= p->count) continue;
            off %= p->every;
        }
        if (off >= p->n) continue;
        if (p->tap) {
            in->touch = 1;
            in->touch_x = p->x;
            in->touch_y = p->y;
        } else {
            in->keys |= p->keys;
        }
    }
}

/* ---- seeded random input: holds of 1..32 frames, then a 0..8 frame gap. */
static uint64_t g_rng;
static int g_hold_left;
static np_input g_hold;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 32);
}

static void random_input(np_input *in) {
    if (g_hold_left <= 0) {
        memset(&g_hold, 0, sizeof g_hold);
        uint32_t r = rnd() % 100;
        static const uint16_t dpad[4] = {NP_KEY_UP, NP_KEY_DOWN, NP_KEY_LEFT, NP_KEY_RIGHT};
        if (r < 45) {
            g_hold.keys = dpad[rnd() % 4];
            if (rnd() % 8 == 0) g_hold.keys |= NP_KEY_B; /* run */
            g_hold_left = 1 + (int)(rnd() % 32);
        } else if (r < 65) {
            g_hold.keys = NP_KEY_A;
            g_hold_left = 1 + (int)(rnd() % 6);
        } else if (r < 77) {
            g_hold.keys = NP_KEY_B;
            g_hold_left = 1 + (int)(rnd() % 6);
        } else if (r < 87) {
            g_hold.touch = 1;
            g_hold.touch_x = (uint16_t)(rnd() % 256);
            g_hold.touch_y = (uint16_t)(rnd() % 192);
            g_hold_left = 1 + (int)(rnd() % 12);
        } else if (r < 91) {
            g_hold.keys = NP_KEY_X;
            g_hold_left = 1 + (int)(rnd() % 4);
        } else if (r < 94) {
            g_hold.keys = NP_KEY_Y;
            g_hold_left = 1 + (int)(rnd() % 4);
        } else if (r < 96) {
            g_hold.keys = NP_KEY_START;
            g_hold_left = 1 + (int)(rnd() % 4);
        } else if (r < 97) {
            g_hold.keys = NP_KEY_SELECT;
            g_hold_left = 1 + (int)(rnd() % 4);
        } else {
            g_hold.keys = (rnd() & 1) ? NP_KEY_L : NP_KEY_R;
            g_hold_left = 1 + (int)(rnd() % 4);
        }
        g_hold_left += (int)(rnd() % 9); /* the release gap is part of the hold */
        g_hold.keys &= 0xfff;
    }
    int releasing = g_hold_left <= 2;
    if (!releasing) *in = g_hold;
    g_hold_left--;
}

/* ---- watchdog: a frame that never returns is a hang. */
static volatile int64_t g_wd_frame = -1;
static volatile double g_wd_start;
static double g_hang_sec = 30;
static uint64_t g_seed;
static int g_random;

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void *watchdog(void *arg) {
    (void)arg;
    for (;;) {
        sleep(1);
        int64_t k = g_wd_frame;
        if (k >= 0 && now_s() - g_wd_start > g_hang_sec) {
            printf("DEFECT hang frame %lld seed %llu: run_frame did not return in %.0fs\n", (long long)k,
                   (unsigned long long)(g_random ? g_seed : 0), g_hang_sec);
            fflush(stdout);
            _exit(3);
        }
    }
    return NULL;
}

/* ---- host callbacks */
typedef struct runner {
    FILE *rom;
    const char *save_path;
} runner;

static int rom_read(void *user, uint32_t offset, void *dst, uint32_t len) {
    runner *r = user;
    if (fseek(r->rom, (long)offset, SEEK_SET) != 0) return -1;
    return fread(dst, 1, len, r->rom) == len ? 0 : -1;
}

static int save_load(void *user, void *dst, uint32_t len) {
    runner *r = user;
    if (!r->save_path) return 0;
    FILE *f = fopen(r->save_path, "rb");
    if (!f) return 0;
    size_t n = fread(dst, 1, len, f);
    fclose(f);
    return n == len ? 1 : -1;
}

static int64_t g_frame;

static int save_store(void *user, const void *src, uint32_t len) {
    runner *r = user;
    if (!r->save_path) return 0;
    FILE *f = fopen(r->save_path, "wb");
    if (!f) return -1;
    int ok = fwrite(src, 1, len, f) == len;
    ok &= fclose(f) == 0;
    fprintf(stderr, "[np_gp] frame %lld: stored %u-byte save to %s\n", (long long)g_frame, len, r->save_path);
    return ok ? 0 : -1;
}

static void log_line(void *user, const char *line) {
    (void)user;
    fprintf(stderr, "%s\n", line);
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

/* Both screens stacked (DS); the GBA has one (screen[1] NULL). */
static int frame_screens(const np_frame *f) { return f->screen[1] ? 2 : 1; }

static int dump_ppm(const char *dir, int64_t number, const np_frame *f) {
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%06lld.ppm", dir, (long long)number);
    FILE *out = fopen(path, "wb");
    if (!out) return -1;
    fprintf(out, "P6\n%u %u\n255\n", f->width, f->height * frame_screens(f));
    uint8_t *row = malloc((size_t)f->width * 3);
    for (int s = 0; s < frame_screens(f); s++)
        for (uint32_t y = 0; y < f->height; y++) {
            for (uint32_t x = 0; x < f->width; x++) {
                uint32_t p = f->screen[s][y * f->stride + x];
                row[3 * x] = (uint8_t)(p >> 16);
                row[3 * x + 1] = (uint8_t)(p >> 8);
                row[3 * x + 2] = (uint8_t)p;
            }
            fwrite(row, 1, (size_t)f->width * 3, out);
        }
    free(row);
    return fclose(out) == 0 ? 0 : -1;
}

static const char *const k_opt_names[] = {"bgm_volume", "se_volume",     "render_scale", "widescreen",  "camera_zoom",
                                          "camera_tilt", "quicksave_seq", "rules",        "text_instant"};
static const char *const k_stat_names[] = {"link_active", "field_ready", "quicksave_seq", "quicksave_result",
                                           "map_id",      "in_battle",   "e2e",           "resets"};

typedef struct opt_set {
    int64_t frame;
    uint32_t opt, value;
} opt_set;

static int parse_opt(const char *v, opt_set *s) {
    char *end;
    s->frame = 0;
    const char *colon = strchr(v, ':'), *eq = strchr(v, '=');
    if (!eq) return -1;
    if (colon && colon < eq) {
        s->frame = strtoll(v, &end, 0);
        if (end != colon) return -1;
        v = colon + 1;
    }
    size_t n = (size_t)(eq - v);
    s->opt = NP_OPT_COUNT;
    for (uint32_t i = 0; i < sizeof k_opt_names / sizeof *k_opt_names; i++)
        if (strlen(k_opt_names[i]) == n && strncmp(v, k_opt_names[i], n) == 0) s->opt = i;
    if (s->opt == NP_OPT_COUNT) {
        unsigned long i = strtoul(v, &end, 0);
        if (end != eq || i >= NP_OPT_COUNT) return -1;
        s->opt = (uint32_t)i;
    }
    s->value = (uint32_t)strtol(eq + 1, &end, 0);
    return *end ? -1 : 0;
}

static int usage(void) {
    fprintf(stderr, "usage: np_gp <rom> [--game diamond|pearl|platinum|black|white|heartgold|soulsilver|ruby|sapphire|emerald] [--frames N] [--save FILE]\n"
                    "             [--schedule FILE]...\n"
                    "             [--dump DIR [--dump-at F,..]... [--dump-every N\n"
                    "             [--dump-from F]]] [-o [F:]NAME=V]... [-e K=V]... [--random SEED\n"
                    "             [--random-from F]] [--hang-sec S] [--time-from F] [--peek F:ADDR:LEN]...\n"
                    "             [--serve 1]\n");
    return 2;
}

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* ---- the per-frame work of both modes: run, hash, judge, report. */
typedef struct frame_run {
    np_core *core;
    np_frame f;
    uint32_t status[NP_STAT_COUNT];
    uint64_t hash, audio_total, prev_number, prev_screen;
    int have_prev, defects, rc;
    int64_t stall_run, audio_run, static_run, static_max, static_max_at, stalls, audio_stalls;
    int64_t time_from, timed_frames;
    double t_timed;
    const opt_set *sets;
    int nsets;
} frame_run;

static int16_t g_audio[2 * 8192];

/* Frame K with input IN. Returns the core's rc: 0, or the run is over. */
static int run_frame(frame_run *fr, int64_t k, const np_input *in) {
    g_frame = k;
    for (int o = 0; o < fr->nsets; o++)
        if (fr->sets[o].frame == k) np_core_set_option(fr->core, fr->sets[o].opt, fr->sets[o].value);

    double t0 = now_s();
    g_wd_start = t0;
    g_wd_frame = k;
    fr->rc = np_core_run_frame(fr->core, in, &fr->f);
    g_wd_frame = -1;
    if (k >= fr->time_from) {
        fr->t_timed += now_s() - t0;
        fr->timed_frames++;
    }
    if (fr->rc != 0) return fr->rc;

    const np_frame *f = &fr->f;
    uint64_t sh = 0xCBF29CE484222325ull;
    for (int s = 0; s < frame_screens(f); s++) {
        sh = fnv(sh, f->screen[s], (size_t)f->stride * f->height * 4);
        fr->hash = fnv(fr->hash, f->screen[s], (size_t)f->stride * f->height * 4);
    }
    size_t n, got = 0;
    while ((n = np_core_audio_read(fr->core, g_audio, 8192)) > 0) {
        fr->hash = fnv(fr->hash, g_audio, n * 4);
        got += n;
    }
    fr->audio_total += got;

    if (fr->have_prev) {
        if (f->number == fr->prev_number) {
            if (++fr->stall_run == 60) {
                printf("DEFECT stall frame %lld seed %llu: the VBlank counter did not advance (%llu)\n",
                       (long long)k, (unsigned long long)(g_random ? g_seed : 0), (unsigned long long)f->number);
                fr->stalls++;
                fr->defects++;
            }
        } else {
            fr->stall_run = 0;
        }
        if (got == 0) {
            if (++fr->audio_run == AUDIO_STALL_FRAMES) {
                printf("DEFECT audio-stall frame %lld seed %llu: no audio for %d frames\n", (long long)k,
                       (unsigned long long)(g_random ? g_seed : 0), AUDIO_STALL_FRAMES);
                fr->audio_stalls++;
                fr->defects++;
            }
        } else {
            fr->audio_run = 0;
        }
        if (sh == fr->prev_screen) {
            if (++fr->static_run > fr->static_max) {
                fr->static_max = fr->static_run;
                fr->static_max_at = k;
            }
        } else {
            fr->static_run = 0;
        }
    }
    fr->have_prev = 1;
    fr->prev_number = f->number;
    fr->prev_screen = sh;

    for (uint32_t i = 0; i < NP_STAT_COUNT; i++) {
        uint32_t v = np_core_status(fr->core, i);
        if (v != fr->status[i] && i < sizeof k_stat_names / sizeof *k_stat_names && i != NP_STAT_QUICKSAVE_SEQ &&
            i != NP_STAT_E2E)
            fprintf(stderr, "[status] frame %lld: %s %u -> %u\n", (long long)k, k_stat_names[i], fr->status[i], v);
        fr->status[i] = v;
    }
    return 0;
}

/* The end of a run in either mode: the save flushed, the way the core
 * stopped, the summary line. Returns the exit status. */
static int finish(frame_run *fr, int64_t k) {
    np_core_save_flush(fr->core);
    if (fr->rc < 0) {
        printf("DEFECT trap frame %lld seed %llu: %s\n", (long long)k, (unsigned long long)(g_random ? g_seed : 0),
               np_core_last_error(fr->core));
        fr->defects++;
    } else if (fr->rc > 0) {
        /* The save lab ends its run with exit(0) once the save is written. */
        if (strstr(np_core_last_error(fr->core), "status 0")) {
            printf("exited frame %lld: %s\n", (long long)k, np_core_last_error(fr->core));
        } else {
            printf("DEFECT exit frame %lld seed %llu: %s\n", (long long)k,
                   (unsigned long long)(g_random ? g_seed : 0), np_core_last_error(fr->core));
            fr->defects++;
        }
    }
    double ms = fr->timed_frames ? fr->t_timed * 1e3 / (double)fr->timed_frames : 0;
    printf("frames %lld  hash %016llx  ms/frame %.3f  fps %.1f  size %ux%u  audio %llu  stalls %lld  "
           "audio-stalls %lld  static-max %lld@%lld  map %u\n",
           (long long)k, (unsigned long long)fr->hash, ms, ms > 0 ? 1e3 / ms : 0, fr->f.width, fr->f.height,
           (unsigned long long)fr->audio_total, (long long)fr->stalls, (long long)fr->audio_stalls,
           (long long)fr->static_max, (long long)fr->static_max_at, np_core_status(fr->core, NP_STAT_MAP_ID));
    fflush(stdout);
    np_core_destroy(fr->core);
    return fr->defects ? 1 : 0;
}

/*
 * ---- serve mode (--serve 1): the run is driven over stdin/stdout one
 * command at a time, for tests/e2e's input bots, which decide each input
 * from what the game shows. One command per line; each answers with one
 * line, after any DEFECT lines the frames it ran printed. Status changes,
 * guest logs and saves go to stderr as in the batch mode.
 *
 *   run N KEYS [X Y] [until NAME=V|NAME!=V]...
 *        hold KEYS ("a+up", "none") and, with X Y, a touch at (X,Y) for N
 *        frames, or until any of the conditions holds after a frame. NAME is a
 *        status (field_ready, map_id, in_battle, quicksave_seq,
 *        quicksave_result, link_active, resets: soft resets so far) or a probe field (field, x, z, y,
 *        facing, move_state, ui, ui_arg; core/include/np_e2e.h). Loaded
 *        schedules add their presses on top.
 *        -> "ok K HIT S0 .. S15": the next frame K, HIT 1 if the condition
 *           stopped it, the 16 status words; "dead K" once the core stopped
 *   e2e         -> "e2e HEX" the probe block, or "e2e none" (PC_E2E unset)
 *   peek A L    -> "peek HEX", L bytes of guest memory at A
 *   dump PATH   -> "ok": the last frame as a PPM (both screens; one on the GBA)
 *   opt NAME=V  -> "ok": an np_core option, from the next frame on
 *   sched PATH  -> "ok": a press schedule, its frames counted from now
 *   flush       -> "ok": the save image stored now if the guest changed it (np_core_save_flush): a GBA
 *                  guest publishes its flash chip every frame and leaves the storing to the host
 *   quit        -> the summary line (as the batch mode ends), then exit
 */
typedef struct serve_cond {
    int stat;  /* status index, or -1 */
    int field; /* probe field offset in bytes, or -1 */
    int neg;
    uint32_t value;
} serve_cond;

static const struct { const char *name; int offset; } k_e2e_fields[] = {
    {"field", offsetof(np_e2e_block, field)},   {"x", offsetof(np_e2e_block, x)},
    {"z", offsetof(np_e2e_block, z)},           {"y", offsetof(np_e2e_block, y)},
    {"facing", offsetof(np_e2e_block, facing)},
    {"move_state", offsetof(np_e2e_block, move_state)}, {"ui", offsetof(np_e2e_block, ui)},
    {"ui_arg", offsetof(np_e2e_block, ui_arg)},
};

static int parse_cond(const char *s, serve_cond *c) {
    const char *op = strstr(s, "!=");
    const char *eq = strchr(s, '=');
    if (!eq) return -1;
    c->neg = op != NULL;
    size_t n = (size_t)((op ? op : eq) - s);
    c->stat = c->field = -1;
    for (uint32_t i = 0; i < sizeof k_stat_names / sizeof *k_stat_names; i++)
        if (strlen(k_stat_names[i]) == n && strncmp(s, k_stat_names[i], n) == 0) c->stat = (int)i;
    for (size_t i = 0; i < sizeof k_e2e_fields / sizeof *k_e2e_fields; i++)
        if (strlen(k_e2e_fields[i].name) == n && strncmp(s, k_e2e_fields[i].name, n) == 0)
            c->field = k_e2e_fields[i].offset;
    if (c->stat < 0 && c->field < 0) return -1;
    c->value = (uint32_t)strtol(eq + 1, NULL, 0);
    return 0;
}

static const np_e2e_block *e2e_block(frame_run *fr) {
    uint32_t addr = fr->status[NP_STAT_E2E];
    if (!addr) return NULL;
    const np_e2e_block *b = (const np_e2e_block *)np_core_guest_ptr(fr->core, addr, sizeof(np_e2e_block));
    return b && b->magic == NP_E2E_MAGIC ? b : NULL;
}

static int cond_holds(frame_run *fr, const serve_cond *c) {
    uint32_t v;
    if (c->stat >= 0) {
        v = fr->status[c->stat];
    } else {
        const np_e2e_block *b = e2e_block(fr);
        if (!b) return 0;
        memcpy(&v, (const uint8_t *)b + c->field, 4);
    }
    return c->neg ? v != c->value : v == c->value;
}

static void print_hex(const char *tag, const uint8_t *p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    fputs(tag, stdout);
    putchar(' ');
    for (size_t i = 0; i < n; i++) {
        putchar(digits[p[i] >> 4]);
        putchar(digits[p[i] & 15]);
    }
    putchar('\n');
}

static int64_t serve(frame_run *fr, int64_t k) {
    static char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        char *save = NULL, *cmd = strtok_r(line, " \t\r\n", &save);
        if (!cmd) continue;
        if (strcmp(cmd, "run") == 0) {
            char *a = strtok_r(NULL, " \t\r\n", &save), *keys = strtok_r(NULL, " \t\r\n", &save);
            if (!a || !keys) {
                printf("error run N KEYS [X Y] [until COND]\n");
                fflush(stdout);
                continue;
            }
            int64_t n = strtoll(a, NULL, 0);
            np_input hold = {0};
            hold.keys = strcmp(keys, "none") == 0 ? 0 : parse_key_names(keys);
            serve_cond c[8];
            int nconds = 0, bad = 0;
            for (char *t = strtok_r(NULL, " \t\r\n", &save); t; t = strtok_r(NULL, " \t\r\n", &save)) {
                if (strcmp(t, "until") == 0) {
                    char *e = strtok_r(NULL, " \t\r\n", &save);
                    bad |= !e || nconds == 8 || parse_cond(e, &c[nconds]) != 0;
                    nconds += !bad;
                } else {
                    char *y = strtok_r(NULL, " \t\r\n", &save);
                    bad |= !y;
                    hold.touch = 1;
                    hold.touch_x = (uint16_t)atoi(t);
                    hold.touch_y = y ? (uint16_t)atoi(y) : 0;
                }
            }
            if (bad) {
                printf("error bad run arguments\n");
                fflush(stdout);
                continue;
            }
            int hit = 0;
            for (int64_t i = 0; i < n; i++) {
                np_input in = hold;
                schedule_input(k, &in);
                if (run_frame(fr, k, &in) != 0) break;
                k++;
                for (int ci = 0; ci < nconds && !hit; ci++) hit = cond_holds(fr, &c[ci]);
                if (hit) break;
            }
            if (fr->rc != 0) {
                printf("dead %lld\n", (long long)k);
                fflush(stdout);
                return k;
            }
            printf("ok %lld %d", (long long)k, hit);
            for (uint32_t i = 0; i < NP_STAT_COUNT; i++) printf(" %u", fr->status[i]);
            putchar('\n');
        } else if (strcmp(cmd, "e2e") == 0) {
            const np_e2e_block *b = e2e_block(fr);
            if (b) print_hex("e2e", (const uint8_t *)b, sizeof *b);
            else printf("e2e none\n");
        } else if (strcmp(cmd, "peek") == 0) {
            char *a = strtok_r(NULL, " \t\r\n", &save), *l = strtok_r(NULL, " \t\r\n", &save);
            uint32_t addr = a ? (uint32_t)strtoul(a, NULL, 0) : 0, len = l ? (uint32_t)strtoul(l, NULL, 0) : 0;
            const uint8_t *m = len && len <= (1u << 20) ? np_core_guest_ptr(fr->core, addr, len) : NULL;
            if (m) print_hex("peek", m, len);
            else printf("error out of range\n");
        } else if (strcmp(cmd, "dump") == 0) {
            char *path = strtok_r(NULL, "\r\n", &save);
            FILE *out = path && fr->have_prev ? fopen(path, "wb") : NULL;
            if (!out) {
                printf("error cannot write %s\n", path ? path : "(no path)");
            } else {
                const np_frame *f = &fr->f;
                fprintf(out, "P6\n%u %u\n255\n", f->width, f->height * frame_screens(f));
                for (int s = 0; s < frame_screens(f); s++)
                    for (uint32_t y = 0; y < f->height; y++)
                        for (uint32_t x = 0; x < f->width; x++) {
                            uint32_t p = f->screen[s][y * f->stride + x];
                            uint8_t rgb[3] = {(uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p};
                            fwrite(rgb, 1, 3, out);
                        }
                printf(fclose(out) == 0 ? "ok\n" : "error short write\n");
            }
        } else if (strcmp(cmd, "opt") == 0) {
            char *v = strtok_r(NULL, " \t\r\n", &save);
            opt_set s;
            if (!v || parse_opt(v, &s) != 0) printf("error bad option\n");
            else {
                np_core_set_option(fr->core, s.opt, s.value);
                printf("ok\n");
            }
        } else if (strcmp(cmd, "sched") == 0) {
            char *path = strtok_r(NULL, "\r\n", &save);
            printf(path && load_schedule(path, k) == 0 ? "ok\n" : "error cannot read schedule\n");
        } else if (strcmp(cmd, "flush") == 0) {
            printf(np_core_save_flush(fr->core) == 0 ? "ok\n" : "error the save store failed\n");
        } else if (strcmp(cmd, "quit") == 0) {
            return k;
        } else {
            printf("error unknown command %s\n", cmd);
        }
        fflush(stdout);
    }
    return k;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    runner r = {0};
    r.rom = fopen(argv[1], "rb");
    if (!r.rom) {
        fprintf(stderr, "np_gp: cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(r.rom, 0, SEEK_END);
    long rom_size = ftell(r.rom);

    const char *options[MAX_OPTIONS + 1];
    int noptions = 0;
    static opt_set sets[MAX_SETS];
    int nsets = 0;
    static int64_t dump_at[MAX_LIST];
    int ndump_at = 0;
    int64_t frames = 600, dump_every = 0, dump_from = 0, random_from = -1, time_from = -1;
    struct { int64_t frame; uint32_t addr, len; } peeks[64];
    int npeeks = 0, serving = 0;
    const char *dump_dir = NULL;
    int game = NP_GAME_PLATINUM;

    for (int i = 2; i < argc; i += 2) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) return usage();
        if (strcmp(a, "--game") == 0) {
            static const char *const names[NP_GAME_COUNT] = {
                [NP_GAME_DIAMOND] = "diamond", [NP_GAME_PEARL] = "pearl", [NP_GAME_PLATINUM] = "platinum",
                [NP_GAME_BLACK] = "black",     [NP_GAME_WHITE] = "white",
                [NP_GAME_HEARTGOLD] = "heartgold", [NP_GAME_SOULSILVER] = "soulsilver",
                [NP_GAME_RUBY] = "ruby", [NP_GAME_SAPPHIRE] = "sapphire", [NP_GAME_EMERALD] = "emerald"};
            game = -1;
            for (int g = 0; g < NP_GAME_COUNT; g++)
                if (names[g] && strcmp(v, names[g]) == 0) game = g;
            if (game < 0) return usage();
        } else if (strcmp(a, "--frames") == 0) frames = strtoll(v, NULL, 0);
        else if (strcmp(a, "--save") == 0) r.save_path = v;
        else if (strcmp(a, "--schedule") == 0) {
            if (load_schedule(v, 0) != 0) {
                fprintf(stderr, "np_gp: cannot read schedule %s\n", v);
                return 2;
            }
        } else if (strcmp(a, "--dump") == 0) dump_dir = v;
        else if (strcmp(a, "--dump-at") == 0) {
            const char *p = v;
            while (*p && ndump_at < MAX_LIST) {
                char *end;
                dump_at[ndump_at++] = strtoll(p, &end, 0);
                if (end == p) return usage();
                p = *end == ',' ? end + 1 : end;
            }
        } else if (strcmp(a, "--dump-every") == 0) dump_every = strtoll(v, NULL, 0);
        else if (strcmp(a, "--dump-from") == 0) dump_from = strtoll(v, NULL, 0);
        else if (strcmp(a, "-e") == 0 && noptions < MAX_OPTIONS) options[noptions++] = v;
        else if (strcmp(a, "-o") == 0 && nsets < MAX_SETS) {
            if (parse_opt(v, &sets[nsets]) != 0) {
                fprintf(stderr, "np_gp: bad option '%s'\n", v);
                return usage();
            }
            nsets++;
        } else if (strcmp(a, "--random") == 0) {
            g_seed = strtoull(v, NULL, 0);
            g_random = 1;
        } else if (strcmp(a, "--random-from") == 0) random_from = strtoll(v, NULL, 0);
        else if (strcmp(a, "--hang-sec") == 0) g_hang_sec = atof(v);
        else if (strcmp(a, "--time-from") == 0) time_from = strtoll(v, NULL, 0);
        else if (strcmp(a, "--serve") == 0) serving = atoi(v) != 0;
        else if (strcmp(a, "--peek") == 0 && npeeks < 64) {
            long long pf;
            unsigned pa, pl;
            if (sscanf(v, "%lld:%i:%i", &pf, (int *)&pa, (int *)&pl) != 3 || pl == 0 || pl > 4096) return usage();
            peeks[npeeks].frame = pf;
            peeks[npeeks].addr = pa;
            peeks[npeeks++].len = pl;
        }
        else return usage();
    }
    qsort(dump_at, (size_t)ndump_at, sizeof *dump_at, cmp_i64);
    for (char **e = environ; e && *e && noptions < MAX_OPTIONS; e++)
        if (strncmp(*e, "PC_", 3) == 0) options[noptions++] = *e;
    options[noptions] = NULL;

    np_host host = {0};
    host.user = &r;
    host.rom_size = (uint32_t)rom_size;
    host.rom_read = rom_read;
    host.save_load = save_load;
    host.save_store = save_store;
    host.log = log_line;
    if (!np_core_available((np_game)game)) {
        fprintf(stderr, "np_gp: that game is not built into this binary\n");
        return 2;
    }
    frame_run fr;
    memset(&fr, 0, sizeof fr);
    fr.core = np_core_create((np_game)game, &host, options);
    if (!fr.core) {
        fprintf(stderr, "np_gp: create failed: %s\n", np_core_create_error());
        return 1;
    }

    if (random_from < 0) random_from = 0;
    fr.time_from = time_from < 0 ? 0 : time_from;
    fr.sets = sets;
    fr.nsets = nsets;
    fr.hash = 0xCBF29CE484222325ull;
    fr.static_max_at = -1;
    g_rng = g_seed * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    if (!g_rng) g_rng = 1;

    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);

    for (uint32_t i = 0; i < NP_STAT_COUNT; i++) fr.status[i] = np_core_status(fr.core, i);
    if (serving) {
        int64_t k = serve(&fr, 0);
        int status = finish(&fr, k);
        fclose(r.rom);
        return status;
    }

    int ndump_i = 0;
    int64_t k = 0;
    for (; k < frames; k++) {
        np_input in = {0};
        if (g_random && k >= random_from) random_input(&in);
        else schedule_input(k, &in);
        if (run_frame(&fr, k, &in) != 0) break;

        for (int p = 0; p < npeeks; p++) {
            if (peeks[p].frame != k) continue;
            const uint8_t *m = np_core_guest_ptr(fr.core, peeks[p].addr, peeks[p].len);
            printf("peek %lld 0x%08x:", (long long)k, peeks[p].addr);
            for (uint32_t o = 0; m && o + 4 <= peeks[p].len; o += 4) {
                uint32_t w;
                memcpy(&w, m + o, 4);
                printf(" %08x", w);
            }
            printf(m ? "\n" : " (out of range)\n");
        }
        if (dump_dir) {
            int want = k + 1 == frames || (dump_every && k >= dump_from && (k - dump_from) % dump_every == 0);
            while (ndump_i < ndump_at && dump_at[ndump_i] <= k) {
                if (dump_at[ndump_i] == k) want = 1;
                ndump_i++;
            }
            if (want && dump_ppm(dump_dir, k, &fr.f) != 0)
                fprintf(stderr, "np_gp: cannot write a frame dump into %s\n", dump_dir);
        }
    }
    int status = finish(&fr, k);
    fclose(r.rom);
    return status;
}
