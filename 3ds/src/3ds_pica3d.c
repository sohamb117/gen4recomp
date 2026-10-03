/*
 * 3ds/src/3ds_pica3d.c: see 3ds_pica3d.h.
 *
 * The coordinate conventions, once, because every bug in a file like this is
 * One of them.
 *
 *   DS screen space   x right, y DOWN, (0,0) the top-left pixel of a 256x192
 *                     picture. FinalPosition is an integer pixel index.
 *   PICA clip space   x right, y UP, the visible volume -w <= x,y <= w and
 *                     -w <= z <= 0, which is the range citro3d's depth map is
 *                     set up for.
 *
 * So y is flipped here and nowhere else. The 3D layer's texture is sampled
 * with its own v flip already (the 3D layer's layer3d_quad), so flipping again would
 * put the picture back upside down; the two are not the same flip and it is
 * worth saying which is which: this one turns a DS y into a PICA y, and that
 * one turns a memory row into a texture coordinate.
 *
 * Depth goes out negative. -1 is the near plane and 0 the far one, so a
 * smaller DS depth (nearer) has to become a more negative PICA z for the
 * usual GPU_LESS test to mean the same thing. That is one subtraction, and it
 * is the reason the W-buffer case comes out needing GPU_GREATER rather than
 * two subtractions cancelling.
 */

#include "3ds_pica3d.h"

#include <stdio.h>
#include <string.h>

#include "../../pc/include/pc_gpu3d.h"

/* The DS's depth field, and its reciprocal's scale. FinalZ is 24 bits in both
 * modes: a Z-buffer polygon carries the normalized device z scaled to it, and
 * a W-buffer polygon carries the renormalized W. */
#define DS_DEPTH_MAX 16777215.0

/*
 * Where each machine asks the polygon what colour it is, and they are half a
 * pixel apart.
 *
 * pc_gpu3d_soft.c evaluates a span at integer x and y, so the point the DS
 * asks about for pixel (x, y) is the corner (x, y). A PICA asks about the
 * pixel's centre, (x + 0.5, y + 0.5), because that is what a rasterizer does.
 * Feeding the DS's own vertex positions therefore samples every attribute half
 * a pixel down and to the right of where the DS sampled it.
 *
 * Moving the geometry by +0.5 puts the two grids back on top of each other,
 * and coverage lands the same way round.
 *
 * It is not exactly a half, because every DS vertex is an integer. At exactly
 * 0.5 every polygon edge lands on a pixel centre, which is a tie for both fill
 * rules, and the two machines break it the other way from each other: the
 * floor's bottom row drew on the PICA and not on the DS, a full 160-pixel row
 * every frame. A thirty-second of a pixel of margin removes every tie and
 * costs a thirty-second of a pixel of sampling.
 *
 * Swept over the new-game replay with the pixel diff, as a share of layer
 * pixels differing by more than one six-bit step: 0 gives 8.89%, 0.25 gives
 * 5.56%, 15/32 gives 3.31%, and it goes back up to 6.73% at exactly 0.5. Flat
 * either side of 15/32, so that is the value and the jump at the end is the
 * tie.
 *
 * It is a variable rather than a literal because it is the kind of claim that
 * wants to be turned off from the SD card and diffed, and because
 * pica3d_project() has to take it back off for the round trip.
 */
static double sSampleOffset = 15.0 / 32.0;

void pica3d_set_sample_offset(double px)
{
    sSampleOffset = px;
}

double pica3d_sample_offset(void)
{
    return sSampleOffset;
}

void pica3d_state_of(const PcGxPolygon *p, int textured,
                     struct pica3d_state *out)
{
    uint32_t attr = p->Attr;

    memset(out, 0, sizeof *out);

    /*
     * The image bits only, the way the texture converter keys its cache: the wrap and flip
     * bits and the coordinate-transform mode change how an image is sampled
     * and not what is in it. Wrapping IS a state change on the PICA, but it
     * belongs to the texture binding rather than to the image, and a texture
     * carries its own, so two polygons sampling one image with different
     * wrap bits are still one draw only if they also agree there. They do not
     * in general, so the wrap bits stay in the key.
     */
    out->texkey = textured ? (p->TexParam & 0x3FFFFFFFu) : 0u;
    out->texpal = textured ? p->TexPalette : 0u;

