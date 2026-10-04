/*
 * np_headless: run a real game core without the SDL shell.
 *
 * For bring-up and CI of the wasm2c'd games: boots a core through np_core.h
 * against a ROM file, runs a number of frames with scripted input, and
 * reports the guest's log, a running hash of the frames and audio, and
 * optional PPM screenshots. Nothing here is mock-specific.
 *
 *   np_headless <diamond|pearl|platinum> <rom.nds> [options]
 *     --frames N         frames to run (default 600)
 *     --save FILE        backup chip file: loaded if present, written on store
 *     --dump DIR         write DIR/frame_NNNNNN.ppm (both screens stacked)
 *     --dump-every N     dump every N frames (default: only the last frame)
 *     --dump-from F      with --dump-every, start dumping at frame F
 *     --press F:KEYS     from frame F hold KEYS (hex NP_KEY_* mask), repeatable
 *     --rtc SECONDS      RTC value (seconds since 2000-01-01) instead of the
 *                        port's deterministic clock
 *     -e KEY=VALUE       guest environment entry, repeatable; PC_* variables
 *                        of this process's environment are passed through too
 *     -o [F:]NAME=VALUE  np_core option (enum np_opt), set before frame F
 *                        (default 0), repeatable. NAME is bgm_volume,
 *                        se_volume, render_scale, widescreen, camera_zoom,
 *                        camera_tilt, quicksave_seq, rules, text_instant or
 *                        an index; VALUE is a C integer (negative allowed).
 *     --rms-from F       measure the audio's RMS from frame F (default 0)
 *     --schedule FILE    shell autotest press schedule (F:keys[:N[:R:C]],
 *                        F:tap:X:Y[:N[:R:C]], +D; shell/README.md), on top
 *                        of --press
 *     --state-test N     snapshot round trips: run N frames, then per round
 *                        save, run M frames hashing them, load, run the same
 *                        M frames again and require the same hash; the next
 *                        round continues from there. Reports sizes and times.
 *     --state-span M     frames per round (default 120)
 *     --state-rounds R   rounds (default 4)
 *     --net PORT         local wireless over UDP (shell/src/net.c), first port
 *                        to try (2009 is the shell's default); LAN broadcast
 *                        and same-machine discovery. While the guest reports
 *                        NP_STAT_LINK_ACTIVE the run is paced to 60 Hz, the
 *                        1x lock a linked console needs.
 *     --net-peer H:P     also say hello to this address, repeatable
 *     --net-id ID        24-bit station id (default random)
 *     --net-drop PCT     drop this share of outgoing datagrams (loss testing)
 *
 * Status changes (enum np_status) are printed as they happen.
 *
 * Exit status: 0 when the frames ran (or the guest exited with 0), 1 on a
 * guest failure, nonzero exit or a state-test mismatch, 2 on usage or I/O
 * errors.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "net.h"
#include "np_core.h"
#include "np_guest_abi.h"

/* The CRT declares Windows' environment block (and mingw even defines
 * environ itself as a macro); POSIX leaves the declaration to us. */
#if defined(_WIN32)
#define np_environ _environ
#else
extern char **environ;
#define np_environ environ
#endif

#define MAX_OPTIONS 128
#define MAX_PRESSES 64
#define MAX_SETS 64

typedef struct runner {
    FILE *rom;
    const char *save_path;
    int64_t rtc;
} runner;

typedef struct press {
    uint64_t frame;
    uint16_t keys;
} press;

typedef struct opt_set {
    uint64_t frame;
    uint32_t opt, value;
} opt_set;

/* One step of a shell press schedule (shell/README.md, press=@file). */
typedef struct sched_step {
    int64_t frame;
    int n, every, count, tap;
    uint16_t keys, x, y;
} sched_step;

#define MAX_SCHED 1024
static sched_step g_sched[MAX_SCHED];
static int g_nsched;

/* "a+up" -> NP_KEY_A | NP_KEY_UP (bit i = names[i]); no strtok, the
 * schedule parser is in the middle of one. */
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

/* The shell autotest's press schedule format, so the same files drive both:
 * steps separated by ';' or newlines, '#' comments,
 *   F:keys[:N[:R:C]]  or  F:tap:X:Y[:N[:R:C]]  or  F:none,
 * F may be "+D" (D frames after the previous step's frame). */
