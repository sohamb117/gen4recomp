/*
 * The truecolour PNG encoder, shared by the port and the viewer.
 *
 * NOT libpng: the port is built -m32 and the libpng on this machine is not,
 * so linking it would make the port's ABI requirement contagious to a
 * dependency it does not otherwise have. What is needed is one colour type,
 * one bit depth, no interlacing and no palette, a couple of hundred lines
 * with a specification behind every one of them (RFC 1950, RFC 1951, the PNG
 * spec).
 *
 * It compresses because a stored-block PNG of two DS screens is 295 KB
 * whatever is on them; static-Huffman deflate with a one-slot hash chain
 * makes a black frame 1,927 bytes. Nothing here has to decompress, which is
 * the half of deflate that is hard.
 *
 * DETERMINISM. No timestamps and no host state: the same pixels give the
 * same bytes. The port's frame dumps are pinned on that, so this is the code
 * that produced them, moved and not rewritten.
 *
 * A header of static functions, because its two callers are different ABIs;
 * the port is -m32, the viewer is the host's native 64-bit, and there is no
 * library either could link. Same arrangement pc_view.h uses for the audio
 * reader, for the same reason. Every buffer is the caller's: the port dumps
 * at a fixed DS resolution into static arrays and the viewer screenshots at
 * whatever size the window is, so one shared buffer would have to be sized
 * for the larger and would be wrong for both.
 */

#ifndef POKEPLATINUM_PC_PNG_H
#define POKEPLATINUM_PC_PNG_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* CRC-32 and Adler-32                                                 */
/* ------------------------------------------------------------------ */

static uint32_t crc_table[256];
static int crc_table_ready;

static void crc_build(void) {
    uint32_t n, c;
    int k;

    for (n = 0; n < 256; n++) {
        c = n;
        for (k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc_table[n] = c;
    }
    crc_table_ready = 1;
}

static uint32_t crc_update(uint32_t c, const unsigned char *p, size_t n) {
    size_t i;

    if (!crc_table_ready) crc_build();
    for (i = 0; i < n; i++) {
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }
    return c;
}

/* RFC 1950 section 9. */
static uint32_t adler32(const unsigned char *p, size_t n) {
    uint32_t a = 1, b = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        a += p[i];
        if (a >= 65521u) a -= 65521u;
        b += a;
        if (b >= 65521u) b -= 65521u;
    }
    return (b << 16) | a;
}

/* ------------------------------------------------------------------ */
/* Deflate, RFC 1951, fixed Huffman codes only                         */
/* ------------------------------------------------------------------ */

/*
 * The bit writer. Deflate packs bits into bytes starting at the least
 * significant bit, *except* that Huffman codes are written most significant
 * bit first; which is why dz_code() reverses and dz_bits() does not.
 */
static unsigned char *dz_out;
static size_t dz_cap, dz_len;
static uint32_t dz_acc;
static int dz_nbits;
static int dz_overflow;

static void dz_byte(unsigned char b) {
    if (dz_len < dz_cap) dz_out[dz_len] = b;
    else dz_overflow = 1;
    dz_len++;
}

static void dz_bits(uint32_t v, int n) {
    dz_acc |= (v & ((1u << n) - 1u)) << dz_nbits;
    dz_nbits += n;
    while (dz_nbits >= 8) {
        dz_byte((unsigned char)(dz_acc & 0xFF));
        dz_acc >>= 8;
        dz_nbits -= 8;
    }
}

static void dz_code(uint32_t code, int n) {
    uint32_t r = 0;
    int i;

    for (i = 0; i < n; i++) r = (r << 1) | ((code >> i) & 1u);
    dz_bits(r, n);
}

/* RFC 1951 section 3.2.6: the fixed literal/length alphabet. */
static void dz_literal(unsigned c) {
    if (c <= 143) dz_code(0x30u + c, 8);
    else          dz_code(0x190u + c - 144u, 9);
}

static void dz_end_of_block(void) {
    dz_code(0, 7);      /* symbol 256 */
}

static const uint16_t dz_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t dz_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dz_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t dz_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static void dz_match(unsigned len, unsigned dist) {
    unsigned sym, i, j;

    for (i = 28; i > 0; i--) {
        if (dz_len_base[i] <= len) break;
    }
    sym = 257u + i;
    if (sym <= 279u) dz_code(sym - 256u, 7);
    else             dz_code(0xC0u + sym - 280u, 8);
    if (dz_len_extra[i]) dz_bits(len - dz_len_base[i], dz_len_extra[i]);

    for (j = 29; j > 0; j--) {
        if (dz_dist_base[j] <= dist) break;
    }
    dz_code(j, 5);
    if (dz_dist_extra[j]) dz_bits(dist - dz_dist_base[j], dz_dist_extra[j]);
}

/*
 * One hash slot per 3-byte prefix, no chain. That is the cheapest match finder
 * that is still a real one, and it is enough for what this compresses: a
 * framebuffer is long runs and repeated rows, both of which the most recent
 * occurrence already matches.
 */
#define DZ_HASH_BITS 15
#define DZ_HASH_SIZE (1 << DZ_HASH_BITS)
#define DZ_MAX_MATCH 258
#define DZ_MIN_MATCH 3
#define DZ_WINDOW    32768

static int32_t dz_head[DZ_HASH_SIZE];

static uint32_t dz_hash(const unsigned char *p) {
    return (uint32_t)(((p[0] << 10) ^ (p[1] << 5) ^ p[2]) & (DZ_HASH_SIZE - 1));
}

