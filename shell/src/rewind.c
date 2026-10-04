/*
 * Rewind history (see rewind.h).
 *
 * Coded deltas live in one ring arena. Entries are appended at `tail` and
 * removed from both ends (newest when stepping back, oldest when room is
 * needed); an entry never straddles the arena's end, so when it does not fit
 * before the end it starts again at offset 0. Delta format: a header, then
 * pairs of (zero-run length, literal length) varints each followed by that
 * many literal bytes, covering the longer of the two snapshots; bytes past
 * a snapshot's length count as zero.
 */
#include "rewind.h"

#include <stdlib.h>
#include <string.h>

typedef struct entry {
    size_t off, size; /* in the arena */
} entry;

typedef struct delta_header {
    uint64_t coded;    /* bytes of run data after this header */
    uint64_t span;     /* bytes the runs cover */
    uint64_t prev_len; /* length of the older snapshot */
} delta_header;

struct np_rewind {
    uint8_t *arena;
    size_t arena_cap;
    size_t tail;
    entry *ring;      /* oldest at ring[first] */
    int ring_cap, first, count;
    size_t used;

    uint8_t *latest; /* newest snapshot, zero past latest_len up to cap */
    size_t latest_len, cap;
    int have_latest;
    uint8_t *xor_buf;   /* cap bytes */
    uint8_t *coded_buf; /* worst-case coding of cap bytes */
};

/* Every run pair but the first skips at least 8 zero bytes, which pays for
 * its two varints, so coding never grows the input by more than a little. */
static size_t coded_worst(size_t n) { return sizeof(delta_header) + n + 64; }

static int grow(np_rewind *r, size_t len)
{
    if (len <= r->cap)
        return 0;
    size_t cap = len + len / 8;
    uint8_t *latest = realloc(r->latest, cap);
    if (!latest)
        return -1;
    memset(latest + r->cap, 0, cap - r->cap);
    r->latest = latest;
    uint8_t *x = realloc(r->xor_buf, cap);
    uint8_t *c = x ? realloc(r->coded_buf, coded_worst(cap)) : NULL;
    if (x)
        r->xor_buf = x;
    if (!c)
        return -1;
    r->coded_buf = c;
    r->cap = cap;
    return 0;
}

np_rewind *np_rewind_create(size_t budget, size_t state_hint)
{
    np_rewind *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->arena_cap = budget;
    r->arena = malloc(budget);
    /* An entry per 4 KB of budget is far more than real deltas need. */
    r->ring_cap = (int)(budget / 4096) + 16;
    r->ring = malloc(sizeof *r->ring * (size_t)r->ring_cap);
    if (!r->arena || !r->ring || grow(r, state_hint ? state_hint : 1)) {
        np_rewind_destroy(r);
        return NULL;
    }
    return r;
}

void np_rewind_destroy(np_rewind *r)
{
    if (!r)
        return;
    free(r->arena);
    free(r->ring);
    free(r->latest);
    free(r->xor_buf);
    free(r->coded_buf);
    free(r);
}

void np_rewind_clear(np_rewind *r)
{
    r->first = r->count = 0;
    r->tail = r->used = 0;
    memset(r->latest, 0, r->latest_len);
    r->latest_len = 0;
    r->have_latest = 0;
}

int np_rewind_depth(const np_rewind *r) { return r->count; }
size_t np_rewind_used(const np_rewind *r) { return r->used; }

static entry *at(np_rewind *r, int i) { return &r->ring[(r->first + i) % r->ring_cap]; }

static void drop_oldest(np_rewind *r)
{
    r->used -= at(r, 0)->size;
    r->first = (r->first + 1) % r->ring_cap;
    if (!--r->count)
        r->tail = 0;
}

/* Finds room for `size` bytes, dropping the oldest entries as needed. */
static int place(np_rewind *r, size_t size, size_t *off)
{
    if (size > r->arena_cap)
        return -1;
    for (;;) {
        if (r->count == r->ring_cap) {
            drop_oldest(r);
            continue;
        }
        if (!r->count) {
            *off = 0;
            return 0;
        }
        size_t oldest = at(r, 0)->off, newest = at(r, r->count - 1)->off;
        if (newest >= oldest) { /* occupied [oldest, tail) */
            if (r->tail + size <= r->arena_cap) {
                *off = r->tail;
                return 0;
            }
            if (size <= oldest) {
                *off = 0;
                return 0;
            }
        } else if (r->tail + size <= oldest) { /* occupied [oldest, end) + [0, tail) */
            *off = r->tail;
            return 0;
        }
        drop_oldest(r);
    }
}

