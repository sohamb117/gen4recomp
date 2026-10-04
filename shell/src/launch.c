/*
 * Command-line and URL launch parsing (see launch.h).
 */
#include "launch.h"

#include <stdio.h>
#include <string.h>

#include "np_core.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

static int ieq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i]))
            return 0;
        if (!a[i])
            return 1;
    }
    return 1;
}

int np_launch_game_from_name(const char *name)
{
    static const char *const ids[NP_GAME_COUNT] = {"diamond", "pearl", "platinum"};
    for (int g = 0; g < NP_GAME_COUNT; g++)
        if (strlen(name) == strlen(ids[g]) && ieq_n(name, ids[g], strlen(ids[g])))
            return g;
    return -1;
}

int np_launch_slot_number(const char *slot)
{
    if (!*slot || strlen(slot) > 4)
        return 0;
    int v = 0;
    for (const char *p = slot; *p; p++) {
        if (*p < '0' || *p > '9')
            return 0;
        v = v * 10 + (*p - '0');
    }
    return v;
}

static void init(np_launch *out)
{
    out->game = -1;
    out->slot[0] = '\0';
    out->force_launcher = 0;
}

static int set_game(np_launch *out, const char *v, char *err, size_t errn)
{
    out->game = np_launch_game_from_name(v);
    if (out->game < 0) {
        snprintf(err, errn, "Unknown game \"%.40s\" (use diamond, pearl or platinum).", v);
        return -1;
    }
    return 0;
}

static int set_slot(np_launch *out, const char *v, char *err, size_t errn)
{
    if (!*v || strlen(v) >= sizeof out->slot) {
        snprintf(err, errn, "Invalid save slot \"%.40s\".", v);
        return -1;
    }
    strcpy(out->slot, v);
    return 0;
}

static int finish(np_launch *out, char *err, size_t errn)
{
    if (out->slot[0] && out->game < 0) {
        snprintf(err, errn, "A save slot was given without a game.");
        return -1;
    }
    return 0;
}

int np_launch_parse_args(int argc, char *const *argv, np_launch *out, char *err, size_t errn)
{
    init(out);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *eq = strchr(a, '=');
        size_t name_len = eq ? (size_t)(eq - a) : strlen(a);
        const char *v = eq ? eq + 1 : (i + 1 < argc ? argv[i + 1] : NULL);
        int takes_value = (name_len == 6 && !strncmp(a, "--game", 6)) || (name_len == 6 && !strncmp(a, "--slot", 6));
        if (!strncmp(a, "-psn_", 5))
            continue; /* Finder launches on old macOS */
        if ((!strncmp(a, "-NS", 3) || !strncmp(a, "-Apple", 6)) && !eq) {
            i++; /* "-NSDocumentRevisionsDebugMode YES" from Xcode */
            continue;
        }
        if (!strcmp(a, "--launcher")) {
            out->force_launcher = 1;
            continue;
        }
        if (!takes_value) {
            snprintf(err, errn, "Unknown option \"%.40s\".", a);
            return -1;
        }
        if (!v) {
            snprintf(err, errn, "%.*s needs a value.", (int)name_len, a);
            return -1;
        }
        if (!eq)
            i++;
        int r = a[2] == 'g' ? set_game(out, v, err, errn) : set_slot(out, v, err, errn);
        if (r)
            return -1;
    }
    return finish(out, err, errn);
}

int np_launch_is_url(const char *s) { return ieq_n(s, "nativeplat:", 11) && strlen(s) >= 11; }

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Decodes s[0..n) into out (outn bytes incl. terminator). -1 on bad escape
 * or overflow. */
static int url_decode(const char *s, size_t n, char *out, size_t outn)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (i + 2 >= n)
                return -1;
            int h = hexval((unsigned char)s[i + 1]), l = hexval((unsigned char)s[i + 2]);
            if (h < 0 || l < 0 || h * 16 + l == 0)
                return -1;
            c = h * 16 + l;
            i += 2;
        }
        if (o + 1 >= outn)
            return -1;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return 0;
}

int np_launch_parse_url(const char *url, np_launch *out, char *err, size_t errn)
{
    init(out);
    if (!np_launch_is_url(url)) {
        snprintf(err, errn, "Not a nativeplat: link.");
        return -1;
    }
    const char *p = url + 11;
    while (*p == '/')
        p++;
    size_t host_len = strcspn(p, "/?#");
    if (host_len && !(host_len == 6 && ieq_n(p, "launch", 6))) {
        snprintf(err, errn, "Unknown link action \"%.*s\".", (int)(host_len > 40 ? 40 : host_len), p);
        return -1;
    }
    p += host_len;
    while (*p == '/')
        p++;
    if (*p != '?') {
        if (*p && *p != '#') {
            snprintf(err, errn, "Malformed link.");
            return -1;
        }
        return 0; /* nativeplat://launch: just bring up the launcher */
    }
    p++;
    while (*p && *p != '#') {
        size_t len = strcspn(p, "&#");
        const char *eq = memchr(p, '=', len);
        char key[16], val[sizeof out->slot];
        if (!eq || (size_t)(eq - p) >= sizeof key || url_decode(eq + 1, len - (size_t)(eq - p) - 1, val, sizeof val)) {
            snprintf(err, errn, "Malformed link parameter.");
            return -1;
        }
        memcpy(key, p, (size_t)(eq - p));
        key[eq - p] = '\0';
        if (!strcmp(key, "game")) {
            if (set_game(out, val, err, errn))
                return -1;
        } else if (!strcmp(key, "slot")) {
            if (set_slot(out, val, err, errn))
                return -1;
        } else if (!strcmp(key, "launcher")) {
            out->force_launcher = strcmp(val, "0") != 0;
        } else {
            snprintf(err, errn, "Unknown link parameter \"%s\".", key);
            return -1;
        }
        p += len;
        if (*p == '&')
            p++;
    }
    return finish(out, err, errn);
}
