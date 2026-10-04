/*
 * GitHub release parsing, version order and sha256sums lookup (release.h).
 * The JSON reader is a small recursive-descent scanner that only keeps the
 * fields the updater shows; everything else is validated and skipped, with
 * nesting capped so hostile input cannot exhaust the stack.
 */
#include "release.h"

#include <stdlib.h>
#include <string.h>

typedef struct cursor {
    const char *p, *end;
    int depth;
} cursor;

#define MAX_DEPTH 64

static void ws(cursor *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

static int hexval(char ch)
{
    return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

static int read_u4(cursor *c, unsigned *v)
{
    if (c->end - c->p < 4)
        return -1;
    *v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(c->p[i]);
        if (h < 0)
            return -1;
        *v = *v << 4 | (unsigned)h;
    }
    c->p += 4;
    return 0;
}

/* Appends code point cp as UTF-8 if it fits (out may be NULL to skip). */
static void put_utf8(char *out, size_t cap, size_t *k, unsigned cp)
{
    char b[4];
    int n;
    if (cp < 0x80) {
        b[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | cp >> 6);
        b[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | cp >> 12);
        b[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | cp >> 18);
        b[1] = (char)(0x80 | (cp >> 12 & 0x3F));
        b[2] = (char)(0x80 | (cp >> 6 & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (out && *k + (size_t)n < cap) {
        memcpy(out + *k, b, (size_t)n);
        *k += (size_t)n;
    }
}

/* A JSON string into out (truncated to cap-1 bytes; NULL to skip). */
static int string(cursor *c, char *out, size_t cap)
{
    ws(c);
    if (c->p >= c->end || *c->p != '"')
        return -1;
    c->p++;
    size_t k = 0;
    while (c->p < c->end && *c->p != '"') {
        unsigned char ch = (unsigned char)*c->p++;
        if (ch < 0x20)
            return -1;
        if (ch != '\\') {
            if (out && k + 1 < cap)
                out[k++] = (char)ch;
            continue;
        }
        if (c->p >= c->end)
            return -1;
        char e = *c->p++;
        unsigned cp;
        switch (e) {
        case '"': case '\\': case '/': cp = (unsigned char)e; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            if (read_u4(c, &cp))
                return -1;
            if (cp >= 0xD800 && cp < 0xDC00) { /* surrogate pair */
                unsigned lo;
                if (c->end - c->p < 6 || c->p[0] != '\\' || c->p[1] != 'u')
                    return -1;
                c->p += 2;
                if (read_u4(c, &lo) || lo < 0xDC00 || lo > 0xDFFF)
                    return -1;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            break;
        default: return -1;
        }
        put_utf8(out, cap, &k, cp);
    }
    if (c->p >= c->end)
        return -1;
    c->p++;
    if (out && cap)
        out[k] = '\0';
    return 0;
}

static int literal(cursor *c, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, word, n))
        return -1;
    c->p += n;
    return 0;
}

static int number(cursor *c, uint64_t *v)
{
    ws(c);
    const char *s = c->p;
    if (c->p < c->end && *c->p == '-')
        c->p++;
    while (c->p < c->end && ((*c->p >= '0' && *c->p <= '9') || *c->p == '.' || *c->p == 'e' || *c->p == 'E' ||
                             *c->p == '+' || *c->p == '-'))
        c->p++;
    if (c->p == s)
        return -1;
    if (v) {
        uint64_t x = 0;
        for (const char *q = s; q < c->p && *q >= '0' && *q <= '9'; q++)
            x = x * 10 + (uint64_t)(*q - '0');
        *v = *s == '-' ? 0 : x;
    }
    return 0;
}

static int skip(cursor *c);

/* Calls field(key) for each member; field parses the value or returns 1
 * to have it skipped. */
typedef int (*field_fn)(cursor *c, const char *key, void *user);

static int object(cursor *c, field_fn field, void *user)
{
    ws(c);
    if (c->p >= c->end || *c->p != '{' || ++c->depth > MAX_DEPTH)
        return -1;
    c->p++;
    ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        c->depth--;
        return 0;
    }
    for (;;) {
        char key[64];
        if (string(c, key, sizeof key))
            return -1;
        ws(c);
        if (c->p >= c->end || *c->p++ != ':')
            return -1;
        int r = field ? field(c, key, user) : 1;
        if (r < 0 || (r > 0 && skip(c)))
            return -1;
        ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        if (c->p < c->end && *c->p == '}') {
            c->p++;
            c->depth--;
            return 0;
        }
        return -1;
    }
}

/* Each element: elem(c, index) parses it, or returns 1 to skip it. */
typedef int (*elem_fn)(cursor *c, int index, void *user);

static int array(cursor *c, elem_fn elem, void *user)
{
    ws(c);
    if (c->p >= c->end || *c->p != '[' || ++c->depth > MAX_DEPTH)
        return -1;
    c->p++;
    ws(c);
    if (c->p < c->end && *c->p == ']') {
        c->p++;
        c->depth--;
        return 0;
    }
    for (int i = 0;; i++) {
        int r = elem ? elem(c, i, user) : 1;
        if (r < 0 || (r > 0 && skip(c)))
            return -1;
        ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            continue;
        }
        if (c->p < c->end && *c->p == ']') {
            c->p++;
            c->depth--;
            return 0;
        }
        return -1;
    }
}

static int skip(cursor *c)
{
    ws(c);
    if (c->p >= c->end)
        return -1;
    switch (*c->p) {
    case '{': return object(c, NULL, NULL);
    case '[': return array(c, NULL, NULL);
    case '"': return string(c, NULL, 0);
    case 't': return literal(c, "true");
    case 'f': return literal(c, "false");
    case 'n': return literal(c, "null");
    default: return number(c, NULL);
    }
}

/* A string field that may be null. */
static int opt_string(cursor *c, char *out, size_t cap)
{
    ws(c);
    if (c->p < c->end && *c->p == 'n') {
        out[0] = '\0';
        return literal(c, "null");
    }
    return string(c, out, cap);
}

static int boolean(cursor *c, int *v)
{
    ws(c);
    if (!literal(c, "true")) {
        *v = 1;
        return 0;
    }
    *v = 0;
    return literal(c, "false");
}

static int asset_field(cursor *c, const char *key, void *user)
{
    np_release_asset *a = user;
    if (!strcmp(key, "name"))
        return opt_string(c, a->name, sizeof a->name);
    if (!strcmp(key, "browser_download_url"))
        return opt_string(c, a->url, sizeof a->url);
    if (!strcmp(key, "size"))
        return number(c, &a->size);
    return 1;
}

static int asset_elem(cursor *c, int index, void *user)
{
    np_release *r = user;
    if (index >= NP_RELEASE_MAX_ASSETS)
        return 1;
    np_release_asset *a = &r->asset[r->nassets++];
    memset(a, 0, sizeof *a);
    return object(c, asset_field, a);
}

static int release_field(cursor *c, const char *key, void *user)
{
    np_release *r = user;
    if (!strcmp(key, "tag_name"))
        return opt_string(c, r->tag, sizeof r->tag);
    if (!strcmp(key, "name"))
        return opt_string(c, r->name, sizeof r->name);
    if (!strcmp(key, "html_url"))
        return opt_string(c, r->html_url, sizeof r->html_url);
    if (!strcmp(key, "draft"))
        return boolean(c, &r->draft);
    if (!strcmp(key, "prerelease"))
        return boolean(c, &r->prerelease);
    if (!strcmp(key, "assets")) {
        r->nassets = 0;
        return array(c, asset_elem, r);
    }
    return 1;
}

int np_release_parse(const char *json, size_t len, np_release *r)
{
    memset(r, 0, sizeof *r);
    cursor c = {json, json + len, 0};
    if (object(&c, release_field, r))
        return -1;
    ws(&c);
    return c.p == c.end && r->tag[0] ? 0 : -1;
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
