/*
 * 3ds/src/3ds_layer3d.h: this frame's 3D layer, as something the PICA can
 * sample.
 *
 * The DS composites the 3D engine as engine a's BG0, so a 2D renderer that
 * cannot draw that layer cannot draw a frame that has 3D on it, and the
 * survey says that is the largest reason left by a wide margin: the 3D layer
 * is the sole obstacle on 17,249 engine-frames, more than twice the colour
 * effects behind it.
 *
 * This file is a seam and not an upload, which is why it is its own file.
 * The compositor asks one question; "this frame's 3D layer, as texels I can
 * put under a quad, and is any of it translucent", and it must not know
 * which renderer answered. The 3D layer answers it by packing pc_gpu3d_soft_line()'s
 * 256x192 output into the texture below; the 3D rasterizer answers the same question with a
 * render target the PICA drew into. 3ds_gpu2d.c does not change between those
 * two and does not get rebuilt for them.
 *
 * Why the upload order is this way round. A software rasterizer feeding a GPU
 * compositor is a linear copy in the direction the memory system likes. The
 * other order (a GPU 3D layer feeding a CPU compositor) means reading
 * 192 KB back off the GPU every frame and stalling on it, which is why the 3D layer
 * comes before the 3D rasterizer rather than after.
 *
 * The texels are the software renderer's own, expanded the same way. The
 * rasterizer works in six bits a channel, the same as the 2D engine, and
 * pc_gpu2d.c's expand() replicates the top two bits on the way to eight. So
 * does this file, because PRESENT_VERIFY compares whole framebuffers byte for
 * byte and a rounding step of its own would be a differing pixel.
 */

#ifndef POKEPLATINUM_3DS_LAYER3D_H
#define POKEPLATINUM_3DS_LAYER3D_H

#include <stdint.h>
#include <stdio.h>

#include "3ds_effect.h"

/*
 * Which renderer answers, read once at start-up from
 * sdmc:/3ds/pokeplatinum/3d.txt, the convention present.txt and bg.txt
 * already carry, and a third file because it is a third axis: what composes
 * the picture, what presents it, and what draws the polygons.
 *
 *   soft   pc/hw/pc_gpu3d_soft.c rasterizes and this file packs its output
 *          into a texture once a frame, the 3D layer, and the oracle the 3D rasterizer is diffed
 *          against
 *   pica   the PICA draws the polygon list into a render target
 */
enum {
    L3D_SOFT = 0,
    L3D_PICA
};

#define L3D_PATH "sdmc:/3ds/pokeplatinum/3d.txt"

int layer3d_mode(void);
const char *layer3d_mode_name(void);

/*
 * The texture the layer is packed into: `width` by `height` texels of RGBA8 in
 * PICA swizzle order, 8x8 blocks in raster order, Morton order inside a
 * block, which is what the tile cache already writes tiles in. It must be at least
 * 256x192 and both sides a multiple of 8; the console hands over a 256x256
 * one, because a PICA texture's sides are powers of two.
 *
 * The pool is passed in rather than allocated here for the reason gpu2d_init()
 * takes one: on the console it is a C3D_Tex's own linear memory, so a pack
 * lands where the GPU reads it and no transfer runs at all, and on a build
 * machine it is a plain array and the packer is testable without a PICA.
 */
int layer3d_init(uint32_t *pool, int width, int height);

/*
 * Pack this frame's 3D layer, and say what came out.
 *
 *   L3D_READY        there is a layer and the quad may be drawn
 *   L3D_TRANSLUCENT  ...and at least one of its pixels has an alpha the DS
 *                    would blend with what is under it, rather than replace
 *
 * Zero means no layer this frame, no pool, or no rasterizer in the binary,
 * and the engine-frame is not the GPU path's.
 *
 * Why translucency is the caller's problem and not this file's. A 3D pixel
 * with alpha under 31 blends with the layer it covers whenever BLDCNT admits
 * that layer as a second operand, and it does so whatever BLDCNT's *effect*
 * field says, which is the asymmetric case in color_composite(). The blend is
 * six-bit with its own rounding, so the PICA's fixed-function blend unit
 * cannot reproduce it exactly and a frame that needs one is not this path's.
 * That test needs a register this file does not read, so it returns the fact
 * and 3ds_gpu2d.c's predicate makes the decision.
 *
 * Call it once a frame, before the software compositor runs, and only when the
 * layer is the only thing left in the way: it walks 49,152 pixels, and a frame
 * that a sprite is going to refuse anyway must not pay for that.
 */
enum {
    L3D_READY       = 1u << 0,
    L3D_TRANSLUCENT = 1u << 1
};

/*
 * The effect pass's colour transform goes in HERE and not in the compositor, because the
 * layer is a texture by the time the compositor sees it and there is no
 * palette left to fold a fade into. The software packer applies it to each
 * pixel as it converts, in six bits, which is exactly what the DS does to a
 * 3D pixel that BLDCNT names as a target. A producer that cannot apply it
 * answers zero, and the frame keeps BG_R_3D.
 */
unsigned layer3d_produce(const struct effect *e);

/*
 * The quad, in the draw order gpu2d_build() is walking: the whole 256x192
 * picture at (ox, oy), textured with what layer3d_produce() packed. Returns 0,
 * or -1 if the vertex buffer had no room; which the caller must treat the
 * same way it treats a tile that would not fit.
 *
 * The scroll is already in the texels. BG0HOFS is wired into the rendering
 * engine on hardware rather than into this layer, and pc_gpu3d_soft_line()
 * applies it; see and draw_bg_3d().
 */
int layer3d_quad(float ox, float oy);

/* Push the packed texels out of the data cache, once, before anything is
 * drawn. A no-op in a frame that packed nothing, and in the PICA path, where
 * the texels are a render target the CPU never touched. */
void layer3d_flush(void);

/*
 * Draw the layer, from INSIDE the open frame and before anything samples it.
 * A no-op for the software packer, whose texels were already there.
 *
 * It exists because the two producers cannot run at the same point:
 * layer3d_produce() is called at survey time, which is outside any citro3d
 * frame, and a PICA cannot be asked to draw there. So the GPU producer splits:
 * convert and batch in `produce`, draw here.
 */
void layer3d_render(void);

/* Whether the barrier between drawing the layer and sampling it should be left
 * out, a diagnostic, and never a default. */
int layer3d_no_split(void);

/*
 * The texture the compositor should bind for the layer quad, or NULL to use
 * the pool it was handed at init. The PICA producer draws into a render target
 * of its own, so the compositor has to be told where the texels ended up.
 */
void *layer3d_texture(void);

/* The running totals, for the perf report. */
void layer3d_report(FILE *f);

/* Self-test, the same shape every model in this port carries. */
int layer3d_selftest(int *ranOut);

#endif /* POKEPLATINUM_3DS_LAYER3D_H */
