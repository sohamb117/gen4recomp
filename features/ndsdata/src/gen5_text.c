/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Gen 5 (Black/White) text and message banks.
 *
 * Message bank (one member of the message NARC a/0/0/2), as read from the
 * US ROMs:
 *   header: u16 section count, u16 entry count, u32 section size,
 *           u32 0, u32 section offset[section count]
 *   section (the US ROMs have one): u32 size, then per entry
 *           u32 offset (from the section start), u16 length in code units,
 *           u16 attributes
 *   entry i: UTF-16 code units XORed with a key that starts at
 *           0x7C89 + i * 0x2983 (mod 2^16) and rotates left by 3 bits after
 *           every unit; the decrypted string ends in 0xFFFF.
 *
 * Text is UTF-16 with game-specific codes: 0xFFFF end, 0xFFFE line break,
 * 0xF000 control (command, argc, args...), 0xF100 packed text, and the
 * private glyphs 0x246D / 0x246E the font draws as the male / female signs
 * ("Nidoran" + 0x246E is species 29 in the species bank).
 */
#include <stdlib.h>
#include <string.h>

#include "gen5.h"

#define G5_KEY_START 0x7C89u
#define G5_KEY_STEP 0x2983u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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
static void puthex(sbuf *b, unsigned v)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 3; i >= 0; i--)
        put(b, &hex[(v >> (i * 4)) & 0xF], 1);
}
static void putcp(sbuf *b, uint32_t cp)
{
    char u[3];
    if (cp < 0x80) {
        u[0] = (char)cp;
        put(b, u, 1);
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | cp >> 6);
        u[1] = (char)(0x80 | (cp & 0x3F));
        put(b, u, 2);
    } else {
        u[0] = (char)(0xE0 | cp >> 12);
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        put(b, u, 3);
    }
}

size_t g5_text_decode(const uint16_t *codes, size_t n, char *out, size_t cap)
{
    sbuf b = {out, cap ? cap - 1 : 0, 0};
    for (size_t j = 0; j < n; j++) {
        const uint16_t c = codes[j];
        if (c == 0xFFFF)
            break;
        if (c == 0xFFFE) {
            put(&b, "\n", 1);
        } else if (c == 0xF000) {
            if (j + 2 >= n)
                break;
            const uint16_t cmd = codes[j + 1], argc = codes[j + 2];
            j += 3;
            puts_(&b, "{CMD_");
            puthex(&b, cmd);
            for (uint16_t k = 0; k < argc && j + k < n; k++) {
                put(&b, " ", 1);
                putu(&b, codes[j + k]);
                if (k + 1 != argc)
                    put(&b, ",", 1);
            }
            put(&b, "}", 1);
            j += argc;
            j--; /* loop increment */
        } else if (c == 0x246D) {
            putcp(&b, 0x2642);
        } else if (c == 0x246E) {
            putcp(&b, 0x2640);
        } else if (c == 0xF100 || (c >= 0xD800 && c < 0xE000)) {
            if (c >= 0xD800 && c < 0xDC00 && j + 1 < n && codes[j + 1] >= 0xDC00 && codes[j + 1] < 0xE000) {
                const uint32_t cp = 0x10000 + ((uint32_t)(c - 0xD800) << 10) + (uint32_t)(codes[j + 1] - 0xDC00);
                char u[4] = {(char)(0xF0 | cp >> 18), (char)(0x80 | ((cp >> 12) & 0x3F)),
                             (char)(0x80 | ((cp >> 6) & 0x3F)), (char)(0x80 | (cp & 0x3F))};
                put(&b, u, 4);
                j++;
            } else {
                puts_(&b, "\\x");
                puthex(&b, c);
            }
        } else {
            putcp(&b, c);
        }
    }
    if (cap)
        out[b.len < cap - 1 ? b.len : cap - 1] = 0;
    return b.len;
}

char *g5_text_decode_alloc(const uint16_t *codes, size_t n)
{
    size_t need = g5_text_decode(codes, n, NULL, 0);
    char *s = malloc(need + 1);
    if (!s)
        return NULL;
    g5_text_decode(codes, n, s, need + 1);
    return s;
}

nd_status g5_msgbank_strings(const uint8_t *data, size_t size, char ***list, uint32_t *count)
{
    if (size < 16)
        return ND_ERR_FORMAT;
    const uint16_t sections = rd16(data), n = rd16(data + 2);
    if (sections == 0 || 12 + (size_t)sections * 4 > size)
        return ND_ERR_FORMAT;
    const uint32_t sec = rd32(data + 12);
    if (sec > size || (size_t)n * 8 + 4 > size - sec)
        return ND_ERR_FORMAT;
    char **out = calloc(n ? n : 1, sizeof(char *));
    uint16_t *codes = NULL;
    size_t codes_cap = 0;
    if (!out)
        return ND_ERR_NOMEM;
    nd_status st = ND_OK;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = data + sec + 4 + i * 8;
        const uint32_t off = rd32(e);
        const uint16_t len = rd16(e + 4);
        if (off > size - sec || (size_t)len * 2 > size - sec - off) {
            st = ND_ERR_FORMAT;
            break;
        }
        if (len > codes_cap) {
            uint16_t *grown = realloc(codes, (size_t)len * sizeof *codes);
            if (!grown) {
                st = ND_ERR_NOMEM;
                break;
            }
            codes = grown;
            codes_cap = len;
        }
        const uint8_t *s = data + sec + off;
        uint16_t key = (uint16_t)(G5_KEY_START + i * G5_KEY_STEP);
        for (uint32_t j = 0; j < len; j++) {
            codes[j] = rd16(s + j * 2) ^ key;
            key = (uint16_t)(key << 3 | key >> 13);
        }
        if (!(out[i] = g5_text_decode_alloc(codes, len))) {
            st = ND_ERR_NOMEM;
            break;
        }
    }
    free(codes);
    if (st != ND_OK) {
        for (uint32_t i = 0; i < n; i++)
            free(out[i]);
        free(out);
        return st;
    }
    *list = out;
    *count = n;
    return ND_OK;
}
