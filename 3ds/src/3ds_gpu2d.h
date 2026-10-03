/*
 * 3ds/src/3ds_gpu2d.h: the DS 2D engines, on the PICA.
 *
 * pc/hw/pc_gpu2d.c composes both screens a scanline at a time on the CPU and
 * that is 101.7 ms a frame on this console (the survey, measured). This file is the
 * other renderer: the same guest state, read the same way, turned into quads
 * the console's own GPU draws.
 *
 * It is not a second renderer in the sense of a rewrite. What it renders is a
 * SUBSET, and the software renderer stays in the binary as the oracle and as
 * the path every frame outside that subset still takes. The background path's subset is text
 * backgrounds; the sprite path added the OBJ layer (one atlas quad per sprite tile);
 * The display-mode work added forced blank, display-off and VRAM display, and capture that
 * does not need a 2D compose. Affine backgrounds, bitmap sprites and the
 * display FIFO stay in software.
 *
 * Which frames are in the subset is a question with a number, so this file
 * answers it before it draws anything. gpu2d_survey() reads the same registers
 * the software renderer's engine_begin() reads and records, per engine and per
 * frame, every reason the GPU path could not take that frame. The counters
 * land in the perf report beside the frame times. A renderer for a subset
 * nothing is in would be a fast path that never runs, and this is what tells
 * those apart before the code exists rather than after.
 *
 * Per frame, not per scanline, and that is not an approximation here. The
 * software renderer reads DISPCNT, the four BGxCNTs, the scroll registers and
 * the blend registers ONCE in engine_begin() and reuses them for all 192
 * lines, only the affine reference points and the two mosaic latches move
 * between lines. So a snapshot taken once a frame is exactly the state the
 * oracle drew from, and a game that changes a register at HBlank is already
 * drawn without that change by both paths. Whatever that costs in accuracy
 * against real hardware, it costs both renderers equally, which is what keeps
 * the byte comparison in PRESENT_VERIFY meaningful.
 */

#ifndef POKEPLATINUM_3DS_GPU2D_H
#define POKEPLATINUM_3DS_GPU2D_H

#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Why an engine-frame is not on the GPU path                          */
/* ------------------------------------------------------------------ */
/*
 * One bit per reason, and a frame collects all of them rather than the first:
 * "the OBJ layer is what keeps the field off the GPU" is only a finding if the
 * frames it names are not also held back by three other things. The names are
 * the report's own strings, in BG_REASONS below.
 */
enum {
    BG_R_OFF      = 1u << 0,   /* POWCNT or the engine's own enable      */
    BG_R_BLANK    = 1u << 1,   /* unused since 11.8: a blank is a fill   */
    BG_R_DISPMODE = 1u << 2,   /* display FIFO, or VRAM display with no
                                * texture to scan the bank into          */
    BG_R_BGMODE   = 1u << 3,   /* BG mode is not 0: an affine layer      */
    BG_R_3D       = 1u << 4,   /* BG0 is the 3D engine and there is no
                                * texture of it this frame (the 3D rasterizer)         */
    BG_R_OBJ      = 1u << 5,   /* a sprite this path does not draw       */
    /*
     * The effect pass took the next three down to what it could not do rather than what
     * it could. A window is a reason only when the OBJ window is on, that
     * one is a per-pixel mask the sprite unit draws, so it is the sprite path's, or
     * when there is no stencil buffer to put the mask in. An effect is a
     * reason only when it is the alpha blend and two enabled layers are its
     * targets, or when a window would run it on part of the screen only.
     * MASTER_BRIGHT is no longer a reason at all: it folds into the palettes,
     * or into a pass of its own on a frame that blends after it. The bits
     * stay numbered as they were so a report from an older run still reads.
     */
    BG_R_WINDOW   = 1u << 6,   /* the OBJ window, or no stencil (11.6)   */
    BG_R_EFFECT   = 1u << 7,   /* a blend this path cannot reproduce     */
    BG_R_MOSAIC   = 1u << 8,   /* a drawn layer has mosaic on (11.6)     */
    BG_R_BRIGHT   = 1u << 9,   /* ...never fires since 11.6              */
    BG_R_CAPTURE  = 1u << 10,  /* capture needs a 2D compose this path
                                * did not draw (the display-mode work)   */
    /*
     * The 3D layer has a translucent pixel on it AND BLDCNT admits the layer
     * underneath as a second operand, so the DS would blend the two, in six
     * bits, with its own rounding, and whatever BLDCNT's effect field says
     * (color_composite()). The 3D layer draws the layer; it does not blend it.
     */
    BG_R_3DBLEND  = 1u << 11
};

