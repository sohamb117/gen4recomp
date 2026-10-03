/*
 * Minimal PNG writer for screenshots. Pixels are stored uncompressed
 * (deflate "stored" blocks): a 256x384 shot is ~300 KB, which is fine for
 * screenshots and keeps the encoder small and free of dependencies.
 */
#ifndef NP_PNG_H
#define NP_PNG_H

#include <stddef.h>
#include <stdint.h>

/* Receives encoded bytes in order. Returns 0 on success. */
typedef int (*np_png_sink)(void *user, const void *data, size_t len);

/* Encodes 0x00RRGGBB pixels (`stride` pixels per row) as 8-bit RGB.
 * Returns 0 on success, -1 if the sink failed or memory ran out. */
int np_png_encode(np_png_sink sink, void *user, const uint32_t *xrgb, uint32_t width, uint32_t height,
                  size_t stride);

uint32_t np_crc32(uint32_t crc, const void *data, size_t len);
uint32_t np_adler32(uint32_t adler, const void *data, size_t len);

#endif
