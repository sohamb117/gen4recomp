/*
 * 3ds/src/3ds_pica3d_gpu.c: see 3ds_pica3d_gpu.h.
 *
 * The state this sets, and why each one is what the DS does.
 *
 *   depth      GPU_LESS with depth writes, or GPU_GREATER for a W-buffer
 *              frame, because 3ds_pica3d.c feeds 1/W there and 1/W runs the
 *              other way. One test per frame, which is sound only because no
 *              frame mixes the two modes; a frame that did is refused.
 *   cull       nothing, and that is the correction that made the ground
 *              appear. pc_gpu3d.c applies PolygonAttr's two facing bits when
 *              it builds the polygon, so culling again removes an arbitrary
 *              subset: the winding that reaches this file is whatever the
 *              geometry engine left rather than a normalised direction.
 *   blend      source over dest for translucent polygons, but not against an
 *              empty pixel: a PICA SRC_ALPHA blend against a clear multiplies
 *              the first surface by its own alpha. Two stencil passes, replace
 *              where nothing has been drawn, then blend where something has.
 *   alpha test greater than zero, which makes an index-zero texel disappear
 *              rather than paint black.
 *   TEV        one stage, modulate: texture times vertex colour, both
 *              channels. The survey found modulation on every one of 6.8 M
 *              polygons and no decal, toon or shadow in the whole replay, so
 *              the other three shading modes are a refusal rather than dead
 *              code.
 *
 * The orientation is the one thing arithmetic cannot settle. A render target
 * on a texture writes in the texture's own layout, and whether the PICA's +y
 * lands on memory row 0 or on the last row is a property of the silicon. The
 * same question for the presented frame was settled with a byte compare, and
 * the answer was not the obvious one. So the flip lives in one named constant
 * below and the first console frame is what sets it.
 */

#include "3ds_pica3d_gpu.h"

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "3ds_layer3d.h"
#include "3ds_pica3d.h"
#include "3ds_tex3d.h"
#include "3ds_tile.h"
#include "3ds_view.h"

#include "../../pc/include/pc_gpu3d.h"

extern const uint8_t poly3d_shbin[];
extern const uint8_t poly3d_shbin_end[];

/*
 * WEAK, because this file is in both links and the geometry engine is only in
 * one: the self-test .3dsx links no pc/hw at all. Undefined and weak is a null
 * pointer on this toolchain, which is the test every other seam in this port
 * uses for the same thing.
 */
extern PcGxPolygon **pc_gpu3d_render_polygons(uint32_t *count)
    __attribute__((weak));
extern const PcGxRenderRegs *pc_gpu3d_render_regs(void) __attribute__((weak));

/* ------------------------------------------------------------------ */
/* Sizes                                                               */
/* ------------------------------------------------------------------ */
/*
 * The DS's polygon RAM is 2,048 polygons and its vertex RAM 6,144 vertices,
 * and a polygon can leave the clipper with ten. Triangulating a fan of n
 * vertices gives n - 2 triangles, so the worst case is bounded by the vertex
 * count rather than by the polygon count: 6,144 vertices in fans is under
 * 3 * (6144 - 2 * 2048) triangles. 8,192 triangles is comfortably past it and
 * costs 786 KB of linear memory.
 *
 * The survey says the real demand is 454 polygons at the peak over the whole
 * replay, so this is sized for the hardware's limit and not for the game's;
 * a frame that overflowed would be a refusal, and a refusal that can only
 * happen on a scene this game never draws is one nobody has to reason about.
 */
/*
 * The target's own size, and it is not the picture's. A PICA texture's sides
 * are powers of two, so the 256x192 the DS draws lives in the top 192 rows of
 * a 256x256 one. Normalized device coordinates span the WHOLE target, so the
 * transform has to be told 256, the first version passed 192, the picture's
 * height, and every DS row landed 4/3 of the way down: an object at the middle
 * of the screen drew a sixth of a screen too low and the bottom third of the
 * layer fell outside the rows the compositor samples. It read on hardware as
 * "lower than usual, and garbled".
 */
#define P3D_TARGET    256

#define P3D_MAX_TRIS  8192
#define P3D_MAX_VERTS (P3D_MAX_TRIS * 3)
#define P3D_MAX_BATCH 1024

/*
 * The last six vertices are edge marking's reset quad and not the game's. It
 * covers the whole target at the far plane and its only job is to put the
 * depth and stencil buffers back to "the rear plane, nothing drawn" between
 * one neighbour direction and the next; which C3D_FrameBufClear cannot do,
 * because it is an immediate GX_MemoryFill and this is the middle of a command
 * list. A quad is one draw and needs no barrier.
 */
#define P3D_QUAD_VERTS 6
#define P3D_QUAD_BASE  (P3D_MAX_VERTS - P3D_QUAD_VERTS)

/*
 * The converted-texture pool. The 3D survey measured the working set at 28 distinct
 * images and 815,104 bytes expanded to RGBA8, so 1.5 MB serves any frame this
 * game draws with room for a scene half again as heavy.
 */
#define P3D_TEX_WORDS (384 * 1024)

struct p3dvertex {
    float x, y, z, w;
    float r, g, b, a;
    float u, v;
};

