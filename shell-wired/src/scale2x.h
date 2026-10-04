/*
 * Scale2x (AdvMAME2x, Andrea Mazzoleni's EPX refinement): doubles a pixel-art
 * image, rounding diagonal staircases without inventing colours. The SMOOTH
 * display effect runs it on the CPU (two 256x192 screens per frame is about
 * 100k pixels) and lets linear filtering do the rest, so it needs no shader
 * on any renderer. SDL-free for unit tests.
 */
#ifndef NP_SCALE2X_H
#define NP_SCALE2X_H

#include <stddef.h>
#include <stdint.h>

/* src: w x h pixels, `stride` pixels apart; dst: 2w x 2h, contiguous. */
void np_scale2x(const uint32_t *src, int w, int h, size_t stride, uint32_t *dst);

#endif
