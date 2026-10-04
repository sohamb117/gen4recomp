/*
 * ZIP reading and inflate (see zip.h). The decoder follows RFC 1951 the way
 * zlib's reference "puff" does: canonical Huffman codes decoded a bit at a
 * time from per-length counts. It is not the fastest inflate, but it is
 * small, exact and easy to audit, and mod packages and skins are a few MB.
 */
#include "zip.h"

#include <string.h>

#include "png.h" /* np_crc32 */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- inflate ------------------------------------------------------------------- */

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct inflate_state {
    const uint8_t *in;
    size_t in_len, in_pos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t out_len, out_pos;
} inflate_state;

typedef struct huffman {
    short count[MAXBITS + 1]; /* codes of each length */
    short symbol[FIXLCODES];  /* symbols in canonical order */
} huffman;

/* `need` bits, LSB first; -1 past the end of the input. */
static int bits(inflate_state *s, int need)
{
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos == s->in_len)
            return -1;
        val |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static int stored(inflate_state *s)
{
    s->bitbuf = 0; /* to a byte boundary */
    s->bitcnt = 0;
    if (s->in_len - s->in_pos < 4)
        return -1;
    unsigned len = rd16(s->in + s->in_pos), nlen = rd16(s->in + s->in_pos + 2);
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFFu) || s->in_len - s->in_pos < len || s->out_len - s->out_pos < len)
        return -1;
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len;
    s->out_pos += len;
    return 0;
}

static int decode(inflate_state *s, const huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int b = bits(s, 1);
        if (b < 0)
            return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1; /* ran out of codes */
}

/* Builds h from code lengths. Returns 0 for a complete code, > 0 for an
 * incomplete one, < 0 for an over-subscribed one. */
static int construct(huffman *h, const short *length, int n)
{
    memset(h->count, 0, sizeof h->count);
    for (int sym = 0; sym < n; sym++)
        h->count[length[sym]]++;
    if (h->count[0] == n)
        return 0; /* no codes: complete, but decode will fail */
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }
    short offs[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym])
            h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static int codes(inflate_state *s, const huffman *lencode, const huffman *distcode)
{
    static const short lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            if (s->out_pos == s->out_len)
                return -1;
            s->out[s->out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29)
                return -1;
            int e = bits(s, lext[sym]);
            if (e < 0)
                return -1;
            size_t len = (size_t)(lbase[sym] + e);
            int dsym = decode(s, distcode);
            if (dsym < 0 || dsym >= 30)
                return -1;
            e = bits(s, dext[dsym]);
            if (e < 0)
                return -1;
            size_t dist = (size_t)(dbase[dsym] + e);
            if (dist > s->out_pos || s->out_len - s->out_pos < len)
                return -1;
            for (; len; len--, s->out_pos++) /* may overlap: byte by byte */
                s->out[s->out_pos] = s->out[s->out_pos - dist];
        }
    }
}

static int fixed(inflate_state *s)
{
    static huffman lencode, distcode;
    static int built;
    if (!built) {
        short lengths[FIXLCODES];
        int sym = 0;
        for (; sym < 144; sym++)
            lengths[sym] = 8;
        for (; sym < 256; sym++)
            lengths[sym] = 9;
        for (; sym < 280; sym++)
            lengths[sym] = 7;
        for (; sym < FIXLCODES; sym++)
            lengths[sym] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; sym++)
            lengths[sym] = 5;
        construct(&distcode, lengths, MAXDCODES);
        built = 1;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(inflate_state *s)
{
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = bits(s, 5), ndist = bits(s, 5), ncode = bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0)
        return -1;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES)
        return -1;
    short lengths[MAXLCODES + MAXDCODES];
    int index;
    for (index = 0; index < ncode; index++) {
        int b = bits(s, 3);
        if (b < 0)
            return -1;
        lengths[order[index]] = (short)b;
    }
    for (; index < 19; index++)
        lengths[order[index]] = 0;
    huffman lencode, distcode;
    if (construct(&lencode, lengths, 19) != 0)
        return -1; /* the code-length code must be complete */
    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, &lencode);
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lengths[index++] = (short)sym;
            continue;
        }
        short len = 0;
        int rep;
        if (sym == 16) {
            if (index == 0)
                return -1;
            len = lengths[index - 1];
            rep = bits(s, 2);
            rep = rep < 0 ? -1 : 3 + rep;
        } else if (sym == 17) {
            rep = bits(s, 3);
            rep = rep < 0 ? -1 : 3 + rep;
        } else {
            rep = bits(s, 7);
            rep = rep < 0 ? -1 : 11 + rep;
        }
        if (rep < 0 || index + rep > nlen + ndist)
            return -1;
        while (rep--)
            lengths[index++] = len;
    }
    if (lengths[256] == 0)
        return -1; /* no end-of-block code */
    int err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
        return -1; /* incomplete only for a single code */
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
        return -1;
    return codes(s, &lencode, &distcode);
}