struct batch {
    int first, count;           /* into the vertex buffer                 */
    int entry;                  /* tex3d cache entry, or -1 untextured    */
    uint32_t texparam;          /* for the wrap bits                      */
    uint8_t blend;
    uint8_t depthWrite;
    uint8_t depthEqual;
    uint8_t polyid;             /* edge marking's stencil reference       */
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static DVLB_s *sDvlb;
static shaderProgram_s sProgram;
static C3D_RenderTarget *sTarget;
static C3D_Tex sTex;                    /* the layer, and the target on it */
static struct p3dvertex *sVerts;
static int sSlideLoc = -1;              /* poly3d.v.pica's uSlide          */
static uint32_t *sTexPool;
static struct batch sBatch[P3D_MAX_BATCH];
static C3D_Tex sBoundTex[256];          /* one per tex3d entry             */
static int sReady;

static int sVertN;
static int sBatchN;
static int sReversed;                   /* this frame is a W-buffer frame  */
static unsigned sFrame;

/*
 * What to leave out, read once from sdmc:/3ds/pokeplatinum/p3d.txt as a
 * space-separated list. This is a bisect and not a feature: when a renderer
 * draws the wrong picture, the question is which of its decisions is wrong,
 * and turning them off one at a time answers in one boot what guessing answers
 * in four.
 *
 *   notex     bind nothing; the polygon is its interpolated vertex colour.
 *   nodepth   no depth test at all, painter's order.
 *   noblend   no translucency; every polygon opaque.
 *   flat      one colour per polygon, ignoring the vertex colours.
 *   dump      write the layer texture to sdmc as a PPM every couple of
 *             seconds, read straight out of VRAM. Not a bisect: four
 *             hypotheses in a row survived reasoning and died on hardware, and
 *             every one would have been settled by looking at the picture.
 *   nosplit   leave out the command-list barrier between rendering the layer
 *             and sampling it. This proves the barrier is what fixed the
 *             picture: without it the console shows a frame that is right for
 *             a second and garbage afterwards, and the emulator shows nothing
 *             wrong either way.
 *   diff      compare every frame's layer with the software rasterizer's own
 *             and report the difference. 3ds_layer3d.c owns it; this flag is
 *             only where it is spelled. It costs a frame's worth of uncached
 *             VRAM reads, so it is never a default.
 */
static int sNoTex, sNoDepth, sNoBlend, sFlat, sNoSplit, sDump, sDiff, sEdge;
static int sEdgeFlat;                   /* paint the edge colour, not blend it */
static double sHalf = 15.0 / 32.0;      /* `half=`, in DS pixels           */

static unsigned long sFrames;           /* frames the draw half ran on     */
static unsigned long sGeomFrames;       /* ...and had geometry on           */
static unsigned long sProduced;         /* frames the build half was asked  */
static unsigned long sRefusedDepth;     /* ...refused: mixed depth modes   */
static unsigned long sRefusedTex;       /* ...refused: a texture would not  */
static unsigned long sRefusedRoom;      /* ...refused: out of vertex room   */
static unsigned long sRefusedMode;      /* ...refused: a shading mode       */
static unsigned long sTris, sDraws;
static unsigned long sTrisMax, sDrawsMax;

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

int pica3d_gpu_init(void)
{
    if (sReady) {
        return 0;
    }

    sDvlb = DVLB_ParseFile((u32 *)poly3d_shbin,
                           (u32)(poly3d_shbin_end - poly3d_shbin));
    if (sDvlb == NULL) {
        fprintf(stderr, "3ds-pica3d: the polygon shader did not parse\n");
        return -1;
    }
    shaderProgramInit(&sProgram);
    shaderProgramSetVsh(&sProgram, &sDvlb->DVLE[0]);
    sSlideLoc = shaderInstanceGetUniformLocation(sProgram.vertexShader,
                                                 "uSlide");

    /*
     * The target is the layer texture itself, which is the whole reason the 3D layer's
     * seam was worth building first: the compositor already samples this
     * texture, so a PICA-drawn layer costs no copy and no readback at all.
     * VRAM rather than linear, because it is a colour buffer the GPU writes
     * every frame and never the CPU.
     */
    if (!C3D_TexInitVRAM(&sTex, P3D_TARGET, P3D_TARGET, GPU_RGBA8)) {
        fprintf(stderr, "3ds-pica3d: no VRAM for the 3D layer target\n");
        return -1;
    }
    C3D_TexSetFilter(&sTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    /*
     * DEPTH24_STENCIL8 rather than DEPTH24, and the eight bits are the 3D pixel work's.
     * Edge marking asks "does the pixel one over belong to a different polygon
     * and sit further away", and the only place a PICA can put a polygon ID is
     * the stencil buffer. It costs 64 KB of VRAM and nothing else, the
     * stencil test is off for every pass but that one.
     */
    sTarget = C3D_RenderTargetCreateFromTex(&sTex, GPU_TEXFACE_2D, 0,
                                            GPU_RB_DEPTH24_STENCIL8);
    if (sTarget == NULL) {
        fprintf(stderr, "3ds-pica3d: no render target on the layer\n");
        return -1;
    }

    sVerts = (struct p3dvertex *)linearAlloc(sizeof *sVerts * P3D_MAX_VERTS);
    if (sVerts == NULL) {
        fprintf(stderr, "3ds-pica3d: no linear memory for %d vertices\n",
                P3D_MAX_VERTS);
        return -1;
    }

    sTexPool = (uint32_t *)linearAlloc(sizeof *sTexPool * P3D_TEX_WORDS);
    if (sTexPool == NULL || tex3d_cache_init(sTexPool, P3D_TEX_WORDS) != 0) {
        fprintf(stderr, "3ds-pica3d: no linear memory for the texture pool\n");
        return -1;
    }

    {
        FILE *f = fopen("sdmc:/3ds/pokeplatinum/p3d.txt", "r");
        char word[16];

        while (f != NULL && fscanf(f, "%15s", word) == 1) {
            if      (!strcmp(word, "notex"))   sNoTex = 1;
            else if (!strcmp(word, "nodepth")) sNoDepth = 1;
            else if (!strcmp(word, "noblend")) sNoBlend = 1;
            else if (!strcmp(word, "flat"))    sFlat = 1;
            else if (!strcmp(word, "nosplit")) sNoSplit = 1;
            else if (!strcmp(word, "dump"))    sDump = 1;
            else if (!strcmp(word, "diff"))    sDiff = 1;
            /* The 3D pixel work's outline. Off until the pixel diff and the frame time
             * have both been asked about it. */
            else if (!strcmp(word, "edge"))    sEdge = 1;
            else if (!strcmp(word, "edgeflat")) { sEdge = 1; sEdgeFlat = 1; }
            /* The sample point, in DS pixels, 3ds_pica3d.c owns the
             * reasoning and the sweep that chose the default. This is here so
             * the sweep can be run again from the card without a build. */
            else if (!strncmp(word, "half=", 5)) sHalf = atof(word + 5);
            else fprintf(stderr, "3ds-pica3d: p3d.txt says \"%s\", ignored\n",
                         word);
        }
        if (f != NULL) {
            fclose(f);
        }
    }

    pica3d_set_sample_offset(sHalf);
    sReady = 1;
    fprintf(stderr, "3ds-pica3d: the PICA draws the polygons%s%s%s%s%s%s%s%s\n",
            sNoTex ? " notex" : "", sNoDepth ? " nodepth" : "",
            sNoBlend ? " noblend" : "", sFlat ? " flat" : "",
            sNoSplit ? " nosplit" : "", sDump ? " dump" : "",
            sDiff ? " diff" : "",
            sEdgeFlat ? " edgeflat" : sEdge ? " edge" : "");
    return 0;
}

int pica3d_gpu_ready(void)
{
    return sReady;
}

void *pica3d_gpu_texture(void)
{
    return sReady ? &sTex : NULL;
}

/* ------------------------------------------------------------------ */
/* The CPU half                                                        */
/* ------------------------------------------------------------------ */

static int emit(const PcGxPolygon *p, unsigned i, unsigned texW, unsigned texH)
{
    struct pica3d_vertex v;

    if (pica3d_vertex(p, i, P3D_TARGET, P3D_TARGET, texW, texH, &v) != 0) {
        return -1;
    }
    /*
     * DS pixel (x, y) IS target pixel (x, y): the picture occupies the top-left
     * 256x192 of the target and the compositor's quad samples exactly that,
     * with v running from 1.0 at memory row 0, the mapping the present path established
     * with a byte compare. So the transform needs the TARGET's size and the
     * picture's origin, which is why P3D_TARGET goes in above and no offset
     * comes with it.
     */
    sVerts[sVertN].x = v.x;
    sVerts[sVertN].y = v.y;
    sVerts[sVertN].z = v.z;
    sVerts[sVertN].w = v.w;
    sVerts[sVertN].r = v.r;
    sVerts[sVertN].g = v.g;
    sVerts[sVertN].b = v.b;
    sVerts[sVertN].a = v.a;
    sVerts[sVertN].u = v.u;
    sVerts[sVertN].v = v.v;
    sVertN++;
    return 0;
}

/*
 * Close the one-pixel cracks a PICA's fill rule leaves between triangles the
 * DS anti-aliases over. Each vertex is pushed 1/2 pixel away from the opposite
 * edge, along the outward normal, so adjacent triangles overlap instead of
 * gapping. Winding-independent: the third vertex says which way is out. The
 * title Giratina's tendrils and the overworld's terrain seams are the same
 * hole.
 */
static void expand_tri(struct p3dvertex *t)
{
    float sx[3], sy[3], w[3], ox[3], oy[3];
    const float ipx = 2.0f / (float)P3D_TARGET;
    int i;

    for (i = 0; i < 3; i++) {
        w[i] = (t[i].w != 0.0f) ? t[i].w : 1.0f;
        sx[i] = t[i].x / w[i];
        sy[i] = t[i].y / w[i];
        ox[i] = 0.0f;
        oy[i] = 0.0f;
    }
    for (i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        int k = (i + 2) % 3;
        float ex = sx[j] - sx[i];
        float ey = sy[j] - sy[i];
        float len = sqrtf(ex * ex + ey * ey);
        float px, py, mx, my, s;

        if (len < 1e-12f) {
            continue;
        }
        px = -ey / len;
        py =  ex / len;
        mx = 0.5f * (sx[i] + sx[j]);
        my = 0.5f * (sy[i] + sy[j]);
        if ((sx[k] - mx) * px + (sy[k] - my) * py > 0.0f) {
            px = -px;
            py = -py;
        }
        s = ipx * 0.5f;
        ox[i] += px * s;
        oy[i] += py * s;
        ox[j] += px * s;
        oy[j] += py * s;
    }
    for (i = 0; i < 3; i++) {
        t[i].x = (sx[i] + ox[i]) * w[i];
        t[i].y = (sy[i] + oy[i]) * w[i];
    }
}

/*
 * The layer, straight out of VRAM, as a PPM.
 *
 * Called from the build half, which runs at survey time, outside any citro3d
 * frame and long after the previous frame was submitted and presented, so the
 * GPU has finished with it and VRAM is uncached to the ARM11. That is the only
 * point in the frame where this is both safe and free of a stall.
 *
 * De-swizzled with the tile cache's own tile_swizzle(), because the sampler's order is
 * the one thing here that is already proven: if the picture comes out scrambled
 * in a way this walk cannot explain, the scramble is the finding.
 */
uint32_t pica3d_gpu_texel(int x, int y)
{
    const uint32_t *tex = (const uint32_t *)sTex.data;
    size_t block;

    if (tex == NULL || x < 0 || y < 0 || x >= P3D_TARGET || y >= P3D_TARGET) {
        return 0u;
    }
    block = (size_t)(y / TILE_SIDE) * (P3D_TARGET / TILE_SIDE)
          + (size_t)(x / TILE_SIDE);
    /* 0xRRGGBBAA, the order the tile cache's tile_texel() writes and the 2D path has
     * already had byte-compared on this console. */
    return tex[block * TILE_TEXELS + (unsigned)tile_swizzle(x & 7, y & 7)];
}

int pica3d_gpu_diff(void)
{
    return sDiff;
}

static void dump_layer(void)
{
    static const char *kPath = "sdmc:/3ds/pokeplatinum/p3d-layer.ppm";
    FILE *f;
    int x, y;

    if (sTex.data == NULL) {
        return;
    }
    f = fopen(kPath, "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", P3D_TARGET, P3D_TARGET);
    for (y = 0; y < P3D_TARGET; y++) {
        for (x = 0; x < P3D_TARGET; x++) {
            uint32_t c = pica3d_gpu_texel(x, y);
            unsigned char rgb[3];

            rgb[0] = (unsigned char)(c >> 24);
            rgb[1] = (unsigned char)(c >> 16);
            rgb[2] = (unsigned char)(c >> 8);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Edge marking                                                       */
/* ------------------------------------------------------------------ */
/*
 * The DS's rule is four neighbour comparisons, not an outline. A pixel is
 * marked when any of its four neighbours belongs to a different opaque polygon
 * ID and lies further away, so the mark lands on the near side of an ID
 * boundary and a sloped surface's own depth gradient marks nothing.
 *
 * A PICA has both comparisons in fixed function and neither of them at the
 * neighbour: the depth and stencil units compare against the buffer at the
 * fragment's own pixel. So the geometry is drawn one pixel over instead, which
 * puts the neighbour's depth and ID under the pixel that wants them:
 *
 *   reset   a quad at the far plane with the rear plane's ID, which is what
 *           makes a polygon against the sky come out outlined.
 *   step A  the opaque geometry, slid one pixel, depth-tested and
 *           depth-written with the ID in the stencil.
 *   step B  the opaque geometry where it really is, depth test "nearer than
 *           the buffer" and stencil test "not equal to my ID", writing the
 *           edge colour.
 *
 * Two things it does not reproduce. The DS only considers a pixel on its own
 * polygon's rasterized edge, which a PICA cannot know per fragment. And step B
 * cannot check that its fragment is the topmost one at its own pixel, so a
 * hidden surface marks if it falls between the visible one and the neighbour.
 *
 * It is blended, not painted: the DS marks the pixel and sets its
 * anti-aliasing coverage to 16, so the final pass blends the edge colour 17/32
 * with the pixel underneath. `edgeflat` paints instead.
 */
static const struct { float dx, dy; } kEdgeDirs[4] = {
    { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f }
};

static unsigned long sEdgeFrames, sEdgeDraws, sEdgeShort;
static float sCmdBufPeak;

/*
 * Where edge marking stops rather than overflows. Filling citro3d's command
 * buffer is not an error anybody gets to see: GPUCMD_AddRawCommands answers a
 * full one with svcBreak, which kills the process before this port's fault
 * screen can say anything, a black window and a report on the card written
 * seconds earlier and therefore innocent about it. That happened once, at the
 * default 256 KB. The buffer is four times that now and the bedroom peaks at
 * 42%, but a heavier scene is a scene nobody has measured, so the directions
 * are checked between one and the next and the pass gives up rather than
 * walking off the end. A frame with two of its four neighbours done has a
 * thinner outline; a frame that oversteps has nothing at all.
 */
#define P3D_CMDBUF_LIMIT 0.80f

/* The whole target at the far end of the depth buffer, in clip space, the
 * shader has no matrix, so these are already normalized device coordinates
 * with w = 1. Rewritten each frame because which end is far depends on the
 * frame's depth mode. */
static void edge_quad_build(void)
{
    static const float kX[6] = { -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, -1.0f };
    static const float kY[6] = { -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f };
    /* window = -z / w, and the far end is the opposite of the test's end. */
    float z = pica3d_depth_keeps_greater(sReversed) ? 0.0f : -1.0f;
    int i;

    for (i = 0; i < P3D_QUAD_VERTS; i++) {
        struct p3dvertex *v = &sVerts[P3D_QUAD_BASE + i];

        memset(v, 0, sizeof *v);
        v->x = kX[i];
        v->y = kY[i];
        v->z = z;
        v->w = 1.0f;
        v->a = 1.0f;            /* the alpha test is on; it must pass */
    }
    GSPGPU_FlushDataCache(&sVerts[P3D_QUAD_BASE],
                          (u32)(sizeof *sVerts * P3D_QUAD_VERTS));
}

/* The whole picture, `dx` pixels right and `dy` pixels down. x and y arrive
 * multiplied through by w, so the slide is too; DS y grows downward where a
 * PICA's grows up, which is the sign. */
static void edge_slide(float dx, float dy)
{
    if (sSlideLoc >= 0) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sSlideLoc,
                      2.0f * dx / (float)P3D_TARGET,
                      -2.0f * dy / (float)P3D_TARGET, 0.0f, 0.0f);
    }
}

/*
 * EdgeTable's entry for a polygon group, five bits a channel out of the DS's
 * table and eight out of this one, the same two expansions pc_gpu3d_soft.c
 * does at soft.c:1521 and the packer does afterwards.
 *
 * It comes out backwards from every other colour in this file. A colour buffer
 * and its clear are 0xRRGGBBAA; the TEV's constant register is 0xAABBGGRR, and
 * the two cannot be told apart by reading the call. A dark grey outline drew
 * bright red, which is the low byte arriving in the red channel.
 */
static u32 edge_colour(const PcGxRenderRegs *r, unsigned polyid)
{
    u16 c = r->EdgeTable[(polyid >> 3) & 7];
    u32 red   = (u32)(c << 1) & 0x3Eu;   if (red)   red++;
    u32 green = (u32)(c >> 4) & 0x3Eu;   if (green) green++;
    u32 blue  = (u32)(c >> 9) & 0x3Eu;   if (blue)  blue++;

    red   = (red   << 2) | (red   >> 4);
    green = (green << 2) | (green >> 4);
    blue  = (blue  << 2) | (blue  >> 4);
    return (0xFFu << 24) | (blue << 16) | (green << 8) | red;
}

unsigned pica3d_gpu_build(void)
{
    const PcGxRenderRegs *r;
    PcGxPolygon **polys;
    uint32_t count = 0, i;
    unsigned flags = L3D_READY;
    int textured;
    struct pica3d_state last;
    int haveLast = 0;

    sVertN = 0;
    sBatchN = 0;
    sFrame++;
    sProduced++;

    /* Every couple of seconds, overwriting, so whatever is on the card when the
     * console is closed came from just before it was closed. */
    if (sDump && sGeomFrames > 0 && (sProduced % 120u) == 0u) {
        dump_layer();
    }

    if (!sReady || pc_gpu3d_render_polygons == NULL) {
        return 0;
    }
    polys = pc_gpu3d_render_polygons(&count);
    if (polys == NULL || count == 0) {
        /*
         * No polygons is NOT no layer: the DS still clears the 3D layer to the
         * rear plane, and a frame that showed the previous frame's polygons
         * would be a visible fault. It is drawn as a clear, which is what the
         * software rasterizer does with an empty list.
         */
        return L3D_READY;
    }

    r = (pc_gpu3d_render_regs != NULL) ? pc_gpu3d_render_regs() : NULL;
    textured = (r != NULL) && ((r->DispCnt & 1u) != 0);

    /*
     * One depth test for the frame, so a frame that disagrees with itself is
     * refused rather than drawn with one of the two answers. The survey counts
     * these; if the count is ever non-zero this becomes two passes.
     */
    sReversed = pica3d_depth_reversed(polys[0]);
    for (i = 1; i < count; i++) {
        if (pica3d_depth_reversed(polys[i]) != sReversed) {
            sRefusedDepth++;
            return 0;
        }
    }

    if (sEdge) {
        edge_quad_build();
    }

    memset(&last, 0, sizeof last);

    for (i = 0; i < count; i++) {
        const PcGxPolygon *p = polys[i];
        struct pica3d_state st;
        unsigned texW = 1, texH = 1;
        int entry = -1;
        unsigned n, k;

        if (p->NumVertices < 3) {
            continue;               /* a line, and this game draws none */
        }

        pica3d_state_of(p, textured, &st);

        /*
         * Only modulation. The survey found no decal, toon-highlight or shadow
         * polygon in 6,806,542, so the other three are refused rather than
         * approximated; a wrong shading mode is a wrong picture that nothing
         * would flag.
         */
        if (st.shading != 0) {
            sRefusedMode++;
            return 0;
        }

        if (st.texkey != 0) {
            int conv = 0;

            entry = tex3d_cache_get(p->TexParam, p->TexPalette, sFrame, &conv);
            if (entry < 0) {
                sRefusedTex++;
                return 0;
            }
            texW = tex3d_width(p->TexParam);
            texH = tex3d_height(p->TexParam);
            if (conv) {
                /* The pool is linear memory the GPU is about to read. */
                GSPGPU_FlushDataCache((void *)tex3d_cache_texels(entry),
                                      (u32)(tex3d_cache_size(entry)
                                            * sizeof(uint32_t)));
            }
        }

        n = p->NumVertices;
        if (sVertN + (int)(n - 2) * 3 > P3D_QUAD_BASE) {
            sRefusedRoom++;
            return 0;
        }

        /*
         * A new batch when the state or the image changes, and only then. The
         * list is walked in order because the order is the picture for the
         * translucent half, so this merges runs rather than gathering sets;
         * which is exactly what `gx-batches` counts.
         */
        if (!haveLast || !pica3d_state_eq(&st, &last)
            || sBatchN == 0) {
            if (sBatchN >= P3D_MAX_BATCH) {
                sRefusedRoom++;
                return 0;
            }
            sBatch[sBatchN].first = sVertN;
            sBatch[sBatchN].count = 0;
            sBatch[sBatchN].entry = entry;
            sBatch[sBatchN].texparam = p->TexParam;
            sBatch[sBatchN].blend = st.blend;
            sBatch[sBatchN].depthWrite = st.depthWrite;
            sBatch[sBatchN].depthEqual = st.depthEqual;
            sBatch[sBatchN].polyid = st.polyid;
            sBatchN++;
            last = st;
            haveLast = 1;
        }

        /* A fan: (0, k, k+1). A quad is the n = 4 case and needs no branch. */
        for (k = 1; k + 1 < n; k++) {
            if (emit(p, 0, texW, texH) != 0
                || emit(p, k, texW, texH) != 0
                || emit(p, k + 1, texW, texH) != 0) {
                /* A zero-W vertex: the DS nails that corner to the top left
                 * and this renderer will not draw a triangle through it. */
                sVertN = sBatch[sBatchN - 1].first
                       + sBatch[sBatchN - 1].count;
                break;
            }
            sBatch[sBatchN - 1].count += 3;
            sTris++;
            expand_tri(&sVerts[sVertN - 3]);
        }

        if (p->Translucent || ((p->Attr >> 16) & 0x1F) < 31u) {
            flags |= L3D_TRANSLUCENT;
        }
    }

    if (sVertN == 0) {
        return L3D_READY;
    }

    GSPGPU_FlushDataCache(sVerts, (u32)(sizeof *sVerts * (size_t)sVertN));
    if ((unsigned long)(sVertN / 3) > sTrisMax) {
        sTrisMax = (unsigned long)(sVertN / 3);
    }
    if ((unsigned long)sBatchN > sDrawsMax) {
        sDrawsMax = (unsigned long)sBatchN;
    }
    return flags;
}

/* ------------------------------------------------------------------ */
/* The GPU half                                                        */
/* ------------------------------------------------------------------ */

/* The DS's rear plane, as the PICA clears: 0xRRGGBBAA out of the latched
 * clear colour, which is five bits a channel with the same six-to-eight
 * expansion every other surface in this port goes through. */
static u32 clear_colour(void)
{
    const PcGxRenderRegs *r =
        (pc_gpu3d_render_regs != NULL) ? pc_gpu3d_render_regs() : NULL;
    u32 c, red, green, blue, alpha;

    if (r == NULL) {
        return 0u;
    }
    c = r->ClearAttr1;
    red   = (c & 0x1Fu) * 2u;      if (red)   red++;
    green = ((c >> 5) & 0x1Fu) * 2u;  if (green) green++;
    blue  = ((c >> 10) & 0x1Fu) * 2u; if (blue)  blue++;
    alpha = (c >> 16) & 0x1Fu;

    red   = (red   << 2) | (red   >> 4);
    green = (green << 2) | (green >> 4);
    blue  = (blue  << 2) | (blue  >> 4);
    /*
     * The layer's alpha is binary and it is not the DS's ALPHA. The 3D layer's packer
     * writes 0xFF for every pixel the rasterizer drew and 0 for every one it
     * did not, and the compositor's alpha test is what turns that into "this
     * pixel is the 3D layer" versus "let the 2D through". Writing the rear
     * plane's real alpha here instead means a rear plane of, say, 16/31
     * expands to 132, passes that test, and paints itself over the background
     * layers, the 3D layer covering the whole screen rather than showing
     * through it.
     */
    return (red << 24) | (green << 16) | (blue << 8) | (alpha != 0u ? 0xFFu : 0u);
}

/* A cached image as something C3D_TexBind takes. The struct is filled by hand
 * rather than by C3D_TexInit because the texels are already in the pool and a
 * second copy of a megabyte a frame is the thing this whole path avoids. */
static C3D_Tex *bind_tex(int entry, uint32_t texparam)
{
    C3D_Tex *t;
    const uint32_t *texels = tex3d_cache_texels(entry);
    static const GPU_TEXTURE_WRAP_PARAM kWrap[3] = {
        GPU_CLAMP_TO_EDGE, GPU_REPEAT, GPU_MIRRORED_REPEAT
    };

    if (entry < 0 || entry >= (int)(sizeof sBoundTex / sizeof *sBoundTex)
        || texels == NULL) {
        return NULL;
    }
    t = &sBoundTex[entry];
    memset(t, 0, sizeof *t);
    t->data = (void *)texels;
    t->fmt = GPU_RGBA8;
    t->size = tex3d_cache_size(entry) * 4u;
    t->width = (u16)tex3d_width(texparam);
    t->height = (u16)tex3d_height(texparam);
    C3D_TexSetFilter(t, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(t, kWrap[tex3d_wrap_s(texparam)],
                      kWrap[tex3d_wrap_t(texparam)]);
    return t;
}

/* One batch, with whatever texture it samples, for a pass that only wants the
 * alpha, the coverage of a polygon is its texture's alpha test and not its
 * outline. */
static void edge_draw_batch(const struct batch *b, int wantColour, u32 colour)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);

    if (b->entry >= 0 && !sNoTex) {
        C3D_Tex *t = bind_tex(b->entry, b->texparam);

        if (t != NULL) {
            C3D_TexBind(0, t);
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
        } else {
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        }
    } else {
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    }
    if (wantColour) {
        C3D_TexEnvColor(env, colour);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, 0, 0);
    } else {
        C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, 0, 0);
    }
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_DrawArrays(GPU_TRIANGLES, b->first, b->count);
    sEdgeDraws++;
}

