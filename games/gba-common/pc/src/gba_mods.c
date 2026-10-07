/*
 * Runtime content packages ("mods") for the GBA cores. The game's code is
 * compiled, so a package can change only the cartridge's data (text,
 * tables, graphics the game reads from ROM): it is a directory holding
 * mod.toml and <id>.ips, an IPS patch for that game's ROM (ruby.ips,
 * sapphire.ips, emerald.ips), applied to the ROM image at boot before the
 * game starts. One package may carry patches for several of the games.
 *
 * The packages and their order: PC_MODS (space-separated; the shell's
 * custom cart), else loadorder.txt, one name per line, '#' comments, both
 * under the host's content root (/content, read-only). As the DS cores'
 * modfs, a broken package stops the boot with "modfs: ...".
 *
 * IPS: "PATCH", then records of a 24-bit big-endian offset and a 16-bit
 * size with that many bytes, or size 0 with a 16-bit count and one byte
 * (a run), until "EOF", optionally followed by a 24-bit truncation size.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gba_port.h"
#include "np_guest_abi.h"

#define MAX_PKGS 64
#define NAME_MAX_LEN 48

static void die_pkg(const char *pkg, const char *what) {
    gba_fatal("modfs: package '%s': %s", pkg, what);
}

/* Reads exactly n bytes, or reports a truncated patch. */
static void get(FILE *f, uint8_t *b, size_t n, const char *pkg) {
    if (fread(b, 1, n, f) != n) die_pkg(pkg, "the IPS patch is truncated");
}

/* Applies one IPS patch to the ROM image of *size bytes (it may grow it). */
static void apply_ips(FILE *f, const char *pkg, uint8_t *rom, uint32_t *size) {
    uint8_t h[5];
    get(f, h, 5, pkg);
    if (memcmp(h, "PATCH", 5) != 0) die_pkg(pkg, "not an IPS patch (no PATCH header)");
    for (;;) {
        uint8_t r[3];
        get(f, r, 3, pkg);
        if (memcmp(r, "EOF", 3) == 0) break;
        uint32_t off = (uint32_t)r[0] << 16 | (uint32_t)r[1] << 8 | r[2];
        uint8_t s[2];
        get(f, s, 2, pkg);
        uint32_t n = (uint32_t)s[0] << 8 | s[1];
        int run = n == 0;
        uint8_t fill = 0;
        if (run) {
            get(f, s, 2, pkg);
            n = (uint32_t)s[0] << 8 | s[1];
            get(f, &fill, 1, pkg);
        }
        if (off + n > NP_GBA_ROM_MAX) die_pkg(pkg, "the IPS patch writes past the cartridge's 32 MB");
        if (run)
            memset(rom + off, fill, n);
        else
            get(f, rom + off, n, pkg);
        if (off + n > *size) *size = off + n;
    }
    uint8_t t[3];
    if (fread(t, 1, 3, f) == 3) {
        uint32_t keep = (uint32_t)t[0] << 16 | (uint32_t)t[1] << 8 | t[2];
        if (keep < *size) {
            memset(rom + keep, 0, *size - keep); /* as past the end of an unpatched ROM */
            *size = keep;
        }
    }
}

static void apply_package(const char *pkg, uint8_t *rom, uint32_t *size) {
    if (!pkg[0] || pkg[0] == '.' || strchr(pkg, '/') || strlen(pkg) >= NAME_MAX_LEN)
        gba_fatal("modfs: unusable package name '%s'", pkg);
    char path[160];
    snprintf(path, sizeof path, "%s/%s/mod.toml", NP_CONTENT_DIR, pkg);
    FILE *f = fopen(path, "rb");
    if (!f) die_pkg(pkg, "not installed (no mod.toml)");
    fclose(f);
    snprintf(path, sizeof path, "%s/%s/%s.ips", NP_CONTENT_DIR, pkg, gba_game.id);
    f = fopen(path, "rb");
    if (!f) {
        char why[96];
        snprintf(why, sizeof why, "has no %s.ips (no patch for %s)", gba_game.id, gba_game.name);
        die_pkg(pkg, why);
    }
    apply_ips(f, pkg, rom, size);
    fclose(f);
    gba_log("modfs: %s.ips from '%s' applied", gba_game.id, pkg);
}

/* Appends one package name; returns the new count. */
static int add_name(char names[][NAME_MAX_LEN], int n, const char *s, size_t len, const char *where) {
    if (n >= MAX_PKGS) gba_fatal("modfs: too many packages in %s", where);
    if (len >= NAME_MAX_LEN) gba_fatal("modfs: package name too long in %s", where);
    memcpy(names[n], s, len);
    names[n][len] = 0;
    return n + 1;
}

void gba_mods_apply(uint8_t *rom, uint32_t *size) {
    static char names[MAX_PKGS][NAME_MAX_LEN];
    int n = 0;
    const char *env = getenv("PC_MODS");
    if (env) {
        for (const char *p = env; *p;) {
            while (*p == ' ' || *p == ',') p++;
            const char *e = p;
            while (*e && *e != ' ' && *e != ',') e++;
            if (e > p) n = add_name(names, n, p, (size_t)(e - p), "PC_MODS");
            p = e;
        }
    } else {
        FILE *f = fopen(NP_CONTENT_DIR "/loadorder.txt", "r");
        if (!f) return; /* no content root, or no packages enabled */
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *c = strchr(line, '#');
            if (c) *c = 0;
            char *s = line, *e = line + strlen(line);
            while (*s == ' ' || *s == '\t') s++;
            while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
            if (e > s) n = add_name(names, n, s, (size_t)(e - s), "loadorder.txt");
        }
        fclose(f);
    }
    for (int i = 0; i < n; i++) apply_package(names[i], rom, size);
}
