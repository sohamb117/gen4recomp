/*
 * GitHub release parsing, version order and sha256sums lookup (release.h).
 * The release JSON is tokenized by json.c; only the fields the updater
 * shows are kept.
 */
#include "release.h"

#include <stdlib.h>
#include <string.h>

#include "json.h"

static int hexval(char ch)
{
    return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

/* Release documents carry the body text and an uploader object per asset;
 * 16k tokens covers far more assets than NP_RELEASE_MAX_ASSETS. */
#define MAX_TOKENS 16384

int np_release_parse(const char *json, size_t len, np_release *r)
{
    memset(r, 0, sizeof *r);
    np_json_tok *t = malloc(sizeof *t * MAX_TOKENS);
    if (!t)
        return -1;
    int ok = np_json_parse(json, len, t, MAX_TOKENS) > 0 && t[0].type == NP_JSON_OBJECT;
    if (ok) {
        np_json_string(json, t, np_json_get(json, t, 0, "tag_name"), r->tag, sizeof r->tag);
        np_json_string(json, t, np_json_get(json, t, 0, "name"), r->name, sizeof r->name);
        np_json_string(json, t, np_json_get(json, t, 0, "html_url"), r->html_url, sizeof r->html_url);
        r->draft = np_json_bool(json, t, np_json_get(json, t, 0, "draft"), 0);
        r->prerelease = np_json_bool(json, t, np_json_get(json, t, 0, "prerelease"), 0);
        int assets = np_json_get(json, t, 0, "assets");
        for (int i = 0; assets >= 0 && i < t[assets].size && r->nassets < NP_RELEASE_MAX_ASSETS; i++) {
            int a = np_json_at(t, assets, i);
            np_release_asset *dst = &r->asset[r->nassets];
            if (t[a].type != NP_JSON_OBJECT)
                continue;
            np_json_string(json, t, np_json_get(json, t, a, "name"), dst->name, sizeof dst->name);
            np_json_string(json, t, np_json_get(json, t, a, "browser_download_url"), dst->url, sizeof dst->url);
            double size = np_json_number(json, t, np_json_get(json, t, a, "size"), 0);
            dst->size = size > 0 ? (uint64_t)size : 0;
            r->nassets++;
        }
    }
    free(t);
    return ok && r->tag[0] ? 0 : -1;
}

/* ---- versions ------------------------------------------------------------------ */

static const char *version_core(const char *v, const char **pre)
{
    if (*v == 'v' || *v == 'V')
        v++;
    *pre = NULL;
    for (const char *p = v; *p; p++)
        if (*p == '-' || *p == '+') {
            if (*p == '-')
                *pre = p + 1;
            break;
        }
    return v;
}

int np_version_compare(const char *a, const char *b)
{
    const char *pa, *pb;
    a = version_core(a, &pa);
    b = version_core(b, &pb);
    for (int part = 0; part < 4; part++) {
        unsigned long x = 0, y = 0;
        if (*a >= '0' && *a <= '9')
            x = strtoul(a, (char **)&a, 10);
        if (*b >= '0' && *b <= '9')
            y = strtoul(b, (char **)&b, 10);
        if (x != y)
            return x < y ? -1 : 1;
        if (*a == '.')
            a++;
        if (*b == '.')
            b++;
    }
    if (!pa != !pb)
        return pa ? -1 : 1; /* a pre-release comes before the release */
    if (pa && pb) {
        size_t na = strcspn(pa, "+"), nb = strcspn(pb, "+");
        int c = strncmp(pa, pb, na < nb ? na : nb);
        return c ? c : (na > nb) - (na < nb);
    }
    return 0;
}

/* ---- assets and digests ----------------------------------------------------------- */

static int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && (hay[i] | 0x20) == (needle[i] | 0x20))
            i++;
        if (i == n)
            return 1;
    }
    return 0;
}

int np_release_pick_asset(const np_release *r, const char *platform)
{
    for (int i = 0; i < r->nassets; i++) {
        const char *n = r->asset[i].name;
        size_t len = strlen(n);
        if (len > 4 && contains_ci(n + len - 4, ".zip") && contains_ci(n, platform) && r->asset[i].url[0])
            return i;
    }
    return -1;
}

int np_release_find_asset(const np_release *r, const char *name)
{
    for (int i = 0; i < r->nassets; i++)
        if (!strcmp(r->asset[i].name, name))
            return i;
    return -1;
}

int np_sha256sums_lookup(const char *text, size_t len, const char *file, char hex[65])
{
    const char *p = text, *end = text + len;
    size_t flen = strlen(file);
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *le = eol ? eol : end;
        if (le > p && le[-1] == '\r')
            le--;
        if (le - p > 66) {
            int ok = 1;
            for (int i = 0; i < 64; i++)
                ok &= hexval(p[i]) >= 0;
            const char *name = p + 64;
            if (ok && *name == ' ') {
                name++;
                if (name < le && (*name == ' ' || *name == '*'))
                    name++;
                if ((size_t)(le - name) == flen && !memcmp(name, file, flen)) {
                    for (int i = 0; i < 64; i++)
                        hex[i] = (char)(p[i] | (p[i] >= 'A' && p[i] <= 'F' ? 0x20 : 0));
                    hex[64] = '\0';
                    return 0;
                }
            }
        }
        p = eol ? eol + 1 : end;
    }
    return -1;
}
