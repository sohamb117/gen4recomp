/*
 * pc/src/pc_gpu3d_gl_tex.c: see pc_gpu3d_gl_tex.h. Started from
 * 3ds/src/3ds_tex3d.c, the pilot whose oracle run this file
 * repeats) and diverged where the target differs: the output is linear
 * RGBA8 rows instead of PICA-swizzled 0xRRGGBBAA blocks, and the guest
 * reads go through the rasterizer's own latched slots instead of a vram
 * walk of this file's own; one latch, one answer, for both producers.
 *
 * The decode switch below is pc_gpu3d_soft.c's texture_lookup() with the
 * wrapping taken out, down to the compressed format's slot-1 arithmetic
 * and its wrap after slot 3. Do not "clean it up" toward GBAtek: the
 * software renderer is the oracle, and the gltex vector suite compares
 * this file against it texel by texel, which is only a proof while the
 * two really are the same arithmetic.
 */

#include "pc_gpu3d_gl_tex.h"

#include <stddef.h>

#include "pc_gpu3d_soft.h"

/* ------------------------------------------------------------------ */
/* Shape                                                               */
/* ------------------------------------------------------------------ */

unsigned pc_gltex_width(uint32_t texparam)
{
    return 8u << ((texparam >> 20) & 7);
}

unsigned pc_gltex_height(uint32_t texparam)
{
    return 8u << ((texparam >> 23) & 7);
}

unsigned pc_gltex_texels(uint32_t texparam)
{
    return pc_gltex_width(texparam) * pc_gltex_height(texparam);
}

int pc_gltex_wrap_s(uint32_t texparam)
{
    if (!(texparam & (1u << 16))) return PC_GLTEX_WRAP_CLAMP;
    return (texparam & (1u << 18)) ? PC_GLTEX_WRAP_MIRROR
                                   : PC_GLTEX_WRAP_REPEAT;
}

int pc_gltex_wrap_t(uint32_t texparam)
{
    if (!(texparam & (1u << 17))) return PC_GLTEX_WRAP_CLAMP;
    return (texparam & (1u << 19)) ? PC_GLTEX_WRAP_MIRROR
                                   : PC_GLTEX_WRAP_REPEAT;
}

/* ------------------------------------------------------------------ */
/* One texel                                                           */
/* ------------------------------------------------------------------ */

/*
 * RGB555 to the eight bits a GL texture holds, through the six the DS
 * composites in: render_pixel()'s `v * 2, and + 1 when that is not zero`,
 * then the surface expansion's top-two-bit replication. Both steps are
 * exact and neither is a rounding choice of this file's. The alpha goes
 * 0-31 to 0-255 the only way that keeps both ends, 0 stays transparent
 * and 31 reaches opaque; the DS blend's (a + 1) / 32 weighting is the
 * blend unit's own and stays with the shading.
 */
static uint32_t expand555(uint32_t c15, uint32_t alpha5)
{
    uint32_t r = (c15 << 1) & 0x3Eu;
    uint32_t g = (c15 >> 4) & 0x3Eu;
    uint32_t b = (c15 >> 9) & 0x3Eu;

    if (r) r++;
    if (g) g++;
    if (b) b++;
    r = (r << 2) | (r >> 4);
    g = (g << 2) | (g >> 4);
    b = (b << 2) | (b >> 4);
    alpha5 = (alpha5 << 3) | (alpha5 >> 2);
    return r | (g << 8) | (b << 16) | (alpha5 << 24);
}

