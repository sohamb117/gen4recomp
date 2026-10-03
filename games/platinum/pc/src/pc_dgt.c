/*
 * The digest library, real.
 *
 * libdgt ships prebuilt (no C in the wrap), so its surface began as weak
 * trap stubs on the wifi-only assumption, and the trap immediately
 * proved that wrong: the save system hashes through DGT at boot. The
 * algorithms are not exotic: DGT_Hash1 is MD5 (16-byte digest, and the
 * SDK's own DGTHash1Context is a textbook MD5 state), DGT_Hash2 is SHA-1
 * (20 bytes, SHA-1 state), and the Hmac entry points are RFC 2104 HMAC
 * over those. Implemented here from the public specifications, laid into
 * the SDK's own context structs, and checked by pc_dgt_selftest()
 * against the standard test vectors (MD5("abc"), SHA1("abc"), RFC 2202).
 *
 * The ForRms variants (ROM header signing for the wifi download-play
 * path) stay weak trap stubs: nothing single-player reaches them, and
 * their exact concatenation order is not worth guessing.
 */
#include <nitro/dgt/dgt.h>
#include <stdio.h>
#include <string.h>

typedef unsigned char u8_;
typedef unsigned int u32_;
typedef unsigned long long u64_;

/* ------------------------------------------------------------------ */
/* MD5 (DGT_Hash1)                                                     */
/* ------------------------------------------------------------------ */

static const u32_ md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const u8_ md5_shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static u32_ rotl32(u32_ v, unsigned n)
{
    return (v << n) | (v >> (32 - n));
}

static void md5_block(DGTHash1Context *ctx, const u8_ block[64])
{
    u32_ m[16], a, b, c, d, f, g, tmp;
    int i;
    for (i = 0; i < 16; i++) {
        m[i] = (u32_)block[i * 4] | ((u32_)block[i * 4 + 1] << 8) |
               ((u32_)block[i * 4 + 2] << 16) | ((u32_)block[i * 4 + 3] << 24);
    }
    a = ctx->a; b = ctx->b; c = ctx->c; d = ctx->d;
    for (i = 0; i < 64; i++) {
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = (u32_)i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (u32_)(5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (u32_)(3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (u32_)(7 * i) % 16;
        }
        tmp = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + md5_k[i] + m[g], md5_shift[i]);
        a = tmp;
    }
    ctx->a += a; ctx->b += b; ctx->c += c; ctx->d += d;
}

void DGT_Hash1Reset(DGTHash1Context *ctx)
{
    ctx->a = 0x67452301u;
    ctx->b = 0xefcdab89u;
    ctx->c = 0x98badcfeu;
    ctx->d = 0x10325476u;
    ctx->length = 0;
}

void DGT_Hash1SetSource(DGTHash1Context *ctx, const void *src,
                        unsigned long len)
{
    const u8_ *p = src;
    u32_ have = (u32_)(ctx->length % 64);
    ctx->length += len;
    if (have) {
        u32_ need = 64 - have;
        if (len < need) {
            memcpy(ctx->buffer8 + have, p, len);
            return;
        }
        memcpy(ctx->buffer8 + have, p, need);
        md5_block(ctx, ctx->buffer8);
        p += need;
        len -= need;
    }
    while (len >= 64) {
        md5_block(ctx, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(ctx->buffer8, p, len);
    }
}

void DGT_Hash1GetDigest_R(void *digest, DGTHash1Context *ctx)
{
    u8_ pad[72];
    u8_ *out = digest;
    u64_ bits = ctx->length * 8;
    u32_ have = (u32_)(ctx->length % 64);
    u32_ padlen = (have < 56) ? (56 - have) : (120 - have);
    u32_ st[4];
    int i;

    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) {
        pad[padlen + i] = (u8_)(bits >> (8 * i));
    }
    DGT_Hash1SetSource(ctx, pad, padlen + 8);

    st[0] = ctx->a; st[1] = ctx->b; st[2] = ctx->c; st[3] = ctx->d;
    for (i = 0; i < 16; i++) {
        out[i] = (u8_)(st[i / 4] >> (8 * (i % 4)));
    }
}