static void edge_mark(void)
{
    const PcGxRenderRegs *r =
        (pc_gpu3d_render_regs != NULL) ? pc_gpu3d_render_regs() : NULL;
    GPU_TESTFUNC nearer;
    unsigned clearid;
    int dir, i;

    if (r == NULL || !(r->DispCnt & (1u << 5)) || sVertN == 0 || sBatchN == 0) {
        return;
    }
    sEdgeFrames++;
    clearid = (r->ClearAttr1 >> 24) & 0x3Fu;
    nearer = pica3d_depth_keeps_greater(sReversed) ? GPU_GREATER : GPU_LESS;

    for (dir = 0; dir < 4; dir++) {
        if (C3D_GetCmdBufUsage() > P3D_CMDBUF_LIMIT) {
            sEdgeShort++;
            break;
        }
        /* Reset: the rear plane, at the far end, with the clear ID. */
        edge_slide(0.0f, 0.0f);
        C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_DEPTH);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO,
                       GPU_ONE, GPU_ZERO);
        C3D_StencilTest(true, GPU_ALWAYS, (int)clearid, 0x3F, 0x3F);
        C3D_StencilOp(GPU_STENCIL_REPLACE, GPU_STENCIL_REPLACE,
                      GPU_STENCIL_REPLACE);
        {
            C3D_TexEnv *env = C3D_GetTexEnv(0);

            C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        }
        C3D_DrawArrays(GPU_TRIANGLES, P3D_QUAD_BASE, P3D_QUAD_VERTS);
        sEdgeDraws++;

        /* Step A: the neighbour's topmost surface, into depth and stencil. */
        edge_slide(kEdgeDirs[dir].dx, kEdgeDirs[dir].dy);
        C3D_DepthTest(true, nearer, GPU_WRITE_DEPTH);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
        for (i = 0; i < sBatchN; i++) {
            const struct batch *b = &sBatch[i];

            if (b->count == 0 || b->blend) {
                continue;       /* the ID edge marking reads is the opaque one */
            }
            C3D_StencilTest(true, GPU_ALWAYS, (int)b->polyid, 0x3F, 0x3F);
            edge_draw_batch(b, 0, 0u);
        }

        /* Step B: where the picture really is, marking the near side. */
        edge_slide(0.0f, 0.0f);
        C3D_DepthTest(true, nearer, GPU_WRITE_COLOR);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
        if (sEdgeFlat) {
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO,
                           GPU_ZERO, GPU_ONE);
        } else {
            /* 17/32 of the edge colour, which is the coverage the DS leaves
             * behind for its own final pass to blend with. */
            C3D_BlendingColor(0x88000000u);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_ALPHA,
                           GPU_ONE_MINUS_CONSTANT_ALPHA, GPU_ZERO, GPU_ONE);
        }
        for (i = 0; i < sBatchN; i++) {
            const struct batch *b = &sBatch[i];

            if (b->count == 0 || b->blend) {
                continue;
            }
            C3D_StencilTest(true, GPU_NOTEQUAL, (int)b->polyid, 0x3F, 0x00);
            edge_draw_batch(b, 1, edge_colour(r, b->polyid));
        }
    }
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
}

