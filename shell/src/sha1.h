/*
 * SHA-1 (FIPS 180-4), used only to identify cartridge dumps the player
 * imports. SHA-1 is not used for security here: the accepted hashes are the
 * well-known No-Intro checksums of the retail US ROMs, so a collision buys an
 * attacker nothing beyond running a file they already control.
 */
#ifndef NP_SHA1_H
#define NP_SHA1_H

#include <stddef.h>
#include <stdint.h>

typedef struct np_sha1 {
    uint32_t h[5];
    uint64_t length; /* bytes hashed so far */
    uint8_t block[64];
    uint32_t used; /* bytes buffered in block */
} np_sha1;

void np_sha1_init(np_sha1 *s);
void np_sha1_update(np_sha1 *s, const void *data, size_t len);
void np_sha1_final(np_sha1 *s, uint8_t digest[20]);

/* Lower-case hex of a digest into `hex` (41 bytes incl. terminator). */
void np_sha1_hex(const uint8_t digest[20], char hex[41]);

#endif