    out->shading    = (uint8_t)((attr >> 4) & 3);
    out->blend      = p->Translucent;
    out->depthEqual = (uint8_t)((attr >> 14) & 1);
    out->wbuffer    = p->WBuffer;

    /*
     * An opaque polygon always writes depth. A translucent one writes it only
     * with Attr bit 11 set, which is the DS's own rule and the reason two
     * translucent surfaces behind each other both show.
     */
    out->depthWrite = p->Translucent ? (uint8_t)((attr >> 11) & 1) : 1u;
    out->polyid     = (uint8_t)((attr >> 24) & 0x3F);
}

int pica3d_state_eq(const struct pica3d_state *a, const struct pica3d_state *b)
{
    return a->texkey == b->texkey && a->texpal == b->texpal
        && a->shading == b->shading
        && a->blend == b->blend && a->depthWrite == b->depthWrite
        && a->depthEqual == b->depthEqual && a->wbuffer == b->wbuffer
        && a->polyid == b->polyid;
}

int pica3d_depth_reversed(const PcGxPolygon *p)
{
    return p->WBuffer != 0;
}

int pica3d_depth_keeps_greater(int reversed)
{
    return !reversed;
}

uint32_t pica3d_depth_clear(int reversed)
{
    /* The far end, which is the opposite end from the test's. */
    return pica3d_depth_keeps_greater(reversed) ? 0u : 0xFFFFFFFFu;
}

/*
 * The window value the PICA would store for a vertex this transform produced,
 * following citro3d's default depth map. Only the selftest needs it, the
 * hardware does it, and it is what makes the rule above checkable rather
 * than asserted.
 */
double pica3d_depth_window(const struct pica3d_vertex *v)
{
    return (v->w != 0.0f) ? -((double)v->z / (double)v->w) : 0.0;
}

