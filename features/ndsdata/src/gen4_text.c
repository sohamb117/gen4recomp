/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Gen 4 charcode <-> UTF-8 and message bank decoding.
 *
 * Message bank format and key schedule follow pret/pokeplatinum
 * src/message.c (MessageBank_Get / DecodeEntry / DecodeString) and
 * tools/msgenc (MsgAlloc::decrypt, DecryptU16String):
 *   header: u16 count, u16 seed
 *   entries[count]: u32 offset, u32 length (in u16 units), each XORed with
 *       k | k << 16, k = (seed * 765 * (index + 1)) & 0xFFFF
 *   string: u16 codes XORed with a key starting at (index + 1) * 596947
 *       (truncated to u16), incremented by 18749 per code.
 * Rendering mirrors MessagesDecoder::DecodeMessage: 0xFFFE introduces a
 * command (code, argc, args...), 0xF100 a 9-bit packed trainer name,
 * 0xFFFF terminates.
 */
#include "ndsdata/ndsdata.h"

#include <stdlib.h>
#include <string.h>

#include "gen4_charmap.h"

#define MSG_KEY_START 596947u
#define MSG_KEY_INC 18749u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const char *lookup_code(const struct g4_char_entry *tab, size_t n, uint16_t code)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (tab[mid].code == code)
            return tab[mid].text;
        if (tab[mid].code < code)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

typedef struct sbuf {
    char *out;
    size_t cap, len;
} sbuf;

static void put(sbuf *b, const char *s, size_t n)
{
    if (b->len < b->cap) {
        size_t room = b->cap - b->len;
        memcpy(b->out + b->len, s, n < room ? n : room);
    }
    b->len += n;
}
static void puts_(sbuf *b, const char *s) { put(b, s, strlen(s)); }
static void putu(sbuf *b, unsigned v)
{
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        put(b, &tmp[--n], 1);
}
static void puthex(sbuf *b, unsigned v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--)
        put(b, &hex[(v >> (i * 4)) & 0xF], 1);
}

static int is_strvar(uint16_t code)
{
    for (size_t i = 0; i < g4_strvar_hi_count; i++)
        if (g4_strvar_hi[i] == (code & 0xFF00))
            return 1;
    return 0;
}

/* Unpack the 9-bit trainer-name encoding (MessagesDecoder::DecodeTrainerNameMessage). */
static size_t unpack_trname(const uint16_t *src, size_t n, uint16_t *dst, size_t cap)
{
    size_t out = 0, p = 1;
    int bit = 0;
    uint16_t c;
    do {
        if (p >= n)
            break;
        c = (uint16_t)((src[p] >> bit) & 0x1FF);
        bit += 9;
        if (bit >= 15) {
            p++;
            bit -= 15;
            if (bit != 0 && p < n)
                c |= (uint16_t)((src[p] << (9 - bit)) & 0x1FF);
        }
        if (out < cap)
            dst[out++] = c;
    } while (p < n && c != 0x1FF);
    return out;
}

static size_t decode_into(sbuf *b, const uint16_t *codes, size_t n, int trname)
{
    for (size_t j = 0; j < n; j++) {
        uint16_t code = codes[j];
        const char *s = lookup_code(g4_charmap, g4_charmap_count, code);
        if (s) {
            puts_(b, s);
        } else if (code == (trname ? 0x01FF : 0xFFFF)) {
            break;
        } else if (code == 0xFFFE) {
            if (j + 2 >= n) /* need cmd + argc */
                break;
            uint16_t cmd = codes[j + 1];
            uint16_t argc = codes[j + 2];
            j += 3;
            put(b, "{", 1);
            int strvar = is_strvar(cmd);
            if (strvar) {
                puts_(b, "STRVAR_");
                /* msgenc prints the high byte left-aligned without leading zeros */
                unsigned hi = cmd >> 8;
                if (hi >= 0x10)
                    puthex(b, hi, 2);
                else
                    puthex(b, hi, 1);
            } else {
                const char *name = lookup_code(g4_cmdmap, g4_cmdmap_count, cmd);
                if (name) {
                    puts_(b, name);
                } else {
                    puts_(b, "CMD_");
                    puthex(b, cmd, 4);
                }
            }
            if (strvar) {
                put(b, " ", 1);
                putu(b, cmd & 0xFF);
                if (argc)
                    put(b, ",", 1);
            }
            for (uint16_t k = 0; k < argc && j + k < n; k++) {
                put(b, " ", 1);
                putu(b, codes[j + k]);
                if (k + 1 != argc)
                    put(b, ",", 1);
            }
            put(b, "}", 1);
            j += argc;
            j--; /* loop increment */
        } else if (code == 0xF100 && !trname) {
            puts_(b, "{TRNAME}");
            uint16_t *tmp = malloc(n * 2 * sizeof(uint16_t) + 2);
            if (!tmp)
                break;
            size_t m = unpack_trname(codes, n, tmp, n * 2 + 1);
            decode_into(b, tmp, m, 1);
            free(tmp);
            break;
        } else {
            puts_(b, "\\x");
            puthex(b, code, 4);
        }
    }
    return b->len;
}

