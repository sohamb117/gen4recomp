/*
 * 3ds/src/3ds_gpu.c: see 3ds_gpu.h.
 *
 * What this file replaces: view_present() walked 98,304 pixels a frame,
 * converted each from 0x00RRGGBB into three bytes and wrote them down a column
 * of the framebuffer, because the panels are mounted at ninety degrees. That
 * is 24.7 ms on hardware and 4.5 ms under the emulator, measured, and nearly
 * constant. Here the same picture is two textured quads: the CPU copies each
 * surface into linear memory with one shift and one or, the transfer engine
 * tiles it into a texture, and the PICA draws it through an orthographic
 * matrix that carries the rotation.
 *
 * Three things make it exact, because "the GPU draws it" is not by itself a
 * claim that the picture is unchanged:
 *
 *   1. 1x, axis-aligned, integer coordinates and GPU_NEAREST. A quad from
 *      x=72 to x=328 covers pixels 72 to 327, and a texcoord interpolated at
 *      each pixel's centre lands in the middle of one texel. Nothing scales.
 *   2. No blending and no depth. The alpha test discards transparent texels
 *      outright, so the blend unit never runs and no rounding can happen.
 *   3. One TEV stage, GPU_REPLACE from texture 0. The texel is the pixel.
 *
 * And PRESENT_VERIFY, which takes none of that on trust: it composes the frame
 * with the old CPU blit into a buffer and compares it byte for byte with what
 * the GPU left in the framebuffer.
 *
 * The surface is copied because pc_video's two surfaces are `static uint32_t`
 * in .bss, which is process heap, and the PICA and the transfer engine take
 * physical addresses and can only reach the linear heap and VRAM. So one copy
 * is unavoidable, and it is the cheapest possible one.
 *
 * The vblank callback is not ours to take. C3D_Init installs one on
 * GSPGPU_EVENT_VBlank0, libctru keeps exactly one callback per event, and the
 * hang detector wants that slot. The order in 3ds_main.c settles it, gpu_init()
 * before watchdog_arm(), and this file never calls C3D_FrameSync so it never
 * needs the counter citro3d's callback keeps.
 */

#include "3ds_gpu.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "3ds_gpu2d.h"
#include "3ds_layer3d.h"
#include "3ds_perf.h"
#include "3ds_sdcard.h"
#include "3ds_tile.h"
#include "3ds_view.h"

/* bin2s' symbols for build/3ds/present.shbin. Declared rather than included:
 * The generated header carries the size as a static, which every translation
 * unit that includes it would then have its own copy of. */
extern const uint8_t present_shbin[];
extern const uint8_t present_shbin_end[];

/* ------------------------------------------------------------------ */
/* Geometry and formats                                                */
/* ------------------------------------------------------------------ */

/*
 * The surface texture. A PICA texture's sides are powers of two, so 192 rows
 * of picture live in the top of a 256x256 texture and the quad samples the
 * first three quarters of it. The 64 unused rows are 64 KB of VRAM per screen
 * and nothing reads them.
 */
#define TEX_SIDE   256

/*
 * WHERE 192 rows of picture are in a 256 ROW TEXTURE, and the answer is not
 * "at the top". The transfer engine reads its linear side bottom-up and the
 * sampler's v axis runs from the bottom of the texture, so an upload shorter
 * than the texture lands against the *far* edge of both: image row 0 samples
 * at v = 64/256 and row 191 at v = 1.0.
 *
 * PRESENT_VERIFY found this and it is the reason that mode exists. The
 * picture looked right in a screenshot, upright, unmirrored, the marker at
 * the top left of what was on screen, because the 64 rows it was short by
 * are letterbox-coloured and the sixty-four rows it lost off the bottom were
 * off the panel. The byte compare said 25,965,868 pixels over 1,532 frames.
 */
#define TEX_V_TOP    ((float)(TEX_SIDE - VIEW_DS_HEIGHT) / (float)TEX_SIDE)
#define TEX_V_BOTTOM 1.0f

/*
 * The font, as one texture. 96 cells of 8x8 in a 128x64 sheet: the 95 code
 * points 3ds_view.c's font has glyphs for, and the hollow box it draws for
 * everything else in the last cell.
 *
 * It is built by calling view_text(), not by reading the font table. Two
 * copies of a hand-drawn font is exactly the kind of thing that drifts, and
 * the atlas would drift silently; the letterbox line would still be text,
 * just the wrong text. Building it through the same function the CPU path
 * draws with means there is only one font in this port.
 */
#define ATLAS_W    128
#define ATLAS_H    64
#define ATLAS_CELL 8
#define ATLAS_COLS (ATLAS_W / ATLAS_CELL)
#define ATLAS_CELLS 96
#define ATLAS_MISSING (ATLAS_CELLS - 1)

/* Longest letterbox line drawn. The CPU path stops at the screen edge, which
 * is 54 glyphs from the picture's left edge on the upper panel; this is the
 * buffer bound, not a display limit. */
#define GPU_TEXT_MAX 96

/*
 * ...and the background path's, which is the buffer's real size: a text background scrolled
 * by anything that is not a multiple of eight shows 33 columns of 25 rows, and
 * an engine can have four of them. Two engines of that is 6,600 quads, and the
 * buffer is sized for the worst case on purpose, gpu2d_build() runs AFTER
 * the software renderer has been skipped for that engine, so a buffer that
 * could run out would be a stale picture rather than a slow one.
 */
#define BG_COLS  (VIEW_DS_WIDTH / TILE_SIDE + 1)
#define BG_ROWS  (VIEW_DS_HEIGHT / TILE_SIDE + 1)
#define BG_MAX_QUADS (2 * 4 * BG_COLS * BG_ROWS)
/*
 * The sprite path: 128 sprites, 64x64 is 64 tiles, both engines, and a Y-wrap copies
 * the tiles once more. A frame that does not fit is a stale picture, because
 * gpu2d_build() runs after the software renderer has been skipped.
 */
#define OBJ_MAX_QUADS (2 * 128 * 8 * 8)

#define GPU_MAX_QUADS (2 + 4 * GPU_TEXT_MAX + BG_MAX_QUADS + OBJ_MAX_QUADS)
#define GPU_MAX_VERTS (GPU_MAX_QUADS * 6)

