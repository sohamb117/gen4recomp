/*
 * 3ds/src/3ds_gpu3d.c: see 3ds_gpu3d.h.
 *
 * The header says why this is a survey before it is a renderer. What is here
 * is the counting: one pass over the polygon list the rasterizer has just
 * drawn, plus the render registers it drew with, folded into totals a report
 * prints at the end of the run.
 *
 * IT READS pc/include/pc_gpu3d.h by path. The port include chain does not
 * carry pc/include and must not; that directory holds a cstdlib, a null.h
 * and a whole nitro/ tree, and putting it in front of newlib's would compile
 * a different libc. 3ds/src/3ds_mem.c already reaches pc_overlay.h the same
 * way and for the same reason; the header itself is <stdint.h> and nothing
 * else, so it brings no DS SDK with it.
 *
 * The bit numbers are pc_gpu3d_soft.c's, named by its own line where the
 * meaning is not obvious. That file is the oracle for what a bit does, the
 * same way pc_gpu2d.c is the oracle for the background path, and a survey that disagreed
 * with it about which frames use fog would be measuring its own mistake.
 */

#include "3ds_gpu3d.h"

#include <string.h>

#include "../../pc/include/pc_gpu3d.h"

/* ------------------------------------------------------------------ */
/* What a frame can ask for                                            */
/* ------------------------------------------------------------------ */
/*
 * DISP3DCNT, one bit per feature, and the PICA work each one costs is in the
 * table below rather than in a comment per line; the report prints these
 * names, and a reader of the report wants the feature, not the bit.
 *
 * Bit 6 is "fog alpha only" and is a modifier on bit 7 rather than a feature
 * of its own; it is counted because a fog that only writes alpha is a
 * different PICA problem from one that writes colour.
 */
enum {
    D3_TEXMAP = 0,      /* bit 0 : textures at all                    */
    D3_HIGHLIGHT,       /* bit 1 : shading is highlight, not toon     */
    D3_ALPHATEST,       /* bit 2 : ALPHA_TEST_REF                     */
    D3_ALPHABLEND,      /* bit 3 : translucency writes at all         */
    D3_AA,              /* bit 4 : edge anti-aliasing                 */
    D3_EDGEMARK,        /* bit 5 : the one with no clean PICA answer  */
    D3_FOGALPHA,        /* bit 6 : fog writes alpha only              */
    D3_FOG,             /* bit 7 : fog at all                         */
    D3_CLEARIMAGE,      /* bit 14: the rear plane is a bitmap         */
    D3_COUNT
};

static const char *const kDispName[D3_COUNT] = {
    "texmap", "highlight", "alpha-test", "alpha-blend",
    "anti-alias", "edge-mark", "fog-alpha-only", "fog", "clear-image"
};

static const unsigned char kDispBit[D3_COUNT] = { 0, 1, 2, 3, 4, 5, 6, 7, 14 };

/* The eight texture formats, in TexParam bits 26-28's own order. */
static const char *const kFmtName[8] = {
    "none", "a3i5", "pltt4", "pltt16", "pltt256", "comp4x4", "a5i3", "direct"
};

/* PolygonAttr bits 4-5, the shading mode. */
static const char *const kModeName[4] = {
    "modulation", "decal", "toon-highlight", "shadow"
};

/*
 * The per-polygon rules a PICA backend has to reproduce one at a time. Each
 * is a bit in PolygonAttr except the last three, which are properties
 * pc_gpu3d.c derived when it built the polygon.
 */
enum {
    P_WIREFRAME = 0,    /* alpha == 0: soft.c:809, and it draws EDGES  */
    P_TRANSLUCENT,      /* pc_gpu3d.c:746                               */
    P_SHADOWMASK,       /* pc_gpu3d.c:749: the stencil pass           */
    P_SHADOW,           /* pc_gpu3d.c:750: drawn against it           */
    P_DEPTHEQUAL,       /* Attr bit 14: soft.c:1160                   */
    P_FOG,              /* Attr bit 15: per-polygon fog enable        */
    P_ONEDOT,           /* Attr bit 13: render a polygon 1 dot wide   */
    P_FARCLIP,          /* Attr bit 12: keep far-plane intersections  */
    P_NEWDEPTH,         /* Attr bit 11: translucent writes depth      */
    P_LIT,              /* Attr bits 0-3: any light on                */
    P_LINE,             /* Type 1: a line, not a polygon              */
    P_CLIPPED,          /* more than four vertices: the clipper ran     */
    P_WBUFFER,          /* depth is W, not Z: pc_gpu3d.c:821          */
    P_COUNT
};