#define BG_R_COUNT 12

/* ------------------------------------------------------------------ */
/* One engine's frame                                                  */
/* ------------------------------------------------------------------ */
/*
 * The subset of pc_gpu2d.c's `struct engine` the GPU path needs, read from
 * the same addresses by the same rules. Affine BG reference points are still
 * missing; those frames stay in software as BG_R_BGMODE. The OBJ layer is
 * here since the sprite path: the sprite unit is a walk of OAM, not a scanline buffer.
 */
struct bg_frame {
    int num;                    /* 0 = engine A, 1 = engine B          */
    uint32_t io;                /* 0x04000000 or 0x04001000            */
    uint32_t dispcnt;           /* already masked for engine B         */
    uint16_t bgcnt[4];
    uint16_t hofs[4], vofs[4];
    uint32_t vram_base, vram_mask;
    uint32_t obj_base, obj_mask;
    uint32_t oam;               /* 0x07000000 or 0x07000400            */
    uint32_t palbase;           /* 0x000 for A, 0x400 for B            */
    uint16_t backdrop;          /* palette entry 0, the cleared colour */
    unsigned enable;            /* DISPCNT bits 8-11, the four layers  */
    int obj;                    /* a colour sprite this path can draw  */
    /*
     * The effect pass's registers, snapshotted here with the rest for the reason the
     * header's opening comment gives: the software renderer reads them once in
     * engine_begin() and draws all 192 lines from that, so a second read at
     * build time could see a different frame's blend.
     *
     * BLDCNT is masked to its 14 bits and the three coefficients are clamped
     * to 16 as hardware clamps them at the write, which is what makes 0x1F and
     * 0x10 the same fade rather than a seventeenth step (engine_begin).
     */
    uint16_t blendcnt;
    uint8_t eva, evb, evy;
    uint16_t master;            /* MASTER_BRIGHT, raw                  */
    uint16_t winh[2], winv[2];  /* WIN0H/WIN1H, WIN0V/WIN1V            */
    uint8_t wincnt[4];          /* WININ's two bytes, then WINOUT's    */
    unsigned reasons;           /* 0 means this engine-frame is ours   */
};

/*
 * Both engines, read once. Call it before the software renderer runs for the
 * same frame: it takes no locks, changes no guest state and costs about a
 * hundred halfword reads, which is nothing against a frame either renderer
 * spends 100 ms in.
 *
 * Returns the number of engines whose `reasons` came back zero.
 */
int gpu2d_survey(struct bg_frame out[2]);

/* The survey's running totals, for the perf report. */
void gpu2d_report(FILE *f);

/*
 * Engine-frames the survey has found drawable since the run began, whatever
 * mode is selected. The frame-time report samples it per window, which is what
 * lets two runs be compared over the SAME SCENE: the counter moves when the
 * game is showing something this renderer covers, so a window with it at zero
 * is a scene neither run drew on the GPU and does not belong in the
 * comparison.
 */
unsigned long gpu2d_eligible_total(void);

/*
 * Engine-frames the software renderer was skipped for since the run began.
 * The frame-time report needs it because its per-window digest is taken from
 * pc_video's surfaces: an engine the PICA composed never wrote one, so the
 * digest of that window describes the last frame the software renderer did
 * compose and not what was on the panel. A window with this moving has no
 * comparable digest, and the report says so by printing zero rather than a
 * plausible number.
 */
unsigned long gpu2d_claimed_total(void);

/* ------------------------------------------------------------------ */
/* The renderer                                                        */
/* ------------------------------------------------------------------ */
/*
 * Which path draws the backgrounds, read once at start-up from
 * sdmc:/3ds/pokeplatinum/bg.txt, the same convention present.txt already
 * carries, and a separate file because the two knobs are separate axes: what
 * *composes* the picture and what *presents* it.
 *
 *   gpu    an engine-frame with nothing in the way (see BG_R_* above) is
 *          composed by the PICA out of tiles, and the software renderer is
 *          not run for that engine at all, the ship path since the console
 *          was asked
 *   soft   pc/hw/pc_gpu2d.c composes every frame, as it did through the sound work;
 *          the oracle, and what every frame outside the subset still takes
 *
 * GPU is the default because a console said so (2026-08-19). On an Old 3DS,
 * over the main menu: the software 2D engine 60.33 -> 14.17 ms and the frame
 * 307.71 -> 271.35, with PRESENT_VERIFY comparing 2,168 screens there,
 * 440 of them composed out of tiles, and finding zero differing pixels.
 * An unreadable or absent file is `gpu`; a file naming something else is
 * refused with a line on stderr rather than quietly taking one of the two.
 *
 * What shipping this costs: pc_video's surfaces are no longer written for an
 * engine the PICA composed, so the frame-time report's per-window digest is
 * not comparable on those windows and says zero (see gpu2d_claimed_total),
 * and a frame-parity run against the PC port has to select `soft`.
 */