/*
 * The tile atlas the tile cache expands into: 8,192 slots as a 1024x512 texture, which
 * is 128 by 64 blocks and therefore slot n at block n. It is two megabytes and
 * it is in linear memory rather than VRAM, which is the opposite of the two
 * surface textures above and for a measured reason; the CPU writes this one
 * every frame, and VRAM on this console is uncached device memory to the ARM11
 * while linear memory is write-back cacheable. A tile expanded into VRAM would
 * pay for its 256 bytes at bus speed twice over.
 *
 * 8,192 rather than the 6,600 a worst-case frame can ask for: the cache is
 * keyed on tile and palette together, so the same tile drawn through two
 * palettes is two slots, and a pool with no slack evicts entries it is about
 * to want again.
 */
#define ATLAS2D_W 1024
#define ATLAS2D_H 512
#define ATLAS2D_SLOTS ((ATLAS2D_W / TILE_SIDE) * (ATLAS2D_H / TILE_SIDE))

/*
 * Linear -> tiled, and the flip.
 *
 * The transfer engine reads its linear side bottom-up, so an upload without
 * GX_TRANSFER_FLIP_VERT(1) lands upside down. That is not a fact this file
 * takes from a document: PRESENT_VERIFY was run both ways on the console and
 * one of them is byte-identical to the CPU blit.
 */
#define TEXTURE_TRANSFER_FLAGS                                                \
    (GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1)                      \
     | GX_TRANSFER_RAW_COPY(0)                                                \
     | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)                           \
     | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8)                          \
     | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

/*
 * Render target -> framebuffer. RGBA8 out of the PICA, RGB8 into the
 * GSP_BGR8_OES framebuffers gfxInitDefault() hands out, the same pair of
 * formats every citro3d application uses, and the reason the CPU path's three
 * bytes per pixel and this path's four never have to agree anywhere but here.
 */
#define DISPLAY_TRANSFER_FLAGS                                                \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0)                      \
     | GX_TRANSFER_RAW_COPY(0)                                                \
     | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)                           \
     | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8)                           \
     | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

/* The clear colour, as the PICA writes a colour: 0xRRGGBBAA. */
#define CLEAR_COLOUR ((VIEW_COLOUR_LETTERBOX << 8) | 0xFFu)

typedef struct gpu_vertex Vertex;

struct screen {
    C3D_RenderTarget *target;
    C3D_Mtx proj;
    C3D_Tex tex;
    uint32_t *stage;            /* linear: one surface, RGBA8            */
    uint8_t *cpu;               /* PRESENT_VERIFY: the blit's own answer  */
    uint8_t *fb;                /* ... and where the GPU's went           */
    size_t fbBytes;
    gfxScreen_t which;
    int width;                  /* 400 or 320, in display coordinates     */
    int picBase, picCount;      /* vertex ranges built this frame         */
    int txtBase, txtCount;
    int stencil;                /* the target has a stencil buffer (11.6)  */
    int engine;                 /* which 2D engine feeds this panel        */
    int tiles;                  /* 11.4 drew it, so `tex` is not read      */
    int tilesShown;             /* ...and what that was for the frame the
                                 * comparison below is about, which is the
                                 * one already on the panel                */
    int exact, exactShown;      /* ...and whether the GPU did any arithmetic
                                 * of its own on it (the effect pass)      */
    int l3d, l3dShown;          /* ...and whether the PICA RASTERIZED its 3D
                                 * layer rather than copying one (the 3D rasterizer)    */
};

static struct screen sScreen[2];
static C3D_Tex sAtlas;
static C3D_Tex sAtlas2D;
/*
 * The 3D layer's 3D layer. 256x256 because a PICA texture's sides are powers of two
 * and the picture is 256x192; the rows past the picture are never sampled. In
 * linear memory rather than VRAM for the same measured reason the tile atlas
 * is; the CPU writes it every frame, and VRAM is uncached to the ARM11.
 */
static C3D_Tex sLayer3D;
static C3D_Tex sVramTex;            /* 11.8: DISPCNT's VRAM display mode */
static unsigned sFrameCounter;
/*
 * What the background path's build cost this frame. It is charged to PERF_BLIT; it is the
 * work that replaces the surface copy, and it happens inside the interval
 * the flush is timed over, so the flush has to give it back or the two spans
 * count it twice. The first hardware run did exactly that: submitting the
 * frame read as 9.7 ms, of which 5.6 was the build, and the leftover "game"
 * span went to its floor of zero.
 */
static u64 sBuildTicks;
static DVLB_s *sDvlb;
static shaderProgram_s sProgram;
static int sUniProjection;
static Vertex *sVerts;
static int sVertCount;

