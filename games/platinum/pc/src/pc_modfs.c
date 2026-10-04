/*
 * Runtime content packages.
 *
 * A directory with mod.toml is a package. A directory with only src/ and
 * patches/, or a plugins/ nest, is a compile-time plugin; listing it in
 * PC_MODS is a boot error that says to use MODS=. The two doors do not
 * share a variable because OpenMMO's fused build is MODS=openmmo and must
 * not be treated as a content package.
 *
 * mod.toml is a flat key = value file. This is not a TOML library. The
 * same line grammar is what ids.toml will use (one key = integer per
 * line, keys quoted if they contain ':').
 *
 * Do not shell cook from here, 3DS compiles this file too. A .cooked/
 * tree without a digest file, or whose digest does not match the FNV-1a
 * of content/ + records/ (same number pc/modcook.py writes), is a boot
 * error that names `make -f pc/Makefile cook`.
 *
 * On wasm the packages come from the host's content directory
 * (np_host.content_root), which the runtime serves read-only as the WASI
 * preopen NP_CONTENT_DIR ("/content"); that is the default PC_MODS_DIR
 * there, so a core created without one sees no packages. A boot error
 * becomes a guest trap carrying the message, which the host reads back
 * as np_core_last_error.
 *
 * Diamond/Pearl (PC_GAME_DP) compile this file for the generic half:
 * packages, load order, cooked digests, whole-file and NARC-member claims.
 * D's FS and NARC readers have D's own layouts (NitroSDK 3.2 FSFile,
 * src/filesystem.c), so the FS_* overrides, the NARC probes and the
 * Platinum-only content (billboard people, extra props, cooked map headers)
 * are left out here; games/diamond/pc/game/pc_dp_modfs.c and
 * pc/patches/arm9/src/filesystem.c.patch are D's half, through the
 * pc_modfs_file_load / pc_modfs_probe_* calls at the end of this file.
 */
#include "pc_modfs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/stat.h>
#if defined(__wasm__)
#include "np_guest_abi.h"
#endif

#include <nitro/fs.h>
#if !defined(PC_GAME_DP)
#include <../libraries/fs/include/command.h>

#include "map_header.h"
#include "narc.h"
#endif

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

#define PC_MODFS_MAX_PKGS 32
#define PC_MODFS_MAX_LIST 16
#define PC_MODFS_MAX_STR  128
#define PC_MODFS_PATH     4096

struct pkg {
    char dir[PC_MODFS_MAX_STR];
    char path[PC_MODFS_PATH];
    char id[PC_MODFS_MAX_STR];
    char name[PC_MODFS_MAX_STR];
    char version[PC_MODFS_MAX_STR];
    char requires[PC_MODFS_MAX_LIST][PC_MODFS_MAX_STR];
    int n_requires;
    char load_after[PC_MODFS_MAX_LIST][PC_MODFS_MAX_STR];
    int n_load_after;
};

static struct pkg sPkgs[PC_MODFS_MAX_PKGS];
static int sNPkgs;

struct claim {
    char *nitro;
    char *host;
    char *who;
};

static struct claim *sFiles;
static int sNFiles;
static int sCapFiles;
static char sHostRet[PC_MODFS_PATH];

struct mclaim {
    char *nitro;
    unsigned index;
    char *host;
    char *who;
};

static struct mclaim *sMembers;
static int sNMembers;
static int sCapMembers;

#if !defined(PC_GAME_DP)
#define PC_MODFS_MAX_GFX 64
#define PC_MODFS_MAX_PROPS 64
#define PC_MODFS_MAX_MAPS 16

struct gfx_claim {
    int gfx;
    int nsbtx;
};

static struct gfx_claim sGfx[PC_MODFS_MAX_GFX];
static int sNGfx;

static int sExtraProps[PC_MODFS_MAX_PROPS];
static int sNExtraProps;

struct map_claim {
    int id;
    struct pc_modfs_map_header h;
};

static struct map_claim sMaps[PC_MODFS_MAX_MAPS];
static int sNMaps;
#endif

struct narc_bind {
    const void *narc;
    char *path;
    int seq_member;
    unsigned seq_pos;
};

static struct narc_bind *sBinds;
static int sNBinds;
static int sCapBinds;

#if !defined(PC_GAME_DP)
extern int NARC_FindID(const char *path);
static void maybe_probe_narc(void);
static void maybe_probe_map(void);
#endif
static void *slurp_host(const char *host, u32 *out_size);

static void die(const char *fmt, ...)
{
    va_list ap;
    char msg[512];
    int n;

    va_start(ap, fmt);
    n = snprintf(msg, sizeof msg, "modfs: ");
    vsnprintf(msg + n, sizeof msg - (size_t)n, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", msg);
#if defined(__wasm__)
    np_host_trap(msg, (uint32_t)strlen(msg));
#else
    exit(2);
#endif
}

static int is_dir(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

static int is_file(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISREG(st.st_mode);
}

static void join_path(char *dst, size_t cap, const char *a, const char *b)
{
    int n = snprintf(dst, cap, "%s/%s", a, b);

    if (n < 0 || (size_t)n >= cap) {
        die("path too long: %s/%s", a, b);
    }
}

static void trim(char *s)
{
    char *a = s;
    char *b;

    while (*a == ' ' || *a == '\t') {
        a++;
    }
    if (a != s) {
        memmove(s, a, strlen(a) + 1);
    }
    b = s + strlen(s);
    while (b > s && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r' || b[-1] == '\n')) {
        *--b = '\0';
    }
}

static void strip_comment(char *s)
{
    int in_quote = 0;

    for (; *s; s++) {
        if (*s == '"') {
            in_quote = !in_quote;
        } else if (*s == '#' && !in_quote) {
            *s = '\0';
            return;
        }
    }
}

static int id_ok(const char *s)
{
    if (*s == '\0') {
        return 0;
    }
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) {
            return 0;
        }
    }
    return 1;
}

static int split_tokens(const char *s, char *out, int max, size_t width,
                        const char *overflow_what)
{
    int n = 0;
    const char *p = s;

    while (*p) {
        int i = 0;
        char *dst;

        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (n >= max) {
            die("too many %s", overflow_what);
        }
        dst = out + (size_t)n * width;
        while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '\r' && *p != '\n') {
            if ((size_t)i + 1 >= width) {
                die("%s too long", overflow_what);
            }
            dst[i++] = *p++;
        }
        dst[i] = '\0';
        n++;
    }
    return n;
}

static int split_names(const char *s, char out[][PC_MODFS_MAX_STR], int max)
{
    return split_tokens(s, (char *)out, max, PC_MODFS_MAX_STR, "packages in PC_MODS");
}

static int read_loadorder(const char *path, char out[][PC_MODFS_MAX_STR], int max)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    int n = 0;

    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);

        if (len == sizeof line - 1 && line[len - 1] != '\n') {
            fclose(f);
            die("line too long in %s", path);
        }
        strip_comment(line);
        trim(line);
        if (line[0] == '\0') {
            continue;
        }
        if (n >= max) {
            fclose(f);
            die("too many packages in %s", path);
        }
        if (strlen(line) >= PC_MODFS_MAX_STR) {
            fclose(f);
            die("package name too long in %s", path);
        }
        memcpy(out[n], line, strlen(line) + 1);
        n++;
    }
    fclose(f);
    return n;
}