static uint32_t texel_at(uint32_t texparam, uint32_t texpal, int si, int ti)
{
    uint32_t vramaddr = (texparam & 0xFFFFu) << 3;
    uint32_t width = pc_gltex_width(texparam);
    uint32_t alpha0 = (texparam & (1u << 29)) ? 0u : 31u;
    uint32_t color = 0, alpha = 0;

    switch ((texparam >> 26) & 7) {
    case 0:
        /* No texture. render_pixel() never calls the unit at all in this
         * case; a converted image of it is one transparent texel, which is
         * the answer that composes to the same picture. */
        return 0;

    case 1: {   /* A3I5 */
        uint32_t pixel;

        vramaddr += (uint32_t)(ti * (int)width + si);
        pixel = pc_gpu3d_soft_tex_read8(vramaddr);
        texpal <<= 4;
        color = pc_gpu3d_soft_texpal_read16(texpal + ((pixel & 0x1Fu) << 1));
        alpha = ((pixel >> 3) & 0x1Cu) + (pixel >> 6);
        break;
    }

    case 2: {   /* 4-colour */
        uint32_t pixel;

        vramaddr += (uint32_t)((ti * (int)width + si) >> 2);
        pixel = (pc_gpu3d_soft_tex_read8(vramaddr) >> ((si & 3) << 1)) & 3u;
        texpal <<= 3;
        color = pc_gpu3d_soft_texpal_read16(texpal + (pixel << 1));
        alpha = (pixel == 0) ? alpha0 : 31u;
        break;
    }

    case 3: {   /* 16-colour */
        uint32_t pixel;

        vramaddr += (uint32_t)((ti * (int)width + si) >> 1);
        pixel = pc_gpu3d_soft_tex_read8(vramaddr);
        pixel = (si & 1) ? (pixel >> 4) : (pixel & 0xFu);
        texpal <<= 4;
        color = pc_gpu3d_soft_texpal_read16(texpal + (pixel << 1));
        alpha = (pixel == 0) ? alpha0 : 31u;
        break;
    }

    case 4: {   /* 256-colour */
        uint32_t pixel;

        vramaddr += (uint32_t)(ti * (int)width + si);
        pixel = pc_gpu3d_soft_tex_read8(vramaddr);
        texpal <<= 4;
        color = pc_gpu3d_soft_texpal_read16(texpal + (pixel << 1));
        alpha = (pixel == 0) ? alpha0 : 31u;
        break;
    }

    case 5: {   /* 4x4 compressed */
        uint32_t slot1addr, val, palinfo, paloffset;

        /*
         * The block's texels and the halfword naming its palette live in
         * DIFFERENT SLOTS, and reading the second out of the first draws a
         * plausible wrong picture. This is pc_gpu3d_soft.c's arithmetic
         * unchanged, down to the wrap after slot 3.
         */
        vramaddr += (uint32_t)((ti & 0x3FC) * ((int)width >> 2))
                  + (uint32_t)(si & 0x3FC);
        vramaddr += (uint32_t)(ti & 3);
        vramaddr &= 0x7FFFFu;

        slot1addr = 0x20000u + ((vramaddr & 0x1FFFCu) >> 1);
        if (vramaddr >= 0x40000u) {
            slot1addr += 0x10000u;
        }

        if (vramaddr >= 0x20000u && vramaddr < 0x40000u) {
            val = 0;    /* reading slot 1 for texels always reads 0 */
        } else {
            val = pc_gpu3d_soft_tex_read8(vramaddr) >> (2 * (si & 3));
        }

        palinfo = pc_gpu3d_soft_tex_read16(slot1addr);
        paloffset = (palinfo & 0x3FFFu) << 2;
        texpal <<= 4;
        alpha = 31u;

        switch (val & 3u) {
        case 0:
            color = pc_gpu3d_soft_texpal_read16(texpal + paloffset);
            break;
        case 1:
            color = pc_gpu3d_soft_texpal_read16(texpal + paloffset + 2);
            break;
        case 2:
            if ((palinfo >> 14) == 1 || (palinfo >> 14) == 3) {
                uint32_t c0 = pc_gpu3d_soft_texpal_read16(texpal + paloffset);
                uint32_t c1 = pc_gpu3d_soft_texpal_read16(texpal + paloffset
                                                          + 2);
                uint32_t r0 = c0 & 0x001F, g0 = c0 & 0x03E0, b0 = c0 & 0x7C00;
                uint32_t r1 = c1 & 0x001F, g1 = c1 & 0x03E0, b1 = c1 & 0x7C00;

                if ((palinfo >> 14) == 1) {
                    color = ((r0 + r1) >> 1)
                          | (((g0 + g1) >> 1) & 0x03E0)
                          | (((b0 + b1) >> 1) & 0x7C00);
                } else {
                    color = ((r0 * 5 + r1 * 3) >> 3)
                          | (((g0 * 5 + g1 * 3) >> 3) & 0x03E0)
                          | (((b0 * 5 + b1 * 3) >> 3) & 0x7C00);
                }
            } else {
                color = pc_gpu3d_soft_texpal_read16(texpal + paloffset + 4);
            }
            break;
        default:
            if ((palinfo >> 14) == 2) {
                color = pc_gpu3d_soft_texpal_read16(texpal + paloffset + 6);
            } else if ((palinfo >> 14) == 3) {
                uint32_t c0 = pc_gpu3d_soft_texpal_read16(texpal + paloffset);
                uint32_t c1 = pc_gpu3d_soft_texpal_read16(texpal + paloffset
                                                          + 2);
                uint32_t r0 = c0 & 0x001F, g0 = c0 & 0x03E0, b0 = c0 & 0x7C00;
                uint32_t r1 = c1 & 0x001F, g1 = c1 & 0x03E0, b1 = c1 & 0x7C00;

                color = ((r0 * 3 + r1 * 5) >> 3)
                      | (((g0 * 3 + g1 * 5) >> 3) & 0x03E0)
                      | (((b0 * 3 + b1 * 5) >> 3) & 0x7C00);
            } else {
                color = 0;
                alpha = 0;
            }
            break;
        }
        break;
    }

    case 6: {   /* A5I3 */
        uint32_t pixel;

        vramaddr += (uint32_t)(ti * (int)width + si);
        pixel = pc_gpu3d_soft_tex_read8(vramaddr);
        texpal <<= 4;
        color = pc_gpu3d_soft_texpal_read16(texpal + ((pixel & 7u) << 1));
        alpha = pixel >> 3;
        break;
    }

    default:    /* 7: direct colour */
        vramaddr += (uint32_t)((ti * (int)width + si) << 1);
        color = pc_gpu3d_soft_tex_read16(vramaddr);
        alpha = (color & 0x8000u) ? 31u : 0u;
        break;
    }

    return expand555(color, alpha);
}

uint32_t pc_gltex_texel(uint32_t texparam, uint32_t texpal, int si, int ti)
{
    return texel_at(texparam, texpal, si, ti);
}

/* ------------------------------------------------------------------ */
/* The whole image                                                     */
/* ------------------------------------------------------------------ */

int pc_gltex_convert(uint32_t *out, uint32_t texparam, uint32_t texpal)
{
    unsigned w = pc_gltex_width(texparam);
    unsigned h = pc_gltex_height(texparam);
    unsigned si, ti;

    if (out == NULL || w * h > PC_GLTEX_MAX_TEXELS) {
        return -1;
    }
    for (ti = 0; ti < h; ti++) {
        for (si = 0; si < w; si++) {
            *out++ = texel_at(texparam, texpal, (int)si, (int)ti);
        }
    }
    return 0;
}
