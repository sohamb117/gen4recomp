/* mod.toml parsing and load-order checks (see modpkg.h). */
#include "modpkg.h"

#include <stdio.h>
#include <string.h>

/* Copies a "quoted" TOML basic string (\" and \\ escapes) from *p into out;
 * advances *p past the closing quote. Returns 0 or -1. */
static int string_value(const char **p, const char *end, char *out, size_t n)
{
    const char *s = *p;
    if (s >= end || *s != '"')
        return -1;
    s++;
    size_t k = 0;
    while (s < end && *s != '"') {
        char c = *s++;
        if (c == '\\' && s < end)
            c = *s++;
        if (c == '\n')
            return -1;
        if (k + 1 < n)
            out[k++] = c;
    }
    if (s >= end)
        return -1;
    out[k] = '\0';
    *p = s + 1;
    return 0;
}

static const char *skip_space(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    return p;
}

/* ["a", "b"] -> up to max strings of `len` chars. Returns the count, -1 on
 * a malformed array. */
static int array_value(const char *p, const char *end, char (*out)[NP_MOD_ID_MAX], int max, char *joined,
                       size_t joined_n)
{
    if (p >= end || *p != '[')
        return -1;
    p++;
    int n = 0;
    if (joined && joined_n)
        joined[0] = '\0';
    for (;;) {
        p = skip_space(p, end);
        if (p < end && *p == ']')
            return n;
        char item[NP_MOD_ID_MAX * 2];
        if (string_value(&p, end, item, sizeof item))
            return -1;
        if (out && n < max)
            snprintf(out[n], NP_MOD_ID_MAX, "%s", item);
        if (joined) {
            size_t len = strlen(joined);
            snprintf(joined + len, joined_n - len, "%s%s", len ? ", " : "", item);
        }
        n++;
        p = skip_space(p, end);
        if (p < end && *p == ',')
            p++;
        else if (p >= end || *p != ']')
            return -1;
    }
}

static int valid_id(const char *id)
{
    if (!id[0])
        return 0;
    for (const char *c = id; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'))
            return 0;
    return 1;
}

int np_mod_parse(const char *text, size_t len, np_mod_info *m, const char **why)
{
    memset(m, 0, sizeof *m);
    const char *p = text, *end = text + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = eol ? eol : end;
        const char *k = skip_space(p, line_end);
        if (k < line_end && *k != '#' && *k != '[') {
            const char *ke = k;
            while (ke < line_end && *ke != '=' && *ke != ' ' && *ke != '\t')
                ke++;
            size_t kl = (size_t)(ke - k);
            const char *v = skip_space(ke, line_end);
            if (v < line_end && *v == '=') {
                v = skip_space(v + 1, line_end);
#define KEY(s) (kl == sizeof(s) - 1 && !memcmp(k, s, kl))
                int r = 0;
                if (KEY("id"))
                    r = string_value(&v, line_end, m->id, sizeof m->id);
                else if (KEY("name"))
                    r = string_value(&v, line_end, m->name, sizeof m->name);
                else if (KEY("version"))
                    r = string_value(&v, line_end, m->version, sizeof m->version);
                else if (KEY("authors"))
                    r = array_value(v, line_end, NULL, 0, m->authors, sizeof m->authors) < 0;
                else if (KEY("requires"))
                    r = (m->nrequires = array_value(v, line_end, m->requires, NP_MOD_DEPS, NULL, 0)) < 0;
                else if (KEY("load_after"))
                    r = (m->nafter = array_value(v, line_end, m->load_after, NP_MOD_DEPS, NULL, 0)) < 0;
#undef KEY
                if (r) {
                    *why = "mod.toml has a malformed value";
                    return -1;
                }
            }
        }
        p = eol ? eol + 1 : end;
    }
    if (m->nrequires > NP_MOD_DEPS || m->nafter > NP_MOD_DEPS) {
        *why = "mod.toml lists too many dependencies";
        return -1;
    }
    if (!valid_id(m->id)) {
        *why = "mod.toml needs an id of lowercase letters, digits and _";
        return -1;
    }
    if (!m->name[0] || !m->version[0]) {
        *why = "mod.toml needs a name and a version";
        return -1;
    }
    return 0;
}

static int index_of(const np_mod_info *const *enabled, int n, const char *id)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(enabled[i]->id, id))
            return i;
    return -1;
}

int np_mod_check_order(const np_mod_info *const *enabled, int n, char *why, size_t whyn)
{
    for (int i = 0; i < n; i++) {
        const np_mod_info *m = enabled[i];
        for (int r = 0; r < m->nrequires; r++)
            if (index_of(enabled, n, m->requires[r]) < 0) {
                snprintf(why, whyn, "needs '%s', which is not enabled", m->requires[r]);
                return i;
            }
        for (int a = 0; a < m->nafter; a++) {
            int j = index_of(enabled, n, m->load_after[a]);
            if (j > i) {
                snprintf(why, whyn, "must load after '%s'", m->load_after[a]);
                return i;
            }
        }
    }
    return -1;
}