/* ------------------------------------------------------------------ */
/* SHA-1 (DGT_Hash2)                                                   */
/* ------------------------------------------------------------------ */

static void sha1_block(DGTHash2Context *ctx, const u8_ block[64])
{
    u32_ w[80], a, b, c, d, e, f, k, tmp;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((u32_)block[i * 4] << 24) | ((u32_)block[i * 4 + 1] << 16) |
               ((u32_)block[i * 4 + 2] << 8) | (u32_)block[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = ctx->h0; b = ctx->h1; c = ctx->h2; d = ctx->h3; e = ctx->h4;
    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        tmp = rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = tmp;
    }
    ctx->h0 += a; ctx->h1 += b; ctx->h2 += c; ctx->h3 += d; ctx->h4 += e;
}

void DGT_Hash2Reset(DGTHash2Context *ctx)
{
    ctx->h0 = 0x67452301u;
    ctx->h1 = 0xefcdab89u;
    ctx->h2 = 0x98badcfeu;
    ctx->h3 = 0x10325476u;
    ctx->h4 = 0xc3d2e1f0u;
    ctx->Nl = 0;
    ctx->Nh = 0;
    ctx->num = 0;
}

void DGT_Hash2SetSource(DGTHash2Context *ctx, const unsigned char *src,
                        unsigned long len)
{
    const u8_ *p = src;
    u8_ *buf = (u8_ *)ctx->data;
    u32_ lo = ctx->Nl + (u32_)(len << 3);
    if (lo < ctx->Nl) {
        ctx->Nh++;
    }
    ctx->Nh += (u32_)(len >> 29);
    ctx->Nl = lo;

    if (ctx->num) {
        u32_ need = 64 - (u32_)ctx->num;
        if (len < need) {
            memcpy(buf + ctx->num, p, len);
            ctx->num += (int)len;
            return;
        }
        memcpy(buf + ctx->num, p, need);
        sha1_block(ctx, buf);
        ctx->num = 0;
        p += need;
        len -= need;
    }
    while (len >= 64) {
        sha1_block(ctx, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(buf, p, len);
        ctx->num = (int)len;
    }
}

void DGT_Hash2GetDigest(DGTHash2Context *ctx, unsigned char *digest)
{
    u8_ pad[72];
    u32_ nl = ctx->Nl, nh = ctx->Nh;
    u32_ have = (u32_)ctx->num;
    u32_ padlen = (have < 56) ? (56 - have) : (120 - have);
    u32_ st[5];
    int i;

    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (i = 0; i < 4; i++) {
        pad[padlen + i] = (u8_)(nh >> (24 - 8 * i));
        pad[padlen + 4 + i] = (u8_)(nl >> (24 - 8 * i));
    }
    DGT_Hash2SetSource(ctx, pad, padlen + 8);

    st[0] = ctx->h0; st[1] = ctx->h1; st[2] = ctx->h2;
    st[3] = ctx->h3; st[4] = ctx->h4;
    for (i = 0; i < 20; i++) {
        digest[i] = (u8_)(st[i / 4] >> (24 - 8 * (i % 4)));
    }
}

/* ------------------------------------------------------------------ */
/* HMAC (RFC 2104) over each                                           */
/* ------------------------------------------------------------------ */

void DGT_Hash1CalcHmac(void *digest, void *bin_ptr, int bin_len,
                       void *key_ptr, int key_len)
{
    DGTHash1Context ctx;
    u8_ k[64], pad[64], inner[16];
    int i;

    memset(k, 0, sizeof k);
    if (key_len > 64) {
        DGT_Hash1Reset(&ctx);
        DGT_Hash1SetSource(&ctx, key_ptr, (unsigned long)key_len);
        DGT_Hash1GetDigest_R(k, &ctx);
    } else {
        memcpy(k, key_ptr, (size_t)key_len);
    }

    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    DGT_Hash1Reset(&ctx);
    DGT_Hash1SetSource(&ctx, pad, 64);
    DGT_Hash1SetSource(&ctx, bin_ptr, (unsigned long)bin_len);
    DGT_Hash1GetDigest_R(inner, &ctx);

    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    DGT_Hash1Reset(&ctx);
    DGT_Hash1SetSource(&ctx, pad, 64);
    DGT_Hash1SetSource(&ctx, inner, sizeof inner);
    DGT_Hash1GetDigest_R(digest, &ctx);
}

void DGT_Hash2CalcHmac(void *digest, void *bin_ptr, int bin_len,
                       void *key_ptr, int key_len)
{
    DGTHash2Context ctx;
    u8_ k[64], pad[64], inner[20];
    int i;

    memset(k, 0, sizeof k);
    if (key_len > 64) {
        DGT_Hash2Reset(&ctx);
        DGT_Hash2SetSource(&ctx, key_ptr, (unsigned long)key_len);
        DGT_Hash2GetDigest(&ctx, k);
    } else {
        memcpy(k, key_ptr, (size_t)key_len);
    }

    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    DGT_Hash2Reset(&ctx);
    DGT_Hash2SetSource(&ctx, pad, 64);
    DGT_Hash2SetSource(&ctx, bin_ptr, (unsigned long)bin_len);
    DGT_Hash2GetDigest(&ctx, inner);

    for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    DGT_Hash2Reset(&ctx);
    DGT_Hash2SetSource(&ctx, pad, 64);
    DGT_Hash2SetSource(&ctx, inner, sizeof inner);
    DGT_Hash2GetDigest(&ctx, digest);
}

/* Overlay-table digest checking on/off; returns the previous flag. The
 * mode itself is only read by FS overlay verification, which this port
 * does not enable (SDK_OVERLAYTABLE_DIGEST is zero for the same reason). */
int DGT_SetOverlayTableMode(int flag)
{
    static int sMode = 0;
    int old = sMode;
    sMode = flag;
    return old;
}

/* ------------------------------------------------------------------ */
/* Vectors                                                             */
/* ------------------------------------------------------------------ */

int pc_dgt_selftest(void)
{
    static const u8_ md5_abc[16] = {
        0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
        0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72,
    };
    static const u8_ sha1_abc[20] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d,
    };
    /* RFC 2202 case 2: key "Jefe", data "what do ya want for nothing?" */
    static const u8_ hmac_jefe[20] = {
        0xef, 0xfc, 0xdf, 0x6a, 0xe5, 0xeb, 0x2f, 0xa2, 0xd2, 0x74,
        0x16, 0xd5, 0xf1, 0x84, 0xdf, 0x9c, 0x25, 0x9a, 0x7c, 0x79,
    };
    DGTHash1Context h1;
    DGTHash2Context h2;
    u8_ d16[16], d20[20];

    DGT_Hash1Reset(&h1);
    DGT_Hash1SetSource(&h1, "abc", 3);
    DGT_Hash1GetDigest_R(d16, &h1);
    if (memcmp(d16, md5_abc, 16) != 0) {
        return 1;
    }

    DGT_Hash2Reset(&h2);
    DGT_Hash2SetSource(&h2, (const unsigned char *)"abc", 3);
    DGT_Hash2GetDigest(&h2, d20);
    if (memcmp(d20, sha1_abc, 20) != 0) {
        return 2;
    }

    DGT_Hash2CalcHmac(d20, "what do ya want for nothing?", 28, "Jefe", 4);
    if (memcmp(d20, hmac_jefe, 20) != 0) {
        return 3;
    }
    return 0;
}
