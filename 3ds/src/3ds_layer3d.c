/*
 * 3ds/src/3ds_layer3d.c: see 3ds_layer3d.h.
 *
 * The whole of the software answer is one pass over 49,152 PIXELS, and the
 * only two things in it that are not obvious are where a pixel goes and what
 * it becomes.
 *
 * Where it goes: straight into swizzle order, the way the tile cache expands a tile.
 * The alternative is a linear staging buffer and a transfer; which is what
 * The GPU present does for a whole surface, and it costs a second 192 KB of traffic and
 * a wait on the transfer engine. A tile is 64 contiguous texels and so is an
 * 8x8 block of this picture, so the pack writes where the GPU already reads.
 *
 * What it becomes: the rasterizer's six bits a channel, expanded to eight by
 * replicating the top two, which is expand() in pc/hw/pc_gpu2d.c and
 * ExpandColor in melonDS. A pixel the rasterizer left at alpha 0 becomes a
 * fully transparent texel rather than a black one, because the alpha test is
 * what makes the layer under it show through; the same mechanism that makes
 * palette index 0 transparent in a tile, and the reason this layer needs no
 * blend state of its own.
 */

#include "3ds_layer3d.h"

#include <stdlib.h>
#include <string.h>

#include "3ds_gpu.h"
#include "3ds_pica3d_gpu.h"
#include "3ds_tile.h"
#include "3ds_view.h"

/*
 * Declared rather than included: pc/include is not on this chain's include
 * path, which is why 3ds_render_wrap.c declares the renderer's type by
 * hand for the same reason.
 *
 * WEAK, because this file is in both links and the rasterizer is only in one:
 * The self-test .3dsx links no pc/hw at all, and an undefined reference there
 * would be a link error in a binary that never draws a frame. Undefined and
 * weak is a null pointer on this toolchain, which is the same test
 * pc_gpu3d_soft_present() would have answered.
 */
extern const uint32_t *pc_gpu3d_soft_line(int y) __attribute__((weak));
extern int pc_gpu3d_soft_present(void) __attribute__((weak));
/*
 * ...and the switch that stops the software rasterizer drawing a frame nobody
 * is going to look at. It rasterizes from pc_gpu3d_vblank() whoever draws the
 * layer, which is 69.71 ms of a 107 ms frame on the emulator and the whole of
 * what the 3D rasterizer exists to save. Turned on here because this file is where the
 * answer to "who draws the layer" is decided, and it is DEFERRED rather than
 * predicted: the rasterizer records that a frame is due and draws it on the
 * first read, so a frame this renderer refuses still gets a software layer,
 * built from the same published list, at the moment the compositor asks.
 */
extern void pc_gpu3d_soft_defer(int on) __attribute__((weak));
extern void pc_gpu3d_soft_final_counts(unsigned long *edgeMarked,
                                       unsigned long *aaBlended)
    __attribute__((weak));

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static uint32_t *sPool;
static int sWidth, sHeight;
static int sBlockCols;
/* Morton order inside a block, taken once: the pack walks it 49,152 times a
 * frame and the shifts are worth more as one load (tile_swizzle). */
static uint8_t sSwizzle[TILE_TEXELS];
static int sMode = L3D_SOFT;

static unsigned long sPacked;       /* frames packed                     */
static unsigned long sTranslucent;  /* ...of which some pixel was blended */
static unsigned long sEmpty;        /* ...and packed nothing at all       */
static int sDirty;                  /* the pool has writes the GPU has not
                                     * been shown yet                     */

/* ------------------------------------------------------------------ */
/* The 3D rasterizer's diff                                            */
/* ------------------------------------------------------------------ */