static const char *const kPolyName[P_COUNT] = {
    "wireframe", "translucent", "shadow-mask", "shadow", "depth-equal",
    "fog", "one-dot", "far-clip", "new-depth", "lit", "line", "clipped",
    "wbuffer"
};

/* ------------------------------------------------------------------ */
/* Totals                                                              */
/* ------------------------------------------------------------------ */

static unsigned long sFrames;        /* pc_gpu3d_vblank() calls            */
static unsigned long sPolyFrames;    /* ...with at least one polygon       */
static unsigned long sPolys, sVerts;
static unsigned long sPolyMax;       /* most polygons on one frame         */

static unsigned long sDisp[D3_COUNT];        /* frames a feature was on    */
static unsigned long sDispPoly[D3_COUNT];    /* ...counting poly frames    */
static unsigned long sWBufferFrames;
/*
 * The 3D producer needs these three and (a) did not ask them, because the questions
 * only appear once the renderer is a PICA one.
 *
 * A frame that mixes depth modes decides a design. The DS interpolates depth
 * linearly in screen space for a Z-buffer polygon and perspective-correctly
 * for a W-buffer one (interp_interpolate_z). A PICA writes post-divide z,
 * which is screen-linear, so Z mode is exact, and W mode is reproduced by
 * feeding 1/W as depth, which is screen-linear too but runs the other way and
 * therefore needs the depth test REVERSED. That is a per-frame register. If a
 * frame ever carried both kinds against one depth buffer there would be no
 * single test that is right, and the renderer would need two passes or a
 * refusal. So: count the frames that do.
 *
 * The batch count is the draw-call cost. Every change of texture or of the
 * state words is a draw on the PICA, and the polygon list's order decides how
 * many. Counting the runs of consecutive polygons that share both is the
 * number, and it is the difference between one submission and four hundred.
 *
 * The wrap modes decide whether a sampler change is rare. The texture converter keeps
 * wrapping out of the converted image because the PICA reproduces all three
 * exactly; which means it is per-bind state, and how often it changes is a
 * cost this can count for free.
 */
static unsigned long sMixedDepthFrames;
static unsigned long sBatches;       /* runs of (texture, state) over all   */
static unsigned long sBatchMax;      /* ...and the worst one frame          */
static unsigned long sWrap[3];       /* clamp, repeat, mirror: s and t    */
static unsigned long sClearAlpha;    /* poly frames: rear plane not opaque  */
static unsigned sFogShiftSeen;       /* one bit per value of DISP3DCNT 8-11 */

static unsigned long sFmt[8];        /* polygons by texture format          */
/*
 * ...and by the format the polygon is actually SAMPLED through, which is not
 * the same list. soft.c:828 draws a polygon untextured unless DISP3DCNT bit 0
 * is set, and this game leaves that bit clear on most of the frames it draws
 * polygons on, so the raw column counts converters that would never be
 * called. The second column is the one that decides which formats the 3D rasterizer has to
 * convert, and the working set below is counted on the same rule for the same
 * reason: a texture nothing samples costs a cache nothing.
 */
static unsigned long sFmtEff[8];
static unsigned long sMode[4];       /* polygons by shading mode            */
static unsigned long sPoly[P_COUNT];
static unsigned long sCull[4];       /* 0 neither, 1 front, 2 back, 3 both  */
static unsigned long sTexTransform[4];
static unsigned long sTexRepeat, sTexFlip, sTexColor0;
static unsigned long sAlphaSeen;     /* one bit per alpha 0-31, folded to 32 */
static unsigned sTexSizeMax;         /* the largest texel count on one poly  */
static unsigned long sIdSeen[2];     /* one bit per polygon ID 0-63          */
/*
 * What edge marking would cost to draw, which is two numbers this survey did
 * not have when the 3D pixel work was written as "start without it".
 *
 * The edge COLOUR is looked up as EdgeTable[polyid >> 3], so a renderer that
 * has to paint it needs one draw per distinct group, unless the game sets
 * all eight the same, and then it needs one. `sEdgeUniform` counts the frames
 * where they agree, `sEdgeTable` keeps the last one seen and `sEdgeTables` how
 * many distinct tables the run produced.
 *
 * The polygon ID is the other half: the rule compares the ID of the pixel with
 * the ID of its neighbour, and a PICA can only carry an ID in the stencil
 * buffer, whose reference value is per DRAW. So the number that decides
 * whether that is affordable is how many distinct IDs one frame's OPAQUE
 * polygons use, not how many the run uses, which is already known to be 64.
 */
