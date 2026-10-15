/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Backward LZ ("BLZ"): the compression the DS SDK applies to the ARM9 binary
 * and to overlays (flag bit 0 of an overlay table entry's last byte). The
 * image decompresses in place from its end towards its start:
 *
 *   footer, last 8 bytes: u32 (header length << 24 | compressed length),
 *                         u32 size increase (0: not compressed)
 *   [0, len - compressed length)        stored as is
 *   [len - compressed, len - header)    compressed stream, read backwards
 *   output                              len + size increase bytes; the
 *                                       decoded bytes fill it from the top
 *
 * Read backwards, the stream is groups of a flag byte (MSB first,
 * 1 = back-reference) and 8 tokens. A literal is one byte; a reference is a
 * u16 read as (byte at lower address) | (byte above) << 8 with
 * length = (v >> 12) + 3 and distance = (v & 0xFFF) + 3, copying from that
 * far above the write position.
 */
#include "ndsdata/ndsdata.h"

#include <stdlib.h>
#include <string.h>

#define BLZ_MAX_OUT (64u * 1024u * 1024u)

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

nd_status nd_blz_decompress(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len)
{
    if (src_len < 8)
        return ND_ERR_FORMAT;
    const uint32_t inc = rd32(src + src_len - 4);
    const uint32_t w = rd32(src + src_len - 8);
    const size_t hdr = w >> 24, enc = w & 0xFFFFFF;
    if (inc == 0) {
        uint8_t *copy = malloc(src_len);
        if (!copy)
            return ND_ERR_NOMEM;
        memcpy(copy, src, src_len);
        *out = copy;
        *out_len = src_len;
        return ND_OK;
    }
    if (hdr < 8 || hdr > enc || enc > src_len || inc > BLZ_MAX_OUT)
        return ND_ERR_FORMAT;
    const size_t total = src_len + inc;
    uint8_t *buf = malloc(total);
    if (!buf)
        return ND_ERR_NOMEM;
    memcpy(buf, src, src_len);
    const size_t end = src_len - enc;
    size_t s = src_len - hdr, d = total;
    while (s > end) {
        uint8_t flags = src[--s];
        for (int bit = 0; bit < 8 && s > end; bit++, flags = (uint8_t)(flags << 1)) {
            if (flags & 0x80) {
                if (s - end < 2)
                    goto bad;
                s -= 2;
                const unsigned v = (unsigned)src[s] | (unsigned)src[s + 1] << 8;
                const size_t n = (v >> 12) + 3, disp = (v & 0xFFF) + 3;
                if (d - end < n || d + disp > total)
                    goto bad;
                for (size_t k = 0; k < n; k++) {
                    d--;
                    buf[d] = buf[d + disp];
                }
            } else {
                if (d <= end)
                    goto bad;
                buf[--d] = src[--s];
            }
        }
    }
    *out = buf;
    *out_len = total;
    return ND_OK;
bad:
    free(buf);
    return ND_ERR_FORMAT;
}