/*
 * What this measures. Every renderer step before this one closed on a byte
 * compare, because each reproduced integer composition exactly. A PICA
 * rasterizer cannot: the DS truncates the depth gradient before interpolating
 * and approximates the perspective curve with eight bits of a factor, and this
 * renderer does not yet mark edges at all. So "can `3d.txt` default to pica" is
 * not a verdict anyone can reason to; it is a distribution, and this measures
 * it.
 *
 * The two pictures are one frame apart, and that is not an approximation.
 * layer3d_produce() runs at survey time, outside any citro3d frame; the PICA's
 * draw half runs later, inside it. So at produce time for frame N the render
 * target still holds frame N-1, while pc_gpu3d_soft_line() answers for frame
 * N. The software picture is therefore kept for one frame and compared then,
 * against the very draws it is paired with.
 *
 * Which frames are comparable: only one where the previous produce built a
 * layer and the frame in between rendered it. produce() is not called every
 * frame, and the draw half redraws whatever the vertex buffer still holds, so
 * a skipped produce leaves a picture in VRAM that no shadow is paired with.
 * layer3d_render() runs every frame and nothing else does.
 *
 * The kinds of disagreement are counted apart because they have different
 * causes. A coverage difference is a pixel one renderer drew and the other did
 * not: an edge rule, the depth test, a clipped polygon. A colour difference is
 * a pixel both drew and disagreed about: interpolation, a texel, the blend.
 * One is a shape being wrong and the other a shade, and a single count cannot
 * tell a missing object from a slightly darker floor.
 *
 * It costs a frame of uncached VRAM reads, so it is a mode and never a
 * default.
 */
#define L3D_DIFF_DUMPS 8            /* pictures of the worst frame, at most */

static uint32_t *sShadow;           /* the software layer, one frame back  */
static int sDiff;
static unsigned long sRenders;      /* layer3d_render() calls: every frame */
static unsigned long sShadowAt;     /* ...its value when the shadow was filled */
static int sShadowValid;

static unsigned long sDiffFrames;   /* frame pairs compared                */
static unsigned long sDiffEqual;    /* ...identical over all 49,152 pixels */
static unsigned long sDiffPixels;   /* differing pixels, over the run      */
static unsigned long sDiffOnly3D;   /* ...the PICA drew, the software did not */
static unsigned long sDiffOnlySoft; /* ...the software drew, the PICA did not */
static unsigned long sDiffColour;   /* ...both drew and disagreed          */
/* The colour disagreements by how far apart they are: one six-bit step, four
 * steps, sixteen, and further. A ramp neighbour and a wrong object look the
 * same in a total and nothing alike here. */
static unsigned long sDiffGap[4];
/*
 * ...and the sum of them, which is the only number two renderings can be
 * Ranked by. A count of differing pixels says a rounding step and a missing
 * outline are the same event, and the buckets say which bucket moved but not
 * which way the picture went: the 3D pixel work's edge marking moved half of `wild` into
 * `far`, which is better by any reading of the picture and worse by a count.
 * The mean gap over the whole surface, differing pixels and identical ones
 * together, settles it in one number.
 */
static unsigned long long sDiffGapSum;
/*
 * ...over a fixed number of pairs, because a run's own total is not comparable
 * with another run's. A slower renderer reaches fewer frames of the replay in
 * the same wall clock, and the frames it does not reach are the easy ones,
 * so a whole-run mean flatters whichever configuration got further. The first
 * L3D_DIFF_WINDOW pairs are the same frames of the same script in every run.
 */
#define L3D_DIFF_WINDOW 1500
static unsigned long long sDiffWindowSum;
static unsigned long sDiffWindowFrames;
static int sDiffWorst;              /* the widest channel gap over the run */
static unsigned long sDiffWorstFrame;   /* the frame with the most differing */
static unsigned long sDiffWorstPixels;
static int sDiffDumps;
static unsigned long sDiffLastFrame, sDiffLastPixels;
/*
 * Frames by how much of them differs, because a total cannot tell a handful of
 * ruined frames from every frame being a little wrong, and those two have
 * nothing in common. The bins are 0, under 100, under 1k, 4k, 8k, 16k, 32k and
 * the rest, of 49,152.
 */
static unsigned long sDiffHist[8];

/* ------------------------------------------------------------------ */
/* The mode file                                                       */
/* ------------------------------------------------------------------ */

static int mode_read(void)
{
    FILE *f = fopen(L3D_PATH, "r");
    char word[16];
    int mode = L3D_SOFT;

    if (f == NULL) {
        return L3D_SOFT;
    }
    if (fscanf(f, "%15s", word) == 1) {
        if (strcmp(word, "soft") == 0) {
            mode = L3D_SOFT;
        } else if (strcmp(word, "pica") == 0) {
            mode = L3D_PICA;
        } else {
            fprintf(stderr, "3ds-layer3d: %s says \"%s\", which is not "
                            "soft/pica, using soft\n", L3D_PATH, word);
        }
    }
    fclose(f);
    return mode;
}

int layer3d_mode(void)
{
    return sMode;
}

