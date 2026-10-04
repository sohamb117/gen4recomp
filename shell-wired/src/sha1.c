/*
 * Portable SHA-1. Hashing a 128 MiB Platinum dump must stay well under a
 * second on a phone, so blocks are consumed straight from the caller's
 * buffer whenever they are aligned to a block boundary instead of being
 * copied through `block`.
 */
#include "sha1.h"

#include <string.h>

static uint32_t rol(uint32_t v, unsigned n) { return (v << n) | (v >> (32u - n)); }

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 |
               (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void np_sha1_init(np_sha1 *s)
{
    s->h[0] = 0x67452301u;
    s->h[1] = 0xEFCDAB89u;
    s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xC3D2E1F0u;
    s->length = 0;
    s->used = 0;
}

void np_sha1_update(np_sha1 *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->length += len;
    if (s->used) {
        size_t take = 64u - s->used;
        if (take > len)
            take = len;
        memcpy(s->block + s->used, p, take);
        s->used += (uint32_t)take;
        p += take;
        len -= take;
        if (s->used < 64)
            return;
        sha1_block(s->h, s->block);
        s->used = 0;
    }
    while (len >= 64) {
        sha1_block(s->h, p);
        p += 64;
        len -= 64;
    }
    memcpy(s->block, p, len);
    s->used = (uint32_t)len;
}

void np_sha1_final(np_sha1 *s, uint8_t digest[20])
{
    uint64_t bits = s->length * 8u;
    s->block[s->used++] = 0x80;
    if (s->used > 56) {
        memset(s->block + s->used, 0, 64u - s->used);
        sha1_block(s->h, s->block);
        s->used = 0;
    }
    memset(s->block + s->used, 0, 56u - s->used);
    for (int i = 0; i < 8; i++)
        s->block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_block(s->h, s->block);
    for (int i = 0; i < 5; i++) {
        digest[4 * i] = (uint8_t)(s->h[i] >> 24);
        digest[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        digest[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        digest[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void np_sha1_hex(const uint8_t digest[20], char hex[41])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 15];
    }
    hex[40] = '\0';
}
