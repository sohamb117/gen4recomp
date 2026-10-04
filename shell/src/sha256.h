/*
 * SHA-256 (FIPS 180-4) for the updater: a downloaded release zip must match
 * the digest the release lists in sha256sums.txt before it is shown to the
 * player. SDL-free and unit-tested against the FIPS vectors.
 */
#ifndef NP_SHA256_H
#define NP_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct np_sha256 {
    uint32_t h[8];
    uint64_t length; /* bytes hashed so far */
    uint8_t block[64];
    uint32_t used; /* bytes buffered in block */
} np_sha256;

void np_sha256_init(np_sha256 *s);
void np_sha256_update(np_sha256 *s, const void *data, size_t len);
void np_sha256_final(np_sha256 *s, uint8_t digest[32]);
/* Lower-case hex into `hex` (65 bytes incl. terminator). */
void np_sha256_hex(const uint8_t digest[32], char hex[65]);

#endif