int pica3d_vertex(const PcGxPolygon *p, unsigned i, int surfW, int surfH,
                  unsigned texW, unsigned texH, struct pica3d_vertex *out)
{
    const PcGxVertex *v;
    double w, ndcx, ndcy, depth;

    if (p == NULL || out == NULL || i >= p->NumVertices
        || surfW <= 0 || surfH <= 0 || texW == 0 || texH == 0) {
        return -1;
    }
    v = p->Vertices[i];
    if (v == NULL) {
        return -1;
    }

    /*
     * W is the one that cannot be zero. The geometry engine already refuses to
     * divide by it; a zero-W vertex gets FinalPosition (0,0) rather than a
     * projected one, so a polygon carrying one has a corner nailed to the
     * top-left whatever this file does. It is refused here so the caller sends
     * the frame to software instead of drawing that corner.
     */
    w = (double)p->FinalW[i];
    if (w == 0.0) {
        return -1;
    }

    /*
     * The screen pixel back to normalized device coordinates. The DS's y grows
     * downward and the PICA's grows upward, so that axis is negated, and it
     * is negated here rather than in the projection matrix so that
     * pica3d_project() below can be a plain inverse and the test can assert
     * the round trip.
     */
    ndcx =  (2.0 * ((double)v->FinalPosition[0] + sSampleOffset)
             / (double)surfW) - 1.0;
    ndcy = -((2.0 * ((double)v->FinalPosition[1] + sSampleOffset)
              / (double)surfH) - 1.0);

    /*
     * Depth, and which of the two rules applies.
     *
     * Z buffer: FinalZ is already the normalized device z, scaled. Screen
     * linear on both machines, so it goes out as it is.
     *
     * W buffer: the DS interpolates W perspective-correctly and the PICA
     * cannot be asked for that, but 1/W is screen-linear, so it is fed
     * instead and the depth test is reversed for the frame. The reciprocal is
     * scaled by the largest W the field can hold, so the result stays in
     * 0..1 and the nearest surface is the LARGEST value rather than the
     * smallest, which is what pica3d_depth_reversed() tells the caller.
     */
    if (p->WBuffer) {
        double fw = (double)p->FinalZ[i];   /* the renormalized W          */

        depth = (fw > 0.0) ? (1.0 / fw) : 1.0;
        if (depth > 1.0) {
            depth = 1.0;
        }
    } else {
        depth = (double)p->FinalZ[i] / DS_DEPTH_MAX;
    }

    /* -1 near, 0 far: the visible volume citro3d's depth map is set for. */
    out->x = (float)(ndcx * w);
    out->y = (float)(ndcy * w);
    out->z = (float)((depth - 1.0) * w);
    out->w = (float)w;

    /*
     * Colour, and `FinalColor` is not the six bits its name suggests. The
     * geometry engine writes `(c5 << 4) + 0xF` for a five-bit vertex colour
     * and 0 for zero (pc_gpu3d.c), so the field runs 0..511 and carries four
     * spare bits for the interpolator. pc_gpu3d_soft.c takes them back off at
     * the pixel, render_pixel() is handed `vr >> 3` and modulates in six
     * bits, so >> 3 is the scale, and dividing the raw field by 63 makes
     * every colour past c5 = 3 saturate to white.
     *
     * That is exactly what it did, and the picture said so before this comment
     * did: an untextured polygon drew as a white box where the DS drew a
     * shadow, and every lit surface drew at full brightness, because
     * modulating by a saturated white is modulating by nothing. It is the
     * largest single difference the pixel diff has found.
     *
     * The polygon's alpha rides in the vertex rather than in a uniform, which
     * is what keeps a change of alpha from being a change of state, and this
     * game uses every value from 1 to 31, so that is the difference between
     * one draw and hundreds.
     *
     * ALPHA 0 is not transparent, it is wireframe. render_pixel() sets the
     * alpha to 31 for it and draws the edges only. Nothing in this game does
     * that (the survey found zero wireframe polygons in 6.8 million) so it
     * is expressed as opaque here and the survey's counter is what would say
     * if that ever stopped being true.
     */
    {
        uint32_t alpha = (p->Attr >> 16) & 0x1F;

        out->r = (float)(v->FinalColor[0] >> 3) / 63.0f;
        out->g = (float)(v->FinalColor[1] >> 3) / 63.0f;
        out->b = (float)(v->FinalColor[2] >> 3) / 63.0f;
        out->a = (alpha == 0u) ? 1.0f : (float)alpha / 31.0f;
    }

    /*
     * Texture coordinates: the DS carries them in 1/16 of a texel, signed, and
     * the PICA wants a fraction of the image. Dividing by 16 and by the size
     * is the whole conversion; the wrapping that makes a coordinate outside
     * the image mean something is the sampler's, which is why the texture converter leaves
     * it out of the converted texels.
     *
     * v is NOT flipped here. The converter writes DS row 0 into texture row 0,
     * and the flip that a PICA texture needs belongs to whoever binds it.
     */
    out->u = (float)v->TexCoords[0] / (16.0f * (float)texW);
    /*
     * AND v counts down from 1, which is the whole of this line. Memory row 0
     * of a PICA texture is v = 1.0, the GPU present settled that with a byte compare and
     * The 3D layer's layer quad already draws by it. 3ds_tex3d.c writes DS texel row 0
     * into memory row 0, so sampling with v = t / height reads every texture
     * upside down. On hardware that is not subtle and it is not obviously a v
     * flip either: it reads as "garbled", because a mirrored texture on a
     *3D model looks like corruption rather than like a reflection.
     */
    out->v = 1.0f - (float)v->TexCoords[1] / (16.0f * (float)texH);
    return 0;
}

