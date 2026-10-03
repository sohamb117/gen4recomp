/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdlib.h>

#include "ndsdata/ndsdata.h"
#include "testutil.h"

static int decomp_eq(const uint8_t *src, size_t n, const void *expect, size_t elen)
{
    uint8_t *out = NULL;
    size_t olen = 0;
    nd_status st = nd_lz_decompress_alloc(src, n, &out, &olen);
    int ok = st == ND_OK && olen == elen && memcmp(out, expect, elen) == 0;
    free(out);
    return ok;
}

/* Reference greedy encoders (test-only) used for round-trip checks. */
static size_t find_match(const uint8_t *d, size_t n, size_t pos, size_t maxlen, size_t *disp)
{
    size_t best = 0;
    size_t start = pos > 4096 ? pos - 4096 : 0;
    for (size_t c = start; c < pos; c++) {
        size_t l = 0;
        while (l < maxlen && pos + l < n && d[c + l] == d[pos + l])
            l++;
        if (l > best) {
            best = l;
            *disp = pos - c;
        }
    }
    return best;
}

static size_t lz_encode(const uint8_t *d, size_t n, uint8_t *out, int lz11)
{
    size_t o = 0;
    out[o++] = lz11 ? 0x11 : 0x10;
    out[o++] = (uint8_t)n;
    out[o++] = (uint8_t)(n >> 8);
    out[o++] = (uint8_t)(n >> 16);
    size_t pos = 0;
    while (pos < n) {
        size_t flag_at = o++;
        uint8_t flags = 0;
        for (int bit = 0; bit < 8 && pos < n; bit++) {
            size_t disp = 0;
            size_t len = find_match(d, n, pos, lz11 ? 0x10110 : 18, &disp);
            if (len >= 3) {
                flags |= (uint8_t)(0x80 >> bit);
                size_t dd = disp - 1;
                if (!lz11) {
                    out[o++] = (uint8_t)(((len - 3) << 4) | (dd >> 8));
                    out[o++] = (uint8_t)dd;
                } else if (len <= 0x10) {
                    out[o++] = (uint8_t)(((len - 1) << 4) | (dd >> 8));
                    out[o++] = (uint8_t)dd;
                } else if (len <= 0x110) {
                    size_t l = len - 0x11;
                    out[o++] = (uint8_t)(l >> 4);
                    out[o++] = (uint8_t)(((l & 0xF) << 4) | (dd >> 8));
                    out[o++] = (uint8_t)dd;
                } else {
                    size_t l = len - 0x111;
                    out[o++] = (uint8_t)(0x10 | (l >> 12));
                    out[o++] = (uint8_t)(l >> 4);
                    out[o++] = (uint8_t)(((l & 0xF) << 4) | (dd >> 8));
                    out[o++] = (uint8_t)dd;
                }
                pos += len;
            } else {
                out[o++] = d[pos++];
            }
        }
        out[flag_at] = flags;
    }
    return o;
}

int main(void)
{
    /* LZ10: "ABC" + back-reference (len 9, disp 3) */
    static const uint8_t v1[] = {0x10, 0x0C, 0x00, 0x00, 0x10, 'A', 'B', 'C', 0x60, 0x02};
    CHECK(decomp_eq(v1, sizeof(v1), "ABCABCABCABC", 12));

    /* LZ10 extended header (24-bit size 0, u32 size follows) */
    static const uint8_t v2[] = {0x10, 0, 0, 0, 3, 0, 0, 0, 0x00, 'a', 'b', 'c'};
    CHECK(decomp_eq(v2, sizeof(v2), "abc", 3));

    /* LZ11 short form: len (b0>>4)+1 */
    static const uint8_t v3[] = {0x11, 0x06, 0x00, 0x00, 0x20, 'X', 'Y', 0x30, 0x01};
    CHECK(decomp_eq(v3, sizeof(v3), "XYXYXY", 6));

    /* LZ11 3-byte form: len 39 (0x11 + 0x16), disp 1 */
    static const uint8_t v4[] = {0x11, 40, 0x00, 0x00, 0x40, 'A', 0x01, 0x60, 0x00};
    char forty[40];
    memset(forty, 'A', sizeof(forty));
    CHECK(decomp_eq(v4, sizeof(v4), forty, sizeof(forty)));

    /* LZ11 4-byte form: len 0x1FF (0x111 + 0xEE), disp 1 */
    static const uint8_t v5[] = {0x11, 0x00, 0x02, 0x00, 0x40, 0x00, 0x10, 0x0E, 0xE0, 0x00};
    static uint8_t zeros[0x200];
    CHECK(decomp_eq(v5, sizeof(v5), zeros, sizeof(zeros)));

    /* Size query */
    size_t sz = 0;
    CHECK(nd_lz_size(v5, sizeof(v5), &sz) == ND_OK && sz == 0x200);

    /* Errors: bad type, reference before start, truncated stream, small dst */
    static const uint8_t bad_type[] = {0x30, 0x01, 0, 0, 0, 0};
    CHECK(nd_lz_size(bad_type, sizeof(bad_type), &sz) == ND_ERR_FORMAT);
    static const uint8_t bad_disp[] = {0x10, 0x03, 0, 0, 0x80, 0x00, 0x05};
    uint8_t tmp[16];
    CHECK(nd_lz_decompress(bad_disp, sizeof(bad_disp), tmp, sizeof(tmp)) == ND_ERR_FORMAT);
    CHECK(nd_lz_decompress(v1, sizeof(v1) - 1, tmp, sizeof(tmp)) == ND_ERR_FORMAT);
    CHECK(nd_lz_decompress(v1, sizeof(v1), tmp, 4) == ND_ERR_RANGE);

    /* Round trips through reference encoders on structured pseudo-random data */
    enum { N = 9000 };
    uint8_t *data = malloc(N), *enc = malloc(N * 2), *dec = malloc(N);
    uint32_t x = 12345;
    for (size_t i = 0; i < N; i++) {
        x = x * 1103515245u + 12345u;
        data[i] = (i % 700 < 350) ? (uint8_t)("pokemon platinum "[i % 17]) : (uint8_t)(x >> 28);
    }
    for (int lz11 = 0; lz11 < 2; lz11++) {
        size_t el = lz_encode(data, N, enc, lz11);
        CHECK(el < N);
        memset(dec, 0, N);
        CHECK(nd_lz_decompress(enc, el, dec, N) == ND_OK);
        CHECK(memcmp(dec, data, N) == 0);
    }
    free(data);
    free(enc);
    free(dec);
    return TEST_RESULT();
}
