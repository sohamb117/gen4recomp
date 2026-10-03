/*
 * 3ds/src/3ds_pica3d.h: a DS polygon as the PICA takes it.
 *
 * There is no model or view transform to write, which is why this is a
 * rasterizer swap rather than a GX translation. pc_gpu3d.c has already done
 * the matrices, the lighting, the clipping and the viewport transform; a
 * polygon arrives with FinalPosition in screen pixels, FinalColor in six bits
 * a channel, FinalZ and FinalW, and the attribute word. What is left is to
 * hand that to a GPU that insists on doing a perspective divide of its own.
 *
 * So the divide is undone before it happens. A vertex goes out as
 * (ndc * w, z, w) rather than (ndc, 1): the PICA divides by w, recovers
 * exactly the screen position the DS computed, and interpolates every
 * attribute perspective-correctly against the same w the DS used. Feeding
 * screen positions with w = 1 would make every texture on a floor affine.
 *
 * Depth is two different problems and the DS says which one.
 * interp_interpolate_z() takes a linear path for a Z-buffer polygon and a
 * perspective-correct one for a W-buffer polygon, and 34.5% of this game's
 * polygons are the second kind.
 *
 *   Z buffer: the DS interpolates FinalZ linearly in screen space and so does
 *             a PICA, but not with the same function: the DS truncates the
 *             gradient first, so its ramp is linear with a rounded slope where
 *             the PICA's is linear with the true one. Measured: the two never
 *             disagree about which of two surfaces is in front once the DS
 *             holds them 0x1000 apart, and disagree on about 0.8% below that.
 *
 *   W buffer: the DS interpolates W perspective-correctly and a PICA cannot be
 *             asked for that directly. But 1/W is screen-linear, so feeding
 *             1/W as the depth reproduces the same curve running the other
 *             way, which means the depth test is reversed for those frames.
 *             This one does not become exact at any separation: the DS
 *             approximates the perspective curve with eight bits of a factor
 *             and the PICA does not approximate it at all, so they differ on
 *             0.048% of comparisons.
 *
 * Those two numbers are why this cannot close on a byte compare the way the
 * present path, the background path and the 3D layer did. They are measured in
 * 3ds/tests/pica3d_vtx.c and pinned there as a regression guard, not as a
 * verdict; whether they are acceptable is a pixel diff on real scenes.
 *
 * That makes the depth test a per-frame register, and a frame carrying both
 * kinds against one depth buffer has no single test that is right.
 * `gx-mixed-depth-frames` counts them.
 *
 * This file touches no GPU and includes no libctru, for the reason the texture
 * converter is its own file: the arithmetic is checkable on a build machine
 * against the software renderer, and the register writing is not.
 */

#ifndef POKEPLATINUM_3DS_PICA3D_H
#define POKEPLATINUM_3DS_PICA3D_H

#include <stdint.h>

struct PcGxPolygon;

/*
 * One vertex, in the form the vertex program consumes. Clip space: the PICA
 * divides x, y and z by w. Colour and texture coordinates are already
 * normalized, because the attribute loader hands over floats and doing it here
 * keeps the shader to a matrix multiply.
 */
struct pica3d_vertex {
    float x, y, z, w;
    float r, g, b, a;
    float u, v;
};

/*
 * The state a draw would have to change. Two polygons with equal state and
 * equal texture are one draw; anything else is a new one. The polygon's ALPHA
 * and its ID are deliberately not here, alpha rides in the vertex colour and
 * costs nothing, and keying on it would put a draw call between almost every
 * pair of polygons, since every value 1-31 appears in this game.
 */
/*
 * There is no cull mode here, and that is not an omission. The DS's PolygonAttr
 * carries two bits saying which facings to keep, and it is tempting to hand
 * them to the PICA inverted; it names the facings to throw away. That would
 * cull twice: pc_gpu3d.c applies those bits itself when it builds the polygon
 * (a failing polygon is dropped and never reaches the list), so everything the
 * renderer sees has already survived. Culling again removes an arbitrary
 * subset, because the surviving winding is whatever the geometry engine left
 * and not normalised to either direction. On hardware that read as the
 * buildings drawing and the ground not.
 */
/*
 * The polygon id is in the key and its alpha is not, and the survey is what
 * Settled the difference. Edge marking compares the ID of a pixel with the ID
 * of its neighbour, and the only place a PICA can carry an ID is the stencil
 * buffer, whose reference value is per DRAW, so the ID has to split batches
 * or it cannot be used at all. `gx-opaque-ids-per-frame max 7 mean 6.22` says
 * what that costs: a frame uses seven of the sixty-four, and they follow
 * objects, so the split lands on boundaries the texture already forced. Alpha
 * is the opposite case and stays out: every value from 1 to 31 appears, and
 * keying on it would put a draw call between almost every pair of polygons.
 */