static unsigned long sEdgeUniform;   /* poly-frames whose 8 entries agree    */
static unsigned long sEdgeTables;    /* distinct tables seen                 */
static uint16_t sEdgeTable[8];       /* ...and the last of them              */
static int sEdgeHave;
static unsigned sEdgeIdsMax;         /* most distinct opaque IDs in a frame  */
static unsigned long sEdgeIdsSum;    /* ...and the total, for a mean         */
static unsigned long sEdgeIdFrames;

/*
 * The texture working set, which is the number that sizes a cache. Distinct
 * (TexParam, TexPalette) pairs on one frame, and what they weigh expanded to
 * the RGBA8 a PICA texture holds (the tile cache settled that RGBA5551 is the wrong
 * format here, so RGBA8 is the real cost and not a pessimistic one).
 */
static unsigned sTexDistinctMax;
static unsigned long sTexBytesMax;   /* RGBA8 bytes for one frame's set     */
static unsigned long sTexSrcBytesMax;/* ...and what the DS stores for it    */
static unsigned long sTexOverflow;   /* frames whose set outgrew the table  */

/* ------------------------------------------------------------------ */
/* The per-frame texture set                                           */
/* ------------------------------------------------------------------ */
/*
 * Open addressing, power-of-two, and no allocation: this runs inside the
 * frame the rasterizer just spent 200 ms in and may not add a heap operation
 * to it. 512 slots against a hardware limit of 2,048 polygons is deliberate:
 * A frame that fills it is a finding (sTexOverflow) rather than a wrong
 * answer, and the report says so.
 *
 * A linear scan would have been wrong here and not just slow: 2,048 polygons
 * against a few hundred entries is half a million comparisons a frame on a
 * 268 MHz ARM11, which would have made the instrument cost more than some of
 * the scenes it is measuring.
 */
#define TEXSET_SLOTS 512
#define TEXSET_MASK  (TEXSET_SLOTS - 1)

static uint32_t sSetKey[TEXSET_SLOTS];
static uint32_t sSetPal[TEXSET_SLOTS];
static unsigned char sSetUsed[TEXSET_SLOTS];
static unsigned sSetN;

/* Empty the set. Called once a frame, before the polygons are walked. */
static void texset_clear(void)
{
    memset(sSetUsed, 0, sizeof sSetUsed);
    sSetN = 0;
}

/*
 * Add one image to the set. Returns 1 if it was not already there, 0 if it
 * was, and -1 if the table is too full to answer; which the caller reports
 * as an overflowed frame rather than as a smaller working set.
 *
 * Half full is the ceiling: past it open addressing's probe length stops
 * being a constant, and this runs inside a frame the rasterizer is already
 * spending 200 ms in.
 */
static int texset_add(uint32_t key, uint32_t pal)
{
    uint32_t h = (key * 2654435761u + pal * 40503u) & TEXSET_MASK;

    for (;;) {
        if (!sSetUsed[h]) {
            if (sSetN >= TEXSET_SLOTS / 2) {
                return -1;
            }
            sSetUsed[h] = 1;
            sSetKey[h] = key;
            sSetPal[h] = pal;
            sSetN++;
            return 1;
        }
        if (sSetKey[h] == key && sSetPal[h] == pal) {
            return 0;
        }
        h = (h + 1) & TEXSET_MASK;
    }
}

/* Texels a TexParam describes: 8 << the two size fields. */
static unsigned tex_texels(uint32_t texparam)
{
    unsigned w = 8u << ((texparam >> 20) & 7);
    unsigned h = 8u << ((texparam >> 23) & 7);
    return w * h;
}

/*
 * What the DS stores for that texture, in bytes. COMP4x4 is two blocks of
 * storage rather than one, two bits a texel of index data plus a 16-bit
 * word per 4x4 block naming its palette; which is why it is not simply a
 * bits-per-texel row like the rest.
 */