void pica3d_gpu_draw(void)
{
    int i;

    if (!sReady) {
        return;
    }

    /*
     * The clear happens whether or not there are polygons: an empty list is a
     * rear plane, not the previous frame. Depth clears to the FAR end, which
     * is a different number in the two modes; see the depth test below.
     *
     * D24S8 packs stencil in the top byte of that fill (depth in bytes 0-2,
     * stencil in byte 3). A W-buffer far of 0xFFFFFFFF therefore left stencil
     * at 0xFF, and the empty-dest first pass tests EQUAL 0, so every
     * translucent fragment on a W-buffer frame; the title Giratina is one,
     * took src-over against the clear and came out a dot matrix. Masking the
     * fill to 24 bits keeps the far depth and starts stencil at 0. Z-buffer
     * far is already 0 and does not move.
     */
    C3D_RenderTargetClear(sTarget, C3D_CLEAR_ALL, clear_colour(),
                          pica3d_depth_clear(sReversed) & 0x00FFFFFFu);
    C3D_FrameDrawOn(sTarget);
    /*
     * Counted HERE and not at the end, because a frame with an empty polygon
     * list is still a frame this renderer drew; it is the rear plane, and
     * the first version of this counter could not tell that apart from never
     * having been called at all.
     */
    sFrames++;

    if (sVertN == 0 || sBatchN == 0) {
        return;
    }
    sGeomFrames++;

    C3D_BindProgram(&sProgram);
    {
        C3D_AttrInfo *attr = C3D_GetAttrInfo();

        AttrInfo_Init(attr);
        AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);   /* v0 = clip position */
        AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 4);   /* v1 = colour        */
        AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 2);   /* v2 = texcoord      */
    }
    {
        C3D_BufInfo *buf = C3D_GetBufInfo();

        BufInfo_Init(buf);
        BufInfo_Add(buf, sVerts, sizeof *sVerts, 3, 0x210);
    }

    /*
     * No viewport of its own, and the first version had one. Restricting the
     * rasterizer to the 256x192 the DS drew looks like free savings, the
     * rows past it are never sampled, but citro3d's viewport arguments are
     * in the framebuffer's own axes, which on this console are the SCREEN's
     * rotated ones, and a square render target is the one case where that
     * cannot be told apart by reading the call. C3D_FrameDrawOn() has already
     * set the full target, correctly, from its own dimensions. The saving was
     * 64 rows of clear.
     */

    /* Unslid: the slide is edge marking's and it is a uniform, so it has to be
     * put back for the pass that draws the picture. */
    edge_slide(0.0f, 0.0f);

    /* One stage: texture times vertex colour, both channels. */
    {
        C3D_TexEnv *env = C3D_GetTexEnv(0);

        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    }
    /* An index-zero texel is transparent and must not paint black. */
    C3D_AlphaTest(true, GPU_GREATER, 0);
    C3D_CullFace(GPU_CULL_NONE);

    for (i = 0; i < sBatchN; i++) {
        const struct batch *b = &sBatch[i];
        GPU_TESTFUNC func;

        if (b->count == 0) {
            continue;
        }

        /*
         * Which end of the depth buffer is near is 3ds_pica3d.h's rule and not
         * A literal here, because it is not the one a desktop habit says and
         * this renderer had it backwards in both modes for a whole task. The
         * reasoning is beside the transform it follows from; the selftest on a
         * build machine is what checks it.
         */
        func = b->depthEqual ? GPU_EQUAL
                             : (pica3d_depth_keeps_greater(sReversed)
                                ? GPU_GREATER : GPU_LESS);
        if (sNoDepth) {
            func = GPU_ALWAYS;
        }
        C3D_DepthTest(!sNoDepth, func,
                      b->depthWrite ? (GPU_WRITE_COLOR | GPU_WRITE_DEPTH)
                                    : GPU_WRITE_COLOR);

        if (b->entry >= 0 && !sNoTex) {
            C3D_Tex *t = bind_tex(b->entry, b->texparam);

            if (t != NULL) {
                C3D_TexBind(0, t);
                C3D_TexEnvSrc(C3D_GetTexEnv(0), C3D_Both,
                              GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
                C3D_TexEnvFunc(C3D_GetTexEnv(0), C3D_Both, GPU_MODULATE);
            }
        } else {
            /* Untextured: the vertex colour alone, which is what
             * render_pixel() answers when DISP3DCNT bit 0 is clear. */
            C3D_TexEnvSrc(C3D_GetTexEnv(0), C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
            C3D_TexEnvFunc(C3D_GetTexEnv(0), C3D_Both, GPU_REPLACE);
        }

        /*
         * Empty destination: write the source as-is, then mark the pixel
         * occupied. Occupied destination: the usual src-over. Edge marking
         * owns the stencil, so this waits when that pass is on.
         */
        if (b->blend && !sNoBlend && !sEdge) {
            C3D_StencilTest(true, GPU_EQUAL, 0, 0xFF, 0xFF);
            C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP,
                          GPU_STENCIL_INCR);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO,
                           GPU_ONE, GPU_ZERO);
            C3D_DrawArrays(GPU_TRIANGLES, b->first, b->count);
            C3D_StencilTest(true, GPU_NOTEQUAL, 0, 0xFF, 0x00);
            C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP,
                          GPU_STENCIL_KEEP);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                           GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                           GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
            C3D_DrawArrays(GPU_TRIANGLES, b->first, b->count);
            sDraws += 2;
        } else {
            if (!sEdge) {
                C3D_StencilTest(true, GPU_ALWAYS, 0, 0x00, 0xFF);
                C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP,
                              GPU_STENCIL_INCR);
            }
            if (b->blend && !sNoBlend) {
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
            } else {
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO,
                               GPU_ONE, GPU_ZERO);
            }
            C3D_DrawArrays(GPU_TRIANGLES, b->first, b->count);
            sDraws++;
        }
    }
    if (!sEdge) {
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    }

    /* Last, because it reads the picture's own depth buffer and then destroys
     * it, and because the DS's own edge marking is a final pass too. */
    if (sEdge) {
        edge_mark();
    }
    {
        /* How close this frame came to filling the command buffer. Overflowing
         * it is an svcBreak and a black screen, so it is watched rather than
         * assumed. */
        float used = C3D_GetCmdBufUsage();

        if (used > sCmdBufPeak) {
            sCmdBufPeak = used;
        }
    }
}

