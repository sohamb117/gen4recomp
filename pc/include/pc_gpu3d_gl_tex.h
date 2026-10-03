/*
 * pc/include/pc_gpu3d_gl_tex.h: DS textures in the form a GL texture holds.
 *
 * The converter is texture_lookup() with the wrapping taken out and the
 * expansion put in, the same sentence 3ds/src/3ds_tex3d.h opens with, because
 * this file is that converter re-aimed: the 3DS wanted PICA swizzle order and
 * 0xRRGGBBAA words, a GL upload wants linear rows of RGBA8 bytes, and
 * everything between the guest bytes and the pixel is identical arithmetic
 * with an exact oracle. The oracle is the software renderer's own texture
 * unit, and the `gltex` vector suite compares every texel of every format
 * against it.
 *
 * What is stored is the unmodulated texel, eight bits a channel through the
 * DS's own five-to-six then six-to-eight expansions. A texture's white is 0xFF
 * where a 2D background's is 0xFB, which is kept by taking the expansion from
 * render_pixel() and not from the tile pipeline. Modulation by the vertex
 * colour stays in the fragment stage, because it is a property of shading and
 * not of the image.
 *
 * Wrapping is sampler state and not part of the image: the DS's clamp, repeat
 * and repeat-with-flip are GL_CLAMP_TO_EDGE, GL_REPEAT and GL_MIRRORED_REPEAT,
 * exact on the DS's power-of-two sides. That decides the cache key: two
 * polygons sampling one image with different wrap bits share one converted
 * copy.
 *
 * Reads go through the rasterizer's own latched slots, so the two producers
 * cannot disagree about where texture memory is.
 */

#ifndef POKEPLATINUM_PC_GPU3D_GL_TEX_H
#define POKEPLATINUM_PC_GPU3D_GL_TEX_H

#include <stdint.h>

/*
 * The largest image accepted, in texels. A TexParam can name 1024x1024,
 * four megabytes of RGBA8 on one polygon, while the 3DS survey measured
 * this game's largest real image at 32,768 texels and a frame's whole
 * working set at 28 images / 815 KB. Twice the largest observed image;
 * past it the converter refuses rather than expands.
 */
#define PC_GLTEX_MAX_TEXELS 65536u

/* The image a cache key names, which is not all of TexParam: wrap and flip
 * (bits 16-19) and the coordinate-transform mode (30-31) change how an
 * image is sampled, not what is in it. */
#define PC_GLTEX_KEY_MASK 0x3FF0FFFFu

unsigned pc_gltex_width(uint32_t texparam);
unsigned pc_gltex_height(uint32_t texparam);
unsigned pc_gltex_texels(uint32_t texparam);

/* The sampler state the image does not carry. */
enum {
    PC_GLTEX_WRAP_CLAMP = 0,    /* GL_CLAMP_TO_EDGE    */
    PC_GLTEX_WRAP_REPEAT,       /* GL_REPEAT           */
    PC_GLTEX_WRAP_MIRROR        /* GL_MIRRORED_REPEAT  */
};

int pc_gltex_wrap_s(uint32_t texparam);
int pc_gltex_wrap_t(uint32_t texparam);

/*
 * One texel: bytes r, g, b, a in memory order (the little-endian word is
 * A<<24 | B<<16 | G<<8 | R), which is what GL_RGBA + GL_UNSIGNED_BYTE
 * uploads. `si` and `ti` are inside the image; wrapping is the sampler's.
 */
uint32_t pc_gltex_texel(uint32_t texparam, uint32_t texpal, int si, int ti);

/*
 * The whole image into `out`, which must hold pc_gltex_texels() words, in
 * linear row order, row 0 of the output is row 0 of the DS texture, the
 * top of the picture; whether the sampler's v runs the other way is the
 * renderer's coordinate business, not the image's.
 *
 * Returns 0, or -1 for an image past PC_GLTEX_MAX_TEXELS.
 */
int pc_gltex_convert(uint32_t *out, uint32_t texparam, uint32_t texpal);

#endif /* POKEPLATINUM_PC_GPU3D_GL_TEX_H */