static void copy_string_val(char *dst, size_t cap, const char *val, const char *file)
{
    const char *p = val;
    size_t n;

    if (p[0] == '[') {
        die("expected a string, not a list, in %s", file);
    }
    if (p[0] == '"') {
        const char *q;

        p++;
        q = strchr(p, '"');
        if (q == NULL) {
            die("unterminated string in %s", file);
        }
        n = (size_t)(q - p);
        q++;
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (*q != '\0') {
            die("expected a string, not a list, in %s", file);
        }
    } else {
        n = strlen(p);
    }
    if (n >= cap) {
        die("value too long in %s", file);
    }
    memcpy(dst, p, n);
    dst[n] = '\0';
}

static int parse_list(const char *val, char out[][PC_MODFS_MAX_STR], int max,
                      const char *file)
{
    char buf[1024];
    const char *p;
    int n = 0;
    size_t len = strlen(val);

    if (len >= sizeof buf) {
        die("line too long in %s", file);
    }
    memcpy(buf, val, len + 1);
    p = buf;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '[') {
        char *end;

        p++;
        end = buf + strlen(buf);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        if (end == p || end[-1] != ']') {
            die("unclosed list in %s", file);
        }
        end[-1] = '\0';
    }

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return 0;
    }

    for (;;) {
        char item[PC_MODFS_MAX_STR];
        int i = 0;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || *p == ',') {
            die("empty list item in %s", file);
        }
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (i + 1 >= PC_MODFS_MAX_STR) {
                    die("value too long in %s", file);
                }
                item[i++] = *p++;
            }
            if (*p != '"') {
                die("unterminated string in %s", file);
            }
            p++;
        } else {
            while (*p && *p != ',' && *p != ' ' && *p != '\t') {
                if (i + 1 >= PC_MODFS_MAX_STR) {
                    die("value too long in %s", file);
                }
                item[i++] = *p++;
            }
        }
        item[i] = '\0';
        if (n >= max) {
            die("too many list items in %s", file);
        }
        memcpy(out[n], item, (size_t)i + 1);
        n++;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            return n;
        }
        if (*p != ',') {
            die("unexpected text after list item in %s", file);
        }
        p++;
    }
}

static void read_mod_toml(struct pkg *pk)
{
    char file[PC_MODFS_PATH];
    FILE *fp;
    char line[1024];
    int seen_id = 0, seen_name = 0, seen_version = 0;
    int seen_authors = 0, seen_requires = 0, seen_after = 0;

    join_path(file, sizeof file, pk->path, "mod.toml");
    fp = fopen(file, "r");
    if (fp == NULL) {
        die("cannot read %s", file);
    }
    pk->n_requires = 0;
    pk->n_load_after = 0;
    pk->id[0] = pk->name[0] = pk->version[0] = '\0';

    while (fgets(line, sizeof line, fp)) {
        char *eq;
        char *key;
        char *val;
        size_t len = strlen(line);

        if (len == sizeof line - 1 && line[len - 1] != '\n') {
            fclose(fp);
            die("line too long in %s", file);
        }
        strip_comment(line);
        trim(line);
        if (line[0] == '\0') {
            continue;
        }

        if (line[0] == '[') {
            char *rb = strchr(line, ']');
            int comma = 0;
            char *p;

            if (rb != NULL) {
                for (p = line + 1; p < rb; p++) {
                    if (*p == ',') {
                        comma = 1;
                        break;
                    }
                }
                if (!comma) {
                    fclose(fp);
                    die("tables are not read; put the package fields at the top (%s)",
                        file);
                }
            }
        }

        eq = strchr(line, '=');
        if (eq == NULL) {
            fclose(fp);
            die("expected key = value in %s", file);
        }
        *eq = '\0';
        key = line;
        val = eq + 1;
        trim(key);
        trim(val);
        if (key[0] == '"') {
            size_t klen = strlen(key);

            if (klen < 2 || key[klen - 1] != '"') {
                fclose(fp);
                die("unterminated key in %s", file);
            }
            key[klen - 1] = '\0';
            key++;
        }
        if (key[0] == '\0') {
            fclose(fp);
            die("empty key in %s", file);
        }

        if (strcmp(key, "id") == 0) {
            if (seen_id) {
                fclose(fp);
                die("duplicate key 'id' in %s", file);
            }
            seen_id = 1;
            copy_string_val(pk->id, sizeof pk->id, val, file);
        } else if (strcmp(key, "name") == 0) {
            if (seen_name) {
                fclose(fp);
                die("duplicate key 'name' in %s", file);
            }
            seen_name = 1;
            copy_string_val(pk->name, sizeof pk->name, val, file);
        } else if (strcmp(key, "version") == 0) {
            if (seen_version) {
                fclose(fp);
                die("duplicate key 'version' in %s", file);
            }
            seen_version = 1;
            copy_string_val(pk->version, sizeof pk->version, val, file);
        } else if (strcmp(key, "authors") == 0) {
            if (seen_authors) {
                fclose(fp);
                die("duplicate key 'authors' in %s", file);
            }
            seen_authors = 1;
            /* Parsed and discarded: authors is metadata, not load-order. */
            {
                char discard[PC_MODFS_MAX_LIST][PC_MODFS_MAX_STR];

                parse_list(val, discard, PC_MODFS_MAX_LIST, file);
            }
        } else if (strcmp(key, "requires") == 0) {
            if (seen_requires) {
                fclose(fp);
                die("duplicate key 'requires' in %s", file);
            }
            seen_requires = 1;
            pk->n_requires = parse_list(val, pk->requires, PC_MODFS_MAX_LIST, file);
        } else if (strcmp(key, "load_after") == 0) {
            if (seen_after) {
                fclose(fp);
                die("duplicate key 'load_after' in %s", file);
            }
            seen_after = 1;
            pk->n_load_after = parse_list(val, pk->load_after, PC_MODFS_MAX_LIST, file);
        } else {
            fclose(fp);
            die("unknown key '%s' in %s", key, file);
        }
    }
    fclose(fp);

    if (!seen_id) {
        die("missing 'id' in %s", file);
    }
    if (!seen_name) {
        die("missing 'name' in %s", file);
    }
    if (!seen_version) {
        die("missing 'version' in %s", file);
    }
    if (!id_ok(pk->id)) {
        die("id '%s' is not [a-z0-9_]+ in %s", pk->id, file);
    }
}

static int is_compile_plugin(const char *pkgpath)
{
    char a[PC_MODFS_PATH];
    char b[PC_MODFS_PATH];

    join_path(a, sizeof a, pkgpath, "src");
    join_path(b, sizeof b, pkgpath, "patches");
    if (is_dir(a) && is_dir(b)) {
        return 1;
    }
    join_path(a, sizeof a, pkgpath, "plugins");
    return is_dir(a);
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);

    if (p == NULL) {
        die("out of memory");
    }
    memcpy(p, s, n);
    return p;
}

static void norm_nitro(char *dst, size_t cap, const char *src)
{
    const char *p = src;
    size_t i = 0;

    if (p[0] == 'r' && p[1] == 'o' && p[2] == 'm' && p[3] == ':') {
        p += 4;
    }
    while (*p == '/' || *p == '\\') {
        p++;
    }
    for (; *p; p++) {
        char c = (*p == '\\') ? '/' : *p;

        if (i + 1 >= cap) {
            die("path too long: %s", src);
        }
        dst[i++] = c;
    }
    dst[i] = '\0';
}