static int sMode = PRESENT_GPU;
static int sReady;
static int sPending;            /* a verified frame is on the panel */
static struct gpu_verdict sVerdict = { 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

/* ------------------------------------------------------------------ */
/* The mode file                                                       */
/* ------------------------------------------------------------------ */

static int mode_read(void)
{
    FILE *f = fopen(PRESENT_PATH, "r");
    char word[16];
    int mode = PRESENT_GPU;

    if (f == NULL) {
        return PRESENT_GPU;
    }
    if (fscanf(f, "%15s", word) == 1) {
        if (strcmp(word, "gpu") == 0) {
            mode = PRESENT_GPU;
        } else if (strcmp(word, "cpu") == 0) {
            mode = PRESENT_CPU;
        } else if (strcmp(word, "verify") == 0) {
            mode = PRESENT_VERIFY;
        } else {
            fprintf(stderr, "3ds-gpu: %s says \"%s\", which is not "
                            "cpu/gpu/verify, using gpu\n",
                    PRESENT_PATH, word);
        }
    }
    fclose(f);
    return mode;
}

int gpu_mode(void)
{
    return sMode;
}

const char *gpu_mode_name(void)
{
    switch (sMode) {
    case PRESENT_CPU:    return "cpu";
    case PRESENT_VERIFY: return "verify";
    default:             return "gpu";
    }
}

int gpu_ready(void)
{
    return sReady;
}

/* ------------------------------------------------------------------ */
/* Start-up                                                            */
/* ------------------------------------------------------------------ */

/*
 * The font sheet, drawn with view_text() into a display-shaped scratch buffer
 * and then read back out of it with view_get(). Both functions are the CPU
 * path's own, so a glyph here is the glyph the fault screen draws, including
 * the lowercase fold and the hollow box for a character the font is missing.
 */
static uint8_t sAtlasScratch[ATLAS_W * ATLAS_H * 3];

static void atlas_build(uint32_t *out)
{
    int cell;
    int x, y;

    view_clear(sAtlasScratch, ATLAS_W, ATLAS_H, 0x00000000u);

    for (cell = 0; cell < ATLAS_CELLS; cell++) {
        char one[2];

        /* The last cell is deliberately a code point the font has no glyph
         * for, so the box it draws is the box this atlas hands out. */
        one[0] = (char)(cell == ATLAS_MISSING ? 0x7F : 0x20 + cell);
        one[1] = '\0';
        view_text(sAtlasScratch, ATLAS_W, ATLAS_H,
                  (cell % ATLAS_COLS) * ATLAS_CELL,
                  (cell / ATLAS_COLS) * ATLAS_CELL,
                  one, VIEW_COLOUR_TEXT);
    }

    for (y = 0; y < ATLAS_H; y++) {
        for (x = 0; x < ATLAS_W; x++) {
            uint32_t px = view_get(sAtlasScratch, ATLAS_W, ATLAS_H, x, y);

            /* Ink where the font drew, and a fully transparent texel
             * everywhere else, the alpha test throws those away. */
            out[y * ATLAS_W + x] = px != 0 ? ((px << 8) | 0xFFu) : 0;
        }
    }
}

/*
 * THE 2D render state, in a function, because the 3D rasterizer changes all of it. The 3D
 * layer's pass binds a different vertex program, a different attribute layout
 * and a different buffer, and turns on depth, culling and blending, so this
 * has to be re-applied before either screen is drawn, or the compositor draws
 * its quads through the polygon shader and shows nothing recognisable.
 *
 * The viewport is NOT here: C3D_FrameDrawOn() sets it from the target it is
 * given, so each screen gets its own without asking.
 */
static void gpu_state_2d(void)
{
    C3D_TexEnv *env;

    C3D_BindProgram(&sProgram);
    {
        C3D_AttrInfo *attr = C3D_GetAttrInfo();

        AttrInfo_Init(attr);
        AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);   /* v0 = position */
        AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);   /* v1 = texcoord */
    }
    if (sVerts != NULL) {
        C3D_BufInfo *buf = C3D_GetBufInfo();

        BufInfo_Init(buf);
        BufInfo_Add(buf, sVerts, sizeof(Vertex), 2, 0x10);
    }

    /* One stage, and the texel is the pixel. */
    env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

    /* No depth, no culling, no blending: see the header of this file. The
     * alpha test is what makes the font's empty texels empty. */
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    /*
     * Source times one, destination times zero: the blend unit runs and
     * computes the source, exactly, in integers. GPU_LOGICOP_COPY would say
     * the same thing more directly and it is not what this asks for, the
     * emulator in this port's gate translates the PICA to OpenGL and logic
     * ops are one of the corners it is known to be thin on, while this pair
     * of factors is what every 2D program on the console already uses.
     */
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO,
                   GPU_ONE, GPU_ZERO);
    C3D_AlphaTest(true, GPU_GREATER, 0x80);
}

static void upload_tiled(C3D_Tex *tex, const uint32_t *linear, int w, int h)
{
    GSPGPU_FlushDataCache((void *)linear, (u32)(w * h * 4));
    C3D_SyncDisplayTransfer((u32 *)linear, GX_BUFFER_DIM(w, h),
                            (u32 *)tex->data, GX_BUFFER_DIM(w, h),
                            TEXTURE_TRANSFER_FLAGS);
}