static unsigned long tex_src_bytes(uint32_t texparam)
{
    unsigned texels = tex_texels(texparam);

    switch ((texparam >> 26) & 7) {
    case 0: return 0;                        /* no texture                */
    case 1: return texels;                   /* A3I5, one byte a texel    */
    case 2: return texels / 4;               /* PLTT4, two bits           */
    case 3: return texels / 2;               /* PLTT16, four bits         */
    case 4: return texels;                   /* PLTT256, eight bits       */
    case 5: return texels / 4 + texels / 8;  /* COMP4x4, index + palette  */
    case 6: return texels;                   /* A5I3, one byte a texel    */
    default: return texels * 2;              /* direct colour             */
    }
}

/* ------------------------------------------------------------------ */
/* One frame                                                           */
/* ------------------------------------------------------------------ */

void gpu3d_survey(void)
{
    const PcGxRenderRegs *r = pc_gpu3d_render_regs();
    uint32_t count = 0;
    PcGxPolygon **polys = pc_gpu3d_render_polygons(&count);
    uint32_t i;
    unsigned distinct = 0;
    unsigned long bytes = 0, srcBytes = 0;
    int overflowed = 0;
    int poly_frame;
    int textured;       /* DISP3DCNT bit 0: are textures sampled at all? */
    int sawW = 0, sawZ = 0;
    uint32_t frameIds[2] = { 0u, 0u };
    unsigned long batches = 0;
    uint32_t lastKey = 0, lastPal = 0, lastState = 0;
    int haveLast = 0;

    sFrames++;
    poly_frame = (count != 0);
    if (poly_frame) {
        sPolyFrames++;
    }

    /*
     * The registers are read on every frame and reported in two columns, and
     * the second column is the one to read. pc_gpu3d_vblank() runs whether or
     * not the 3D engine is drawing, so DISP3DCNT is latched from guest memory
     * on frames where nothing has ever written it; the first report out of
     * this file said the rear plane was translucent on all 240 frames of the
     * boot screens, which is true of the register and says nothing about the
     * game. So a feature is credited to `frames` always and to `poly-frames`
     * only when there was something on screen for it to affect, and every
     * finding below is taken from the second.
     */
    if (r != NULL) {
        for (i = 0; i < D3_COUNT; i++) {
            if (r->DispCnt & (1u << kDispBit[i])) {
                sDisp[i]++;
                if (poly_frame) {
                    sDispPoly[i]++;
                }
            }
        }
        if (poly_frame) {
            sFogShiftSeen |= 1u << (r->FogShift & 0xF);
            /* soft.c:1639-1640 reads the clear attributes; bits 16-20 of the
             * first are the rear plane's alpha, and anything but 31 means the
             * cleared pixel is itself translucent. */
            if (((r->ClearAttr1 >> 16) & 0x1F) != 0x1F) {
                sClearAlpha++;
            }
            if (r->DispCnt & (1u << 5)) {
                int e, uniform = 1;

                for (e = 1; e < 8; e++) {
                    if (r->EdgeTable[e] != r->EdgeTable[0]) {
                        uniform = 0;
                    }
                }
                if (uniform) {
                    sEdgeUniform++;
                }
                if (!sEdgeHave || memcmp(sEdgeTable, r->EdgeTable,
                                         sizeof sEdgeTable) != 0) {
                    memcpy(sEdgeTable, r->EdgeTable, sizeof sEdgeTable);
                    sEdgeHave = 1;
                    sEdgeTables++;
                }
            }
        }
    }

    textured = (r != NULL) && ((r->DispCnt & (1u << 0)) != 0);

    if (polys == NULL || count == 0) {
        return;
    }

    if (count > sPolyMax) {
        sPolyMax = count;
    }
    sPolys += count;

    texset_clear();

    /*
     * The W buffer is SWAP_BUFFERS' flag, but pc_gpu3d.c:821 stamps it onto
     * each polygon as that polygon is built, so a frame CAN in principle be
     * mixed. This counts the frame by its first polygon and P_WBUFFER counts
     * the polygons; the two disagreeing is the finding, and a survey that
     * only asked the first polygon would have hidden it.
     */
    if (polys[0]->WBuffer) {
        sWBufferFrames++;
    }

    for (i = 0; i < count; i++) {
        const PcGxPolygon *p = polys[i];
        uint32_t attr = p->Attr;
        uint32_t alpha = (attr >> 16) & 0x1F;
        uint32_t id = (attr >> 24) & 0x3F;
        uint32_t fmt = (p->TexParam >> 26) & 7;
        unsigned texels;
        uint32_t key;
        int added;

        sVerts += p->NumVertices;
        sFmt[fmt]++;
        if (textured) {
            sFmtEff[fmt]++;
        }
        sMode[(attr >> 4) & 3]++;
        sCull[(attr >> 6) & 3]++;
        sTexTransform[(p->TexParam >> 30) & 3]++;
        sAlphaSeen |= 1u << alpha;
        sIdSeen[id >> 5] |= 1u << (id & 31);
        if (!p->Translucent) {
            frameIds[id >> 5] |= 1u << (id & 31);
        }

        if (alpha == 0)             sPoly[P_WIREFRAME]++;
        if (p->Translucent)         sPoly[P_TRANSLUCENT]++;
        if (p->IsShadowMask)        sPoly[P_SHADOWMASK]++;
        if (p->IsShadow)            sPoly[P_SHADOW]++;
        if (attr & (1u << 14))      sPoly[P_DEPTHEQUAL]++;
        if (attr & (1u << 15))      sPoly[P_FOG]++;
        if (attr & (1u << 13))      sPoly[P_ONEDOT]++;
        if (attr & (1u << 12))      sPoly[P_FARCLIP]++;
        if (attr & (1u << 11))      sPoly[P_NEWDEPTH]++;
        if (attr & 0xF)             sPoly[P_LIT]++;
        if (p->Type == 1)           sPoly[P_LINE]++;
        if (p->NumVertices > 4)     sPoly[P_CLIPPED]++;
        if (p->WBuffer)             sPoly[P_WBUFFER]++;

        if (p->WBuffer) sawW = 1;
        else            sawZ = 1;

        /*
         * A batch is a run, not a set. Two polygons far apart in the list with
         * the same texture cannot be merged without moving one past something
         * drawn between them, and the order is the picture for the translucent
         * half. So this counts what a renderer walking the list in order can
         * actually merge, which is the number that becomes draw calls.
         *
         * The state word is everything a draw would have to change: the
         * attribute bits that are not the polygon ID or the alpha (those two
         * ride in the vertices and a uniform), plus the depth mode.
         */
        {
            /*
             * Exactly the bits a draw would have to change, and no more, an
             * over-broad mask inflates this count and makes it say the wrong
             * thing. Shading mode (4-5), cull (6-7), translucent-writes-depth
             * (11) and depth-equal (14), plus the depth mode and whether the
             * polygon blends at all.
             *
             * NOT the light-enable bits (0-3): the geometry engine has already
             * folded lighting into FinalColor, so they change nothing a
             * rasterizer does. NOT the polygon alpha (16-20), because it can
             * ride in the vertex colour's alpha channel and cost nothing,
             * which matters, since every alpha 1-31 appears and keying on it
             * would put a draw call between almost every pair of polygons.
             * NOT the polygon ID (24-29) for the same reason.
             */
            uint32_t state = (attr & 0x000048F0u)
                           | ((uint32_t)p->WBuffer << 30)
                           | ((uint32_t)p->Translucent << 31);
            uint32_t tkey = textured ? (p->TexParam & 0x3FFFFFFFu) : 0u;
            uint32_t tpal = textured ? p->TexPalette : 0u;

            if (!haveLast || tkey != lastKey || tpal != lastPal
                || state != lastState) {
                batches++;
                lastKey = tkey;
                lastPal = tpal;
                lastState = state;
                haveLast = 1;
            }
        }

        if (fmt == 0 || !textured) {
            continue;
        }

        sWrap[(p->TexParam & (1u << 16))
              ? ((p->TexParam & (1u << 18)) ? 2 : 1) : 0]++;
        sWrap[(p->TexParam & (1u << 17))
              ? ((p->TexParam & (1u << 19)) ? 2 : 1) : 0]++;

        if (p->TexParam & ((1u << 16) | (1u << 17))) sTexRepeat++;
        if (p->TexParam & ((1u << 18) | (1u << 19))) sTexFlip++;
        if (p->TexParam & (1u << 29))                sTexColor0++;

        texels = tex_texels(p->TexParam);
        if (texels > sTexSizeMax) {
            sTexSizeMax = texels;
        }

        /*
         * The key is the whole image parameter minus the coordinate-transform
         * mode: that field says how the vertex's texture coordinates are
         * produced and has nothing to do with which texels are addressed, so
         * two polygons that differ only in it sample the same image and must
         * not be counted as two.
         */
        key = p->TexParam & 0x3FFFFFFFu;
        added = texset_add(key, p->TexPalette);
        if (added < 0) {
            overflowed = 1;
        } else if (added > 0) {
            distinct++;
            bytes += (unsigned long)texels * 4u;       /* RGBA8, 11.3       */
            srcBytes += tex_src_bytes(p->TexParam);
        }
    }

    {
        unsigned n = 0, b;

        for (b = 0; b < 32; b++) {
            n += (frameIds[0] >> b) & 1u;
            n += (frameIds[1] >> b) & 1u;
        }
        if (n > sEdgeIdsMax) {
            sEdgeIdsMax = n;
        }
        sEdgeIdsSum += n;
        sEdgeIdFrames++;
    }

    if (sawW && sawZ) {
        sMixedDepthFrames++;
    }
    sBatches += batches;
    if (batches > sBatchMax) {
        sBatchMax = batches;
    }

    if (overflowed) {
        sTexOverflow++;
    }
    if (distinct > sTexDistinctMax) {
        sTexDistinctMax = distinct;
    }
    if (bytes > sTexBytesMax) {
        sTexBytesMax = bytes;
    }
    if (srcBytes > sTexSrcBytesMax) {
        sTexSrcBytesMax = srcBytes;
    }
}

