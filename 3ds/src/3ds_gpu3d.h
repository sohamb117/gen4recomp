/*
 * 3ds/src/3ds_gpu3d.h: the DS 3D engine, on the PICA.
 *
 * pc/hw/pc_gpu3d.c is the geometry half in C, and pc/hw/pc_gpu3d_soft.c is the
 * rasterizer that turns its polygon list into 256x192 pixels on the CPU. On
 * this console that rasterizer is 201.9 ms of a 311.5 ms field frame, measured
 * on hardware, which is why the field is the scene this port cannot run and
 * why the PICA rasterizer exists: swap the rasterizer, keep the geometry.
 *
 * The survey comes first in this file because it came first in the work, and
 * the rule it left is that a renderer states its subset in numbers before it
 * writes a line of GPU code. The first sketch listed eight features to port,
 * texture formats through a cache, toon through a LUT, fog through C3D_FogLut,
 * shadow volumes through the stencil, and edge marking with no clean PICA
 * answer at all, and it listed them as a design rather than a measurement.
 * Some of them this game never asks for. Which ones is a count, and this file
 * is the count.
 *
 * What it counts and why each one decides something.
 *
 *   The render registers, per frame: toon or highlight, alpha test, alpha
 *   blending, anti-aliasing, edge marking, fog, the rear-plane bitmap. Each is
 *   a chunk of PICA work, and one that fires on no frame is a chunk that is
 *   not written. Edge marking is the one with no clean answer, so how often it
 *   is on is the difference between a design problem and a paragraph.
 *
 *   The texture formats, per polygon. The sketch names six and a converter is
 *   a day of work each, so the list is a guess until this says otherwise.
 *
 *   The polygon attribute, per polygon: shadow volumes, wireframe, the
 *   depth-equal test, per-polygon fog, the 1-dot rule, the far-plane rule.
 *   Every one is a rasterizer rule the PICA does not have natively.
 *
 *   The distinct textures a frame samples and what they weigh, expanded to
 *   RGBA8. That is what a texture cache has to hold to serve a frame without
 *   thrashing.
 *
 * The counts are host-independent, so the emulator answers them exactly. What
 * the emulator gets wrong is time: it priced one change at seventeen times
 * less than the console did, and there is no time in this report.
 *
 * The survey reads the published list through the same
 * pc_gpu3d_render_polygons() the rasterizer does. It changes no guest state
 * and allocates nothing.
 */

#ifndef POKEPLATINUM_3DS_GPU3D_H
#define POKEPLATINUM_3DS_GPU3D_H

#include <stdio.h>

/*
 * One frame of the 3D engine, after SWAP_BUFFERS published its list and the
 * rasterizer drew it. Called from 3ds/src/3ds_gpu3d_wrap.c, which is already
 * wrapped around exactly that event.
 *
 * A frame with no polygons on it is still a frame and is still counted: "the
 * 3D engine ran and had nothing to draw" and "the 3D engine did not run" are
 * different facts, and the second is most of this game.
 */
void gpu3d_survey(void);

/* The running totals, into the frame-time report beside the background path's. */
void gpu3d_report(FILE *f);

/*
 * Frames the survey has seen with at least one polygon on them, since the run
 * began. The frame-time report samples it per window for the reason the background path's
 * eligible count is sampled per window: it says whether a window is over a
 * scene with 3D on it at all, so two runs can be compared over the same one.
 */
unsigned long gpu3d_poly_frames(void);

/* Self-test, the same shape every model in this port carries. */
int gpu3d_selftest(int *ranOut);

#endif /* POKEPLATINUM_3DS_GPU3D_H */
