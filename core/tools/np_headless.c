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
 *     --press F:KEYS     from frame F hold KEYS (hex NP_KEY_* mask), repeatable
 *     --rtc SECONDS      RTC value (seconds since 2000-01-01) instead of the
 *                        port's deterministic clock
 *     -e KEY=VALUE       guest environment entry, repeatable; PC_* variables
 *                        of this process's environment are passed through too
 *
 * Exit status: 0 when the frames ran (or the guest exited with 0), 1 on a
 * guest failure or nonzero exit, 2 on usage or I/O errors.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np_core.h"

#if defined(_WIN32)
#define environ _environ
#endif
extern char **environ;

#define MAX_OPTIONS 128
#define MAX_PRESSES 64

typedef struct runner {
    FILE *rom;
    const char *save_path;
    int64_t rtc;
} runner;

typedef struct press {
    uint64_t frame;
    uint16_t keys;
} press;

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

static int usage(void) {
    fprintf(stderr, "usage: np_headless <diamond|pearl|platinum> <rom.nds> [--frames N] [--save FILE] [--dump DIR]\n"
                    "                   [--dump-every N] [--press F:KEYS]... [--rtc SECONDS] [-e KEY=VALUE]...\n");
    return 2;
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
    uint64_t frames = 600, dump_every = 0;
    const char *dump_dir = NULL;
    int have_rtc = 0;

    for (int i = 3; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) return usage();
        if (strcmp(a, "--frames") == 0) frames = strtoull(v, NULL, 0);
        else if (strcmp(a, "--save") == 0) r.save_path = v;
        else if (strcmp(a, "--dump") == 0) dump_dir = v;
        else if (strcmp(a, "--dump-every") == 0) dump_every = strtoull(v, NULL, 0);
        else if (strcmp(a, "--rtc") == 0) r.rtc = strtoll(v, NULL, 0), have_rtc = 1;
        else if (strcmp(a, "-e") == 0 && noptions < MAX_OPTIONS) options[noptions++] = v;
        else if (strcmp(a, "--press") == 0 && npresses < MAX_PRESSES) {
            char *colon;
            presses[npresses].frame = strtoull(v, &colon, 0);
            if (*colon != ':') return usage();
            presses[npresses++].keys = (uint16_t)strtoul(colon + 1, NULL, 16);
        } else
            return usage();
        i++;
    }
    for (char **e = environ; e && *e && noptions < MAX_OPTIONS; e++)
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

    if (!np_core_available((np_game)game)) {
        fprintf(stderr, "np_headless: %s is not built into this binary\n", names[game]);
        return 2;
    }
    np_core *core = np_core_create((np_game)game, &host, options);
    if (!core) {
        fprintf(stderr, "np_headless: create failed: %s\n", np_core_create_error());
        return 1;
    }

    uint64_t hash = 0xCBF29CE484222325ull, audio_frames = 0;
    static int16_t audio[2 * 8192];
    np_frame f;
    int rc = 0;
    uint64_t ran = 0;
    for (; ran < frames; ran++) {
        np_input in = {0};
        for (int p = 0; p < npresses; p++)
            if (presses[p].frame <= ran) in.keys = presses[p].keys;
        rc = np_core_run_frame(core, &in, &f);
        if (rc != 0) break;
        hash = fnv(hash, f.screen[0], (size_t)f.stride * f.height * 4);
        hash = fnv(hash, f.screen[1], (size_t)f.stride * f.height * 4);
        size_t n;
        while ((n = np_core_audio_read(core, audio, 8192)) > 0) {
            hash = fnv(hash, audio, n * 4);
            audio_frames += n;
        }
        int last = ran + 1 == frames;
        if (dump_dir && (last || (dump_every && ran % dump_every == 0)) && dump_ppm(dump_dir, f.number, &f) != 0)
            fprintf(stderr, "np_headless: cannot write a frame dump into %s\n", dump_dir);
    }
    np_core_save_flush(core);

    printf("frames %llu  audio %llu frames @ %u Hz  hash %016llx\n", (unsigned long long)ran,
           (unsigned long long)audio_frames, np_core_audio_rate(core), (unsigned long long)hash);
    int status = 0;
    if (rc < 0) {
        printf("FAILED at frame %llu: %s\n", (unsigned long long)ran, np_core_last_error(core));
        status = 1;
    } else if (rc > 0) {
        printf("exited at frame %llu: %s\n", (unsigned long long)ran, np_core_last_error(core));
        status = strstr(np_core_last_error(core), "status 0") ? 0 : 1;
    }
    np_core_destroy(core);
    fclose(r.rom);
    return status;
}
