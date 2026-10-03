/*
 * pc/include/pc_gpu3d_gl.h: the 3D layer's second producer, behind the seam
 * pc_gpu3d_soft_defer() already cut.
 *
 * The seam is a question, not an upload, the same shape the 3DS port proved
 * on real silicon (3ds/src/3ds_layer3d.h): vblank records that a frame is
 * due, and whichever producer is selected answers the line reads. The
 * consumers, pc_gpu3d_soft_line()'s native point-sample and the HD
 * compose's fine grid, must not know which renderer answered, so a GPU
 * producer renders offscreen in this process and lands the same pixels in
 * the same buffers. Nothing crosses the view channel that did not before.
 *
 * Which producer answers is PC_GPU3D (--gpu3d), read once at start-up:
 *
 *   soft   pc/hw/pc_gpu3d_soft.c rasterizes, the default, the oracle,
 *          and the ship path on every host, forever ("any PC" includes
 *          "any PC" includes machines whose driver is the problem)
 *   gl     the offscreen GPU path, a spike with kill criteria chosen in
 *          before it existed. Refused, with a printed line
 *          and the run staying soft, whenever the context cannot be had.
 *
 * The selection is start-up-only on purpose: the rasterizer's buffers, the
 * page shape and every instrument's assumptions are fixed before guest code
 * runs, and a mid-run producer swap is a class of bug nothing here needs.
 */

#ifndef POKEPLATINUM_PC_GPU3D_GL_H
#define POKEPLATINUM_PC_GPU3D_GL_H

#include <stdint.h>

struct PcGxPolygon;

/*
 * Read PC_GPU3D and try to bring the selected producer up. Returns 0 when
 * the run stays soft (unset, "soft", or "gl" refused, the refusal prints
 * its reason once), 1 when the gl producer is live, and -1 when the value
 * is not a producer at all (the caller owns the usage error, the way it
 * does for --hd3d).
 */
int pc_gpu3d_gl_select(void);

/* Whether the gl producer answers this run's frames. */
int pc_gpu3d_gl_active(void);

/*
 * Draw one latched frame with the gl producer and land its pixels in the
 * rasterizer's own rows. Called from render_frame_now() after the latch,
 * with the published polygon list. Returns the polygon count drawn (>= 0):
 * The frame is landed and the software rasterizer must NOT run, or
 * -1 for a frame this renderer refuses (shadow polygons, a non-modulate
 * shading mode, fog, the clear image, mixed depth modes, a texture past
 * the converter, overflow): the caller rasterizes in software, which is
 * the fallback's whole design, and the refusal is counted for the exit
 * report.
 */
int pc_gpu3d_gl_frame(struct PcGxPolygon **polys, int npolys);

/*
 * The landing strip, owned by pc_gpu3d_soft.c because the buffers are its
 * statics: visible row `y` of the render grid, mutable, and the grid's
 * size (width includes the wide margins, both axes carry the HD scale).
 * Valid between the frame's latch and its end, the same window every
 * other producer-facing export keeps.
 */
uint32_t *pc_gpu3d_gl_row(int y);
int pc_gpu3d_gl_grid_w(void);
int pc_gpu3d_gl_grid_h(void);

#endif /* POKEPLATINUM_PC_GPU3D_GL_H */