void pica3d_project(const struct pica3d_vertex *v, int surfW, int surfH,
                    double *sx, double *sy, double *depth)
{
    double w = (double)v->w;

    if (w == 0.0) {
        w = 1.0;
    }
    /* ...less the half pixel pica3d_vertex() added, so this stays the plain
     * inverse the self-test asserts the round trip against. */
    if (sx != NULL) {
        *sx = (((double)v->x / w) + 1.0) * 0.5 * (double)surfW - sSampleOffset;
    }
    if (sy != NULL) {
        *sy = (1.0 - ((double)v->y / w)) * 0.5 * (double)surfH - sSampleOffset;
    }
    if (depth != NULL) {
        *depth = ((double)v->z / w) + 1.0;
    }
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#define CHECK(cond)                                                          \
    do {                                                                     \
        ran++;                                                               \
        if (!(cond)) {                                                       \
            fprintf(stderr, "  3ds_pica3d.c:%d failed\n", __LINE__);          \
            bad++;                                                           \
        }                                                                    \
    } while (0)

int pica3d_selftest(int *ranOut)
{
    int ran = 0, bad = 0;
    PcGxPolygon poly;
    PcGxVertex vtx;
    struct pica3d_vertex out;
    struct pica3d_state a, b;
    double sx, sy, depth;

    memset(&poly, 0, sizeof poly);
    memset(&vtx, 0, sizeof vtx);
    poly.NumVertices = 1;
    poly.Vertices[0] = &vtx;
    poly.Attr = 31u << 16;              /* opaque, modulation, no culling */

    /*
     * The round trip, which is the property the file exists to have: a screen
     * pixel out of the geometry engine, through the clip-space form, through
     * the divide the PICA will perform, and back to the same pixel. A sign
     * error on either axis fails here and nowhere else without a GPU.
     */
    vtx.FinalPosition[0] = 200;
    vtx.FinalPosition[1] = 40;
    poly.FinalW[0] = 0x1000;
    poly.FinalZ[0] = 0x400000;
    CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == 0);
    pica3d_project(&out, 256, 192, &sx, &sy, &depth);
    CHECK(sx > 199.99 && sx < 200.01);
    CHECK(sy > 39.99 && sy < 40.01);

    /* ...and it has to hold at a different W, which is the whole point of
     * multiplying through by it. */
    poly.FinalW[0] = 0x7FFF;
    CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == 0);
    pica3d_project(&out, 256, 192, &sx, &sy, NULL);
    CHECK(sx > 199.99 && sx < 200.01);
    CHECK(sy > 39.99 && sy < 40.01);

    /* The corners, where an off-by-one in the flip hides. */
    vtx.FinalPosition[0] = 0;
    vtx.FinalPosition[1] = 0;
    CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == 0);
    pica3d_project(&out, 256, 192, &sx, &sy, NULL);
    CHECK(sx > -0.01 && sx < 0.01);
    CHECK(sy > -0.01 && sy < 0.01);
    /* DS y = 0 is the TOP, so it must be the PICA's +y. */
    CHECK(out.y > 0.0f);

    /* Depth: nearer is more negative, and the near plane is -w. */
    poly.FinalW[0] = 0x1000;
    poly.FinalZ[0] = 0;
    CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == 0);
    pica3d_project(&out, 256, 192, NULL, NULL, &depth);
    CHECK(depth > -0.001 && depth < 0.001);
    poly.FinalZ[0] = 0xFFFFFF;
    CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == 0);
    pica3d_project(&out, 256, 192, NULL, NULL, &depth);
    CHECK(depth > 0.999 && depth < 1.001);

    /* W-buffer depth runs the other way: a LARGER W is further, and 1/W is
     * therefore smaller, so the test has to be reversed, and the flag has to
     * say so. */
    poly.WBuffer = 1;
    poly.FinalZ[0] = 0x100;
    CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == 0);
    pica3d_project(&out, 256, 192, NULL, NULL, &depth);
    {
        double near_ = depth;

        poly.FinalZ[0] = 0x8000;        /* further away */
        CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == 0);
        pica3d_project(&out, 256, 192, NULL, NULL, &depth);
        CHECK(depth < near_);
    }
    CHECK(pica3d_depth_reversed(&poly) == 1);
    poly.WBuffer = 0;
    CHECK(pica3d_depth_reversed(&poly) == 0);

    /*
     * The surface is the render target and not the picture, and this pins the
     * distinction because getting it wrong is invisible in every host check:
     * Both are self-consistent, and only the compositor sampling the top 192
     * rows of a 256-row target shows it. A DS pixel must land on the SAME
     * numbered pixel of the target, so the middle of the picture is target row
     * 96 and not target row 128.
     */
    vtx.FinalPosition[0] = 128;
    vtx.FinalPosition[1] = 96;
    poly.FinalW[0] = 0x1000;
    CHECK(pica3d_vertex(&poly, 0, 256, 256, 8, 8, &out) == 0);
    pica3d_project(&out, 256, 256, &sx, &sy, NULL);
    CHECK(sy > 95.99 && sy < 96.01);
    /* ...and the bottom row of the picture is target row 192, inside a 256-row
     * target, which is what the compositor's quad stops at. */
    vtx.FinalPosition[1] = 192;
    CHECK(pica3d_vertex(&poly, 0, 256, 256, 8, 8, &out) == 0);
    pica3d_project(&out, 256, 256, NULL, &sy, NULL);
    CHECK(sy > 191.99 && sy < 192.01);
    vtx.FinalPosition[0] = 200;
    vtx.FinalPosition[1] = 40;

    /* A zero W is refused rather than divided by. */
    poly.FinalW[0] = 0;
    CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == -1);
    poly.FinalW[0] = 0x1000;

    /*
     * Texture coordinates: 1/16 texel to a fraction of the image, and v counts
     * DOWN from 1 because memory row 0 of a PICA texture is v = 1.0. Getting
     * that wrong samples every texture upside down, which reads on hardware as
     * garbled rather than as mirrored.
     */
    vtx.TexCoords[0] = 16 * 8;          /* eight texels across an 8-wide  */
    vtx.TexCoords[1] = 16 * 4;
    CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == 0);
    CHECK(out.u > 0.999f && out.u < 1.001f);
    CHECK(out.v > 0.499f && out.v < 0.501f);
    /* Texel row 0 is the TOP of the image and therefore v = 1. */
    vtx.TexCoords[1] = 0;
    CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == 0);
    CHECK(out.v > 0.999f && out.v < 1.001f);
    /* ...and the last row is the bottom, at v = 0. */
    vtx.TexCoords[1] = 16 * 8;
    CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == 0);
    CHECK(out.v > -0.001f && out.v < 0.001f);
    vtx.TexCoords[0] = 0;
    vtx.TexCoords[1] = 0;

    /*
     * The vertex colour's scale, which nothing checked until it was wrong on
     * every polygon in the game. `FinalColor` is (c5 << 4) + 0xF, so white is
     * 511 and not 63; the renderer wants pc_gpu3d_soft.c's own `>> 3`, which
     * puts white at 1.0 and a mid grey near the middle rather than saturated.
     */
    vtx.FinalColor[0] = (31 << 4) + 0xF;        /* c5 = 31, white          */
    vtx.FinalColor[1] = (16 << 4) + 0xF;        /* c5 = 16, about half     */
    vtx.FinalColor[2] = 0;                      /* c5 = 0 stays 0          */
    CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == 0);
    CHECK(out.r > 0.999f && out.r < 1.001f);
    CHECK(out.g > 0.51f && out.g < 0.53f);
    CHECK(out.b > -0.001f && out.b < 0.001f);
    vtx.FinalColor[0] = vtx.FinalColor[1] = vtx.FinalColor[2] = 0;

    /* Alpha rides in the vertex, and 0 means wireframe rather than invisible. */
    poly.Attr = (31u << 16);
    CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == 0);
    CHECK(out.a > 0.999f);
    poly.Attr = (16u << 16);
    CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == 0);
    CHECK(out.a > 0.51f && out.a < 0.52f);
    poly.Attr = 0;
    CHECK(pica3d_vertex(&poly, 0, 256, 192, 8, 8, &out) == 0);
    CHECK(out.a > 0.999f);

    /*
     * The cull bits are NOT state, because pc_gpu3d.c has already applied them,
     * so two polygons that differ only there are still one draw. Culling
     * them a second time is what made the ground disappear.
     */
    poly.Attr = (1u << 6);
    pica3d_state_of(&poly, 0, &a);
    poly.Attr = (2u << 6);
    pica3d_state_of(&poly, 0, &b);
    CHECK(pica3d_state_eq(&a, &b));

    /* The polygon ID splits a batch, because edge marking's stencil reference
     * is per draw. Seven a frame, so it is affordable; see the header. */
    poly.Attr = (9u << 24);
    pica3d_state_of(&poly, 0, &a);
    CHECK(a.polyid == 9);
    poly.Attr = (10u << 24);
    pica3d_state_of(&poly, 0, &b);
    CHECK(!pica3d_state_eq(&a, &b));
    poly.Attr = 0;

    /* An opaque polygon always writes depth; a translucent one asks. */
    poly.Attr = 0;
    poly.Translucent = 0;
    pica3d_state_of(&poly, 0, &a);
    CHECK(a.depthWrite == 1);
    poly.Translucent = 1;
    pica3d_state_of(&poly, 0, &a);
    CHECK(a.depthWrite == 0);
    poly.Attr = (1u << 11);
    pica3d_state_of(&poly, 0, &a);
    CHECK(a.depthWrite == 1);
    poly.Translucent = 0;

    /* An untextured polygon carries no image identity, whatever TexParam
     * says; which is what stops DISP3DCNT bit 0 being clear from splitting
     * one draw into hundreds. */
    poly.Attr = 0;
    poly.TexParam = 0x12345678u;
    poly.TexPalette = 0x99u;
    pica3d_state_of(&poly, 0, &a);
    CHECK(a.texkey == 0 && a.texpal == 0);
    pica3d_state_of(&poly, 1, &b);
    CHECK(b.texkey != 0);
    CHECK(!pica3d_state_eq(&a, &b));
    pica3d_state_of(&poly, 1, &a);
    CHECK(pica3d_state_eq(&a, &b));

    /* The alpha is not state, and it rides in the vertex for that reason. The
     * polygon ID is, since the 3D pixel work: the stencil reference is per draw. */
    poly.Attr = (7u << 16) | (13u << 24);
    pica3d_state_of(&poly, 1, &a);
    poly.Attr = (29u << 16) | (13u << 24);
    pica3d_state_of(&poly, 1, &b);
    CHECK(pica3d_state_eq(&a, &b));
    poly.Attr = (29u << 16) | (44u << 24);
    pica3d_state_of(&poly, 1, &b);
    CHECK(!pica3d_state_eq(&a, &b));

    /*
     * Which depth test keeps the nearer surface, checked rather than asserted.
     * Two vertices at the same pixel, one near and one far, in both modes: the
     * window value the PICA would store for the nearer one has to be the one
     * the frame's test keeps, and the clear has to be the other end. The
     * renderer had this backwards in both modes and every check in this file
     * passed, because they all compare in the DS's depth units and the error is
     * in the mapping out of them.
     */
    poly.Attr = 31u << 16;
    poly.NumVertices = 1;
    vtx.FinalPosition[0] = 128;
    vtx.FinalPosition[1] = 96;
    poly.FinalW[0] = 0x1000;
    {
        struct pica3d_vertex vNear, vFar;
        int mode;

        for (mode = 0; mode < 2; mode++) {
            double dNear, dFar;
            int rev;

            poly.WBuffer = (uint8_t)mode;
            rev = pica3d_depth_reversed(&poly);
            /* The DS's own convention on both: a smaller stored value is
             * nearer for Z, and a smaller W is nearer for the other. */
            poly.FinalZ[0] = 0x000100;
            CHECK(pica3d_vertex(&poly, 0, 256, 256, 1, 1, &vNear) == 0);
            dNear = pica3d_depth_window(&vNear);
            poly.FinalZ[0] = 0x7F0000;
            CHECK(pica3d_vertex(&poly, 0, 256, 256, 1, 1, &vFar) == 0);
            dFar = pica3d_depth_window(&vFar);

            if (pica3d_depth_keeps_greater(rev)) {
                CHECK(dNear > dFar);
                CHECK(pica3d_depth_clear(rev) == 0u);
            } else {
                CHECK(dNear < dFar);
                CHECK(pica3d_depth_clear(rev) == 0xFFFFFFFFu);
            }
        }
        poly.WBuffer = 0;
    }

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return bad;
}
