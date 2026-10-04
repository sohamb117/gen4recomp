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
 *   np_gp <rom.nds> [options]
 *     --game NAME         diamond, pearl or platinum (default platinum)
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "np_core.h"
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
 * previous step of the same file. */
static int load_schedule(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char line[512];
    int64_t prev = 0;
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
            p->frame = rel ? prev + frame : frame;
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

static int dump_ppm(const char *dir, int64_t number, const np_frame *f) {
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%06lld.ppm", dir, (long long)number);
    FILE *out = fopen(path, "wb");
    if (!out) return -1;
    fprintf(out, "P6\n%u %u\n255\n", f->width, f->height * 2);
    uint8_t *row = malloc((size_t)f->width * 3);
    for (int s = 0; s < 2; s++)
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
                                           "map_id",      "in_battle"};

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
    fprintf(stderr, "usage: np_gp <rom.nds> [--game diamond|pearl|platinum] [--frames N] [--save FILE]\n"
                    "             [--schedule FILE]...\n"
                    "             [--dump DIR [--dump-at F,..]... [--dump-every N\n"
                    "             [--dump-from F]]] [-o [F:]NAME=V]... [-e K=V]... [--random SEED\n"
                    "             [--random-from F]] [--hang-sec S] [--time-from F] [--peek F:ADDR:LEN]...\n");
    return 2;
}

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
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
    opt_set sets[MAX_SETS];
    int nsets = 0;
    static int64_t dump_at[MAX_LIST];
    int ndump_at = 0;
    int64_t frames = 600, dump_every = 0, dump_from = 0, random_from = -1, time_from = -1;
    struct { int64_t frame; uint32_t addr, len; } peeks[64];
    int npeeks = 0;
    const char *dump_dir = NULL;
    int game = NP_GAME_PLATINUM;

    for (int i = 2; i < argc; i += 2) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) return usage();
        if (strcmp(a, "--game") == 0) {
            static const char *const names[NP_GAME_COUNT] = {"diamond", "pearl", "platinum"};
            game = -1;
            for (int g = 0; g < NP_GAME_COUNT; g++)
                if (strcmp(v, names[g]) == 0) game = g;
            if (game < 0) return usage();
        } else if (strcmp(a, "--frames") == 0) frames = strtoll(v, NULL, 0);
        else if (strcmp(a, "--save") == 0) r.save_path = v;
        else if (strcmp(a, "--schedule") == 0) {
            if (load_schedule(v) != 0) {
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
    np_core *core = np_core_create((np_game)game, &host, options);
    if (!core) {
        fprintf(stderr, "np_gp: create failed: %s\n", np_core_create_error());
        return 1;
    }

    if (random_from < 0) random_from = 0;
    if (time_from < 0) time_from = 0;
    g_rng = g_seed * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    if (!g_rng) g_rng = 1;

    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);

    uint32_t status[NP_STAT_COUNT];
    for (uint32_t i = 0; i < NP_STAT_COUNT; i++) status[i] = np_core_status(core, i);
    uint64_t hash = 0xCBF29CE484222325ull, audio_total = 0, prev_number = 0, prev_screen = 0;
    int have_prev = 0, defects = 0, ndump_i = 0;
    int64_t stall_run = 0, audio_run = 0, static_run = 0, static_max = 0, static_max_at = -1;
    int64_t stalls = 0, audio_stalls = 0;
    double t_timed = 0;
    int64_t timed_frames = 0;
    np_frame f;
    int rc = 0;
    static int16_t audio[2 * 8192];

    int64_t k = 0;
    for (; k < frames; k++) {
        g_frame = k;
        np_input in = {0};
        if (g_random && k >= random_from) random_input(&in);
        else schedule_input(k, &in);
        for (int o = 0; o < nsets; o++)
            if (sets[o].frame == k) np_core_set_option(core, sets[o].opt, sets[o].value);

        double t0 = now_s();
        g_wd_start = t0;
        g_wd_frame = k;
        rc = np_core_run_frame(core, &in, &f);
        g_wd_frame = -1;
        if (k >= time_from) {
            t_timed += now_s() - t0;
            timed_frames++;
        }
        if (rc != 0) break;

        uint64_t sh = fnv(0xCBF29CE484222325ull, f.screen[0], (size_t)f.stride * f.height * 4);
        sh = fnv(sh, f.screen[1], (size_t)f.stride * f.height * 4);
        hash = fnv(hash, f.screen[0], (size_t)f.stride * f.height * 4);
        hash = fnv(hash, f.screen[1], (size_t)f.stride * f.height * 4);
        size_t n, got = 0;
        while ((n = np_core_audio_read(core, audio, 8192)) > 0) {
            hash = fnv(hash, audio, n * 4);
            got += n;
        }
        audio_total += got;

        if (have_prev) {
            if (f.number == prev_number) {
                if (++stall_run == 60) {
                    printf("DEFECT stall frame %lld seed %llu: the VBlank counter did not advance (%llu)\n",
                           (long long)k, (unsigned long long)(g_random ? g_seed : 0),
                           (unsigned long long)f.number);
                    stalls++;
                    defects++;
                }
            } else {
                stall_run = 0;
            }
            if (got == 0) {
                if (++audio_run == AUDIO_STALL_FRAMES) {
                    printf("DEFECT audio-stall frame %lld seed %llu: no audio for %d frames\n", (long long)k,
                           (unsigned long long)(g_random ? g_seed : 0), AUDIO_STALL_FRAMES);
                    audio_stalls++;
                    defects++;
                }
            } else {
                audio_run = 0;
            }
            if (sh == prev_screen) {
                if (++static_run > static_max) {
                    static_max = static_run;
                    static_max_at = k;
                }
            } else {
                static_run = 0;
            }
        }
        have_prev = 1;
        prev_number = f.number;
        prev_screen = sh;

        for (uint32_t i = 0; i < NP_STAT_COUNT; i++) {
            uint32_t v = np_core_status(core, i);
            if (v != status[i] && i < sizeof k_stat_names / sizeof *k_stat_names && i != NP_STAT_QUICKSAVE_SEQ)
                fprintf(stderr, "[status] frame %lld: %s %u -> %u\n", (long long)k, k_stat_names[i], status[i], v);
            status[i] = v;
        }

        for (int p = 0; p < npeeks; p++) {
            if (peeks[p].frame != k) continue;
            const uint8_t *m = np_core_guest_ptr(core, peeks[p].addr, peeks[p].len);
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
            if (want && dump_ppm(dump_dir, k, &f) != 0)
                fprintf(stderr, "np_gp: cannot write a frame dump into %s\n", dump_dir);
        }
    }
    np_core_save_flush(core);
    if (rc < 0) {
        printf("DEFECT trap frame %lld seed %llu: %s\n", (long long)k, (unsigned long long)(g_random ? g_seed : 0),
               np_core_last_error(core));
        defects++;
    } else if (rc > 0) {
        /* The save lab ends its run with exit(0) once the save is written. */
        if (strstr(np_core_last_error(core), "status 0")) {
            printf("exited frame %lld: %s\n", (long long)k, np_core_last_error(core));
        } else {
            printf("DEFECT exit frame %lld seed %llu: %s\n", (long long)k,
                   (unsigned long long)(g_random ? g_seed : 0), np_core_last_error(core));
            defects++;
        }
    }
    double ms = timed_frames ? t_timed * 1e3 / (double)timed_frames : 0;
    printf("frames %lld  hash %016llx  ms/frame %.3f  fps %.1f  size %ux%u  audio %llu  stalls %lld  "
           "audio-stalls %lld  static-max %lld@%lld  map %u\n",
           (long long)k, (unsigned long long)hash, ms, ms > 0 ? 1e3 / ms : 0, f.width, f.height,
           (unsigned long long)audio_total, (long long)stalls, (long long)audio_stalls, (long long)static_max,
           (long long)static_max_at, np_core_status(core, NP_STAT_MAP_ID));
    fflush(stdout);
    np_core_destroy(core);
    fclose(r.rom);
    return defects ? 1 : 0;
}
