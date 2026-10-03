/*
 * 3ds/src/3ds_pica3d_gpu.h: the PICA drawing the 3D layer.
 *
 * This is the second answer to the 3D layer's seam and nothing else. `3ds_layer3d.h`
 * asks one question; this frame's 3D layer as texels the compositor can put
 * under a quad, and is any of it translucent, and the 3D layer answers it by packing
 * the software rasterizer's output. This file answers the same question by
 * drawing the polygon list into a render target. `3ds_gpu2d.c` does not change
 * and is not rebuilt for it.
 *
 * Why it is two calls and not one. `layer3d_produce()` runs where the survey
 * runs, before the software 2D renderer, and OUTSIDE any citro3d frame, because
 * that is the only point between the game's last store and the renderer's first
 * read. A PICA cannot be asked to draw there. So the work is split at the only
 * seam that exists: the CPU half (walk the list, convert vertices, convert
 * textures, decide the batches) runs in `produce`, and the GPU half runs from
 * `3ds_gpu.c` once the frame is open. That is the same shape the background path already has,
 * where `gpu2d_build()` writes vertices and `screen_draw()` issues the draws.
 *
 * The polygon list is still valid at draw time. `pc_gpu3d_vblank()` flips the
 * bank at the END of the vblank, so `render_polygon_ram` points into the bank
 * the geometry engine is NOT writing until the next vblank, and the frame is
 * presented long before that. The vertices are converted in `produce` anyway,
 * so the draw half reads nothing of the guest's.
 *
 * What it does not do yet, and each is a measurement away rather than a
 * guess: edge marking (the 3D pixel work, and the survey says two poly-frames in three
 * want it), the polygon-ID translucency rules, and the clipped-polygon fan
 * past four vertices (0.8% of polygons).
 */

#ifndef POKEPLATINUM_3DS_PICA3D_GPU_H
#define POKEPLATINUM_3DS_PICA3D_GPU_H

#include <stdint.h>
#include <stdio.h>

/*
 * Bring up the render target, the vertex program and the texture pool.
 * Returns 0, or -1, in which case `3d.txt` asking for `pica` falls back to
 * the software packer and says so once, which is the behaviour the 3D layer already
 * has for a build with no renderer linked at all.
 *
 * Call it from gpu_init(), AFTER C3D_Init() and after the layer texture
 * exists: the target is created ON that texture, so the compositor samples
 * exactly what this drew with no copy in between.
 */
int pica3d_gpu_init(void);
int pica3d_gpu_ready(void);

/*
 * The CPU half. Walks this frame's polygon list, converts every vertex and
 * every texture it samples, and records the draws. Returns the same flags
 * `layer3d_produce()` returns, L3D_READY, plus L3D_TRANSLUCENT when some
 * pixel of it would blend, or 0 when there is no layer this frame or the
 * frame is one this renderer refuses.
 *
 * A refusal is a frame the compositor sends to software, not a wrong picture.
 * Three things cause one: a texture bigger than the converter's ceiling, a
 * frame whose polygons disagree about the depth mode (there is one depth test
 * per frame and no single one would be right), and running out of vertex room.
 */
unsigned pica3d_gpu_build(void);

/*
 * The GPU half, from inside the open frame and before anything samples the
 * layer. A no-op when `build` produced nothing.
 */
void pica3d_gpu_draw(void);

/* The texture the compositor binds for the layer quad. NULL until init. */
void *pica3d_gpu_texture(void);

/*
 * One texel of the layer this renderer left in VRAM, as 0xRRGGBBAA, at (x, y)
 * in the DS picture's own axes. Zero before init or off the target.
 *
 * Read it from the build half and nowhere else. That is the one point in the
 * frame where the previous frame's draws have landed, the GPU is idle and VRAM
 * is uncached to the ARM11, the same window dump_layer() takes. What it
 * therefore returns is the PREVIOUS frame's layer, which is what the 3D rasterizer's diff
 * has to be paired against.
 */
uint32_t pica3d_gpu_texel(int x, int y);

/* Whether p3d.txt asked for the layer diff; see 3ds_layer3d.c, which owns
 * it because it is the file that can reach both renderers. */
int pica3d_gpu_diff(void);

int pica3d_gpu_no_split(void);

void pica3d_gpu_report(FILE *f);

#endif /* POKEPLATINUM_3DS_PICA3D_GPU_H */