size_t g4_text_decode(const uint16_t *codes, size_t n, char *out, size_t cap)
{
    sbuf b = {out, cap ? cap - 1 : 0, 0};
    decode_into(&b, codes, n, 0);
    if (cap)
        out[b.len < cap - 1 ? b.len : cap - 1] = 0;
    return b.len;
}

char *g4_text_decode_alloc(const uint16_t *codes, size_t n)
{
    size_t need = g4_text_decode(codes, n, NULL, 0);
    char *s = malloc(need + 1);
    if (!s)
        return NULL;
    g4_text_decode(codes, n, s, need + 1);
    return s;
}

/* Find the encoding entry whose text equals s[0..len). */
static const struct g4_char_entry *find_enc(const char *s, size_t len)
{
    size_t lo = 0, hi = g4_charmap_enc_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const char *t = g4_charmap_enc[mid].text;
        size_t tl = strlen(t);
        int c = memcmp(t, s, tl < len ? tl : len);
        if (c == 0)
            c = tl < len ? -1 : (tl > len ? 1 : 0);
        if (c == 0)
            return &g4_charmap_enc[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static size_t max_enc_len(void)
{
    static size_t cached;
    if (!cached) {
        size_t m = 1;
        for (size_t i = 0; i < g4_charmap_enc_count; i++) {
            size_t l = strlen(g4_charmap_enc[i].text);
            if (l > m)
                m = l;
        }
        cached = m;
    }
    return cached;
}

nd_status g4_text_encode(const char *utf8, uint16_t *out, size_t cap, size_t *out_len)
{
    size_t n = 0, pos = 0, total = strlen(utf8), maxl = max_enc_len();
    while (pos < total) {
        const struct g4_char_entry *e = NULL;
        size_t l = maxl < total - pos ? maxl : total - pos;
        for (; l > 0; l--) {
            if ((e = find_enc(utf8 + pos, l)) != NULL)
                break;
        }
        if (!e)
            return ND_ERR_FORMAT;
        if (n + 1 >= cap)
            return ND_ERR_RANGE;
        out[n++] = e->code;
        pos += l;
    }
    if (n >= cap)
        return ND_ERR_RANGE;
    out[n] = 0xFFFF;
    if (out_len)
        *out_len = n;
    return ND_OK;
}

/* --------------------------------------------------------- message banks */

nd_status nd_msgbank_parse(nd_msgbank *bank, const uint8_t *data, size_t size)
{
    if (size < 4)
        return ND_ERR_FORMAT;
    bank->data = data;
    bank->size = size;
    bank->count = rd16(data);
    bank->seed = rd16(data + 2);
    if (4 + (size_t)bank->count * 8 > size)
        return ND_ERR_FORMAT;
    return ND_OK;
}

nd_status nd_msgbank_get_codes(const nd_msgbank *bank, uint32_t index, uint16_t *dst, size_t cap, size_t *len)
{
    if (index >= bank->count)
        return ND_ERR_NOT_FOUND;
    const uint8_t *e = bank->data + 4 + index * 8;
    uint32_t k = (bank->seed * 765u * (index + 1)) & 0xFFFF;
    k |= k << 16;
    uint32_t off = rd32(e) ^ k;
    uint32_t n = rd32(e + 4) ^ k;
    if (off > bank->size || (uint64_t)n * 2 > bank->size - off)
        return ND_ERR_FORMAT;
    *len = n;
    if (!dst || cap < n)
        return ND_ERR_RANGE;
    uint16_t key = (uint16_t)((index + 1) * MSG_KEY_START);
    const uint8_t *s = bank->data + off;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = rd16(s + i * 2) ^ key;
        key = (uint16_t)(key + MSG_KEY_INC);
    }
    return ND_OK;
}

nd_status nd_msgbank_get_utf8(const nd_msgbank *bank, uint32_t index, char **out)
{
    size_t n = 0;
    nd_status st = nd_msgbank_get_codes(bank, index, NULL, 0, &n);
    if (st != ND_ERR_RANGE && st != ND_OK)
        return st;
    uint16_t *codes = malloc((n ? n : 1) * sizeof(uint16_t));
    if (!codes)
        return ND_ERR_NOMEM;
    if ((st = nd_msgbank_get_codes(bank, index, codes, n, &n)) != ND_OK) {
        free(codes);
        return st;
    }
    char *s = g4_text_decode_alloc(codes, n);
    free(codes);
    if (!s)
        return ND_ERR_NOMEM;
    *out = s;
    return ND_OK;
}