enum {
    BG_SOFT = 0,
    BG_GPU
};

#define BG_PATH "sdmc:/3ds/pokeplatinum/bg.txt"

int gpu2d_mode(void);
const char *gpu2d_mode_name(void);

/*
 * The atlas pool. `slots` tiles of 64 texels, which must be the linear-memory
 * side of a PICA texture whose 8x8 blocks are in raster order, slot n is
 * block n, because tile_expand*() writes swizzle order and a block is 64
 * contiguous texels. `width` is that texture's width in texels, which
 * is what turns a slot number into texture coordinates.
 */
int gpu2d_init(uint32_t *pool, int slots, int width);

/*
 * The texture DISPCNT's VRAM display mode is packed into. Same shape as
 * The 3D layer's layer: 8x8 blocks in raster order, Morton inside a block, sides a
 * power of two at least 256. Without one, mode 2 stays in software.
 */
void gpu2d_set_vramtex(uint32_t *pool, int side);
const uint32_t *gpu2d_vram_tex(void);
int gpu2d_vram_side(void);

/*
 * The capture unit, for an engine-A frame this path claimed. Software already
 * ran it when the engine was not claimed. A no-op if bit 31 is clear.
 */
void gpu2d_capture(void);

/*
 * Whether the render targets have a stencil buffer under them, which is what
 * The effect pass's window mask and its "what is under this pixel" test are carried in.
 * Told rather than asked, because the answer is one C3D_RenderTargetCreate
 * away in 3ds_gpu.c and this file has no business calling citro3d. Without it
 * the frames that need either stay in software.
 */
void gpu2d_set_stencil(int have);

/*
 * Two questions, and they are not the same one in verify mode.
 *
 *   gpu2d_draws()   the PICA composes this engine out of tiles this frame
 *   gpu2d_claims()  ...and the software renderer is therefore not run for it
 *
 * They differ under PRESENT_VERIFY, which composes every frame both ways and
 * compares them byte for byte: there the GPU draws the tiles AND the software
 * renderer runs, because its surface is the answer being compared against.
 * A claim without a draw never happens; a draw without a claim is the whole
 * of how this task is checked.
 */
int gpu2d_draws(int engine);
int gpu2d_claims(int engine);

/*
 * Expand what this engine's frame needs and emit its quads, in the draw order
 * draw_scanline() composes in: priority 3 first and BG0 last within a
 * priority, so the last quad over a pixel is the layer the DS would have left
 * there. (ox, oy) is where the picture's top left corner sits on the panel.
 *
 * Returns the number of quads, or -1 if the vertex buffer or the atlas ran
 * out; which the caller must treat as "this engine is not drawable this
 * frame" rather than as a partial picture.
 *
 * The backdrop is the first quad it emits, since the effect pass: it is a layer like any
 * other to the blend unit, BLDCNT names it as a target with a bit of its own,
 * and the pixel a translucent layer blends with is the backdrop wherever
 * nothing else drew.
 */
int gpu2d_build(int engine, unsigned frame, float ox, float oy);

/* ------------------------------------------------------------------ */
/* One draw                                                            */
/* ------------------------------------------------------------------ */
/*
 * Why this is a list and not a range. The background path could put every quad of an
 * engine-frame into one call, because every one of them came out of the same
 * texture under the same state; the 3D layer split it into three, because the 3D
 * layer is a quad out of a second texture at the position BG0's priority puts
 * it. The effect pass has three reasons to split it again and none of them is a texture:
 * A window makes the stencil test per layer, a blended layer draws once per
 * side of the "is what I cover a target-2 layer" test, and the backdrop is a
 * constant colour rather than a texel.
 *
 * So the compositor is handed the draws themselves, in order, and 3ds_gpu.c
 * walks them without knowing which of the three reasons produced any one.
 * Two draws may share one vertex range, the blended and unblended halves of
 * a layer are the same quads under different state, and emitting them twice
 * would double the vertex buffer for nothing.
 */