static uint8_t *put_varint(uint8_t *p, size_t v)
{
    while (v >= 0x80) {
        *p++ = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    *p++ = (uint8_t)v;
    return p;
}

static const uint8_t *get_varint(const uint8_t *p, size_t *v)
{
    size_t x = 0;
    for (int shift = 0;; shift += 7) {
        uint8_t b = *p++;
        x |= (size_t)(b & 0x7F) << shift;
        if (!(b & 0x80))
            break;
    }
    *v = x;
    return p;
}

/* Run-length codes x[0..n) into out; returns the coded length. Zero runs
 * shorter than 8 bytes stay in the literal, so short gaps cost nothing. */
static size_t encode(const uint8_t *x, size_t n, uint8_t *out)
{
    uint8_t *p = out;
    size_t i = 0;
    while (i < n) {
        size_t z = i;
        while (z + 8 <= n) {
            uint64_t w;
            memcpy(&w, x + z, 8);
            if (w)
                break;
            z += 8;
        }
        while (z < n && !x[z])
            z++;
        size_t lit = z;
        /* Literal until a zero run of at least 8 bytes or the end. */
        while (lit < n) {
            if (!x[lit]) {
                size_t run = lit;
                while (run < n && run - lit < 8 && !x[run])
                    run++;
                if (run - lit >= 8 || run == n)
                    break;
                lit = run;
            } else {
                lit++;
            }
        }
        p = put_varint(p, z - i);
        p = put_varint(p, lit - z);
        memcpy(p, x + z, lit - z);
        p += lit - z;
        i = lit;
    }
    return (size_t)(p - out);
}

int np_rewind_push(np_rewind *r, const void *state, size_t len)
{
    if (grow(r, len)) {
        np_rewind_clear(r);
        return -1;
    }
    if (r->have_latest) {
        size_t span = len > r->latest_len ? len : r->latest_len;
        const uint8_t *s = state;
        /* x = new ^ latest; latest is zero past its length, new is read
         * only within its own. */
        size_t common = len < span ? len : span;
        size_t k = 0;
        for (; k + 8 <= common; k += 8) {
            uint64_t a, b;
            memcpy(&a, s + k, 8);
            memcpy(&b, r->latest + k, 8);
            a ^= b;
            memcpy(r->xor_buf + k, &a, 8);
        }
        for (; k < common; k++)
            r->xor_buf[k] = s[k] ^ r->latest[k];
        memcpy(r->xor_buf + common, r->latest + common, span - common);

        delta_header h = {0, span, r->latest_len};
        h.coded = encode(r->xor_buf, span, r->coded_buf + sizeof h);
        memcpy(r->coded_buf, &h, sizeof h);
        size_t size = (sizeof h + (size_t)h.coded + 7) & ~(size_t)7;
        size_t off;
        if (place(r, size, &off)) {
            np_rewind_clear(r); /* one delta bigger than the whole budget */
        } else {
            memcpy(r->arena + off, r->coded_buf, sizeof h + (size_t)h.coded);
            *at(r, r->count) = (entry){off, size};
            r->count++;
            r->used += size;
            r->tail = off + size;
        }
    }
    memcpy(r->latest, state, len);
    if (r->latest_len > len)
        memset(r->latest + len, 0, r->latest_len - len);
    r->latest_len = len;
    r->have_latest = 1;
    return 0;
}

int np_rewind_step_back(np_rewind *r, const uint8_t **state, size_t *len)
{
    if (!r->count)
        return -1;
    entry *e = at(r, r->count - 1);
    delta_header h;
    memcpy(&h, r->arena + e->off, sizeof h);
    const uint8_t *p = r->arena + e->off + sizeof h, *end = p + h.coded;
    size_t i = 0;
    while (p < end) {
        size_t zeros, lit;
        p = get_varint(p, &zeros);
        p = get_varint(p, &lit);
        i += zeros;
        for (size_t k = 0; k < lit; k++)
            r->latest[i + k] ^= p[k];
        p += lit;
        i += lit;
    }
    /* Bytes past the older snapshot's length are zero again by now. */
    r->latest_len = (size_t)h.prev_len;
    r->used -= e->size;
    r->count--;
    r->tail = r->count ? at(r, r->count - 1)->off + at(r, r->count - 1)->size : 0;
    *state = r->latest;
    *len = r->latest_len;
    return 0;
}