const char *layer3d_mode_name(void)
{
    return sMode == L3D_PICA ? "pica" : "soft";
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

int layer3d_init(uint32_t *pool, int width, int height)
{
    int x, y;

    sMode = mode_read();
    if (sMode == L3D_PICA && pc_gpu3d_soft_defer != NULL) {
        pc_gpu3d_soft_defer(1);
    }
    if (sMode == L3D_PICA) {
        if (pica3d_gpu_init() != 0) {
            /*
             * The renderer refused to come up, no VRAM for the target, or no
             * shader. Fall back rather than draw nothing, and say so once: a
             * knob that quietly does the other thing is how a measured run
             * gets measured wrong.
             */
            fprintf(stderr, "3ds-layer3d: the PICA renderer would not start, "
                            "using soft\n");
            sMode = L3D_SOFT;
            if (pc_gpu3d_soft_defer != NULL) {
                pc_gpu3d_soft_defer(0);
            }
        } else if (pica3d_gpu_diff()) {
            sShadow = (uint32_t *)malloc((size_t)VIEW_DS_WIDTH
                                         * VIEW_DS_HEIGHT * sizeof *sShadow);
            sDiff = sShadow != NULL;
            if (!sDiff) {
                fprintf(stderr, "3ds-layer3d: no room for the diff's shadow, "
                                "so p3d.txt's `diff` does nothing\n");
            }
        }
    }
    if (pool == NULL || width < VIEW_DS_WIDTH || height < VIEW_DS_HEIGHT
        || (width % TILE_SIDE) != 0 || (height % TILE_SIDE) != 0) {
        return -1;
    }
    sPool = pool;
    sWidth = width;
    sHeight = height;
    sBlockCols = width / TILE_SIDE;
    for (y = 0; y < TILE_SIDE; y++) {
        for (x = 0; x < TILE_SIDE; x++) {
            sSwizzle[y * TILE_SIDE + x] = (uint8_t)tile_swizzle(x, y);
        }
    }
    /*
     * The rows past the picture are never sampled, the quad's texture
     * coordinates stop at 192, but a texture with nothing in them is one
     * fewer thing to explain if a coordinate is ever a row out.
     */
    memset(sPool, 0, (size_t)width * (size_t)height * sizeof *sPool);
    sDirty = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* The pack                                                            */
/* ------------------------------------------------------------------ */

/* Six bits per channel out to eight, replicating the top two, pc_gpu2d.c's
 * expand(), and it has to be exactly that. */
static inline uint32_t texel(uint32_t c)
{
    uint32_t r = c & 0x3Fu, g = (c >> 8) & 0x3Fu, b = (c >> 16) & 0x3Fu;

    r = (r << 2) | (r >> 4);
    g = (g << 2) | (g >> 4);
    b = (b << 2) | (b >> 4);
    return (r << 24) | (g << 16) | (b << 8) | 0xFFu;
}

/* ------------------------------------------------------------------ */
/* The diff                                                            */
/* ------------------------------------------------------------------ */

/*
 * The three pictures of one frame: what the software rasterizer drew, what the
 * PICA drew, and where they disagree.
 *
 * The map is the point and the other two are its caption. A count says a frame
 * has 900 differing pixels; only the map says whether that is a missing object,
 * one polygon's edge, or a whole floor a shade out, and those three want
 * completely different work. Red is the PICA drawing where the software did
 * not, blue is the software drawing where the PICA did not, and green is a
 * pixel both drew and disagreed about, brighter the further apart.
 */
static void diff_dump(const char *const *kPath)
{
    int which, x, y;

    for (which = 0; which < 3; which++) {
        FILE *f = fopen(kPath[which], "wb");

        if (f == NULL) {
            return;
        }
        fprintf(f, "P6\n%d %d\n255\n", VIEW_DS_WIDTH, VIEW_DS_HEIGHT);
        for (y = 0; y < VIEW_DS_HEIGHT; y++) {
            for (x = 0; x < VIEW_DS_WIDTH; x++) {
                uint32_t raw = sShadow[(size_t)y * VIEW_DS_WIDTH + x];
                uint32_t sw = (raw >> 24) != 0u ? texel(raw & 0x00FFFFFFu) : 0u;
                uint32_t pica = pica3d_gpu_texel(x, y);
                uint32_t c = which == 0 ? sw : pica;
                unsigned char rgb[3];

                if (which == 2) {
                    int drewS = (sw & 0xFFu) != 0;
                    int drewP = (pica & 0xFFu) != 0;
                    int gap = 0, ch;

                    for (ch = 8; ch <= 24; ch += 8) {
                        int a = (int)((sw >> ch) & 0xFFu);
                        int b = (int)((pica >> ch) & 0xFFu);

                        if ((a > b ? a - b : b - a) > gap) {
                            gap = a > b ? a - b : b - a;
                        }
                    }
                    rgb[0] = (unsigned char)(drewP && !drewS ? 255 : 0);
                    rgb[2] = (unsigned char)(drewS && !drewP ? 255 : 0);
                    rgb[1] = (unsigned char)(drewS && drewP && gap
                                             ? (gap > 63 ? 255 : 64 + gap * 3)
                                             : 0);
                } else {
                    rgb[0] = (unsigned char)(c >> 24);
                    rgb[1] = (unsigned char)(c >> 16);
                    rgb[2] = (unsigned char)(c >> 8);
                }
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
    }
}

/*
 * The layer the PICA left in VRAM against the software picture kept from the
 * produce before this one. Both are read as 0xRRGGBBAA, and "drawn" is a
 * non-zero alpha in both; which is not a convention of this file's but the
 * compositor's own test on the layer quad.
 */
static void diff_previous(void)
{
    unsigned long differing = 0;
    unsigned long long gapsum = 0;
    int x, y;

    for (y = 0; y < VIEW_DS_HEIGHT; y++) {
        for (x = 0; x < VIEW_DS_WIDTH; x++) {
            uint32_t raw = sShadow[(size_t)y * VIEW_DS_WIDTH + x];
            uint32_t sw = (raw >> 24) != 0u ? texel(raw & 0x00FFFFFFu) : 0u;
            uint32_t pica = pica3d_gpu_texel(x, y);
            int drewS = (sw & 0xFFu) != 0;
            int drewP = (pica & 0xFFu) != 0;
            int gap = 0, ch;

            if (drewS != drewP) {
                differing++;
                gapsum += 255u;         /* drawn against not drawn is the most */
                if (drewP) {
                    sDiffOnly3D++;
                } else {
                    sDiffOnlySoft++;
                }
                continue;
            }
            if (!drewS) {
                continue;           /* both left it to the layer below */
            }
            for (ch = 8; ch <= 24; ch += 8) {
                int a = (int)((sw >> ch) & 0xFFu);
                int b = (int)((pica >> ch) & 0xFFu);
                int d = a > b ? a - b : b - a;

                if (d > gap) {
                    gap = d;
                }
            }
            if (gap == 0) {
                continue;
            }
            differing++;
            gapsum += (unsigned long long)gap;
            sDiffColour++;
            sDiffGap[gap <= 4 ? 0 : gap <= 16 ? 1 : gap <= 64 ? 2 : 3]++;
            if (gap > sDiffWorst) {
                sDiffWorst = gap;
            }
        }
    }

    sDiffFrames++;
    sDiffPixels += differing;
    sDiffGapSum += gapsum;
    if (sDiffWindowFrames < L3D_DIFF_WINDOW) {
        sDiffWindowFrames++;
        sDiffWindowSum += gapsum;
    }
    sDiffHist[differing == 0 ? 0 : differing < 100 ? 1 : differing < 1000 ? 2
              : differing < 4000 ? 3 : differing < 8000 ? 4
              : differing < 16000 ? 5 : differing < 32000 ? 6 : 7]++;
    /*
     * ...and a recent frame, overwriting, which is a different question from
     * the worst one: the record set is written eight times and they are all
     * early, so on their own they say what the first scene looked like and
     * nothing about the rest of the run. This is dump_layer()'s own policy and
     * for its own reason, whatever is on the card when the console is closed
     * came from just before it was closed.
     */
    if ((sDiffFrames % 240u) == 0u) {
        static const char *const kLast[3] = {
            "sdmc:/3ds/pokeplatinum/l3d-last-soft.ppm",
            "sdmc:/3ds/pokeplatinum/l3d-last-pica.ppm",
            "sdmc:/3ds/pokeplatinum/l3d-last-map.ppm"
        };

        sDiffLastFrame = sDiffFrames;
        sDiffLastPixels = differing;
        diff_dump(kLast);
    }
    if (differing == 0) {
        sDiffEqual++;
        return;
    }
    /*
     * The pictures come out on the frame that beats the record, and at most a
     * handful of times: what a decision needs is the worst case this run
     * produced, and writing 576 KB to an SD card from inside a frame is not
     * something to do once a second.
     */
    if (differing > sDiffWorstPixels) {
        sDiffWorstPixels = differing;
        sDiffWorstFrame = sDiffFrames;
        if (sDiffDumps < L3D_DIFF_DUMPS) {
            static const char *const kWorst[3] = {
                "sdmc:/3ds/pokeplatinum/l3d-diff-soft.ppm",
                "sdmc:/3ds/pokeplatinum/l3d-diff-pica.ppm",
                "sdmc:/3ds/pokeplatinum/l3d-diff-map.ppm"
            };

            sDiffDumps++;
            diff_dump(kWorst);
        }
    }
}

/* This frame's software picture, kept raw, six bits a channel with the DS's
 * own alpha, because the alpha is what says whether the rasterizer drew the
 * pixel at all and the expansion is cheaper to repeat than to store. */
static void shadow_fill(void)
{
    int y;

    if (pc_gpu3d_soft_line == NULL || pc_gpu3d_soft_present == NULL
        || !pc_gpu3d_soft_present()) {
        sShadowValid = 0;
        return;
    }
    for (y = 0; y < VIEW_DS_HEIGHT; y++) {
        const uint32_t *row = pc_gpu3d_soft_line(y);

        if (row == NULL) {
            sShadowValid = 0;
            return;
        }
        memcpy(&sShadow[(size_t)y * VIEW_DS_WIDTH], row,
               (size_t)VIEW_DS_WIDTH * sizeof *sShadow);
    }
    sShadowValid = 1;
    sShadowAt = sRenders;
}

unsigned layer3d_produce(const struct effect *e)
{
    unsigned flags = L3D_READY;
    int drawn = 0;
    int by, bx, y, x;

    /*
     * The 3D rasterizer's answer to the same question. It converts and batches here and
     * draws from layer3d_render() once the frame is open, because a PICA
     * cannot be asked to draw at survey time; see 3ds_pica3d_gpu.h.
     *
     * It has nowhere to put the effect pass's TRANSFORM. The polygons carry their own
     * vertex colours and the textures are converted once and cached, so a
     * fade would have to be a TEV stage, eight-bit arithmetic where the DS
     * is six, on a layer the compositor then blends. Refused rather than
     * approximated: the frame goes back to the software renderer whole.
     */
    if (sMode == L3D_PICA) {
        unsigned flags;

        /*
         * The diff reads VRAM BEFORE the build half runs, which is the same
         * quiet window dump_layer() takes: the previous frame is long
         * submitted and presented and this thread is between the game's last
         * store and the renderer's first read.
         */
        if (sShadow != NULL && sShadowValid && sRenders == sShadowAt + 1) {
            diff_previous();
        }
        sShadowValid = 0;
        flags = effect_is_identity(e) ? pica3d_gpu_build() : 0u;
        /* A refused frame is drawn in software and the render target is left
         * as a bare clear, so there is nothing for the next produce to be
         * paired with; which is what leaving the shadow invalid says. */
        if (sShadow != NULL && flags != 0u) {
            shadow_fill();
        }
        return flags;
    }

    if (sPool == NULL || pc_gpu3d_soft_present == NULL
        || pc_gpu3d_soft_line == NULL || !pc_gpu3d_soft_present()) {
        return 0;
    }

    for (by = 0; by < VIEW_DS_HEIGHT / TILE_SIDE; by++) {
        const uint32_t *row[TILE_SIDE];

        for (y = 0; y < TILE_SIDE; y++) {
            row[y] = pc_gpu3d_soft_line(by * TILE_SIDE + y);
            if (row[y] == NULL) {
                /* No rasterizer output at all. Not a black layer: a black
                 * layer is a picture, and this is the absence of one. */
                return 0;
            }
        }
        for (bx = 0; bx < VIEW_DS_WIDTH / TILE_SIDE; bx++) {
            uint32_t *dst = sPool
                          + (size_t)(by * sBlockCols + bx) * TILE_TEXELS;

            for (y = 0; y < TILE_SIDE; y++) {
                const uint32_t *src = row[y] + bx * TILE_SIDE;

                for (x = 0; x < TILE_SIDE; x++) {
                    uint32_t c = src[x];
                    uint32_t a = c >> 24;

                    if (a == 0u) {
                        /* draw_bg_3d() SKIPS this pixel rather than drawing it
                         * transparent, which is what leaves the layer under it
                         * as it was. The alpha test is the same skip. */
                        dst[sSwizzle[y * TILE_SIDE + x]] = 0u;
                        continue;
                    }
                    if (a < 31u) {
                        flags |= L3D_TRANSLUCENT;
                    }
                    drawn++;
                    dst[sSwizzle[y * TILE_SIDE + x]] =
                        texel(effect_colour6(e, c & 0x00FFFFFFu));
                }
            }
        }
    }

    sPacked++;
    if (flags & L3D_TRANSLUCENT) {
        sTranslucent++;
    }
    if (drawn == 0) {
        sEmpty++;
    }
    sDirty = 1;
    return flags;
}

/* ------------------------------------------------------------------ */
/* The quad                                                            */
/* ------------------------------------------------------------------ */

int layer3d_quad(float ox, float oy)
{
    struct gpu_vertex *v = gpu_vertex_alloc(6);
    float u0 = 0.0f;
    float u1 = (float)VIEW_DS_WIDTH / (float)sWidth;
    /* Memory row 0 of a texture written this way is v = 1, the mapping the GPU present
     * established with a byte compare and cell_quad() draws tiles by. So the
     * picture's top row is the largest v and it counts down. */
    float v0 = 1.0f;
    float v1 = 1.0f - (float)VIEW_DS_HEIGHT / (float)sHeight;
    float x0 = ox, y0 = oy;
    float x1 = ox + (float)VIEW_DS_WIDTH, y1 = oy + (float)VIEW_DS_HEIGHT;

    if (v == NULL || sPool == NULL) {
        return -1;
    }
    v[0] = (struct gpu_vertex){ x0, y0, 0.5f, u0, v0 };
    v[1] = (struct gpu_vertex){ x0, y1, 0.5f, u0, v1 };
    v[2] = (struct gpu_vertex){ x1, y1, 0.5f, u1, v1 };
    v[3] = (struct gpu_vertex){ x0, y0, 0.5f, u0, v0 };
    v[4] = (struct gpu_vertex){ x1, y1, 0.5f, u1, v1 };
    v[5] = (struct gpu_vertex){ x1, y0, 0.5f, u1, v0 };
    return 0;
}

void layer3d_render(void)
{
    /* Every frame, and it is the only thing here that is: the diff pairs a
     * picture with the shadow taken one render earlier, and produce() alone
     * cannot see the frames it was not asked about. */
    sRenders++;
    if (sMode == L3D_PICA) {
        pica3d_gpu_draw();
    }
}

int layer3d_no_split(void)
{
    return sMode == L3D_PICA && pica3d_gpu_no_split();
}

void *layer3d_texture(void)
{
    return sMode == L3D_PICA ? pica3d_gpu_texture() : NULL;
}

void layer3d_flush(void)
{
    /* Nothing the CPU wrote in the PICA path: the texels are a render target
     * the GPU filled, and it needs no cache maintenance from this side. */
    if (sMode == L3D_PICA) {
        return;
    }
    if (sPool == NULL || !sDirty) {
        return;
    }
    /* The picture's blocks are the first rows of blocks in the texture, so
     * what was written is one contiguous run and not the whole 256 KB. */
    gpu_cache_flush(sPool, (size_t)sWidth * VIEW_DS_HEIGHT * sizeof *sPool);
    sDirty = 0;
}

void layer3d_report(FILE *f)
{
    fprintf(f, "l3d-mode %s\n", layer3d_mode_name());
    fprintf(f, "l3d-packed %lu\n", sPacked);
    /* Frames the 2D path then had to refuse because BLDCNT would have blended
     * one of these pixels; the number that says whether the six-bit blend is
     * worth a design of its own. */
    fprintf(f, "l3d-translucent %lu\n", sTranslucent);
    /* ...and frames where the layer was on and had nothing on it, which is a
     * pack the game paid for and saw nothing from. */
    fprintf(f, "l3d-empty %lu\n", sEmpty);
    if (sDiff) {
        /*
         * The 3D rasterizer's question, as a distribution rather than a verdict. `pairs` is
         * how many frames both renderers drew and could be compared, always
         * fewer than the frames produced, because a refused frame leaves the
         * target a bare clear and a skipped produce leaves it a redraw.
         */
        fprintf(f, "l3d-diff pairs %lu identical %lu pixels %lu of %lu\n",
                sDiffFrames, sDiffEqual, sDiffPixels,
                sDiffFrames * (unsigned long)(VIEW_DS_WIDTH * VIEW_DS_HEIGHT));
        /* A shape being wrong, split from a shade being wrong. */
        fprintf(f, "l3d-diff-cover only3d %lu onlysoft %lu\n",
                sDiffOnly3D, sDiffOnlySoft);
        /*
         * ...and what the software renderer's final pass did to the pixels
         * This one is being compared with. Both DISP3DCNT bits are on for
         * nearly every frame this game draws, and the difference a PICA leaves
         * is on the silhouettes, so these two counts are what say which of
         * the two a renderer without either should be built to reproduce.
         */
        if (pc_gpu3d_soft_final_counts != NULL) {
            unsigned long marked = 0, blended = 0;

            pc_gpu3d_soft_final_counts(&marked, &blended);
            fprintf(f, "l3d-soft-final edge-marked %lu aa-blended %lu"
                       " per-frame %.1f %.1f\n",
                    marked, blended,
                    sDiffFrames ? (double)marked / (double)sDiffFrames : 0.0,
                    sDiffFrames ? (double)blended / (double)sDiffFrames : 0.0);
        }
        fprintf(f, "l3d-diff-colour %lu step %lu near %lu far %lu wild %lu"
                   " worst %d\n",
                sDiffColour, sDiffGap[0], sDiffGap[1], sDiffGap[2],
                sDiffGap[3], sDiffWorst);
        /* The one number to rank two renderings by: how far apart the two
         * pictures are per pixel of surface, counting the identical ones. */
        fprintf(f, "l3d-diff-mean %.4f first %lu %.4f\n",
                sDiffFrames
                  ? (double)sDiffGapSum
                    / ((double)sDiffFrames * (VIEW_DS_WIDTH * VIEW_DS_HEIGHT))
                  : 0.0,
                sDiffWindowFrames,
                sDiffWindowFrames
                  ? (double)sDiffWindowSum
                    / ((double)sDiffWindowFrames
                       * (VIEW_DS_WIDTH * VIEW_DS_HEIGHT))
                  : 0.0);
        /* ...and the frame a picture was kept of, which is the one a decision
         * about this renderer should be made on. */
        fprintf(f, "l3d-diff-worst frame %lu pixels %lu dumps %d\n",
                sDiffWorstFrame, sDiffWorstPixels, sDiffDumps);
        /* ...and the pair the `l3d-last-*` pictures are of, which is a
         * TYPICAL frame and not the worst one. */
        fprintf(f, "l3d-diff-last frame %lu pixels %lu\n",
                sDiffLastFrame, sDiffLastPixels);
        fprintf(f, "l3d-diff-hist none %lu u100 %lu u1k %lu u4k %lu u8k %lu"
                   " u16k %lu u32k %lu more %lu\n",
                sDiffHist[0], sDiffHist[1], sDiffHist[2], sDiffHist[3],
                sDiffHist[4], sDiffHist[5], sDiffHist[6], sDiffHist[7]);
    }
    if (sMode == L3D_PICA) {
        pica3d_gpu_report(f);
    }
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#define CHECK(cond)                                                          \
    do {                                                                     \
        ran++;                                                               \
        if (!(cond)) {                                                       \
            fprintf(stderr, "  3ds_layer3d.c:%d failed\n", __LINE__);         \
            bad++;                                                           \
        }                                                                    \
    } while (0)

int layer3d_selftest(int *ranOut)
{
    int ran = 0, bad = 0;

    /*
     * The expansion, against the numbers it has to agree with. 0x3E is what a
     * 15-bit palette colour reaches (five bits shifted up one) and it
     * expands to the 2D engine's own white, the tile cache's 0xFB. The rasterizer
     * interpolates in six bits and does reach 0x3F, which expands to 0xFF: the
     * 3D layer has a whiter white than any tile, and that is the hardware's
     * and not a rounding step of this file's.
     */
    CHECK(texel(0x3E3E3Eu | (31u << 24)) == 0xFBFBFBFFu);
    CHECK(texel(0x3F3F3Fu | (31u << 24)) == 0xFFFFFFFFu);
    CHECK(texel(0x000000u | (31u << 24)) == 0x000000FFu);
    CHECK(texel(0x000001u | (31u << 24)) == 0x040000FFu);
    CHECK(texel(0x000100u | (31u << 24)) == 0x000400FFu);
    CHECK(texel(0x010000u | (31u << 24)) == 0x000004FFu);

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return bad;
}
