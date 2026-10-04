/*
 * PNG = signature + IHDR + one IDAT holding a zlib stream of stored deflate
 * blocks + IEND. Because stored blocks have a fixed overhead, the IDAT length
 * is known up front and the image is streamed row by row through the sink
 * with only one row of scratch memory.
 */
#include "png.h"

#include <stdlib.h>

static uint32_t crc_table[256];
static int crc_ready;

uint32_t np_crc32(uint32_t crc, const void *data, size_t len)
{
    if (!crc_ready) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[n] = c;
        }
        crc_ready = 1;
    }
    const uint8_t *p = data;
    crc = ~crc;
    while (len--)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t np_adler32(uint32_t adler, const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t a = adler & 0xFFFF, b = adler >> 16;
    while (len) {
        /* 5552 is the largest run that cannot overflow 32 bits before mod. */
        size_t n = len < 5552 ? len : 5552;
        len -= n;
        while (n--) {
            a += *p++;
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    return b << 16 | a;
}

typedef struct png_out {
    np_png_sink sink;
    void *user;
    uint32_t crc;  /* of the chunk being written */
    uint32_t adler;
    uint64_t raw_left;   /* uncompressed bytes not yet emitted */
    uint32_t block_left; /* bytes left in the current stored block */
    int failed;
} png_out;

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void emit(png_out *o, const void *data, size_t len)
{
    if (o->failed || !len)
        return;
    o->crc = np_crc32(o->crc, data, len);
    if (o->sink(o->user, data, len))
        o->failed = 1;
}

static void chunk_begin(png_out *o, const char type[4], uint32_t len)
{
    uint8_t hdr[4];
    put_be32(hdr, len);
    if (!o->failed && o->sink(o->user, hdr, 4))
        o->failed = 1;
    o->crc = 0;
    emit(o, type, 4);
}

static void chunk_end(png_out *o)
{
    uint8_t c[4];
    put_be32(c, o->crc);
    if (!o->failed && o->sink(o->user, c, 4))
        o->failed = 1;
}

/* Uncompressed image bytes, wrapped into stored blocks as they go. */
static void emit_raw(png_out *o, const uint8_t *data, size_t len)
{
    o->adler = np_adler32(o->adler, data, len);
    while (len) {
        if (!o->block_left) {
            uint32_t n = o->raw_left > 65535u ? 65535u : (uint32_t)o->raw_left;
            uint8_t h[5] = {(uint8_t)(o->raw_left == n), (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)~n,
                            (uint8_t)(~n >> 8)};
            emit(o, h, 5);
            o->block_left = n;
        }
        size_t take = len < o->block_left ? len : o->block_left;
        emit(o, data, take);
        data += take;
        len -= take;
        o->block_left -= (uint32_t)take;
        o->raw_left -= take;
    }
}

int np_png_encode(np_png_sink sink, void *user, const uint32_t *xrgb, uint32_t width, uint32_t height,
                  size_t stride)
{
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png_out o = {sink, user, 0, 1, 0, 0, 0};
    if (!width || !height || sink(user, signature, 8))
        return -1;

    uint8_t ihdr[13];
    put_be32(ihdr, width);
    put_be32(ihdr + 4, height);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* truecolor RGB */
    ihdr[10] = 0; /* deflate */
    ihdr[11] = 0; /* adaptive filtering (we always pick filter 0) */
    ihdr[12] = 0; /* no interlace */
    chunk_begin(&o, "IHDR", 13);
    emit(&o, ihdr, 13);
    chunk_end(&o);

    size_t row_bytes = 1 + (size_t)width * 3;
    uint64_t raw = (uint64_t)row_bytes * height;
    uint64_t blocks = (raw + 65534u) / 65535u;
    uint64_t idat_len = 2 + raw + 5 * blocks + 4;
    if (idat_len > 0x7FFFFFFFu)
        return -1;
    uint8_t *row = malloc(row_bytes);
    if (!row)
        return -1;

    chunk_begin(&o, "IDAT", (uint32_t)idat_len);
    static const uint8_t zlib_header[2] = {0x78, 0x01};
    emit(&o, zlib_header, 2);
    o.raw_left = raw;
    for (uint32_t y = 0; y < height && !o.failed; y++) {
        const uint32_t *src = xrgb + (size_t)y * stride;
        row[0] = 0;
        for (uint32_t x = 0; x < width; x++) {
            row[1 + 3 * x] = (uint8_t)(src[x] >> 16);
            row[2 + 3 * x] = (uint8_t)(src[x] >> 8);
            row[3 + 3 * x] = (uint8_t)src[x];
        }
        emit_raw(&o, row, row_bytes);
    }
    free(row);
    uint8_t adler[4];
    put_be32(adler, o.adler);
    emit(&o, adler, 4);
    chunk_end(&o);

    chunk_begin(&o, "IEND", 0);
    chunk_end(&o);
    return o.failed ? -1 : 0;
}