static void claim_file(const char *nitro, const char *host, const char *who)
{
    int i;

    for (i = 0; i < sNFiles; i++) {
        if (strcmp(sFiles[i].nitro, nitro) == 0) {
            if (strcmp(sFiles[i].who, who) != 0) {
                fprintf(stderr,
                        "modfs: '%s' and '%s' both replace %s; later wins, '%s' loses\n",
                        sFiles[i].who, who, nitro, sFiles[i].who);
            }
            free(sFiles[i].host);
            free(sFiles[i].who);
            sFiles[i].host = xstrdup(host);
            sFiles[i].who = xstrdup(who);
            return;
        }
    }
    if (sNFiles >= sCapFiles) {
        int cap = sCapFiles ? sCapFiles * 2 : 16;
        struct claim *n = (struct claim *)realloc(sFiles, (size_t)cap * sizeof *n);

        if (n == NULL) {
            die("out of memory");
        }
        sFiles = n;
        sCapFiles = cap;
    }
    sFiles[sNFiles].nitro = xstrdup(nitro);
    sFiles[sNFiles].host = xstrdup(host);
    sFiles[sNFiles].who = xstrdup(who);
    sNFiles++;
}

static int parse_idx(const char *s, unsigned *out)
{
    unsigned v = 0;
    const char *p = s;

    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    for (; *p; p++) {
        unsigned n;

        if (*p < '0' || *p > '9') {
            return 0;
        }
        n = v * 10u + (unsigned)(*p - '0');
        if (n < v) {
            return 0;
        }
        v = n;
    }
    *out = v;
    return 1;
}

static void claim_member(const char *nitro, unsigned index, const char *host,
                         const char *who)
{
    int i;

    if (index >= 65535u) {
        die("append past 65535: %s/%u", nitro, index);
    }
    for (i = 0; i < sNMembers; i++) {
        if (sMembers[i].index == index && strcmp(sMembers[i].nitro, nitro) == 0) {
            if (strcmp(sMembers[i].who, who) != 0) {
                fprintf(stderr,
                        "modfs: '%s' and '%s' both replace %s/%u; later wins, '%s' loses\n",
                        sMembers[i].who, who, nitro, index, sMembers[i].who);
            }
            free(sMembers[i].host);
            free(sMembers[i].who);
            sMembers[i].host = xstrdup(host);
            sMembers[i].who = xstrdup(who);
            return;
        }
    }
    if (sNMembers >= sCapMembers) {
        int cap = sCapMembers ? sCapMembers * 2 : 16;
        struct mclaim *n = (struct mclaim *)realloc(sMembers, (size_t)cap * sizeof *n);

        if (n == NULL) {
            die("out of memory");
        }
        sMembers = n;
        sCapMembers = cap;
    }
    sMembers[sNMembers].nitro = xstrdup(nitro);
    sMembers[sNMembers].index = index;
    sMembers[sNMembers].host = xstrdup(host);
    sMembers[sNMembers].who = xstrdup(who);
    sNMembers++;
}

static void walk_narc_tree(const char *dir, const char *rel, const char *who)
{
    DIR *d = opendir(dir);
    struct dirent *ent;

    if (d == NULL) {
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        char child[PC_MODFS_PATH];
        char childrel[PC_MODFS_PATH];
        int n;

        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
            continue;
        }
        join_path(child, sizeof child, dir, ent->d_name);
        if (rel[0] == '\0') {
            n = snprintf(childrel, sizeof childrel, "%s", ent->d_name);
        } else {
            n = snprintf(childrel, sizeof childrel, "%s/%s", rel, ent->d_name);
        }
        if (n < 0 || (size_t)n >= sizeof childrel) {
            closedir(d);
            die("path too long: %s/%s", rel, ent->d_name);
        }
        if (is_file(child)) {
            const char *slash = strrchr(childrel, '/');
            const char *leaf = slash ? slash + 1 : childrel;
            unsigned idx;

            if (!parse_idx(leaf, &idx)) {
                closedir(d);
                die("member name is not an index: %s", childrel);
            }
            if (slash == NULL) {
                closedir(d);
                die("member path missing the narc: %s", childrel);
            }
            {
                size_t nlen = (size_t)(slash - childrel);
                char nitro[PC_MODFS_PATH];

                if (nlen == 0 || nlen >= sizeof nitro) {
                    closedir(d);
                    die("path too long: %s", childrel);
                }
                memcpy(nitro, childrel, nlen);
                nitro[nlen] = '\0';
                claim_member(nitro, idx, child, who);
            }
        } else if (is_dir(child)) {
            walk_narc_tree(child, childrel, who);
        } else {
            closedir(d);
            die("claimed file missing: %s", childrel);
        }
    }
    closedir(d);
}

static void walk_fs_tree(const char *dir, const char *rel, const char *who)
{
    DIR *d = opendir(dir);
    struct dirent *ent;

    if (d == NULL) {
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        char child[PC_MODFS_PATH];
        char childrel[PC_MODFS_PATH];
        int n;

        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
            continue;
        }
        join_path(child, sizeof child, dir, ent->d_name);
        if (rel[0] == '\0') {
            n = snprintf(childrel, sizeof childrel, "%s", ent->d_name);
        } else {
            n = snprintf(childrel, sizeof childrel, "%s/%s", rel, ent->d_name);
        }
        if (n < 0 || (size_t)n >= sizeof childrel) {
            closedir(d);
            die("path too long: %s/%s", rel, ent->d_name);
        }
        if (is_file(child)) {
            claim_file(childrel, child, who);
        } else if (is_dir(child)) {
            walk_fs_tree(child, childrel, who);
        } else {
            closedir(d);
            die("claimed file missing: %s", childrel);
        }
    }
    closedir(d);
}

static void scan_pkg_files(const struct pkg *pk)
{
    char p[PC_MODFS_PATH];

    join_path(p, sizeof p, pk->path, "replace");
    walk_fs_tree(p, "", pk->dir);
    join_path(p, sizeof p, pk->path, ".cooked/fs");
    walk_fs_tree(p, "", pk->dir);
    join_path(p, sizeof p, pk->path, "narc");
    walk_narc_tree(p, "", pk->dir);
    join_path(p, sizeof p, pk->path, ".cooked/narc");
    walk_narc_tree(p, "", pk->dir);
}

#if !defined(PC_GAME_DP)
static void load_billboard_gfx(const struct pkg *pk)
{
    char path[PC_MODFS_PATH];
    FILE *f;
    char line[128];

    join_path(path, sizeof path, pk->path, ".cooked/generated/billboard_gfx.txt");
    if (!is_file(path)) {
        return;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        die("cannot read %s", path);
    }
    while (fgets(line, sizeof line, f) != NULL) {
        char *hash = strchr(line, '#');
        int gfx, nsbtx;
        int i;

        if (hash != NULL) {
            *hash = '\0';
        }
        if (sscanf(line, "%d %d", &gfx, &nsbtx) != 2) {
            continue;
        }
        if (gfx < 0 || nsbtx < 0 || nsbtx >= 65535) {
            fclose(f);
            die("bad billboard row in %s: %d %d", pk->dir, gfx, nsbtx);
        }
        for (i = 0; i < sNGfx; i++) {
            if (sGfx[i].gfx == gfx) {
                sGfx[i].nsbtx = nsbtx;
                break;
            }
        }
        if (i == sNGfx) {
            if (sNGfx >= PC_MODFS_MAX_GFX) {
                fclose(f);
                die("too many cooked billboard people (max %d)",
                    PC_MODFS_MAX_GFX);
            }
            sGfx[sNGfx].gfx = gfx;
            sGfx[sNGfx].nsbtx = nsbtx;
            sNGfx++;
        }
    }
    fclose(f);
}