int np_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
    inflate_state s = {in, in_len, 0, 0, 0, out, out_len, 0};
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (last < 0 || type < 0)
            return -1;
        int err = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
        if (err)
            return -1;
    } while (!last);
    return s.out_pos == out_len ? 0 : -1;
}

/* ---- zip --------------------------------------------------------------------- */

int np_zip_open(np_zip *z, const void *data, size_t len)
{
    const uint8_t *p = data;
    memset(z, 0, sizeof *z);
    if (len < 22)
        return -1;
    /* The end record is in the last 22 + 65535 (comment) bytes. */
    size_t stop = len > 22 + 65535 ? len - 22 - 65535 : 0;
    for (size_t i = len - 22 + 1; i-- > stop;) {
        if (rd32(p + i) != 0x06054b50u)
            continue;
        if (rd16(p + i + 4) != 0 || rd16(p + i + 6) != 0)
            return -1; /* multi-disk */
        uint16_t count = rd16(p + i + 10);
        uint32_t size = rd32(p + i + 12), off = rd32(p + i + 16);
        if (off == 0xFFFFFFFFu || count == 0xFFFF || (size_t)off + size > i)
            return -1; /* ZIP64 or inconsistent */
        z->data = p;
        z->len = len;
        z->cdir = off;
        z->count = count;
        return 0;
    }
    return -1;
}

int np_zip_entry_at(const np_zip *z, uint32_t i, np_zip_entry *e)
{
    if (i >= z->count)
        return -1;
    size_t off = z->cdir;
    for (uint32_t k = 0;; k++) {
        if (off + 46 > z->len || rd32(z->data + off) != 0x02014b50u)
            return -1;
        const uint8_t *h = z->data + off;
        size_t nlen = rd16(h + 28), xlen = rd16(h + 30), clen = rd16(h + 32);
        if (off + 46 + nlen + xlen + clen > z->len)
            return -1;
        if (k == i) {
            if (nlen == 0 || nlen >= sizeof e->name)
                return -1;
            memset(e, 0, sizeof *e);
            memcpy(e->name, h + 46, nlen);
            if (memchr(e->name, 0, nlen))
                return -1;
            e->method = rd16(h + 10);
            e->crc = rd32(h + 16);
            e->csize = rd32(h + 20);
            e->size = rd32(h + 24);
            e->local = rd32(h + 42);
            e->is_dir = e->name[nlen - 1] == '/';
            uint32_t attr = rd32(h + 38);
            int unix_made = h[5] == 3; /* "version made by" host: Unix */
            e->is_symlink = unix_made && ((attr >> 16) & 0170000u) == 0120000u;
            return 0;
        }
        off += 46 + nlen + xlen + clen;
    }
}

int np_zip_find(const np_zip *z, const char *name, np_zip_entry *e)
{
    for (uint32_t i = 0; i < z->count; i++)
        if (!np_zip_entry_at(z, i, e) && !strcmp(e->name, name))
            return (int)i;
    return -1;
}

int np_zip_extract(const np_zip *z, const np_zip_entry *e, void *out)
{
    if (e->local + 30 > z->len || rd32(z->data + e->local) != 0x04034b50u)
        return -1;
    const uint8_t *h = z->data + e->local;
    size_t start = e->local + 30 + rd16(h + 26) + rd16(h + 28);
    if (start > z->len || z->len - start < e->csize)
        return -1;
    const uint8_t *src = z->data + start;
    if (e->method == 0) {
        if (e->csize != e->size)
            return -1;
        memcpy(out, src, e->size);
    } else if (e->method == 8) {
        if (np_inflate(src, e->csize, out, e->size))
            return -1;
    } else {
        return -1;
    }
    return np_crc32(0, out, e->size) == e->crc ? 0 : -1;
}

int np_zip_name_safe(const char *name)
{
    if (!name[0] || name[0] == '/' || strchr(name, '\\') || strchr(name, ':'))
        return 0;
    const char *p = name;
    while (*p) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
            return 0;
        if (!end)
            break;
        p = end + 1; /* a trailing '/' ends the loop on the empty rest */
    }
    return 1;
}
