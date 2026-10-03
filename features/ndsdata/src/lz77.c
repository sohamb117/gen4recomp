/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Nintendo LZ77 decompression (BIOS LZ77UnComp type 0x10, and the LZ11
 * extension type 0x11 used by later SDK tools).
 *
 * Header: u8 type, u24 decompressed size (little endian). If the 24-bit size
 * is 0, a u32 size follows (extended header). Then groups of a flag byte
 * (MSB first, 1 = back-reference) followed by 8 tokens.
 *  0x10 reference: 2 bytes, len = (b0 >> 4) + 3, disp = ((b0 & 15) << 8 | b1) + 1
 *  0x11 reference: by b0 >> 4:
 *    0:   3 bytes, len = ((b0 & 15) << 4 | b1 >> 4) + 0x11
 *    1:   4 bytes, len = ((b0 & 15) << 12 | b1 << 4 | b2 >> 4) + 0x111
 *    2-F: 2 bytes, len = (b0 >> 4) + 1
 *    disp = ((last-but-one byte & 15) << 8 | last byte) + 1
 */
#include "ndsdata/ndsdata.h"

#include <stdlib.h>

static nd_status lz_header(const uint8_t *src, size_t src_len, size_t *out_size, size_t *hdr_len)
{
    if (src_len < 4 || (src[0] != 0x10 && src[0] != 0x11))
        return ND_ERR_FORMAT;
    size_t size = (size_t)src[1] | ((size_t)src[2] << 8) | ((size_t)src[3] << 16);
    size_t h = 4;
    if (size == 0) {
        if (src_len < 8)
            return ND_ERR_FORMAT;
        size = (size_t)src[4] | ((size_t)src[5] << 8) | ((size_t)src[6] << 16) | ((size_t)src[7] << 24);
        h = 8;
    }
    *out_size = size;
    *hdr_len = h;
    return ND_OK;
}

nd_status nd_lz_size(const uint8_t *src, size_t src_len, size_t *out_size)
{
    size_t h;
    return lz_header(src, src_len, out_size, &h);
}

nd_status nd_lz_decompress(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len)
{
    size_t size, i;
    nd_status st = lz_header(src, src_len, &size, &i);
    if (st != ND_OK)
        return st;
    if (dst_len < size)
        return ND_ERR_RANGE;
    const int lz11 = src[0] == 0x11;
    size_t o = 0;
    while (o < size) {
        if (i >= src_len)
            return ND_ERR_FORMAT;
        uint8_t flags = src[i++];
        for (int bit = 0; bit < 8 && o < size; bit++, flags <<= 1) {
            if (!(flags & 0x80)) {
                if (i >= src_len)
                    return ND_ERR_FORMAT;
                dst[o++] = src[i++];
                continue;
            }
            size_t len, disp;
            if (i + 2 > src_len)
                return ND_ERR_FORMAT;
            uint8_t b0 = src[i];
            if (!lz11) {
                len = (size_t)(b0 >> 4) + 3;
                disp = ((size_t)(b0 & 0x0F) << 8 | src[i + 1]) + 1;
                i += 2;
            } else {
                switch (b0 >> 4) {
                case 0:
                    if (i + 3 > src_len)
                        return ND_ERR_FORMAT;
                    len = ((size_t)(b0 & 0x0F) << 4 | (size_t)(src[i + 1] >> 4)) + 0x11;
                    disp = ((size_t)(src[i + 1] & 0x0F) << 8 | src[i + 2]) + 1;
                    i += 3;
                    break;
                case 1:
                    if (i + 4 > src_len)
                        return ND_ERR_FORMAT;
                    len = ((size_t)(b0 & 0x0F) << 12 | (size_t)src[i + 1] << 4 | (size_t)(src[i + 2] >> 4)) + 0x111;
                    disp = ((size_t)(src[i + 2] & 0x0F) << 8 | src[i + 3]) + 1;
                    i += 4;
                    break;
                default:
                    len = (size_t)(b0 >> 4) + 1;
                    disp = ((size_t)(b0 & 0x0F) << 8 | src[i + 1]) + 1;
                    i += 2;
                    break;
                }
            }
            if (disp > o)
                return ND_ERR_FORMAT;
            if (len > size - o)
                len = size - o; /* the BIOS stops once the output size is reached */
            for (size_t k = 0; k < len; k++, o++)
                dst[o] = dst[o - disp];
        }
    }
    return ND_OK;
}

nd_status nd_lz_decompress_alloc(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len)
{
    size_t size;
    nd_status st = nd_lz_size(src, src_len, &size);
    if (st != ND_OK)
        return st;
    uint8_t *buf = malloc(size ? size : 1);
    if (!buf)
        return ND_ERR_NOMEM;
    if ((st = nd_lz_decompress(src, src_len, buf, size)) != ND_OK) {
        free(buf);
        return st;
    }
    *out = buf;
    *out_len = size;
    return ND_OK;
}