static int screen_init(struct screen *s, gfxScreen_t which, int width)
{
    s->which = which;
    s->width = width;

    /*
     * The target's own axes are the framebuffer's: 240 across, 400 or 320
     * down. Mtx_OrthoTilt is what turns the display coordinates this file
     * builds quads in into that.
     *
     * A stencil buffer, which is the effect pass's and not a depth test. Nothing here is
     * sorted by depth and nothing ever will be; the DS's layer order is the
     * draw order. What the stencil carries is the window unit's six-bit mask
     * and one more bit saying whether the pixel under this one came from a
     * layer BLDCNT admits as a blend's second operand, which is the question
     * the DS's blend unit asks and the only per-pixel state a compositor of
     * quads has nowhere else to put. There is no stencil-only format on this
     * silicon, so the depth half comes along at four bytes a pixel and is
     * never tested.
     *
     * A target without one is not fatal: gpu2d_survey() refuses the frames
     * that would need it and they stay in software, which is what this build
     * did before the effect pass.
     */
    s->target = C3D_RenderTargetCreate(VIEW_SCREEN_HEIGHT, width,
                                       GPU_RB_RGBA8,
                                       GPU_RB_DEPTH24_STENCIL8);
    if (s->target == NULL) {
        s->target = C3D_RenderTargetCreate(VIEW_SCREEN_HEIGHT, width,
                                           GPU_RB_RGBA8, -1);
        if (s->target == NULL) {
            return -1;
        }
        fprintf(stderr, "3ds-gpu: no VRAM for a stencil buffer, windows "
                        "and blends stay in software\n");
    } else {
        s->stencil = 1;
    }
    C3D_RenderTargetSetOutput(s->target, which, GFX_LEFT,
                              DISPLAY_TRANSFER_FLAGS);
    Mtx_OrthoTilt(&s->proj, 0.0f, (float)width, (float)VIEW_SCREEN_HEIGHT,
                  0.0f, 1.0f, -1.0f, true);

    if (!C3D_TexInitVRAM(&s->tex, TEX_SIDE, TEX_SIDE, GPU_RGBA8)) {
        return -1;
    }
    C3D_TexSetFilter(&s->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&s->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    s->stage = (uint32_t *)linearAlloc(VIEW_DS_WIDTH * VIEW_DS_HEIGHT * 4);
    if (s->stage == NULL) {
        return -1;
    }
    return 0;
}

int gpu_init(void)
{
    uint32_t *atlas;
    int i;

    sMode = mode_read();
    if (sMode == PRESENT_CPU) {
        fprintf(stderr, "3ds-gpu: %s asked for the CPU blit\n", PRESENT_PATH);
        return -1;
    }

    /*
     * Four times the default command buffer, because overflowing it is not an
     * Error you get to see. Libctru's GPUCMD_AddRawCommands answers a full
     * buffer with svcBreak, which kills the process without reaching this
     * port's fault screen, a black window and no report. The 3D pixel work's edge
     * marking draws the opaque geometry eight more times a frame, and the
     * default 256 KB is not enough for that. It costs 768 KB of linear memory
     * on a machine with 64 MB, and `gpu-cmdbuf` in the report is what says how
     * close any of it comes.
     */
    if (!C3D_Init(4 * C3D_DEFAULT_CMDBUF_SIZE)) {
        fprintf(stderr, "3ds-gpu: C3D_Init failed; the CPU blit stays\n");
        return -1;
    }

    if (screen_init(&sScreen[0], GFX_TOP, VIEW_TOP_WIDTH) != 0
        || screen_init(&sScreen[1], GFX_BOTTOM, VIEW_BOTTOM_WIDTH) != 0) {
        fprintf(stderr, "3ds-gpu: no render target or no VRAM for it\n");
        return -1;
    }

    sDvlb = DVLB_ParseFile((u32 *)present_shbin,
                           (u32)(present_shbin_end - present_shbin));
    if (sDvlb == NULL) {
        fprintf(stderr, "3ds-gpu: the shader did not parse\n");
        return -1;
    }
    shaderProgramInit(&sProgram);
    shaderProgramSetVsh(&sProgram, &sDvlb->DVLE[0]);
    C3D_BindProgram(&sProgram);
    sUniProjection = shaderInstanceGetUniformLocation(sProgram.vertexShader,
                                                      "projection");

    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);   /* v0 = position */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);   /* v1 = texcoord */

    sVerts = (Vertex *)linearAlloc(sizeof(Vertex) * GPU_MAX_VERTS);
    if (sVerts == NULL) {
        fprintf(stderr, "3ds-gpu: no linear memory for the vertex buffer\n");
        return -1;
    }
    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, sVerts, sizeof(Vertex), 2, 0x10);

    gpu_state_2d();

    if (!C3D_TexInitVRAM(&sAtlas, ATLAS_W, ATLAS_H, GPU_RGBA8)) {
        fprintf(stderr, "3ds-gpu: no VRAM for the font\n");
        return -1;
    }
    C3D_TexSetFilter(&sAtlas, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sAtlas, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    atlas = (uint32_t *)linearAlloc(ATLAS_W * ATLAS_H * 4);
    if (atlas == NULL) {
        fprintf(stderr, "3ds-gpu: no linear memory for the font\n");
        return -1;
    }
    atlas_build(atlas);
    upload_tiled(&sAtlas, atlas, ATLAS_W, ATLAS_H);
    linearFree(atlas);

    /*
     * The background path's atlas, and the pool the tile cache expands into is this texture's
     * own linear memory: a tile is 64 contiguous texels in PICA swizzle order
     * and a block of this texture is the same 64, so an expansion is
     * already where the GPU reads it and no transfer runs at all.
     *
     * A failure here is not fatal. gpu2d_init() refuses the pool, the survey
     * claims no engine and every frame is composed in software, which is the
     * behaviour this build had before this task.
     */
    if (!C3D_TexInit(&sAtlas2D, ATLAS2D_W, ATLAS2D_H, GPU_RGBA8)) {
        fprintf(stderr, "3ds-gpu: no linear memory for the tile atlas, "
                        "the software 2D engine stays\n");
    } else {
        C3D_TexSetFilter(&sAtlas2D, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sAtlas2D, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        if (gpu2d_init((uint32_t *)sAtlas2D.data, ATLAS2D_SLOTS,
                       ATLAS2D_W) != 0) {
            fprintf(stderr, "3ds-gpu: the tile cache refused the atlas\n");
        }
        /* The effect pass: both panels or neither, because an engine can feed either. */
        gpu2d_set_stencil(sScreen[0].stencil && sScreen[1].stencil);
        fprintf(stderr, "3ds-gpu: backgrounds are %s\n", gpu2d_mode_name());
    }

    /*
     * A failure here is not fatal either: layer3d_init() refuses, the survey
     * never gets a layer, and every frame with 3D on it is composed in
     * software; which is the behaviour this build had before the 3D layer.
     */
    if (!C3D_TexInit(&sLayer3D, TEX_SIDE, TEX_SIDE, GPU_RGBA8)) {
        fprintf(stderr, "3ds-gpu: no linear memory for the 3D layer, "
                        "frames with 3D on them stay in software\n");
    } else {
        C3D_TexSetFilter(&sLayer3D, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sLayer3D, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        if (layer3d_init((uint32_t *)sLayer3D.data, TEX_SIDE, TEX_SIDE) != 0) {
            fprintf(stderr, "3ds-gpu: the 3D layer refused its texture\n");
        } else {
            fprintf(stderr, "3ds-gpu: the 3D layer is %s\n",
                    layer3d_mode_name());
        }
    }

    /*
     * The display-mode work's VRAM display. Same size as the 3D layer: 256x256 RGBA8 in linear
     * memory, packed in swizzle order so a scan of the bank is a CPU write
     * and no transfer. Without it, mode 2 stays in software.
     */
    if (!C3D_TexInit(&sVramTex, TEX_SIDE, TEX_SIDE, GPU_RGBA8)) {
        fprintf(stderr, "3ds-gpu: no linear memory for VRAM display, "
                        "those frames stay in software\n");
    } else {
        C3D_TexSetFilter(&sVramTex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sVramTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        gpu2d_set_vramtex((uint32_t *)sVramTex.data, TEX_SIDE);
    }

    if (sMode == PRESENT_VERIFY) {
        /*
         * One framebuffer per screen while verifying, and that is what makes
         * the comparison possible at all: with the pair libctru hands out,
         * the buffer citro3d transferred into is swapped away by a callback
         * on another thread and this thread cannot say which of the two it
         * was. A single buffer is torn on a slow frame and exactly one
         * address, which is the trade a measurement wants.
         */
        for (i = 0; i < 2; i++) {
            struct screen *s = &sScreen[i];

            gfxSetDoubleBuffering(s->which, false);
            s->fbBytes = (size_t)s->width * VIEW_SCREEN_HEIGHT * 3;
            s->cpu = (uint8_t *)malloc(s->fbBytes);
            if (s->cpu == NULL) {
                fprintf(stderr, "3ds-gpu: no room to verify\n");
                return -1;
            }
        }
    }

    sReady = 1;
    fprintf(stderr, "3ds-gpu: citro3d is up, present mode %s\n",
            gpu_mode_name());
    return 0;
}

/* ------------------------------------------------------------------ */
/* One frame                                                           */
/* ------------------------------------------------------------------ */

struct gpu_vertex *gpu_vertex_alloc(int count)
{
    Vertex *v;

    if (sVerts == NULL || sVertCount + count > GPU_MAX_VERTS) {
        return NULL;
    }
    v = sVerts + sVertCount;
    sVertCount += count;
    return v;
}

int gpu_vertex_count(void)
{
    return sVertCount;
}

void gpu_cache_flush(const void *p, size_t bytes)
{
    GSPGPU_FlushDataCache((void *)p, (u32)bytes);
}

static void quad(float x, float y, float w, float h,
                 float u0, float v0, float u1, float v1)
{
    Vertex *v = gpu_vertex_alloc(6);

    if (v == NULL) {
        return;
    }

    /* Two triangles, wound either way: culling is off. z is anywhere inside
     * the clip volume, because the depth test is off as well. */
    v[0] = (Vertex){ x,     y,     0.5f, u0, v0 };
    v[1] = (Vertex){ x,     y + h, 0.5f, u0, v1 };
    v[2] = (Vertex){ x + w, y + h, 0.5f, u1, v1 };
    v[3] = (Vertex){ x,     y,     0.5f, u0, v0 };
    v[4] = (Vertex){ x + w, y + h, 0.5f, u1, v1 };
    v[5] = (Vertex){ x + w, y,     0.5f, u1, v0 };
}

/*
 * One letterbox line, a quad per character. Placement is view_text()'s:
 * VIEW_GLYPH_ADVANCE across per character, and the line stops at the edge of
 * the panel rather than wrapping.
 */
static void text_quads(int x, int y, const char *s)
{
    int drawn;

    if (s == NULL) {
        return;
    }
    for (drawn = 0; *s != '\0' && drawn < GPU_TEXT_MAX;
         s++, drawn++, x += VIEW_GLYPH_ADVANCE) {
        unsigned char c = (unsigned char)*s;
        int cell = (c >= 0x20 && c <= 0x7E) ? (int)c - 0x20 : ATLAS_MISSING;
        float u = (float)((cell % ATLAS_COLS) * ATLAS_CELL) / (float)ATLAS_W;
        float v = (float)((cell / ATLAS_COLS) * ATLAS_CELL) / (float)ATLAS_H;

        quad((float)x, (float)y, (float)ATLAS_CELL, (float)ATLAS_CELL,
             u, v,
             u + (float)ATLAS_CELL / (float)ATLAS_W,
             v + (float)ATLAS_CELL / (float)ATLAS_H);
    }
}

static void screen_build(struct screen *s, const uint32_t *px,
                         const char *lineTop, const char *lineBottom)
{
    int ox = VIEW_ORIGIN_X(s->width);
    int oy = VIEW_ORIGIN_Y(VIEW_SCREEN_HEIGHT);

    /*
     * The picture quad, for a screen the software renderer composed. A screen
     * The background path draws has no picture quad at all: its first draw is the backdrop,
     * and gpu2d_build() emits that itself, since the effect pass landed the backdrop is a
     * layer the blend unit can name, not a clear.
     */
    s->picBase = sVertCount;
    if (px != NULL && !s->tiles) {
        quad((float)ox, (float)oy, (float)VIEW_DS_WIDTH,
             (float)VIEW_DS_HEIGHT, 0.0f, TEX_V_TOP, 1.0f, TEX_V_BOTTOM);
    }
    s->picCount = sVertCount - s->picBase;

    if (s->tiles) {
        /*
         * Timed into PERF_BLIT rather than into PERF_FLUSH beside the other
         * vertices: this is what replaces the surface copy on a screen the background path
         * draws, and the two paths' numbers are only comparable if the same
         * work is under the same name.
         */
        u64 mark = svcGetSystemTick();
        int built = gpu2d_build(s->engine, sFrameCounter, (float)ox,
                                (float)oy);
        u64 spent = svcGetSystemTick() - mark;

        perf_phase(PERF_BLIT, spent);
        sBuildTicks += spent;
        if (built < 0) {
            /*
             * The atlas or the vertex buffer ran out after the software
             * renderer had already been told to skip this engine, so the
             * surface behind it is the frame before's. Counted rather than
             * hidden: 3ds_gpu2d.c's own counter is what says this ever
             * happened; it is in the frame-time report as
             * bg-build-failed, and PRESENT_VERIFY is what would say the
             * picture moved.
             */
            s->tiles = 0;
            s->picCount = 0;
        } else {
            s->exact = gpu2d_exact(s->engine);
            s->l3d = layer3d_mode() == L3D_PICA
                     && gpu2d_layer3d(s->engine);
        }
    }

    s->txtBase = sVertCount;
    text_quads(ox, VIEW_TEXT_TOP_Y, lineTop);
    text_quads(ox, VIEW_TEXT_BOTTOM_Y, lineBottom);
    s->txtCount = sVertCount - s->txtBase;
}

/*
 * One of the DS's sixteenths as one of the PICA's 255ths, and this is where
 * The effect pass stops being exact. A blend coefficient is k/16 on hardware and k'/255
 * here, and 16/255 is not 1/16, so the two disagree by a fraction of a step
 * on most inputs, and the DS then rounds its six-bit result where the PICA
 * rounds its eight-bit one. The endpoints are exact (0 and 16 give 0 and 255)
 * and the middle is within a step. Everything else in this task avoids the
 * blend unit for exactly this reason; the alpha blend is the one effect that
 * cannot, because its second operand is another layer's pixel.
 */
static inline u32 coefficient(unsigned k)
{
    return ((u32)k * 255u + 8u) / 16u;
}

/*
 * One draw out of the list. The effect pass turned the compositor's inner loop inside
 * out: what to bind, what to test and what to blend is 3ds_gpu2d.c's answer
 * and not this file's, because the reasons a frame needs three draws instead
 * of one are all that file's, a window, a blended layer, a backdrop that is
 * a colour rather than a texel.
 *
 * The state is set per draw and not left behind, and both C3D_GetTexEnv()
 * calls are there for the reason the backdrop's comment gave first: taking
 * the pointer once and writing through it after a draw changes the struct in
 * this process and nothing on the GPU.
 */
static void draw_one(const struct bg_draw *d)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);

    if (d->count <= 0) {
        return;
    }

    if (d->colour) {
        /*
         * A constant colour, which is what the backdrop and the fade are.
         * C3D_TexEnvColor takes 0xAABBGGRR and the port's texels are
         * 0xRRGGBBAA; the same four bytes read from the other end, because
         * the PICA writes a colour most significant channel first and reads a
         * constant least significant channel first.
         */
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
        C3D_TexEnvColor(env, ((d->constant & 0xFF000000u) >> 24)
                             | ((d->constant & 0x00FF0000u) >> 8)
                             | ((d->constant & 0x0000FF00u) << 8)
                             | ((d->constant & 0x000000FFu) << 24));
    } else if (d->tex == BG_TEX_NONE) {
        /* The window rectangles: stencil only. The source does not matter and
         * the alpha has to pass the test, so it is the constant white. */
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, 0, 0);
        C3D_TexEnvColor(env, 0xFFFFFFFFu);
    } else {
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
        if (d->tex == BG_TEX_LAYER3D) {
            /* The 3D rasterizer draws into a render target of its own, so the layer's
             * texels are not always in the pool gpu_init() handed over. */
            C3D_Tex *l3d = (C3D_Tex *)layer3d_texture();

            C3D_TexBind(0, l3d != NULL ? l3d : &sLayer3D);
        } else if (d->tex == BG_TEX_VRAM) {
            C3D_TexBind(0, &sVramTex);
        } else {
            C3D_TexBind(0, &sAtlas2D);
        }
    }

    /*
     * Any alpha at all is the 3D layer, which is not the threshold the rest
     * Of the frame uses. A tile's texel is opaque or index zero, so 0x80
     * splits it; the 3D layer's alpha says how translucent the pixel is, and a
     * 3D pixel at 8/31 is still a pixel that is THERE.
     */
    C3D_AlphaTest(true, GPU_GREATER, d->tex == BG_TEX_LAYER3D ? 0 : 0x80);

    /*
     * THE STENCIL, and the write mask is what keeps the two things in it
     * apart: a layer writes bit 7 and leaves the window mask alone. The test
     * is (ref & test) == (stencil & test), so one EQUAL with a two-bit mask
     * asks "this layer may draw here AND what it covers is a target-2 layer".
     */
    C3D_StencilTest(d->test != 0 || d->write != 0,
                    d->test != 0 ? GPU_EQUAL : GPU_ALWAYS,
                    d->ref, d->test, d->write);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP,
                  d->write != 0 ? GPU_STENCIL_REPLACE : GPU_STENCIL_KEEP);
    /* A draw with no colour of its own writes the stencil and nothing else. */
    C3D_DepthTest(false, GPU_ALWAYS,
                  d->nocolour || (d->tex == BG_TEX_NONE && !d->colour)
                      ? (GPU_WRITEMASK)0 : GPU_WRITE_COLOR);

    switch (d->blend) {
    case BG_BLEND_ALPHA:
        /*
         * Eva and evb as two independent coefficients out of one register:
         * The source's factor is the constant colour and the destination's is
         * the constant alpha, so a blend the DS writes as sixteenths is one
         * blend register here. Sixteenths are not representable in eighths of
         * a byte (16/255 is not 1/16), which is why this is the one part of
         * the effect pass a byte compare cannot close on. See 3ds_effect.h.
         */
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                       GPU_CONSTANT_COLOR, GPU_CONSTANT_ALPHA,
                       GPU_ONE, GPU_ZERO);
        C3D_BlendingColor(coefficient(d->eva) * 0x00010101u
                          | (coefficient(d->evb) << 24));
        break;
    case BG_BLEND_FADE:
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                       GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA,
                       GPU_ONE, GPU_ZERO);
        C3D_BlendingColor(coefficient(d->eva) << 24);
        break;
    default:
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO,
                       GPU_ONE, GPU_ZERO);
        break;
    }

    C3D_DrawArrays(GPU_TRIANGLES, d->base, d->count);
}