/*
 * Compress `src` into `dst` as a single final static-Huffman block wrapped in
 * a zlib stream. Returns the byte count, or 0 if it did not fit; which is a
 * loud failure at the call site rather than a truncated file.
 */
static size_t dz_compress(unsigned char *dst, size_t cap,
                          const unsigned char *src, size_t n) {
    size_t i = 0, k;
    uint32_t sum;

    if (cap < 6) return 0;

    /* RFC 1950 section 2.2: CM=8, CINFO=7 (32 KB window), no preset
     * dictionary, and the check byte that makes the pair a multiple of 31. */
    dst[0] = 0x78;
    dst[1] = 0x01;

    dz_out = dst + 2;
    dz_cap = cap - 6;       /* two header bytes and four for the Adler sum */
    dz_len = 0;
    dz_acc = 0;
    dz_nbits = 0;
    dz_overflow = 0;

    memset(dz_head, 0xFF, sizeof dz_head);

    dz_bits(1, 1);          /* BFINAL */
    dz_bits(1, 2);          /* BTYPE = 01, fixed Huffman */

    while (i < n) {
        size_t best = 0, best_dist = 0;

        if (i + DZ_MIN_MATCH <= n) {
            uint32_t h = dz_hash(src + i);
            int32_t cand = dz_head[h];

            dz_head[h] = (int32_t)i;
            if (cand >= 0 && (size_t)cand < i && i - (size_t)cand <= DZ_WINDOW) {
                size_t dist = i - (size_t)cand;
                size_t maxlen = n - i;
                size_t l = 0;

                if (maxlen > DZ_MAX_MATCH) maxlen = DZ_MAX_MATCH;
                while (l < maxlen && src[(size_t)cand + l] == src[i + l]) l++;
                if (l >= DZ_MIN_MATCH) {
                    best = l;
                    best_dist = dist;
                }
            }
        }

        if (best >= DZ_MIN_MATCH) {
            dz_match((unsigned)best, (unsigned)best_dist);
            /* Insert the positions the match covered, so the next match can
             * start inside it. Skipping this costs a lot on tiled images. */
            for (k = 1; k < best; k++) {
                if (i + k + DZ_MIN_MATCH <= n) {
                    dz_head[dz_hash(src + i + k)] = (int32_t)(i + k);
                }
            }
            i += best;
        } else {
            dz_literal(src[i]);
            i++;
        }
    }

    dz_end_of_block();
    if (dz_nbits) dz_byte((unsigned char)(dz_acc & 0xFF));

    if (dz_overflow) return 0;

    sum = adler32(src, n);
    dst[2 + dz_len + 0] = (unsigned char)(sum >> 24);
    dst[2 + dz_len + 1] = (unsigned char)(sum >> 16);
    dst[2 + dz_len + 2] = (unsigned char)(sum >> 8);
    dst[2 + dz_len + 3] = (unsigned char)(sum);
    return 2 + dz_len + 4;
}

static void be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)(v);
}

static int png_chunk(FILE *f, const char *type,
                     const unsigned char *data, size_t n) {
    unsigned char hdr[4];
    uint32_t c;

    be32(hdr, (uint32_t)n);
    if (fwrite(hdr, 1, 4, f) != 4) return 0;
    if (fwrite(type, 1, 4, f) != 4) return 0;
    if (n && fwrite(data, 1, n, f) != n) return 0;

    c = crc_update(0xFFFFFFFFu, (const unsigned char *)type, 4);
    c = crc_update(c, data, n) ^ 0xFFFFFFFFu;
    be32(hdr, c);
    return fwrite(hdr, 1, 4, f) == 4;
}


/*
 * Write `raw`, h scanlines of one filter byte followed by w RGB triples,
 * filter type 0, as a PNG at `path`, using `z` as the compression buffer.
 * 1 on success. The caller sizes both: raw is h * (1 + 3w), and z wants half
 * as much again plus a little, because static-Huffman deflate can expand
 * incompressible input. `who` names the caller in any complaint.
 */
static int pc_png_write_raw(const char *path, unsigned w, unsigned h,
                            const unsigned char *raw, size_t rawn,
                            unsigned char *z, size_t zcap,
                            const char *who) {
    static const unsigned char sig[8] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
    };
    unsigned char ihdr[13];
    size_t zn;
    FILE *f;
    int ok;

    zn = dz_compress(z, zcap, raw, rawn);
    if (zn == 0) {
        fprintf(stderr,
                "%s: the image did not fit in the compression buffer "
                "(%u bytes for %u of image).\n"
                "  That is the caller's sizing, not anything the picture did.\n",
                who, (unsigned)zcap, (unsigned)rawn);
        return 0;
    }

    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "%s: cannot write %s: %s\n", who, path,
                strerror(errno));
        return 0;
    }

    be32(ihdr + 0, w);
    be32(ihdr + 4, h);
    ihdr[8]  = 8;       /* bit depth */
    ihdr[9]  = 2;       /* colour type 2: truecolour, no alpha */
    ihdr[10] = 0;       /* compression method: deflate */
    ihdr[11] = 0;       /* filter method 0 */
    ihdr[12] = 0;       /* no interlace */

    ok = fwrite(sig, 1, 8, f) == 8
         && png_chunk(f, "IHDR", ihdr, sizeof ihdr)
         && png_chunk(f, "IDAT", z, zn)
         && png_chunk(f, "IEND", NULL, 0);
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        fprintf(stderr, "%s: writing %s failed: %s\n", who, path,
                strerror(errno));
    }
    return ok;
}

#endif /* POKEPLATINUM_PC_PNG_H */