struct pica3d_state {
    uint32_t texkey;        /* TexParam's image bits, 0 when untextured   */
    uint32_t texpal;
    uint8_t  shading;       /* Attr bits 4-5: 0 modulation, 1 decal, ...  */
    uint8_t  blend;         /* the polygon is translucent                  */
    uint8_t  depthWrite;    /* a translucent polygon may or may not        */
    uint8_t  depthEqual;    /* Attr bit 14: the test is "equal", not "less"*/
    uint8_t  wbuffer;
    uint8_t  polyid;        /* Attr bits 24-29, for edge marking's stencil */
};

void pica3d_state_of(const struct PcGxPolygon *p, int textured,
                     struct pica3d_state *out);
int  pica3d_state_eq(const struct pica3d_state *a,
                     const struct pica3d_state *b);

/*
 * Whether this polygon's depth runs backwards on the PICA, which is exactly
 * "is it a W-buffer polygon", 1/W decreases where W increases. The renderer
 * sets one depth test for the frame from this; a frame that disagreed with
 * itself is what `gx-mixed-depth-frames` is counting.
 */
int pica3d_depth_reversed(const struct PcGxPolygon *p);

/*
 * ...and which end of the PICA's depth buffer that puts the near plane on,
 * which is a separate question and got the opposite answer for a whole task.
 *
 * The transform below writes NDC z in [-1, 0], near at -1, because that is the
 * range the PICA requires and the range citro3d's own projection matrices
 * produce. citro3d's default depth map is C3D_DepthMap(true, -1.0f, 0.0f), so
 * the WINDOW value is -z: the near plane is 1.0 and the far plane is 0.0. A
 * buffer cleared to 0xFFFFFFFF and tested with GPU_LESS, the habit every
 * desktop API teaches, therefore keeps the FARTHEST surface of every pair.
 * It did, for as long as the only thing drawn was one model against the rear
 * plane, where back-face culling hides the difference; an interior drew as a
 * house with nothing in it.
 *
 * So the rule lives here, next to the transform it follows from, rather than
 * in the renderer where nothing on a build machine can reach it:
 *
 *   pica3d_depth_keeps_greater()  1 = the test keeps the larger window value
 *   pica3d_depth_clear()          ...and what the buffer is cleared to, which
 *                                 is always the far end
 *
 * `reversed` is pica3d_depth_reversed() for the frame. A W-buffer frame is fed
 * 1/W, which is largest at the near plane, so it comes out on the other end of
 * the window range and takes the other test.
 */
int pica3d_depth_keeps_greater(int reversed);
uint32_t pica3d_depth_clear(int reversed);

/* The window value the PICA would store for a vertex this file produced,
 * following that same depth map. The hardware does this; it is here so the
 * rule above can be CHECKED against the transform rather than asserted
 * beside it. */
double pica3d_depth_window(const struct pica3d_vertex *v);

/*
 * The sample point, in DS pixels, added to every vertex position. The DS asks
 * a span what colour it is at the integer corner (x, y); a PICA asks at the
 * pixel centre (x + 0.5, y + 0.5). Half a pixel of geometry puts the two grids
 * back together; see the note beside the constant. Settable so `half=` in
 * p3d.txt can turn it off and the pixel diff can price it.
 */
void   pica3d_set_sample_offset(double px);
double pica3d_sample_offset(void);

/*
 * Vertex `i` of `p`, for a `surfW` x `surfH` render target.
 *
 * `texW`/`texH` size the texture the polygon samples, because DS texture
 * coordinates are in 1/16 texel units of a specific image and the PICA wants a
 * fraction of one. Pass 1, 1 for an untextured polygon.
 *
 * Returns 0, or -1 when the vertex cannot be expressed, a zero W, which the
 * DS itself refuses to divide by.
 */
int pica3d_vertex(const struct PcGxPolygon *p, unsigned i,
                  int surfW, int surfH, unsigned texW, unsigned texH,
                  struct pica3d_vertex *out);

/*
 * What the PICA's own divide will make of a vertex: the screen position, and
 * the depth it writes. Exposed because it is the only way to check the
 * transform without a PICA, the test asserts that this recovers the DS's own
 * `FinalPosition`, which is the property the whole file exists to have.
 */
void pica3d_project(const struct pica3d_vertex *v, int surfW, int surfH,
                    double *sx, double *sy, double *depth);

/* Self-test, the same shape every model in this port carries. */
int pica3d_selftest(int *ranOut);

#endif /* POKEPLATINUM_3DS_PICA3D_H */