static void screen_draw(struct screen *s)
{
    C3D_RenderTargetClear(s->target, C3D_CLEAR_COLOR, CLEAR_COLOUR, 0);
    C3D_FrameDrawOn(s->target);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sUniProjection, &s->proj);

    if (s->tiles) {
        const struct bg_draw *list;
        int n = gpu2d_drawlist(s->engine, &list);
        int i;

        for (i = 0; i < n; i++) {
            draw_one(&list[i]);
        }
        /* ...and back to the state the text below is drawn under, which is
         * also the state every other caller of this file expects. */
        gpu_state_2d();
    } else if (s->picCount > 0) {
        C3D_TexBind(0, &s->tex);
        C3D_DrawArrays(GPU_TRIANGLES, s->picBase, s->picCount);
    }
    if (s->txtCount > 0) {
        C3D_TexBind(0, &sAtlas);
        C3D_DrawArrays(GPU_TRIANGLES, s->txtBase, s->txtCount);
    }
}

/* ------------------------------------------------------------------ */
/* PRESENT_VERIFY                                                      */
/* ------------------------------------------------------------------ */

/*
 * The framebuffer the GPU wrote, against the framebuffer the CPU blit would
 * have written for the same frame. Both are GSP_BGR8_OES in display order, so
 * the comparison is a memcmp and the first disagreement is turned back into a
 * coordinate with the layout view_put() uses.
 */