/* Whether the caller should leave out the barrier; see the note beside
 * layer3d_render()'s call site, and `nosplit` above. */
int pica3d_gpu_no_split(void)
{
    return sNoSplit;
}

void pica3d_gpu_report(FILE *f)
{
    /* Three different numbers, because the difference between them is where a
     * missing picture would be: asked for, drawn at all, drawn with geometry. */
    if (sNoTex || sNoDepth || sNoBlend || sFlat || sNoSplit || sDump || sDiff) {
        fprintf(f, "p3d-bisect%s%s%s%s%s%s%s\n",
                sNoTex ? " notex" : "", sNoDepth ? " nodepth" : "",
                sNoBlend ? " noblend" : "", sFlat ? " flat" : "",
                sNoSplit ? " nosplit" : "", sDump ? " dump" : "",
                sDiff ? " diff" : "");
    }
    fprintf(f, "p3d-half %.3f\n", pica3d_sample_offset());
    if (sEdge) {
        fprintf(f, "p3d-edge frames %lu draws %lu cut-short %lu\n",
                sEdgeFrames, sEdgeDraws, sEdgeShort);
    }
    fprintf(f, "p3d-cmdbuf-peak %.3f\n", (double)sCmdBufPeak);
    fprintf(f, "p3d-produced %lu frames %lu with-geometry %lu\n",
            sProduced, sFrames, sGeomFrames);
    fprintf(f, "p3d-triangles %lu max %lu\n", sTris, sTrisMax);
    fprintf(f, "p3d-draws %lu max %lu\n", sDraws, sDrawsMax);
    /* Every reason this renderer sent a frame back to software, separately:
     * One of them is a design decision and three are ceilings. */
    fprintf(f, "p3d-refused depth %lu texture %lu room %lu mode %lu\n",
            sRefusedDepth, sRefusedTex, sRefusedRoom, sRefusedMode);
    /*
     * And the texture cache, which is the one part of this renderer whose
     * behaviour changes as a run goes on: entries accumulate, the pool fills,
     * and a scene heavier than the survey measured would start rebuilding it
     * every frame. A picture that is right for a second and wrong afterwards
     * is exactly what that looks like, so the numbers belong in the report
     * rather than in a guess about it.
     */
    tex3d_report(f);
}
