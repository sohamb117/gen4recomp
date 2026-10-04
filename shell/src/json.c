/* JSON tokenizer and lookups (see json.h). */
#include "json.h"

#include <stdlib.h>
#include <string.h>

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Scans a string starting after its opening quote; returns the index of the
 * closing quote or -1. Escapes are checked for shape only here. */
static long scan_string(const char *s, size_t len, size_t i)
{
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"')
            return (long)i;
        if (c < 0x20)
            return -1;
        if (c == '\\') {
            if (++i >= len)
                return -1;
            if (s[i] == 'u') {
                if (len - i < 5)
                    return -1;
                for (int k = 1; k <= 4; k++)
                    if (hexval(s[i + k]) < 0)
                        return -1;
                i += 4;
            } else if (!strchr("\"\\/bfnrt", s[i])) {
                return -1;
            }
        }
        i++;
    }
    return -1;
}

static long scan_primitive(const char *s, size_t len, size_t i)
{
    size_t start = i;
    while (i < len && !is_ws(s[i]) && !strchr(",]}:", s[i]))
        i++;
    size_t n = i - start;
    const char *p = s + start;
    if ((n == 4 && !memcmp(p, "true", 4)) || (n == 5 && !memcmp(p, "false", 5)) || (n == 4 && !memcmp(p, "null", 4)))
        return (long)i;
    /* -?digits(.digits)?([eE][+-]?digits)? */
    size_t k = 0;
    if (k < n && p[k] == '-')
        k++;
    size_t d = k;
    while (k < n && p[k] >= '0' && p[k] <= '9')
        k++;
    if (k == d)
        return -1;
    if (k < n && p[k] == '.') {
        d = ++k;
        while (k < n && p[k] >= '0' && p[k] <= '9')
            k++;
        if (k == d)
            return -1;
    }
    if (k < n && (p[k] == 'e' || p[k] == 'E')) {
        k++;
        if (k < n && (p[k] == '+' || p[k] == '-'))
            k++;
        d = k;
        while (k < n && p[k] >= '0' && p[k] <= '9')
            k++;
        if (k == d)
            return -1;
    }
    return k == n ? (long)i : -1;
}

int np_json_parse(const char *s, size_t len, np_json_tok *t, int max)
{
    int stack[NP_JSON_MAX_DEPTH]; /* open containers */
    int depth = 0, count = 0;
    /* What the next token may be: a value, a key, or ',' / ':' / close. */
    enum { WANT_VALUE, WANT_KEY_OR_CLOSE, WANT_KEY, WANT_COLON, WANT_COMMA_OR_CLOSE, WANT_VALUE_OR_CLOSE, DONE } want =
        WANT_VALUE;
    size_t i = 0;
    for (;;) {
        while (i < len && is_ws(s[i]))
            i++;
        if (i == len)
            return want == DONE ? count : -1;
        char c = s[i];
        if (want == DONE)
            return -1; /* trailing garbage */
        if (want == WANT_COLON) {
            if (c != ':')
                return -1;
            i++;
            want = WANT_VALUE;
            continue;
        }
        if (want == WANT_COMMA_OR_CLOSE || want == WANT_KEY_OR_CLOSE || want == WANT_VALUE_OR_CLOSE) {
            int top = stack[depth - 1];
            char close = t[top].type == NP_JSON_OBJECT ? '}' : ']';
            if (c == close) {
                t[top].next = count;
                t[top].end = (int)i + 1;
                depth--;
                i++;
                want = depth == 0 ? DONE : WANT_COMMA_OR_CLOSE;
                continue;
            }
            if (want == WANT_COMMA_OR_CLOSE) {
                if (c != ',')
                    return -1;
                i++;
                want = t[top].type == NP_JSON_OBJECT ? WANT_KEY : WANT_VALUE;
                continue;
            }
            want = want == WANT_KEY_OR_CLOSE ? WANT_KEY : WANT_VALUE;
        }
        if (count == max)
            return -1;
        np_json_tok *tok = &t[count];
        int is_key = want == WANT_KEY;
        if (is_key && c != '"')
            return -1;
        if (c == '{' || c == '[') {
            if (depth == NP_JSON_MAX_DEPTH)
                return -1;
            *tok = (np_json_tok){c == '{' ? NP_JSON_OBJECT : NP_JSON_ARRAY, (int)i, (int)i, 0, 0};
            if (depth && t[stack[depth - 1]].type == NP_JSON_ARRAY)
                t[stack[depth - 1]].size++; /* object members count at their key */
            stack[depth++] = count++;
            i++;
            want = c == '{' ? WANT_KEY_OR_CLOSE : WANT_VALUE_OR_CLOSE;
            continue;
        }
        long end;
        if (c == '"') {
            end = scan_string(s, len, i + 1);
            if (end < 0)
                return -1;
            *tok = (np_json_tok){NP_JSON_STRING, (int)i + 1, (int)end, 0, count + 1};
            i = (size_t)end + 1;
        } else {
            end = scan_primitive(s, len, i);
            if (end < 0 || (size_t)end == i)
                return -1;
            *tok = (np_json_tok){NP_JSON_PRIMITIVE, (int)i, (int)end, 0, count + 1};
            i = (size_t)end;
        }
        count++;
        if (is_key) {
            t[stack[depth - 1]].size++;
            want = WANT_COLON;
        } else if (depth == 0) {
            want = DONE;
        } else {
            if (t[stack[depth - 1]].type == NP_JSON_ARRAY)
                t[stack[depth - 1]].size++;
            want = WANT_COMMA_OR_CLOSE;
        }
    }
}