/*
 * The first frame the tile path got wrong, as two pictures on the SD card:
 * what the software renderer composed and what the PICA left in the
 * framebuffer. A count and a coordinate say a frame is wrong; only the two
 * pictures say HOW, a cell mirrored, a picture shifted, a palette swapped
 * and a wrong corner of the atlas all produce the same first coordinate.
 *
 * De-rotated on the way out, because the panels are mounted at ninety degrees
 * and nothing that reads a PPM knows that.
 */
static void dump_ppm(const char *path, const uint8_t *fb, int width)
{
    FILE *f = sd_open_write(path);
    int x, y;

    if (f == NULL) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", width, VIEW_SCREEN_HEIGHT);
    for (y = 0; y < VIEW_SCREEN_HEIGHT; y++) {
        for (x = 0; x < width; x++) {
            size_t i = ((size_t)x * VIEW_SCREEN_HEIGHT
                        + (size_t)(VIEW_SCREEN_HEIGHT - 1 - y)) * 3u;
            unsigned char rgb[3];

            /* The framebuffer is GSP_BGR8_OES: blue first. */
            rgb[0] = fb[i + 2];
            rgb[1] = fb[i + 1];
            rgb[2] = fb[i];
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static void verify_compare(struct screen *s, int index)
{
    size_t i;
    int differs = 0;

    GSPGPU_InvalidateDataCache(s->fb, (u32)s->fbBytes);

    for (i = 0; i < s->fbBytes; i += 3) {
        if (s->fb[i] != s->cpu[i] || s->fb[i + 1] != s->cpu[i + 1]
            || s->fb[i + 2] != s->cpu[i + 2]) {
            sVerdict.pixels++;
            if (!differs) {
                differs = 1;
            }
            /*
             * On a frame the blend unit ran on, HOW FAR apart is the question
             * and not whether: see struct gpu_verdict. The gap is per channel
             * and the worst one over the run is what the verdict is read
             * against.
             *
             * A frame with a pica-rasterized 3D layer is the outer class of
             * the three and takes precedence over the blend: it can differ by
             * a whole pixel rather than by a rounding, so folding one into the
             * other's total would put an unbounded number inside a bounded
             * claim.
             */
            if (s->l3dShown || !s->exactShown) {
                unsigned long *pixels = s->l3dShown ? &sVerdict.l3d_pixels
                                                    : &sVerdict.approx_pixels;
                int *worst = s->l3dShown ? &sVerdict.l3d_worst
                                         : &sVerdict.approx_worst;
                int c;

                (*pixels)++;
                for (c = 0; c < 3; c++) {
                    int a = s->fb[i + (size_t)c];
                    int b = s->cpu[i + (size_t)c];
                    int gap = a > b ? a - b : b - a;

                    if (gap > *worst) {
                        *worst = gap;
                    }
                }
                continue;
            }
            if (s->tilesShown && !sVerdict.tile_first) {
                size_t px = i / 3;

                sVerdict.tile_first = 1;
                sVerdict.tile_x = (int)(px / VIEW_SCREEN_HEIGHT);
                sVerdict.tile_y = VIEW_SCREEN_HEIGHT - 1
                                  - (int)(px % VIEW_SCREEN_HEIGHT);
                sVerdict.tile_want = (uint32_t)s->cpu[i]
                                     | ((uint32_t)s->cpu[i + 1] << 8)
                                     | ((uint32_t)s->cpu[i + 2] << 16);
                sVerdict.tile_got = (uint32_t)s->fb[i]
                                    | ((uint32_t)s->fb[i + 1] << 8)
                                    | ((uint32_t)s->fb[i + 2] << 16);
                dump_ppm(PRESENT_WANT_PATH, s->cpu, s->width);
                dump_ppm(PRESENT_GOT_PATH, s->fb, s->width);
            }
            if (sVerdict.first_screen < 0) {
                size_t px = i / 3;

                sVerdict.first_screen = index;
                sVerdict.first_tiles = s->tilesShown;
                sVerdict.first_x = (int)(px / VIEW_SCREEN_HEIGHT);
                sVerdict.first_y = VIEW_SCREEN_HEIGHT - 1
                                   - (int)(px % VIEW_SCREEN_HEIGHT);
                sVerdict.first_want = (uint32_t)s->cpu[i]
                                      | ((uint32_t)s->cpu[i + 1] << 8)
                                      | ((uint32_t)s->cpu[i + 2] << 16);
                sVerdict.first_got = (uint32_t)s->fb[i]
                                     | ((uint32_t)s->fb[i + 1] << 8)
                                     | ((uint32_t)s->fb[i + 2] << 16);
            }
        }
    }

    sVerdict.frames++;
    if (!differs) {
        sVerdict.equal++;
    }
    if (s->l3dShown) {
        sVerdict.l3d_frames++;
        if (!differs) {
            sVerdict.l3d_equal++;
        }
    } else if (s->exactShown) {
        sVerdict.exact_frames++;
        if (!differs) {
            sVerdict.exact_equal++;
        }
    } else {
        sVerdict.approx_frames++;
    }
    if (s->tilesShown) {
        sVerdict.tile_frames++;
        if (!differs) {
            sVerdict.tile_equal++;
        }
    }
}

void gpu_verdict_get(struct gpu_verdict *out)
{
    *out = sVerdict;
}

int gpu_write_report(const char *path)
{
    FILE *f;

    if (sMode != PRESENT_VERIFY) {
        return 0;
    }
    f = sd_open_write(path);
    if (f == NULL) {
        return -1;
    }
    /*
     * Two claims, one verdict, and a third kind of frame that is not yet a
     * CLAIM. Every frame the PICA composed without doing arithmetic of its own
     * has to be byte-identical, which is what the GPU present, the background path and the 3D layer closed on and
     * what a regression in any of them would break. Every frame that ran
     * BLDCNT's alpha blend has to be within one six-bit step, because the PICA
     * cannot be exact there and a bound is the strongest true thing to say.
     * Either one failing fails the run.
     *
     * A frame whose 3D layer the PICA rasterized is REPORTED and not judged,
     * because what it should be judged against is what the 3D rasterizer is measuring.
     * It counts neither for nor against the verdict, and it is subtracted from
     * the other two rather than added to them, so a run in `3d.txt = pica`
     * still says whether everything ELSE regressed, which is the question this
     * report was built to answer.
     */
    fprintf(f, "verdict %s\n",
            sVerdict.frames == 0
                ? "NONE"
                : ((sVerdict.exact_equal == sVerdict.exact_frames
                    && sVerdict.approx_worst <= PRESENT_SLACK) ? "PASS"
                                                               : "FAIL"));
    fprintf(f, "mode %s\n", gpu_mode_name());
    fprintf(f, "frames %lu\n", sVerdict.frames);
    fprintf(f, "identical %lu\n", sVerdict.equal);
    fprintf(f, "tile-frames %lu\n", sVerdict.tile_frames);
    fprintf(f, "tile-identical %lu\n", sVerdict.tile_equal);
    fprintf(f, "differing-pixels %lu\n", sVerdict.pixels);
    fprintf(f, "exact-frames %lu\n", sVerdict.exact_frames);
    fprintf(f, "exact-identical %lu\n", sVerdict.exact_equal);
    fprintf(f, "approx-frames %lu\n", sVerdict.approx_frames);
    fprintf(f, "approx-pixels %lu\n", sVerdict.approx_pixels);
    fprintf(f, "approx-worst %d of %d allowed\n", sVerdict.approx_worst,
            PRESENT_SLACK);
    if (sVerdict.l3d_frames != 0) {
        fprintf(f, "l3d-frames %lu\n", sVerdict.l3d_frames);
        fprintf(f, "l3d-identical %lu\n", sVerdict.l3d_equal);
        fprintf(f, "l3d-pixels %lu\n", sVerdict.l3d_pixels);
        fprintf(f, "l3d-worst %d\n", sVerdict.l3d_worst);
    }
    if (sVerdict.tile_first) {
        fprintf(f, "tile-first %d,%d want %06lX got %06lX\n",
                sVerdict.tile_x, sVerdict.tile_y,
                (unsigned long)sVerdict.tile_want,
                (unsigned long)sVerdict.tile_got);
    }
    if (sVerdict.first_screen >= 0) {
        fprintf(f, "first %s %s %d,%d want %06lX got %06lX\n",
                sVerdict.first_tiles ? "tiles" : "surface",
                sVerdict.first_screen == 0 ? "upper" : "lower",
                sVerdict.first_x, sVerdict.first_y,
                (unsigned long)sVerdict.first_want,
                (unsigned long)sVerdict.first_got);
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* The present                                                         */
/* ------------------------------------------------------------------ */

void gpu_present(const uint32_t *top, const uint32_t *bottom,
                 const char *lineTop, const char *lineBottom, int upper)
{
    const uint32_t *px[2];
    u64 mark;
    int i;

    if (!sReady) {
        view_present(top, bottom, lineTop, lineBottom);
        return;
    }

    px[0] = top;
    px[1] = bottom;
    sFrameCounter++;
    mark = svcGetSystemTick();

    /*
     * Which engine feeds which panel is POWCNT1's DSEL bit and therefore the
     * game's decision; it is read once in 3ds_frame.c and handed down, rather
     * than read again here, so the surfaces and the tiles cannot disagree
     * about it inside one frame.
     */
    for (i = 0; i < 2; i++) {
        sScreen[i].engine = i == 0 ? (upper & 1) : ((upper & 1) ^ 1);
        sScreen[i].tiles = gpu2d_draws(sScreen[i].engine);
    }

    /*
     * The surfaces first, and outside the frame on purpose: C3D_SyncDisplayTransfer
     * waits for the previous frame's queue before it runs, so by the time both
     * uploads are done the last frame is on the panel and the textures are
     * nobody's to read.
     */
    for (i = 0; i < 2; i++) {
        struct screen *s = &sScreen[i];
        int n;

        /* A screen the background path draws has no surface to copy: the software renderer
         * was not run for that engine and the pixels never existed. */
        if (px[i] == NULL || s->tiles) {
            continue;
        }
        for (n = 0; n < VIEW_DS_WIDTH * VIEW_DS_HEIGHT; n++) {
            s->stage[n] = (px[i][n] << 8) | 0xFFu;
        }
        upload_tiled(&s->tex, s->stage, VIEW_DS_WIDTH, VIEW_DS_HEIGHT);
    }

    perf_phase(PERF_BLIT, svcGetSystemTick() - mark);
    mark = svcGetSystemTick();

    sVertCount = 0;
    sBuildTicks = 0;
    for (i = 0; i < 2; i++) {
        screen_build(&sScreen[i], px[i], lineTop, lineBottom);
    }
    /* The tiles the background path expanded this frame, out of the data cache in one pass
     * once both screens have asked for all of them. */
    gpu2d_flush();
    /* ...and the 3D layer, packed back at survey time and not touched since. */
    layer3d_flush();
    /*
     * The vertices are written with the CPU and read by the GPU, so they have
     * to leave the data cache. C3D_FrameEnd would flush the whole linear heap
     * for us (which is 32 MB on this console) so the frame is ended with
     * GX_CMDLIST_FLUSH instead and this is the flush that replaces it.
     */
    GSPGPU_FlushDataCache(sVerts, (u32)(sizeof(Vertex) * (size_t)sVertCount));

    if (!C3D_FrameBegin(0)) {
        return;
    }

    if (sMode == PRESENT_VERIFY && sPending) {
        for (i = 0; i < 2; i++) {
            verify_compare(&sScreen[i], i);
        }
        if (sVerdict.frames % PRESENT_REPORT_EVERY == 0) {
            (void)gpu_write_report(PRESENT_REPORT_PATH);
        }
    }

    /*
     * The 3D rasterizer's producer draws HERE and not at survey time: it needs the frame
     * open, and it has to land before either screen samples the layer. A no-op
     * for the software packer, whose texels were written outside the frame.
     */
    layer3d_render();
    /*
     * And a barrier, which is the one line this whole path turns on. The pass
     * above renders INTO a texture that the two screens below then SAMPLE, and
     * citro3d does not split the command list when the framebuffer changes,
     * C3D_SetFrameBuf only flags the state, and the only split is the one
     * C3D_FrameEnd does. So without this the layer's draws and the compositor's
     * reads of the same texels sit in one list with no cache invalidate between
     * them, and the texture unit is free to serve what it had before.
     *
     * It is invisible on the emulator and fatal on the console, which is why
     * it survived a bisect: Azahar serialises and models no texture cache, so
     * its picture was correct throughout while hardware showed a frame that was
     * right for about a second and garbage afterwards, the pipeline filling.
     * Turning textures off in the 3D pass changed nothing because the hazard is
     * on the 2D side, reading the layer, which that knob never touched.
     */
    if (!layer3d_no_split()) {
        C3D_FrameSplit(GX_CMDLIST_FLUSH);
    }
    /* ...and put back everything that pass changed. */
    gpu_state_2d();

    for (i = 0; i < 2; i++) {
        screen_draw(&sScreen[i]);
    }
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    gpu2d_end_frame();
    {
        u64 spent = svcGetSystemTick() - mark;

        perf_phase(PERF_FLUSH, spent > sBuildTicks ? spent - sBuildTicks : 0);
    }

    if (sMode == PRESENT_VERIFY) {
        /*
         * What the blit would have written, for the frame just handed to the
         * GPU. It is compared at the top of the next present, because that is
         * where this thread knows the transfer has landed.
         */
        for (i = 0; i < 2; i++) {
            struct screen *s = &sScreen[i];

            s->fb = gfxGetFramebuffer(s->which, GFX_LEFT, NULL, NULL);
            s->tilesShown = s->tiles;
            s->exactShown = !s->tiles || s->exact;
            s->l3dShown = s->tiles && s->l3d;
            view_compose(s->cpu, s->width, VIEW_SCREEN_HEIGHT, px[i],
                         lineTop, lineBottom);
        }
        sPending = 1;
    }
}

void gpu_exit(void)
{
    if (!sReady) {
        return;
    }
    sReady = 0;
    (void)gpu_write_report(PRESENT_REPORT_PATH);
    C3D_Fini();
}