static void load_extra_props(const struct pkg *pk)
{
    char path[PC_MODFS_PATH];
    FILE *f;
    char line[128];

    join_path(path, sizeof path, pk->path, ".cooked/generated/extra_props.txt");
    if (!is_file(path)) {
        return;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        die("cannot read %s", path);
    }
    while (fgets(line, sizeof line, f) != NULL) {
        char *hash = strchr(line, '#');
        int id;
        int i;

        if (hash != NULL) {
            *hash = '\0';
        }
        if (sscanf(line, "%d", &id) != 1) {
            continue;
        }
        if (id < 0 || id >= 768) {
            fclose(f);
            die("bad extra prop in %s: %d", pk->dir, id);
        }
        for (i = 0; i < sNExtraProps; i++) {
            if (sExtraProps[i] == id) {
                break;
            }
        }
        if (i == sNExtraProps) {
            if (sNExtraProps >= PC_MODFS_MAX_PROPS) {
                fclose(f);
                die("too many cooked props (max %d)", PC_MODFS_MAX_PROPS);
            }
            sExtraProps[sNExtraProps++] = id;
        }
    }
    fclose(f);
}

static void load_cooked_maps(const struct pkg *pk)
{
    char path[PC_MODFS_PATH];
    FILE *f;
    char line[256];

    join_path(path, sizeof path, pk->path, ".cooked/generated/cooked_maps.txt");
    if (!is_file(path)) {
        return;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        die("cannot read %s", path);
    }
    while (fgets(line, sizeof line, f) != NULL) {
        char *hash = strchr(line, '#');
        struct map_claim row;
        int n;
        int i;

        if (hash != NULL) {
            *hash = '\0';
        }
        memset(&row, 0, sizeof row);
        n = sscanf(line,
                   "%d %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u "
                   "%u %u %u %u",
                   &row.id,
                   &row.h.area, &row.h.preloaded, &row.h.matrix,
                   &row.h.scripts, &row.h.init_scripts, &row.h.msg,
                   &row.h.day, &row.h.night, &row.h.wild, &row.h.events,
                   &row.h.label, &row.h.window, &row.h.weather,
                   &row.h.camera, &row.h.map_type, &row.h.battle_bg,
                   &row.h.bike, &row.h.run, &row.h.escape, &row.h.fly);
        if (n != 21) {
            continue;
        }
        if (row.id < 0) {
            fclose(f);
            die("bad cooked map in %s: %d", pk->dir, row.id);
        }
        for (i = 0; i < sNMaps; i++) {
            if (sMaps[i].id == row.id) {
                sMaps[i] = row;
                break;
            }
        }
        if (i == sNMaps) {
            if (sNMaps >= PC_MODFS_MAX_MAPS) {
                fclose(f);
                die("too many cooked maps (max %d)", PC_MODFS_MAX_MAPS);
            }
            sMaps[sNMaps++] = row;
        }
    }
    fclose(f);
}
#endif

static void scan_modfs_root(const char *root)
{
    char p[PC_MODFS_PATH];

    if (!is_dir(root)) {
        die("PC_MODFS root is not a directory: %s", root);
    }
    join_path(p, sizeof p, root, "fs");
    walk_fs_tree(p, "", root);
    join_path(p, sizeof p, root, "narc");
    walk_narc_tree(p, "", root);
}

/*
 * Same FNV-1a-64 pc/modcook.py writes. Host-side, not the SDK MD5: this
 * file is compiled for 3DS too and must not pull pc_state.c. Collision
 * cost is a stale tree that boots, not a wrong answer shipped.
 */
#define COOK_FNV_OFFSET 0xcbf29ce484222325ULL
#define COOK_FNV_PRIME  0x100000001b3ULL

struct cook_file {
    char *rel;
    char *host;
};

static uint64_t cook_fnv(uint64_t h, const void *buf, size_t n)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= COOK_FNV_PRIME;
    }
    return h;
}

static int cook_file_cmp(const void *a, const void *b)
{
    const struct cook_file *fa = (const struct cook_file *)a;
    const struct cook_file *fb = (const struct cook_file *)b;

    return strcmp(fa->rel, fb->rel);
}

static void collect_cook_inputs(const char *dir, const char *rel,
                                struct cook_file **list, int *n, int *cap)
{
    DIR *d = opendir(dir);
    struct dirent *ent;

    if (d == NULL) {
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        char child[PC_MODFS_PATH];
        char childrel[PC_MODFS_PATH];
        int written;

        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
            continue;
        }
        join_path(child, sizeof child, dir, ent->d_name);
        if (rel[0] == '\0') {
            written = snprintf(childrel, sizeof childrel, "%s", ent->d_name);
        } else {
            written = snprintf(childrel, sizeof childrel, "%s/%s", rel, ent->d_name);
        }
        if (written < 0 || (size_t)written >= sizeof childrel) {
            closedir(d);
            die("path too long: %s/%s", rel, ent->d_name);
        }
        if (is_file(child)) {
            if (*n >= *cap) {
                int next = *cap ? *cap * 2 : 16;
                struct cook_file *grown = (struct cook_file *)realloc(
                    *list, (size_t)next * sizeof *grown);

                if (grown == NULL) {
                    closedir(d);
                    die("out of memory");
                }
                *list = grown;
                *cap = next;
            }
            (*list)[*n].rel = xstrdup(childrel);
            (*list)[*n].host = xstrdup(child);
            (*n)++;
        } else if (is_dir(child)) {
            collect_cook_inputs(child, childrel, list, n, cap);
        } else {
            closedir(d);
            die("claimed file missing: %s", childrel);
        }
    }
    closedir(d);
}

static uint64_t hash_cook_inputs(const struct pkg *pk)
{
    struct cook_file *list = NULL;
    int n = 0;
    int cap = 0;
    int i;
    uint64_t h = COOK_FNV_OFFSET;
    char root[PC_MODFS_PATH];

    join_path(root, sizeof root, pk->path, "content");
    collect_cook_inputs(root, "content", &list, &n, &cap);
    join_path(root, sizeof root, pk->path, "records");
    collect_cook_inputs(root, "records", &list, &n, &cap);
    if (n > 1) {
        qsort(list, (size_t)n, sizeof *list, cook_file_cmp);
    }
    for (i = 0; i < n; i++) {
        FILE *f;
        char buf[4096];
        size_t got;

        h = cook_fnv(h, list[i].rel, strlen(list[i].rel));
        h = cook_fnv(h, "", 1);
        f = fopen(list[i].host, "rb");
        if (f == NULL) {
            die("claimed file missing: %s", list[i].rel);
        }
        while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
            h = cook_fnv(h, buf, got);
        }
        fclose(f);
        h = cook_fnv(h, "", 1);
        free(list[i].rel);
        free(list[i].host);
    }
    free(list);
    return h;
}

