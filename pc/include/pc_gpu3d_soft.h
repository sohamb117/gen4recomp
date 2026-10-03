/*
 * The rasterizer.
 *
 * The back half of the DS's 3D pipeline. The geometry engine leaves a
 * double-buffered polygon list at pc_gpu3d_render_polygons(); this turns it
 * into 256x192 pixels, with the depth and W buffers, the alpha test,
 * translucency and the polygon-ID rules, the shadow-volume stencil, edge
 * marking, fog and anti-aliasing.
 *
 * What a pixel is here: one u32 per pixel, `r | (g << 8) | (b << 16) |
 * (a << 24)`, with 6 bits of colour per channel and 5 bits of alpha, which is
 * upstream's ColorBuffer layout and not the 15-bit BGR555 the 2D engines work
 * in. The 3D engine has more precision than the framebuffer does and the
 * conversion happens in the compositor, so keeping the wide form here is what
 * lets the compositor blend at the precision hardware blends at.
 *
 * Where the registers come from is the part that is not melonDS's problem.
 * DISP3DCNT, the clear colour and depth, the fog and toon tables, the edge
 * colours and ALPHA_TEST_REF are all outside the command window, so identity
 * mapping leaves them as plain guest memory and nothing hooks a store to one.
 * They are therefore latched out of guest memory by pc_gpu3d_vblank(), at the
 * same point upstream copies its own shadow registers, and the rasterizer
 * reads only the latched copy.
 *
 * Derived from melonDS. See pc/hw/pc_gpu3d_soft.c's header and pc/hw/README
 * for why it is C here rather than melonDS's C++ linked in.
 */

#ifndef POKEDIAMOND_PC_GPU3D_SOFT_H
#define POKEDIAMOND_PC_GPU3D_SOFT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_GX_SCREEN_W 256
#define PC_GX_SCREEN_H 192

/* Put the rasterizer in its power-on state: the buffers hold nothing and the
 * shadow-mask stencil is cold. Called from pc_gpu3d_reset(). */
void pc_gpu3d_soft_reset(void);

/*
 * Draw the published polygon list into the colour, depth and attribute
 * buffers. Called from pc_gpu3d_vblank() once the list has been published,
 * which is where hardware starts a frame; the geometry engine has already
 * swapped banks by then, so the list this draws is stable for the whole frame.
 *
 * pc_gpu3d.c carries a weak no-op of this, so a build without the rasterizer
 * still links; pc_gpu3d_soft_present() says which one is in the binary.
 */
void pc_gpu3d_soft_render_frame(void);

/*
 * Defer the frame until somebody asks for it, which is how a port that has its
 * own 3D renderer stops paying for this one.
 *
 * Off, the call above rasterizes where it always has and nothing about this
 * file's timing changes; which is the desktop port, and the reason it is a
 * switch rather than a rewrite. On, the call only records that a frame is due,
 * and the first of pc_gpu3d_soft_present() / _line() / _line_wide() / _line_hd()
 * runs it. A frame nobody reads is a frame nobody pays for.
 *
 * It is deferred and not predicted, and that is what makes it safe: the
 * decision about who draws a frame is made after the vblank that would have
 * rasterized it, so a port cannot answer at vblank time without guessing, and a
 * wrong guess is a stale picture. Rendering on first read cannot be wrong by
 * construction, nobody can be handed a line that was not drawn for the list
 * currently published.
 *
 * The inputs are all still valid when the deferred call runs: the geometry
 * engine swapped banks at that vblank and does not swap again until the next
 * one, and the rendering registers were latched into a copy of their own.
 */
void pc_gpu3d_soft_defer(int on);

/*
 * Skip the next frame: the next pc_gpu3d_soft_render_frame() is a no-op and
 * every reader keeps the previous frame's picture, then the one after draws
 * as always. The pacer's frame skip uses this at --hd3d 2 and above, where
 * the rasterizer dominates the frame and a machine that cannot afford it at
 * 60 Hz would otherwise run the whole simulation (audio included) slow.
 * One-shot: each skipped frame is a separate call.
 */
void pc_gpu3d_soft_skip_next(void);

/*
 * The texture spaces, read as latched for this frame, for the gl
 * producer's converter (pc/include/pc_gpu3d_gl_tex.h), so both producers
 * read texture memory through one latch and one slot walk. Valid from the
 * frame's latch to its end, the same window the rasterizer's spans use.
 */
unsigned int pc_gpu3d_soft_tex_read8(unsigned int addr);
unsigned int pc_gpu3d_soft_tex_read16(unsigned int addr);
unsigned int pc_gpu3d_soft_texpal_read16(unsigned int addr);

/* One scanline of the result, 256 pixels, with engine A's BG0 X-scroll applied
 * as GetLine does, 0 <= y < 192. The storage is the renderer's own and is
 * valid until the next pc_gpu3d_soft_render_frame(). */
const uint32_t *pc_gpu3d_soft_line(int y);

/* Wide rendering: how many columns each line carries (256 native, set from
 * pc_gpu3d_set_wide() at startup), and the full line for the viewer's wide
 * compose. pc_gpu3d_soft_line() always returns the native centre. */
void pc_gpu3d_soft_set_width(int w);
const uint32_t *pc_gpu3d_soft_line_wide(int y, int *w);

/* True high-resolution 3D: rasterize at `s` times both axes, up to
 * PC_GPU3D_HD_MAX. pc_gpu3d_soft_line() keeps answering the native
 * point-sample; the full-resolution rows leave through
 * pc_gpu3d_soft_line_hd(). */
void pc_gpu3d_soft_set_scale(int s);
const uint32_t *pc_gpu3d_soft_line_hd(int hy, int *w, int *s);
/* The same row into the CALLER'S scratch (RENDER_W_MAX words), safe from
 * several threads at once provided something rendered the frame first,
 * fetch any one row before fanning out. */
const uint32_t *pc_gpu3d_soft_line_hd_r(int hy, int *w, int *s,
                                        uint32_t *scratch);

/*
 * How many pixels the final pass has touched over the run, edge marking and
 * anti-aliasing counted apart. Both are DISP3DCNT bits this game leaves on for
 * nearly every frame, and a renderer that has neither needs to know which of
 * the two the difference it leaves on a silhouette is made of.
 */
void pc_gpu3d_soft_final_counts(unsigned long *edgeMarked, unsigned long *aaBlended);

/* PC_SURVEY3D, arm the feature-use survey: shading modes,
 * texture formats, translucency, fog, alpha test, span shapes, counted with
 * the renderer's own predicates and printed at exit. The pixel loop is never
 * touched, so a survey run still measures the real spans. */
void pc_gpu3d_soft_survey_init(void);

/* Arm PC_GPU3D_DIFF (--gpu3d-diff DIR): every rendered frame, the selected
 * producer's picture diffed against the software oracle's own render of the
 * same latch, claim-class verdicts, per-row/per-column WHERE, and worst/
 * typical picture triples into DIR. Under the soft
 * producer it is the rasterizer against its own re-render: the null test,
 * and a determinism gate on the banded pool. Desktop only. */
void pc_gpu3d_soft_diff_init(void);

/* Whether the real rasterizer is linked in rather than the weak no-op. */
int pc_gpu3d_soft_present(void);

/* How many polygons the last pc_gpu3d_soft_render_frame() set up, i.e. after
 * degenerate ones were dropped. For tests and for the frame budget. */
uint32_t pc_gpu3d_soft_polygons_drawn(void);

#ifdef __cplusplus
}
#endif

#endif /* POKEDIAMOND_PC_GPU3D_SOFT_H */