static int load_schedule(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char line[512];
    int64_t prev = 0;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        for (char *step = strtok(line, ";\n"); step; step = strtok(NULL, ";\n")) {
            while (*step == ' ' || *step == '\t' || *step == '\r') step++;
            if (!*step) continue;
            if (g_nsched == MAX_SCHED) break;
            /* sscanf, not strtok: strtok is splitting the steps. */
            int rel = *step == '+';
            char k[64] = {0};
            int frame = 0, v[5] = {0};
            int got = sscanf(step + rel, "%d:%63[a-zA-Z+]:%d:%d:%d:%d:%d", &frame, k, &v[0], &v[1], &v[2], &v[3],
                             &v[4]) -
                      2;
            if (got < 0) {
                fclose(f);
                fprintf(stderr, "np_headless: bad schedule step \"%s\"\n", step);
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

static const char *const k_opt_names[] = {"bgm_volume", "se_volume",     "render_scale", "widescreen",  "camera_zoom",
                                          "camera_tilt", "quicksave_seq", "rules",        "text_instant"};
static const char *const k_stat_names[] = {"link_active", "field_ready", "quicksave_seq", "quicksave_result",
                                           "map_id"};

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

static int save_store(void *user, const void *src, uint32_t len) {
    runner *r = user;
    if (!r->save_path) return 0;
    FILE *f = fopen(r->save_path, "wb");
    if (!f) return -1;
    int ok = fwrite(src, 1, len, f) == len;
    ok &= fclose(f) == 0;
    fprintf(stderr, "[headless] stored %u-byte save to %s\n", len, r->save_path);
    return ok ? 0 : -1;
}

static int64_t rtc_now(void *user) {
    return ((runner *)user)->rtc;
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

static double now_ms(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static int dump_ppm(const char *dir, uint64_t number, const np_frame *f) {
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%06llu.ppm", dir, (unsigned long long)number);
    FILE *out = fopen(path, "wb");
    if (!out) return -1;
    fprintf(out, "P6\n%u %u\n255\n", f->width, f->height * 2);
    for (int s = 0; s < 2; s++)
        for (uint32_t y = 0; y < f->height; y++)
            for (uint32_t x = 0; x < f->width; x++) {
                uint32_t p = f->screen[s][y * f->stride + x];
                uint8_t rgb[3] = {(uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p};
                fwrite(rgb, 1, 3, out);
            }
    return fclose(out) == 0 ? 0 : -1;
}

static int parse_opt(const char *v, opt_set *s) {
    char *end;
    s->frame = 0;
    const char *colon = strchr(v, ':'), *eq = strchr(v, '=');
    if (!eq) return -1;
    if (colon && colon < eq) {
        s->frame = strtoull(v, &end, 0);
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

/* --net: the np_host transport callbacks over shell/src/net.c. */
static np_net *g_net;

static uint32_t net_self_cb(void *user) {
    (void)user;
    return np_net_self(g_net);
}

static int net_send_cb(void *user, uint32_t peer, const void *buf, uint32_t len) {
    (void)user;
    return np_net_send(g_net, peer, buf, len);
}

static int net_recv_cb(void *user, uint32_t *peer, void *buf, uint32_t cap) {
    (void)user;
    return np_net_recv(g_net, peer, buf, cap);
}

static void net_log_cb(void *user, const char *line) {
    (void)user;
    fprintf(stderr, "[net] %s\n", line);
}

/* Sleeps until `deadline` (now_ms() clock), for the 60 Hz link pacing. */
static void sleep_until(double deadline) {
    double wait = deadline - now_ms();
    if (wait <= 0) return;
#if defined(_WIN32)
    Sleep((DWORD)wait);
#else
    usleep((useconds_t)(wait * 1000.0));
#endif
}

static int usage(void) {
    fprintf(stderr, "usage: np_headless <diamond|pearl|platinum> <rom.nds> [--frames N] [--save FILE] [--dump DIR]\n"
                    "                   [--dump-every N [--dump-from F]] [--press F:KEYS]... [--rtc SECONDS] [-e KEY=VALUE]...\n"
                    "                   [-o [F:]NAME=VALUE]... [--rms-from F] [--schedule FILE]\n"
                    "                   [--state-test N [--state-span M] [--state-rounds R]]\n"
                    "                   [--net PORT [--net-peer HOST:PORT]... [--net-id ID] [--net-drop PCT]]\n");
    return 2;
}

typedef struct session {
    np_core *core;
    const press *presses;
    int npresses;
    const opt_set *sets;
    int nsets;
    uint64_t rms_from;
    double sumsq[2];
    uint64_t rms_frames, audio_frames;
    uint32_t status[NP_STAT_COUNT];
    int quiet;       /* state-test replays: no status prints */
    int hash_status; /* state test: the status is part of the hash */
} session;

/* Runs frame `k` (0-based) with its scripted input and options; folds the
 * frame and its audio (and with hash_status the status) into *hash.
 * Returns run_frame's rc. */
static int step(session *s, uint64_t k, np_frame *f, uint64_t *hash) {
    np_input in = {0};
    for (int p = 0; p < s->npresses; p++)
        if (s->presses[p].frame <= k) in.keys = s->presses[p].keys;
    schedule_input((int64_t)k, &in);
    for (int o = 0; o < s->nsets; o++)
        if (s->sets[o].frame == k) np_core_set_option(s->core, s->sets[o].opt, s->sets[o].value);
    int rc = np_core_run_frame(s->core, &in, f);
    if (rc != 0) return rc;
    /* The same formula as ever (screens at stride x height, then audio),
     * so a hash recorded before stays comparable. */
    *hash = fnv(*hash, f->screen[0], (size_t)f->stride * f->height * 4);
    *hash = fnv(*hash, f->screen[1], (size_t)f->stride * f->height * 4);
    static int16_t audio[2 * 8192];
    size_t n;
    while ((n = np_core_audio_read(s->core, audio, 8192)) > 0) {
        *hash = fnv(*hash, audio, n * 4);
        s->audio_frames += n;
        if (k >= s->rms_from) {
            for (size_t i = 0; i < n; i++) {
                s->sumsq[0] += (double)audio[2 * i] * audio[2 * i];
                s->sumsq[1] += (double)audio[2 * i + 1] * audio[2 * i + 1];
            }
            s->rms_frames += n;
        }
    }
    for (uint32_t i = 0; i < NP_STAT_COUNT; i++) {
        uint32_t v = np_core_status(s->core, i);
        if (s->hash_status) *hash = fnv(*hash, &v, 4);
        if (v != s->status[i] && !s->quiet) {
            if (i < sizeof k_stat_names / sizeof *k_stat_names)
                fprintf(stderr, "[status] frame %llu: %s %u -> %u\n", (unsigned long long)k, k_stat_names[i],
                        s->status[i], v);
            else
                fprintf(stderr, "[status] frame %llu: status[%u] %u -> %u\n", (unsigned long long)k, i, s->status[i],
                        v);
        }
        s->status[i] = v;
    }
    return 0;
}

/* --state-test: returns 0 when every round replayed identically. */
static int state_test(session *s, uint64_t first, uint64_t span, int rounds, uint64_t *ran) {
    np_frame f;
    uint64_t scratch = 0;
    for (; *ran < first; ++*ran)
        if (step(s, *ran, &f, &scratch) != 0) return -1;
    int bad = 0;
    for (int r = 0; r < rounds; r++) {
        const uint64_t at = *ran;
        size_t cap = np_core_state_size(s->core);
        if (cap == 0) {
            fprintf(stderr, "state-test: no snapshot possible: %s\n", np_core_last_error(s->core));
            return -1;
        }
        void *buf = malloc(cap);
        size_t len = 0;
        double t0 = now_ms();
        if (!buf || np_core_state_save(s->core, buf, cap, &len) != 0) {
            fprintf(stderr, "state-test: save failed: %s\n", np_core_last_error(s->core));
            free(buf);
            return -1;
        }
        double t_save = now_ms() - t0;
        /* The host's own state at the save point: its options (a load
         * leaves them alone by contract) and the status it last saw. */
        uint32_t status_at[NP_STAT_COUNT], opts_at[NP_OPT_COUNT];
        memcpy(status_at, s->status, sizeof status_at);
        for (uint32_t i = 0; i < NP_OPT_COUNT; i++) opts_at[i] = np_core_get_option(s->core, i);
        uint64_t h1 = 0xCBF29CE484222325ull, h2 = h1;
        s->hash_status = 1;
        fprintf(stderr, "[state] round %d: saved at frame %llu\n", r, (unsigned long long)at);
        for (uint64_t k = at; k < at + span; k++)
            if (step(s, k, &f, &h1) != 0) {
                free(buf);
                return -1;
            }
        t0 = now_ms();
        if (np_core_state_load(s->core, buf, len) != 0) {
            fprintf(stderr, "state-test: load failed: %s\n", np_core_last_error(s->core));
            free(buf);
            return -1;
        }
        double t_load = now_ms() - t0;
        fprintf(stderr, "[state] round %d: loaded, replaying\n", r);
        memcpy(s->status, status_at, sizeof status_at);
        for (uint32_t i = 0; i < NP_OPT_COUNT; i++) np_core_set_option(s->core, i, opts_at[i]);
        s->quiet = 1;
        for (uint64_t k = at; k < at + span; k++)
            if (step(s, k, &f, &h2) != 0) {
                free(buf);
                return -1;
            }
        s->quiet = 0;
        s->hash_status = 0;
        *ran = at + span;
        printf("state round %d: frame %llu  size %zu bytes (bound %zu)  save %.2f ms  load %.2f ms  "
               "%llu frames %016llx / replay %016llx  %s\n",
               r, (unsigned long long)at, len, cap, t_save, t_load, (unsigned long long)span,
               (unsigned long long)h1, (unsigned long long)h2, h1 == h2 ? "ok" : "MISMATCH");
        bad += h1 != h2;
        free(buf);
    }
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 3) return usage();
    static const char *const names[NP_GAME_COUNT] = {"diamond", "pearl", "platinum"};
    int game = -1;
    for (int g = 0; g < NP_GAME_COUNT; g++)
        if (strcmp(argv[1], names[g]) == 0) game = g;
    if (game < 0) return usage();

    runner r = {0};
    r.rom = fopen(argv[2], "rb");
    if (!r.rom) {
        fprintf(stderr, "np_headless: cannot open %s\n", argv[2]);
        return 2;
    }
    fseek(r.rom, 0, SEEK_END);
    long rom_size = ftell(r.rom);

    const char *options[MAX_OPTIONS + 1];
    int noptions = 0;
    press presses[MAX_PRESSES];
    int npresses = 0;
    opt_set sets[MAX_SETS];
    int nsets = 0;
    uint64_t frames = 600, dump_every = 0, dump_from = 0, rms_from = 0, state_first = 0, state_span = 120;
    int state_rounds = 4, do_state = 0;
    const char *dump_dir = NULL;
    int have_rtc = 0;
    int net_on = 0, net_drop = 0, npeers = 0;
    uint16_t net_port = 0;
    uint32_t net_id = 0;
    const char *net_peers[8];

    for (int i = 3; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) return usage();
        if (strcmp(a, "--frames") == 0) frames = strtoull(v, NULL, 0);
        else if (strcmp(a, "--save") == 0) r.save_path = v;
        else if (strcmp(a, "--dump") == 0) dump_dir = v;
        else if (strcmp(a, "--dump-every") == 0) dump_every = strtoull(v, NULL, 0);
        else if (strcmp(a, "--dump-from") == 0) dump_from = strtoull(v, NULL, 0);
        else if (strcmp(a, "--rtc") == 0) r.rtc = strtoll(v, NULL, 0), have_rtc = 1;
        else if (strcmp(a, "--rms-from") == 0) rms_from = strtoull(v, NULL, 0);
        else if (strcmp(a, "--schedule") == 0) {
            if (load_schedule(v) != 0) {
                fprintf(stderr, "np_headless: cannot read schedule %s\n", v);
                return 2;
            }
        }
        else if (strcmp(a, "--state-test") == 0) state_first = strtoull(v, NULL, 0), do_state = 1;
        else if (strcmp(a, "--state-span") == 0) state_span = strtoull(v, NULL, 0);
        else if (strcmp(a, "--state-rounds") == 0) state_rounds = atoi(v);
        else if (strcmp(a, "-e") == 0 && noptions < MAX_OPTIONS) options[noptions++] = v;
        else if (strcmp(a, "-o") == 0 && nsets < MAX_SETS) {
            if (parse_opt(v, &sets[nsets]) != 0) {
                fprintf(stderr, "np_headless: bad option '%s'\n", v);
                return usage();
            }
            nsets++;
        } else if (strcmp(a, "--net") == 0) {
            net_on = 1;
            net_port = (uint16_t)strtoul(v, NULL, 0);
        } else if (strcmp(a, "--net-peer") == 0 && npeers < 8) {
            net_peers[npeers++] = v;
        } else if (strcmp(a, "--net-id") == 0) {
            net_id = (uint32_t)strtoul(v, NULL, 0);
        } else if (strcmp(a, "--net-drop") == 0) {
            net_drop = atoi(v);
        } else if (strcmp(a, "--press") == 0 && npresses < MAX_PRESSES) {
            char *colon;
            presses[npresses].frame = strtoull(v, &colon, 0);
            if (*colon != ':') return usage();
            presses[npresses++].keys = (uint16_t)strtoul(colon + 1, NULL, 16);
        } else
            return usage();
        i++;
    }
    for (char **e = np_environ; e && *e && noptions < MAX_OPTIONS; e++)
        if (strncmp(*e, "PC_", 3) == 0) options[noptions++] = *e;
    options[noptions] = NULL;

    np_host host = {0};
    host.user = &r;
    host.rom_size = (uint32_t)rom_size;
    host.rom_read = rom_read;
    host.save_load = save_load;
    host.save_store = save_store;
    host.rtc_now = have_rtc ? rtc_now : NULL;
    host.log = log_line;
    if (net_on) {
        char err[160];
        np_net_config nc = {0};
        nc.port = net_port;
        nc.station_id = net_id;
        nc.lan_discovery = 1;
        nc.drop_percent = net_drop;
        nc.log = net_log_cb;
        g_net = np_net_open(&nc, err, sizeof err);
        if (!g_net) {
            fprintf(stderr, "np_headless: --net: %s\n", err);
            return 2;
        }
        for (int p = 0; p < npeers; p++)
            if (np_net_add_peer(g_net, net_peers[p], err, sizeof err) != 0) {
                fprintf(stderr, "np_headless: --net-peer: %s\n", err);
                return 2;
            }
        host.net_self = net_self_cb;
        host.net_send = net_send_cb;
        host.net_recv = net_recv_cb;
    }

    if (!np_core_available((np_game)game)) {
        fprintf(stderr, "np_headless: %s is not built into this binary\n", names[game]);
        return 2;
    }
    np_core *core = np_core_create((np_game)game, &host, options);
    if (!core) {
        fprintf(stderr, "np_headless: create failed: %s\n", np_core_create_error());
        return 1;
    }

    session s = {0};
    s.core = core;
    s.presses = presses;
    s.npresses = npresses;
    s.sets = sets;
    s.nsets = nsets;
    s.rms_from = rms_from;

    uint64_t hash = 0xCBF29CE484222325ull;
    np_frame f;
    int rc = 0, state_rc = 0;
    uint64_t ran = 0;
    if (do_state) {
        state_rc = state_test(&s, state_first, state_span, state_rounds, &ran);
        if (state_rc < 0) rc = np_core_run_frame(core, NULL, &f); /* reports the failure below */
        if (frames < ran) frames = ran;
    }
    double t0 = now_ms();
    uint64_t timed_from = ran;
    double next_frame = t0;
    for (; rc == 0 && state_rc >= 0 && ran < frames; ran++) {
        if (g_net) {
            np_net_poll(g_net);
            /* Linked: real time, as a console runs, or the partner's MP
             * lifetime runs out while this side races ahead. */
            if (np_core_status(core, NP_STAT_LINK_ACTIVE)) {
                next_frame += 1000.0 / 60.0;
                sleep_until(next_frame);
            } else {
                next_frame = now_ms();
            }
        }
        rc = step(&s, ran, &f, &hash);
        if (rc != 0) break;
        int last = ran + 1 == frames;
        if (dump_dir && (last || (dump_every && ran >= dump_from && (ran - dump_from) % dump_every == 0)) &&
            dump_ppm(dump_dir, f.number, &f) != 0)
            fprintf(stderr, "np_headless: cannot write a frame dump into %s\n", dump_dir);
    }
    double elapsed = now_ms() - t0;
    np_core_save_flush(core);

    printf("frames %llu  audio %llu frames @ %u Hz  hash %016llx\n", (unsigned long long)ran,
           (unsigned long long)s.audio_frames, np_core_audio_rate(core), (unsigned long long)hash);
    if (ran > timed_from)
        fprintf(stderr, "[headless] %.3f ms/frame, last frame %ux%u\n", elapsed / (double)(ran - timed_from),
                f.width, f.height);
    if (s.rms_frames)
        printf("audio rms from frame %llu: L %.1f  R %.1f\n", (unsigned long long)rms_from,
               sqrt(s.sumsq[0] / (double)s.rms_frames), sqrt(s.sumsq[1] / (double)s.rms_frames));
    printf("status");
    for (uint32_t i = 0; i < sizeof k_stat_names / sizeof *k_stat_names; i++)
        printf(" %s=%u", k_stat_names[i], np_core_status(core, i));
    printf("\n");
    int status = state_rc != 0 ? 1 : 0;
    if (rc < 0) {
        printf("FAILED at frame %llu: %s\n", (unsigned long long)ran, np_core_last_error(core));
        status = 1;
    } else if (rc > 0) {
        printf("exited at frame %llu: %s\n", (unsigned long long)ran, np_core_last_error(core));
        if (!strstr(np_core_last_error(core), "status 0")) status = 1;
    }
    np_core_destroy(core);
    np_net_close(g_net);
    fclose(r.rom);
    return status;
}
