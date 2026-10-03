/* C reimplementation of the two RC4 PRGA routines the tree keeps only as
 * hand-written ARM assembly (lib/crypto/asm/rc4.s and rc4_fast.s). The Init
 * halves are already C in lib/crypto/src/rc4_init.c.
 *
 * Both are standard RC4: i is incremented before use, j += S[i], S[i] and
 * S[j] swap, and the keystream byte is S[(S[i] + S[j]) & 0xFF]. The "fast"
 * variant keeps every S entry and both indices as byte<<24 in a u32 (that is
 * what CRYPTO_RC4FastInit's final `s[i] <<= 24` loop sets up), so index
 * arithmetic wraps in the top byte for free, the asm's `lsr #22` addressing
 * is that same byte scaled to a word offset. Verified line-by-line against
 * the .s files; the trailing state stores match too (the byte variant stores
 * i and j back as bytes, the fast variant stores i masked to the top byte and
 * j as the raw accumulator). */

#include <nitro/types.h>

#include <stdio.h>
#include <string.h>

#include "crypto/rc4.h"

void CRYPTO_RC4Encrypt(CRYPTORC4Context *ctx, const void *in, u32 len, void *out)
{
    u8 i = ctx->i;
    u8 j = ctx->j;
    u8 *s = ctx->s;
    const u8 *src = in;
    u8 *dst = out;

    while (len--) {
        i++;
        u8 ti = s[i];
        j += ti;
        u8 tj = s[j];
        s[i] = tj;
        s[j] = ti;
        *dst++ = (u8)(*src++ ^ s[(u8)(ti + tj)]);
    }

    ctx->i = i;
    ctx->j = j;
}

void CRYPTO_RC4FastEncrypt(CRYPTORC4FastContext *ctx, const void *in, u32 len, void *out)
{
    u32 i = ctx->i; /* byte<<24 */
    u32 j = ctx->j; /* byte<<24 accumulator */
    u32 *s = ctx->s; /* entries are byte<<24 */
    const u8 *src = in;
    u8 *dst = out;

    while (len--) {
        i += 1u << 24;
        u32 ti = s[i >> 24];
        j += ti;
        u32 tj = s[j >> 24];
        s[i >> 24] = tj;
        s[j >> 24] = ti;
        *dst++ = (u8)(*src++ ^ (u8)(s[(ti + tj) >> 24] >> 24));
    }

    ctx->i = i & 0xFF000000u;
    ctx->j = j;
}

/* ------------------------------------------------------------------ */
/* Vectors                                                             */
/* ------------------------------------------------------------------ */

/*
 * The classic RC4 test vector: key "Key", plaintext "Plaintext" ->
 * BB F3 16 E8 D9 40 AF 0A D3. It is the one every RC4 implementation is checked
 * against, which is the point; these routines are a reimplementation in C of
 * the two ARM assembly versions this port cannot run
 * (lib/crypto/asm/rc4{,_fast}.s), and a reimplementation validated against
 * nothing is a guess.
 *
 * BOTH VARIANTS, because they are different code. CRYPTO_RC4Encrypt keeps the
 * S-box as bytes and CRYPTO_RC4FastEncrypt keeps it as words for speed; the same
 * key and plaintext must come out the same either way, and a divergence between
 * them is the failure mode a single-variant test would miss entirely.
 *
 * And a split call, because the context carries state. RC4 is a stream cipher:
 * encrypting "Plain" then "text" from the same context must produce exactly what
 * encrypting "Plaintext" in one call does. That is the property the game relies
 * on and the one a fresh-context-per-call test cannot see, if i and j were
 * reset, or the S-box were re-keyed, the one-shot vector would still pass.
 */
int pc_rc4_selftest(void)
{
    static const char key[] = "Key";
    static const char pt[] = "Plaintext";
    static const unsigned char want[9] = {
        0xBB, 0xF3, 0x16, 0xE8, 0xD9, 0x40, 0xAF, 0x0A, 0xD3
    };
    CRYPTORC4Context ctx;
    CRYPTORC4FastContext fctx;
    unsigned char out[9];
    int ok = 1;

    /* One shot, byte S-box. */
    memset(out, 0, sizeof out);
    CRYPTO_RC4Init(&ctx, key, (u32)(sizeof key - 1));
    CRYPTO_RC4Encrypt(&ctx, pt, (u32)(sizeof pt - 1), out);
    if (memcmp(out, want, sizeof want) != 0) {
        fprintf(stderr, "pc-rc4: RC4Encrypt one-shot mismatch\n");
        ok = 0;
    }

    /* One shot, word S-box. */
    memset(out, 0, sizeof out);
    CRYPTO_RC4FastInit(&fctx, key, (u32)(sizeof key - 1));
    CRYPTO_RC4FastEncrypt(&fctx, pt, (u32)(sizeof pt - 1), out);
    if (memcmp(out, want, sizeof want) != 0) {
        fprintf(stderr, "pc-rc4: RC4FastEncrypt one-shot mismatch\n");
        ok = 0;
    }

    /* Split across two calls: the keystream must continue, not restart. */
    memset(out, 0, sizeof out);
    CRYPTO_RC4Init(&ctx, key, (u32)(sizeof key - 1));
    CRYPTO_RC4Encrypt(&ctx, pt, 5, out);
    CRYPTO_RC4Encrypt(&ctx, pt + 5, 4, out + 5);
    if (memcmp(out, want, sizeof want) != 0) {
        fprintf(stderr, "pc-rc4: split-call state did not carry over\n");
        ok = 0;
    }

    memset(out, 0, sizeof out);
    CRYPTO_RC4FastInit(&fctx, key, (u32)(sizeof key - 1));
    CRYPTO_RC4FastEncrypt(&fctx, pt, 5, out);
    CRYPTO_RC4FastEncrypt(&fctx, pt + 5, 4, out + 5);
    if (memcmp(out, want, sizeof want) != 0) {
        fprintf(stderr, "pc-rc4: RC4Fast split-call state did not carry over\n");
        ok = 0;
    }

    return ok;
}