enum {
    BG_BLEND_NONE = 0,  /* source times one: the picture is the last quad   */
    BG_BLEND_ALPHA,     /* BLDCNT's effect 1, by EVA and EVB                */
    /*
     * MASTER_BRIGHT over the finished picture, towards the draw's own
     * constant colour by EVY sixteenths. Only on a frame whose fade could not
     * be folded into the palettes; see gpu2d_build().
     */
    BG_BLEND_FADE
};

enum {
    BG_TEX_ATLAS = 0,   /* 11.3's tile atlas                            */
    BG_TEX_LAYER3D,     /* 11.7's texture of the 3D layer               */
    BG_TEX_VRAM,        /* 11.8: a bank scanned as RGBA8                */
    BG_TEX_NONE         /* a constant colour, or no colour written at all */
};

struct bg_draw {
    int base, count;    /* the vertex range, which the next draw may repeat */
    uint8_t tex;        /* BG_TEX_*                                         */
    /*
     * The stencil, which carries the window mask in bits 0-5 and, in bit 7,
     * whether the pixel under this one came from a layer BLDCNT admits as a
     * blend's second operand. `test` is the input mask and 0 means no test;
     * `ref` is what it is compared against; `write` is the write mask and 0
     * means this draw leaves the stencil alone.
     *
     * No draw both tests and writes one bit, which is a rule and not a
     * coincidence. The PICA writes the same reference it compares, so a draw
     * that passes where a bit is set cannot leave that bit clear, and a
     * blended layer's two halves are exactly that pair, one passing where the
     * target-2 bit is set and one where it is clear. Whichever of them wrote
     * the bit would move pixels into or out of the other's test. So they
     * write nothing, and a third draw over the same vertices with no colour
     * marks the layer afterwards (gpu2d_build).
     */
    uint8_t ref, test, write;
    uint8_t nocolour;   /* the stencil only: this draw leaves no pixel */
    uint8_t blend;      /* BG_BLEND_*                                       */
    /* BG_BLEND_ALPHA's two coefficients, and BG_BLEND_FADE's one in `eva`. */
    uint8_t eva, evb;
    uint8_t colour;     /* 1 = draw `constant` rather than a texture         */
    uint32_t constant;  /* 0xRRGGBBAA                                        */
};

/* The stencil's own bits, so the compositor and the test name them the same
 * way this file does. */
#define BG_ST_WIN   0x3Fu   /* the window unit's six-bit mask               */
#define BG_ST_EFF   0x20u   /* ...bit 5 of it, "the colour effect may run"  */
#define BG_ST_T2    0x80u   /* the pixel here came from a target-2 layer    */

/* This engine's draws, in order. Returns how many. */
int gpu2d_drawlist(int engine, const struct bg_draw **out);

/*
 * Whether this frame's draws are all copies, no blend unit, no fade pass;
 * which is what says the picture can still be held to a byte compare. The
 * verify path counts the two kinds of frame apart because a PICA blend cannot
 * be the DS's to the byte (3ds_effect.h), and a bound is the strongest true
 * claim over one.
 */
int gpu2d_exact(int engine);

/*
 * Whether this frame's draws include the 3D layer; which, when `3d.txt` says
 * `pica`, is the one thing in the picture the PICA RASTERIZED rather than
 * copied. The 3D rasterizer: that layer cannot be byte-identical to the software
 * rasterizer's and the reason is arithmetic the DS does and a PICA does not,
 * so a frame carrying one is a third kind of frame in the verify path, held to
 * neither the byte compare nor the effect pass's one-step bound.
 */
int gpu2d_layer3d(int engine);


/* Push the tiles expanded this frame out of the data cache, once, after every
 * engine has been built and before anything is drawn. */
void gpu2d_flush(void);

/*
 * The frame is submitted, so the survey's answer has been used up: the next
 * present draws tiles only if a survey ran for it. A frame presented without
 * one is not hypothetical, the diagnostic loop in 3ds_main.c presents
 * frames the game's renderer never saw, and drawing that frame from the last
 * snapshot would put a menu on a screen nobody composed.
 */
void gpu2d_end_frame(void);

/* Self-test, the same shape every model in this port carries. */
int gpu2d_selftest(int *ranOut);

#endif /* POKEPLATINUM_3DS_GPU2D_H */