int np_json_get(const char *s, const np_json_tok *t, int obj, const char *key)
{
    if (obj < 0 || t[obj].type != NP_JSON_OBJECT)
        return -1;
    int k = obj + 1;
    for (int m = 0; m < t[obj].size; m++) {
        int v = t[k].next;
        if (np_json_eq(s, t, k, key))
            return v;
        k = t[v].next;
    }
    return -1;
}

int np_json_at(const np_json_tok *t, int arr, int i)
{
    if (arr < 0 || t[arr].type != NP_JSON_ARRAY || i < 0 || i >= t[arr].size)
        return -1;
    int k = arr + 1;
    while (i--)
        k = t[k].next;
    return k;
}

int np_json_eq(const char *s, const np_json_tok *t, int i, const char *str)
{
    if (i < 0 || t[i].type != NP_JSON_STRING)
        return 0;
    size_t n = strlen(str);
    return (size_t)(t[i].end - t[i].start) == n && !memcmp(s + t[i].start, str, n);
}

static void put_utf8(char *out, size_t cap, size_t *k, unsigned cp)
{
    char b[4];
    int n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (n == 1)
        b[0] = (char)cp;
    else if (n == 2)
        b[0] = (char)(0xC0 | cp >> 6), b[1] = (char)(0x80 | (cp & 0x3F));
    else if (n == 3)
        b[0] = (char)(0xE0 | cp >> 12), b[1] = (char)(0x80 | (cp >> 6 & 0x3F)), b[2] = (char)(0x80 | (cp & 0x3F));
    else
        b[0] = (char)(0xF0 | cp >> 18), b[1] = (char)(0x80 | (cp >> 12 & 0x3F)), b[2] = (char)(0x80 | (cp >> 6 & 0x3F)),
        b[3] = (char)(0x80 | (cp & 0x3F));
    if (*k + (size_t)n < cap) {
        memcpy(out + *k, b, (size_t)n);
        *k += (size_t)n;
    }
}

static unsigned u4(const char *p)
{
    return (unsigned)(hexval(p[0]) << 12 | hexval(p[1]) << 8 | hexval(p[2]) << 4 | hexval(p[3]));
}

int np_json_string(const char *s, const np_json_tok *t, int i, char *out, size_t cap)
{
    if (i < 0 || t[i].type != NP_JSON_STRING || !cap)
        return -1;
    size_t k = 0;
    for (int p = t[i].start; p < t[i].end; p++) {
        char c = s[p];
        if (c != '\\') {
            if (k + 1 < cap)
                out[k++] = c;
            continue;
        }
        c = s[++p];
        unsigned cp;
        switch (c) {
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            cp = u4(s + p + 1);
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00) {
                if (p + 6 >= t[i].end || s[p + 1] != '\\' || s[p + 2] != 'u')
                    return -1;
                unsigned lo = u4(s + p + 3);
                if (lo < 0xDC00 || lo > 0xDFFF)
                    return -1;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            }
            break;
        default: cp = (unsigned char)c; break; /* " \ / */
        }
        put_utf8(out, cap, &k, cp);
    }
    out[k] = '\0';
    return 0;
}

double np_json_number(const char *s, const np_json_tok *t, int i, double fallback)
{
    if (i < 0 || t[i].type != NP_JSON_PRIMITIVE)
        return fallback;
    char c = s[t[i].start];
    if (c != '-' && (c < '0' || c > '9'))
        return fallback;
    char buf[64];
    int n = t[i].end - t[i].start;
    if (n >= (int)sizeof buf)
        return fallback;
    memcpy(buf, s + t[i].start, (size_t)n);
    buf[n] = '\0';
    return strtod(buf, NULL);
}

int np_json_bool(const char *s, const np_json_tok *t, int i, int fallback)
{
    if (i < 0 || t[i].type != NP_JSON_PRIMITIVE)
        return fallback;
    if (s[t[i].start] == 't')
        return 1;
    if (s[t[i].start] == 'f')
        return 0;
    return fallback;
}