static int digest_matches(const char *path, uint64_t want)
{
    FILE *f;
    char line[64];
    size_t len;
    unsigned long long got;
    int i;

    f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    if (fgets(line, sizeof line, f) == NULL) {
        fclose(f);
        return 0;
    }
    fclose(f);
    len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
        line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r') {
            line[--len] = '\0';
        }
    }
    if (len != 3 + 16 || line[0] != 'v' || line[1] != '1' || line[2] != ' ') {
        return 0;
    }
    for (i = 0; i < 16; i++) {
        char c = line[3 + i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    got = 0;
    for (i = 0; i < 16; i++) {
        char c = line[3 + i];
        unsigned nibble = (c <= '9') ? (unsigned)(c - '0')
                                     : (unsigned)(c - 'a' + 10);

        got = (got << 4) | nibble;
    }
    return got == (unsigned long long)want;
}

static void check_cooked(const struct pkg *pk)
{
    char p[PC_MODFS_PATH];
    char digest[PC_MODFS_PATH];

    join_path(p, sizeof p, pk->path, ".cooked");
    if (!is_dir(p)) {
        return;
    }
    join_path(digest, sizeof digest, p, "digest");
    if (!is_file(digest) || !digest_matches(digest, hash_cook_inputs(pk))) {
#if defined(PC_GAME_DP)
        die("stale .cooked/ in '%s'; run python3 pc/modcook.py --mods %s"
            " in games/diamond", pk->dir, pk->dir);
#else
        die("stale .cooked/ in '%s'; run make -f pc/Makefile cook", pk->dir);
#endif
    }
}

static int find_by_id(const char *id)
{
    int i;

    for (i = 0; i < sNPkgs; i++) {
        if (strcmp(sPkgs[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

void pc_modfs_boot(void)
{
    const char *mods_dir;
    const char *mods;
    const char *modfs;
    char names[PC_MODFS_MAX_PKGS][PC_MODFS_MAX_STR];
    char roots[PC_MODFS_MAX_PKGS][PC_MODFS_PATH];
    int n = 0;
    int nroots = 0;
    int i, j;
    char lo_path[PC_MODFS_PATH];

    sNPkgs = 0;
    sNFiles = 0;
    sNMembers = 0;
#if !defined(PC_GAME_DP)
    sNGfx = 0;
#endif

    mods_dir = getenv("PC_MODS_DIR");
    if (mods_dir == NULL || mods_dir[0] == '\0') {
#if defined(__wasm__)
        mods_dir = NP_CONTENT_DIR;
#else
        mods_dir = "pc/mods";
#endif
    }
    mods = getenv("PC_MODS");
    modfs = getenv("PC_MODFS");
    if (modfs != NULL && modfs[0] != '\0') {
        nroots = split_tokens(modfs, (char *)roots, PC_MODFS_MAX_PKGS,
                              PC_MODFS_PATH, "PC_MODFS roots");
    }

    if (mods != NULL) {
        n = split_names(mods, names, PC_MODFS_MAX_PKGS);
    } else {
        join_path(lo_path, sizeof lo_path, mods_dir, "loadorder.txt");
        n = read_loadorder(lo_path, names, PC_MODFS_MAX_PKGS);
        if (n < 0) {
            n = 0;
        }
    }

    if (n == 0 && nroots == 0) {
#if !defined(PC_GAME_DP)
        maybe_probe_narc();
#endif
        return;
    }

    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (strcmp(names[i], names[j]) == 0) {
                die("package '%s' listed twice", names[i]);
            }
        }
    }

    for (i = 0; i < n; i++) {
        struct pkg *pk = &sPkgs[sNPkgs];
        char toml[PC_MODFS_PATH];

        if (strlen(names[i]) >= sizeof pk->dir) {
            die("package name too long");
        }
        memcpy(pk->dir, names[i], strlen(names[i]) + 1);
        join_path(pk->path, sizeof pk->path, mods_dir, pk->dir);

        if (!is_dir(pk->path)) {
            die("unknown package '%s' (looked in %s)", pk->dir, mods_dir);
        }
        join_path(toml, sizeof toml, pk->path, "mod.toml");
        if (!is_file(toml)) {
            if (is_compile_plugin(pk->path)) {
                die("'%s' is a compile-time plugin; use MODS=, not PC_MODS",
                    pk->dir);
            }
            die("unknown package '%s' (looked in %s)", pk->dir, mods_dir);
        }
        read_mod_toml(pk);
        check_cooked(pk);
        sNPkgs++;
    }

    for (i = 0; i < sNPkgs; i++) {
        for (j = i + 1; j < sNPkgs; j++) {
            if (strcmp(sPkgs[i].id, sPkgs[j].id) == 0) {
                die("packages '%s' and '%s' share id '%s'",
                    sPkgs[i].dir, sPkgs[j].dir, sPkgs[i].id);
            }
        }
    }

    for (i = 0; i < sNPkgs; i++) {
        for (j = 0; j < sPkgs[i].n_requires; j++) {
            if (find_by_id(sPkgs[i].requires[j]) < 0) {
                die("'%s' requires '%s', which is not enabled",
                    sPkgs[i].id, sPkgs[i].requires[j]);
            }
        }
        for (j = 0; j < sPkgs[i].n_load_after; j++) {
            int k = find_by_id(sPkgs[i].load_after[j]);

            if (k < 0) {
                die("'%s' must load after '%s', which is not enabled",
                    sPkgs[i].id, sPkgs[i].load_after[j]);
            }
            if (k >= i) {
                die("'%s' must load after '%s', but '%s' is later",
                    sPkgs[i].id, sPkgs[i].load_after[j], sPkgs[k].id);
            }
        }
    }

    for (i = 0; i < sNPkgs; i++) {
        scan_pkg_files(&sPkgs[i]);
#if !defined(PC_GAME_DP)
        load_billboard_gfx(&sPkgs[i]);
        load_extra_props(&sPkgs[i]);
        load_cooked_maps(&sPkgs[i]);
#endif
    }
    for (i = 0; i < nroots; i++) {
        scan_modfs_root(roots[i]);
    }

    fprintf(stderr, "modfs: %d files, %d members, order=[", sNFiles, sNMembers);
    for (i = 0; i < sNPkgs; i++) {
        if (i > 0) {
            fputs(", ", stderr);
        }
        fputs(sPkgs[i].dir, stderr);
    }
    fputs("]\n", stderr);
#if !defined(PC_GAME_DP)
    maybe_probe_map();
    maybe_probe_narc();
#endif
}

const char *pc_modfs_host_file(const char *nitro_path)
{
    char key[PC_MODFS_PATH];
    int i;

    if (nitro_path == NULL || nitro_path[0] == '\0') {
        return NULL;
    }
    norm_nitro(key, sizeof key, nitro_path);
    for (i = 0; i < sNFiles; i++) {
        if (strcmp(sFiles[i].nitro, key) == 0) {
            if (!is_file(sFiles[i].host)) {
                die("claimed file missing: %s", key);
            }
            if (strlen(sFiles[i].host) >= sizeof sHostRet) {
                die("path too long: %s", sFiles[i].host);
            }
            memcpy(sHostRet, sFiles[i].host, strlen(sFiles[i].host) + 1);
            return sHostRet;
        }
    }
    return NULL;
}

const char *pc_modfs_host_member(const char *nitro_path, unsigned index)
{
    char key[PC_MODFS_PATH];
    int i;

    if (nitro_path == NULL || nitro_path[0] == '\0') {
        return NULL;
    }
    norm_nitro(key, sizeof key, nitro_path);
    for (i = 0; i < sNMembers; i++) {
        if (sMembers[i].index == index && strcmp(sMembers[i].nitro, key) == 0) {
            if (!is_file(sMembers[i].host)) {
                die("claimed file missing: %s/%u", key, index);
            }
            if (strlen(sMembers[i].host) >= sizeof sHostRet) {
                die("path too long: %s", sMembers[i].host);
            }
            memcpy(sHostRet, sMembers[i].host, strlen(sMembers[i].host) + 1);
            return sHostRet;
        }
    }
    return NULL;
}

int pc_modfs_member_stat(const char *nitro_path, unsigned index, unsigned *out_size)
{
    const char *host;
    struct stat st;

    host = pc_modfs_host_member(nitro_path, index);
    if (host == NULL) {
        return 0;
    }
    if (stat(host, &st) != 0 || !S_ISREG(st.st_mode)
        || st.st_size < 0 || st.st_size > 0x7fffffffL) {
        die("claimed file missing: %s/%u", nitro_path, index);
    }
    if (out_size != NULL) {
        *out_size = (unsigned)st.st_size;
    }
    return 1;
}

int pc_modfs_member_read(const char *nitro_path, unsigned index,
                         void *dest, unsigned offset, unsigned bytesToRead)
{
    const char *host;
    u32 size = 0;
    void *buf;
    unsigned n;

    host = pc_modfs_host_member(nitro_path, index);
    if (host == NULL) {
        return 0;
    }
    buf = slurp_host(host, &size);
    if (buf == NULL) {
        die("claimed file missing: %s/%u", nitro_path, index);
    }
    if (offset > size) {
        n = 0;
    } else if (bytesToRead == 0) {
        n = size - offset;
    } else if (offset + bytesToRead > size) {
        n = size - offset;
    } else {
        n = bytesToRead;
    }
    if (n != 0 && dest != NULL) {
        memcpy(dest, (u8 *)buf + offset, n);
    }
    free(buf);
    return 1;
}

void pc_modfs_bind_narc(const void *narc, const char *nitro_path)
{
    char key[PC_MODFS_PATH];
    int i;

    if (narc == NULL || nitro_path == NULL || nitro_path[0] == '\0') {
        return;
    }
    norm_nitro(key, sizeof key, nitro_path);
    for (i = 0; i < sNBinds; i++) {
        if (sBinds[i].narc == narc) {
            free(sBinds[i].path);
            sBinds[i].path = xstrdup(key);
            sBinds[i].seq_member = -1;
            sBinds[i].seq_pos = 0;
            return;
        }
    }
    if (sNBinds >= sCapBinds) {
        int cap = sCapBinds ? sCapBinds * 2 : 16;
        struct narc_bind *n = (struct narc_bind *)realloc(sBinds, (size_t)cap * sizeof *n);

        if (n == NULL) {
            die("out of memory");
        }
        sBinds = n;
        sCapBinds = cap;
    }
    sBinds[sNBinds].narc = narc;
    sBinds[sNBinds].path = xstrdup(key);
    sBinds[sNBinds].seq_member = -1;
    sBinds[sNBinds].seq_pos = 0;
    sNBinds++;
}

void pc_modfs_unbind_narc(const void *narc)
{
    int i;

    if (narc == NULL) {
        return;
    }
    for (i = 0; i < sNBinds; i++) {
        if (sBinds[i].narc == narc) {
            free(sBinds[i].path);
            sBinds[i] = sBinds[sNBinds - 1];
            sNBinds--;
            return;
        }
    }
}

const char *pc_modfs_narc_path(const void *narc)
{
    int i;

    if (narc == NULL) {
        return NULL;
    }
    for (i = 0; i < sNBinds; i++) {
        if (sBinds[i].narc == narc) {
            return sBinds[i].path;
        }
    }
    return NULL;
}

static struct narc_bind *find_bind(const void *narc)
{
    int i;

    if (narc == NULL) {
        return NULL;
    }
    for (i = 0; i < sNBinds; i++) {
        if (sBinds[i].narc == narc) {
            return &sBinds[i];
        }
    }
    return NULL;
}

void pc_modfs_narc_seq_note(const void *narc, unsigned member,
                            unsigned offset, unsigned n)
{
    struct narc_bind *b = find_bind(narc);

    if (b == NULL) {
        return;
    }
    b->seq_member = (int)member;
    b->seq_pos = offset + n;
}

void pc_modfs_narc_seq_clear(const void *narc)
{
    struct narc_bind *b = find_bind(narc);

    if (b == NULL) {
        return;
    }
    b->seq_member = -1;
    b->seq_pos = 0;
}

int pc_modfs_narc_seq_read(const void *narc, void *dest, unsigned n)
{
    struct narc_bind *b = find_bind(narc);

    if (b == NULL || b->seq_member < 0 || b->path == NULL) {
        return 0;
    }
    if (!pc_modfs_member_stat(b->path, (unsigned)b->seq_member, NULL)) {
        return 0;
    }
    if (!pc_modfs_member_read(b->path, (unsigned)b->seq_member, dest,
                              b->seq_pos, n)) {
        return 0;
    }
    b->seq_pos += n;
    return 1;
}

int pc_modfs_narc_seq_seek(const void *narc, unsigned delta)
{
    struct narc_bind *b = find_bind(narc);

    if (b == NULL || b->seq_member < 0 || b->path == NULL) {
        return 0;
    }
    if (!pc_modfs_member_stat(b->path, (unsigned)b->seq_member, NULL)) {
        return 0;
    }
    b->seq_pos += delta;
    return 1;
}

unsigned pc_modfs_narc_file_count(const char *nitro_path, unsigned rom_count)
{
    char key[PC_MODFS_PATH];
    unsigned highest = 0;
    int have = 0;
    int i;
    unsigned idx;

    if (nitro_path == NULL || nitro_path[0] == '\0') {
        return rom_count;
    }
    norm_nitro(key, sizeof key, nitro_path);
    for (i = 0; i < sNMembers; i++) {
        if (strcmp(sMembers[i].nitro, key) == 0) {
            if (!have || sMembers[i].index > highest) {
                highest = sMembers[i].index;
            }
            have = 1;
        }
    }
    if (!have || highest < rom_count) {
        return rom_count;
    }
    if (highest >= 65535u) {
        die("append past 65535: %s/%u", key, highest);
    }
    for (idx = rom_count; idx <= highest; idx++) {
        int found = 0;

        for (i = 0; i < sNMembers; i++) {
            if (sMembers[i].index == idx && strcmp(sMembers[i].nitro, key) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            die("append hole: %s/%u (highest %u)", key, idx, highest);
        }
    }
    return highest + 1u;
}

#if !defined(PC_GAME_DP)
int pc_modfs_billboard_nsbtx(int gfx_id)
{
    int i;

    for (i = 0; i < sNGfx; i++) {
        if (sGfx[i].gfx == gfx_id) {
            return sGfx[i].nsbtx;
        }
    }
    return -1;
}

int pc_modfs_extra_prop(int index)
{
    if (index < 0 || index >= sNExtraProps) {
        return -1;
    }
    return sExtraProps[index];
}

int pc_modfs_map_header(int id, struct pc_modfs_map_header *out)
{
    int i;

    if (out == NULL) {
        return 0;
    }
    for (i = 0; i < sNMaps; i++) {
        if (sMaps[i].id == id) {
            *out = sMaps[i].h;
            return 1;
        }
    }
    return 0;
}

static void maybe_probe_map(void)
{
    const char *spec;
    char *end;
    long id;
    unsigned area;
    unsigned matrix;

    spec = getenv("PC_MODFS_PROBE_MAP");
    if (spec == NULL || spec[0] == '\0') {
        return;
    }
    id = strtol(spec, &end, 10);
    if (end == spec || *end != '\0' || id < 0 || id > 65535) {
        die("PC_MODFS_PROBE_MAP wants a header id, got '%s'", spec);
    }
    /* Call the game getters, not the table, so a missing hook is a miss. */
    area = MapHeader_GetAreaDataArchiveID((enum MapHeaderID)id);
    matrix = MapHeader_GetMapMatrixID((enum MapHeaderID)id);
    fprintf(stderr, "modfs: probe-map %ld area=%u matrix=%u\n",
            id, area, matrix);
    exit(0);
}

static void maybe_probe_narc(void)
{
    const char *spec;
    char path[PC_MODFS_PATH];
    const char *slash;
    unsigned idx;
    int id;
    u32 size;
    unsigned osize;
    u8 *buf;
    u8 *obuf;
    u8 slice;
    u8 slice2;
    NARC n;
    u32 i;

    spec = getenv("PC_MODFS_PROBE_NARC");
    if (spec == NULL || spec[0] == '\0') {
        return;
    }
    slash = strrchr(spec, '/');
    if (slash == NULL || !parse_idx(slash + 1, &idx)) {
        die("PC_MODFS_PROBE_NARC wants <nitro-path>/<idx>, got '%s'", spec);
    }
    if ((size_t)(slash - spec) >= sizeof path || slash == spec) {
        die("PC_MODFS_PROBE_NARC wants <nitro-path>/<idx>, got '%s'", spec);
    }
    memcpy(path, spec, (size_t)(slash - spec));
    path[slash - spec] = '\0';
    norm_nitro(path, sizeof path, path);

    id = NARC_FindID(path);
    if (id < 0) {
        die("unknown narc path: %s", path);
    }

    size = NARC_GetMemberSizeByIndexPair((enum NarcID)id, (int)idx);
    buf = (u8 *)malloc(size ? size : 1);
    if (buf == NULL) {
        die("out of memory");
    }
    NARC_ReadWholeMemberByIndexPair(buf, (enum NarcID)id, (int)idx);

    memset(&n, 0, sizeof n);
    pc_modfs_bind_narc(&n, path);
    osize = NARC_GetMemberSize(&n, idx);
    if (osize != (unsigned)size) {
        die("probe-narc size mismatch: pair=%u obj=%u", (unsigned)size, osize);
    }
    obuf = (u8 *)malloc(size ? size : 1);
    if (obuf == NULL) {
        die("out of memory");
    }
    NARC_ReadWholeMember(&n, idx, obuf);
    if (size != 0 && memcmp(buf, obuf, size) != 0) {
        die("probe-narc object read != pair read");
    }
    if (size != 0) {
        slice = 0;
        slice2 = 0;
        NARC_ReadFromMember(&n, idx, 0, 1, &slice);
        NARC_ReadFromMemberByIndexPair(&slice2, (enum NarcID)id, (int)idx, 0, 1);
        if (slice != buf[0] || slice2 != buf[0]) {
            die("probe-narc slice mismatch");
        }
    }
    pc_modfs_unbind_narc(&n);

    fprintf(stderr, "modfs: probe-narc %s/%u %u ", path, idx, (unsigned)size);
    for (i = 0; i < size; i++) {
        fprintf(stderr, "%02x", buf[i]);
    }
    fputc('\n', stderr);
    free(buf);
    free(obuf);
    exit(0);
}
#endif

/*
 * Measured on the weakened fs_file.o (2026-08-16):
 *   FS_OpenFile same-TU calls ConvertPathToFileID and OpenFileFast.
 *   FS_ReadFile / CloseFile / OpenFileFast have no same-TU callers.
 *   Game src/ never calls OpenFileFast (0 TUs). fs_overlay.o and
 *   fs_archive.o reference OpenFileFast / ReadFile / CloseFile
 *   cross-TU, so a strong pc/src def wins on ELF and PE without
 *   demote-def. Do not add these four to Makefile.win's DEMOTE list
 *   unless a later scan finds a same-TU caller.
 */

static void *slurp_host(const char *host, u32 *out_size)
{
    FILE *f;
    long sz;
    void *buf;
    size_t n;

    f = fopen(host, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0 || sz > 0x7fffffffL) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    n = (size_t)sz;
    buf = malloc(n ? n : 1);
    if (buf == NULL) {
        fclose(f);
        die("out of memory");
    }
    if (n != 0 && fread(buf, 1, n, f) != n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (u32)n;
    return buf;
}

#if !defined(PC_GAME_DP)
static BOOL open_host_file(FSFile *p_file, const char *host, const char *nitro)
{
    u32 size = 0;
    void *buf = slurp_host(host, &size);

    if (buf == NULL) {
        die("claimed file missing: %s", nitro);
    }
    if (!FS_CreateFileFromMemory(p_file, buf, size)) {
        free(buf);
        die("claimed file missing: %s", nitro);
    }
    p_file->stat |= FS_FILE_STATUS_USER_RESERVED_BIT;
    return TRUE;
}

static unsigned read_rom_file_count(const char *path)
{
    FSFile file;
    u16 fatbStart = 0;
    u32 chunkSize = 0;
    u16 numFiles = 0;

    FS_InitFile(&file);
    if (!FS_OpenFile(&file, path)) {
        die("cannot open %s to count members", path);
    }
    FS_SeekFile(&file, 12, FS_SEEK_SET);
    FS_ReadFile(&file, &fatbStart, 2);
    FS_SeekFile(&file, fatbStart + 4, FS_SEEK_SET);
    FS_ReadFile(&file, &chunkSize, 4);
    FS_ReadFile(&file, &numFiles, 2);
    (void)FS_CloseFile(&file);
    (void)chunkSize;
    return numFiles;
}

static void maybe_probe_count(void)
{
    static int sProbed;
    const char *spec;
    char path[PC_MODFS_PATH];
    const char *slash;
    unsigned idx;
    unsigned rom;
    unsigned count;
    unsigned size;
    u8 *buf;
    NARC n;
    u16 got;
    u32 i;

    if (sProbed) {
        return;
    }
    spec = getenv("PC_MODFS_PROBE_COUNT");
    if (spec == NULL || spec[0] == '\0') {
        return;
    }
    sProbed = 1;

    slash = strrchr(spec, '/');
    if (slash == NULL || !parse_idx(slash + 1, &idx)) {
        die("PC_MODFS_PROBE_COUNT wants <nitro-path>/<idx>, got '%s'", spec);
    }
    if ((size_t)(slash - spec) >= sizeof path || slash == spec) {
        die("PC_MODFS_PROBE_COUNT wants <nitro-path>/<idx>, got '%s'", spec);
    }
    memcpy(path, spec, (size_t)(slash - spec));
    path[slash - spec] = '\0';
    norm_nitro(path, sizeof path, path);

    rom = read_rom_file_count(path);
    count = pc_modfs_narc_file_count(path, rom);

    memset(&n, 0, sizeof n);
    FS_InitFile(&n.file);
    if (!FS_OpenFile(&n.file, path)) {
        die("cannot open %s to count members", path);
    }
    pc_modfs_bind_narc(&n, path);
    {
        u32 chunkSize = 0;

        FS_SeekFile(&n.file, 12, FS_SEEK_SET);
        FS_ReadFile(&n.file, &(n.fatbStart), 2);
        FS_SeekFile(&n.file, n.fatbStart + 4, FS_SEEK_SET);
        FS_ReadFile(&n.file, &chunkSize, 4);
        FS_ReadFile(&n.file, &(n.numFiles), 2);
        n.numFiles = (u16)pc_modfs_narc_file_count(path, n.numFiles);
        (void)chunkSize;
    }
    got = NARC_GetFileCount(&n);
    if ((unsigned)got != count) {
        die("probe-count mismatch: grow=%u GetFileCount=%u", count, (unsigned)got);
    }

    if (!pc_modfs_member_stat(path, idx, &size)) {
        die("probe-count member not claimed: %s/%u", path, idx);
    }
    buf = (u8 *)malloc(size ? size : 1);
    if (buf == NULL) {
        die("out of memory");
    }
    if (!pc_modfs_member_read(path, idx, buf, 0, 0)) {
        die("probe-count member not claimed: %s/%u", path, idx);
    }

    fprintf(stderr, "modfs: probe-count %s %u %u %u ", path, count, idx, size);
    for (i = 0; i < size; i++) {
        fprintf(stderr, "%02x", buf[i]);
    }
    fputc('\n', stderr);
    free(buf);
    (void)FS_CloseFile(&n.file);
    pc_modfs_unbind_narc(&n);
    exit(0);
}

static void maybe_probe(void)
{
    static int sProbed;
    const char *probe;
    FSFile file;
    u32 len;
    u8 *buf;
    u32 i;

    if (sProbed) {
        return;
    }
    probe = getenv("PC_MODFS_PROBE");
    if (probe == NULL || probe[0] == '\0') {
        return;
    }
    sProbed = 1;

    FS_InitFile(&file);
    if (!FS_OpenFile(&file, probe)) {
        fprintf(stderr, "modfs: probe '%s' failed\n", probe);
        exit(2);
    }
    len = FS_GetLength(&file);
    buf = (u8 *)malloc(len ? len : 1);
    if (buf == NULL) {
        die("out of memory");
    }
    if (len != 0 && FS_ReadFile(&file, buf, (s32)len) != (s32)len) {
        die("probe short read: %s", probe);
    }
    (void)FS_CloseFile(&file);
    fprintf(stderr, "modfs: probe %s %u ", probe, (unsigned)len);
    for (i = 0; i < len; i++) {
        fprintf(stderr, "%02x", buf[i]);
    }
    fputc('\n', stderr);
    free(buf);
    exit(0);
}

BOOL FS_OpenFile(FSFile *p_file, const char *path)
{
    const char *host;
    FSFileID id;

    maybe_probe();
    maybe_probe_count();
    host = pc_modfs_host_file(path);
    if (host != NULL) {
        return open_host_file(p_file, host, path);
    }
    return (FS_ConvertPathToFileID(&id, path) && FS_OpenFileFast(p_file, id));
}

BOOL FS_OpenFileFast(FSFile *p_file, FSFileID file_id)
{
    if (!file_id.arc) {
        return FALSE;
    }
    p_file->arc = file_id.arc;
    /* An FSArchive is host .bss (fsi_arc_rom), and an FSFile can live in
     * guest memory, NNS_SndArcInit's is at the top of the port window,
     * so this word is a host pointer that a relink moves. The SDK's own
     * FS_OpenFileFast is patched to mark it (pc/patches/.../fs_file.c.patch,
     * third hunk); this file REPLACES that function and did not carry the
     * mark over, so the port window's digest could not be pinned across two
     * toolchains. Found by the ARM build: 103 of the window's words moved
     * between x86 and armhf and every one but this and heap->player was
     * already marked. */
    { extern void pc_state_mark_host_field(const void *);
      pc_state_mark_host_field(&p_file->arc); }
    p_file->arg.openfilefast.id = file_id;
    if (!FSi_SendCommand(p_file, FS_COMMAND_OPENFILEFAST)) {
        return FALSE;
    }
    p_file->stat |= FS_FILE_STATUS_IS_FILE;
    p_file->stat &= ~FS_FILE_STATUS_IS_DIR;
    return TRUE;
}

s32 FS_ReadFile(FSFile *p_file, void *dst, s32 len)
{
    const s32 pos = (s32)p_file->prop.file.pos;
    const s32 rest = (s32)p_file->prop.file.bottom - pos;
    const u32 org = (u32)len;

    if (len > rest) {
        len = rest;
    }
    if (len < 0) {
        len = 0;
    }
    p_file->arg.readfile.dst = dst;
    p_file->arg.readfile.len_org = org;
    p_file->arg.readfile.len = (u32)len;
    p_file->stat |= FS_FILE_STATUS_SYNC;
    (void)FSi_SendCommand(p_file, FS_COMMAND_READFILE);
    if (FS_WaitAsync(p_file)) {
        return (s32)p_file->prop.file.pos - pos;
    }
    return -1;
}

BOOL FS_CloseFile(FSFile *p_file)
{
    if (p_file->stat & FS_FILE_STATUS_USER_RESERVED_BIT) {
        void *buf = (void *)(uintptr_t)p_file->prop.file.top;

        p_file->stat &= ~FS_FILE_STATUS_USER_RESERVED_BIT;
        free(buf);
    }
    if (!FSi_SendCommand(p_file, FS_COMMAND_CLOSEFILE)) {
        return FALSE;
    }
    p_file->arc = NULL;
    p_file->command = FS_COMMAND_INVALID;
    p_file->stat &= ~(FS_FILE_STATUS_IS_FILE | FS_FILE_STATUS_IS_DIR);
    return TRUE;
}

#else /* PC_GAME_DP */

/*
 * Diamond/Pearl's half lives with the game's headers
 * (games/diamond/pc/game/pc_dp_modfs.c): it opens a claimed file as a
 * memory-backed FSFile of D's own layout and runs the probes once D's FS is
 * up. These are the calls it makes into the generic half.
 */

void *pc_modfs_file_load(const char *nitro_path, unsigned *out_size)
{
    const char *host = pc_modfs_host_file(nitro_path);
    u32 size = 0;
    void *buf;

    if (host == NULL) {
        return NULL;
    }
    buf = slurp_host(host, &size);
    if (buf == NULL) {
        die("claimed file missing: %s", nitro_path);
    }
    *out_size = (unsigned)size;
    return buf;
}

void pc_modfs_file_free(void *buf)
{
    free(buf);
}

void *pc_modfs_alloc(unsigned size)
{
    void *p = malloc(size ? size : 1);

    if (p == NULL) {
        die("out of memory");
    }
    return p;
}

void pc_modfs_fatal(const char *msg)
{
    die("%s", msg);
}

const char *pc_modfs_probe_file(void)
{
    static int sProbed;
    const char *probe;

    if (sProbed) {
        return NULL;
    }
    sProbed = 1;
    probe = getenv("PC_MODFS_PROBE");
    return (probe != NULL && probe[0] != '\0') ? probe : NULL;
}

int pc_modfs_probe_member(char *path, unsigned cap, unsigned *out_idx)
{
    static int sProbed;
    const char *spec;
    const char *slash;

    if (sProbed) {
        return 0;
    }
    sProbed = 1;
    spec = getenv("PC_MODFS_PROBE_NARC");
    if (spec == NULL || spec[0] == '\0') {
        return 0;
    }
    slash = strrchr(spec, '/');
    if (slash == NULL || slash == spec || !parse_idx(slash + 1, out_idx)
        || (size_t)(slash - spec) >= cap) {
        die("PC_MODFS_PROBE_NARC wants <nitro-path>/<idx>, got '%s'", spec);
    }
    memcpy(path, spec, (size_t)(slash - spec));
    path[slash - spec] = '\0';
    norm_nitro(path, cap, path);
    return 1;
}

void pc_modfs_probe_report(const char *kind, const char *path, int index,
                           const void *buf, unsigned n)
{
    const u8 *p = (const u8 *)buf;
    unsigned i;

    if (index >= 0) {
        fprintf(stderr, "modfs: %s %s/%d %u ", kind, path, index, n);
    } else {
        fprintf(stderr, "modfs: %s %s %u ", kind, path, n);
    }
    for (i = 0; i < n; i++) {
        fprintf(stderr, "%02x", p[i]);
    }
    fputc('\n', stderr);
    exit(0);
}

#endif /* PC_GAME_DP */