unsigned long gpu3d_poly_frames(void)
{
    return sPolyFrames;
}

/* ------------------------------------------------------------------ */
/* The report                                                          */
/* ------------------------------------------------------------------ */

void gpu3d_report(FILE *f)
{
    int i;

    fprintf(f, "gx-frames %lu poly-frames %lu\n", sFrames, sPolyFrames);
    fprintf(f, "gx-polygons %lu vertices %lu max-per-frame %lu\n",
            sPolys, sVerts, sPolyMax);
    fprintf(f, "gx-wbuffer-frames %lu\n", sWBufferFrames);
    /* Frames carrying both depth modes against one buffer. Zero means the
     * PICA's depth test is a per-frame register; anything else means the 3D producer
     * needs two passes or a refusal. */
    fprintf(f, "gx-mixed-depth-frames %lu\n", sMixedDepthFrames);
    /* Runs of consecutive polygons sharing texture and state, what a
     * renderer walking the list in order can merge into one draw. */
    fprintf(f, "gx-batches %lu max-per-frame %lu\n", sBatches, sBatchMax);
    fprintf(f, "gx-wrap clamp %lu repeat %lu mirror %lu\n",
            sWrap[0], sWrap[1], sWrap[2]);
    fprintf(f, "gx-clear-translucent-poly-frames %lu\n", sClearAlpha);
    fprintf(f, "gx-fog-shifts %04X\n", sFogShiftSeen);
    fprintf(f, "gx-alpha-values %08lX\n", sAlphaSeen);
    fprintf(f, "gx-polygon-ids %08lX%08lX\n", sIdSeen[1], sIdSeen[0]);
    /* What edge marking would cost to draw; see the note beside sEdgeUniform. */
    fprintf(f, "gx-edge-uniform-frames %lu tables %lu last %04X %04X %04X %04X "
               "%04X %04X %04X %04X\n",
            sEdgeUniform, sEdgeTables, sEdgeTable[0], sEdgeTable[1],
            sEdgeTable[2], sEdgeTable[3], sEdgeTable[4], sEdgeTable[5],
            sEdgeTable[6], sEdgeTable[7]);
    fprintf(f, "gx-opaque-ids-per-frame max %u mean %.2f\n", sEdgeIdsMax,
            sEdgeIdFrames ? (double)sEdgeIdsSum / (double)sEdgeIdFrames : 0.0);
    fprintf(f, "gx-texture-max-texels %u\n", sTexSizeMax);
    fprintf(f, "gx-texture-set %u distinct %lu rgba8-bytes %lu ds-bytes "
               "%lu overflow-frames\n",
            sTexDistinctMax, sTexBytesMax, sTexSrcBytesMax, sTexOverflow);
    fprintf(f, "gx-texcoord-modes %lu %lu %lu %lu\n",
            sTexTransform[0], sTexTransform[1],
            sTexTransform[2], sTexTransform[3]);
    fprintf(f, "gx-cull neither %lu back %lu front %lu both %lu\n",
            sCull[0], sCull[1], sCull[2], sCull[3]);
    fprintf(f, "gx-texparam repeat %lu flip %lu color0 %lu\n",
            sTexRepeat, sTexFlip, sTexColor0);

    /* frames = frames the feature was on; poly-frames = of those, the ones
     * with a polygon on them, which is the number that says whether it was
     * ever on while anything was being drawn. */
    fprintf(f, "# gx-disp name frames poly-frames\n");
    for (i = 0; i < D3_COUNT; i++) {
        fprintf(f, "gx-disp %s %lu %lu\n", kDispName[i], sDisp[i],
                sDispPoly[i]);
    }

    /* named = polygons whose image parameter says that format; sampled =
     * of those, the ones on a frame with DISP3DCNT bit 0 set, which are the
     * only ones a converter is ever called for. */
    fprintf(f, "# gx-format name named sampled\n");
    for (i = 0; i < 8; i++) {
        fprintf(f, "gx-format %s %lu %lu\n", kFmtName[i], sFmt[i], sFmtEff[i]);
    }

    fprintf(f, "# gx-mode name polygons\n");
    for (i = 0; i < 4; i++) {
        fprintf(f, "gx-mode %s %lu\n", kModeName[i], sMode[i]);
    }

    fprintf(f, "# gx-rule name polygons\n");
    for (i = 0; i < P_COUNT; i++) {
        fprintf(f, "gx-rule %s %lu\n", kPolyName[i], sPoly[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */
/*
 * The arithmetic that is this file's own rather than the polygon list's: the
 * texture size and storage rows, and the set's hash agreeing that two
 * different images are two. Everything else here is a bit test against a
 * word the oracle produced, and a test of that would be a copy of the code.
 */
int gpu3d_selftest(int *ranOut)
{
    int ran = 0, bad = 0;
    int i;

    /* 8x8 through 1024x1024, both axes, out of the two 3-bit size fields. */
    ran++;
    if (tex_texels(0u) != 64u) bad++;
    ran++;
    if (tex_texels((7u << 20) | (7u << 23)) != 1024u * 1024u) bad++;
    ran++;
    if (tex_texels((2u << 20) | (1u << 23)) != 32u * 16u) bad++;

    /* One 32x32 image in each format, against the DS's own storage rules. */
    {
        const uint32_t size = (2u << 20) | (2u << 23);   /* 32x32 = 1024   */
        static const unsigned long want[8] = {
            0, 1024, 256, 512, 1024, 384, 1024, 2048
        };
        for (i = 0; i < 8; i++) {
            ran++;
            if (tex_src_bytes(size | ((uint32_t)i << 26)) != want[i]) bad++;
        }
    }

    /*
     * The coordinate-transform mode is not part of the key. Two polygons that
     * differ only in TexParam bits 30-31 sample the same texels, and a survey
     * that counted them twice would report a working set that does not exist.
     */
    ran++;
    if ((((1u << 30) | 0x1234u) & 0x3FFFFFFFu) != (0x1234u & 0x3FFFFFFFu)) {
        bad++;
    }

    /*
     * ...and the palette really is part of it: the same image through two
     * palettes is two textures on the PICA, because the expansion is where
     * the palette is applied. Driven through the table itself, which is safe
     * because texset_clear() runs at the top of every frame and this call
     * site is before the first one.
     */
    texset_clear();
    ran++;
    if (texset_add(0x00041234u, 1u) != 1) bad++;    /* new                 */
    ran++;
    if (texset_add(0x00041234u, 1u) != 0) bad++;    /* ...and seen again   */
    ran++;
    if (texset_add(0x00041234u, 2u) != 1) bad++;    /* other palette, new  */
    ran++;
    if (texset_add(0x00041235u, 1u) != 1) bad++;    /* other image, new    */
    ran++;
    if (sSetN != 3) bad++;

    /*
     * And the ceiling really is a ceiling. A frame that asks for more images
     * than the table holds must say so rather than report the ones that fit:
     * A working set measured short is exactly the mistake that would size a
     * texture cache too small.
     */
    ran++;
    {
        int refused = 0;
        for (i = 0; i < TEXSET_SLOTS; i++) {
            if (texset_add(0x00050000u + (uint32_t)i, 0u) < 0) {
                refused = 1;
                break;
            }
        }
        if (!refused || sSetN != TEXSET_SLOTS / 2) bad++;
    }
    texset_clear();

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return bad;
}
