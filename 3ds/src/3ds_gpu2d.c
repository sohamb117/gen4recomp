/*
 * 3ds/src/3ds_gpu2d.c: see 3ds_gpu2d.h.
 *
 * The survey is first in this file and it was first in the task. The survey spent a
 * day proving that the cheap wins everybody expects are not the ones a profile
 * names, and the lesson it left is that a renderer states its subset in
 * numbers before it writes a line of GPU code. The background path drew a frame only if it
 * had no sprite on it; the sprite path takes that reason away for the sprites this path
 * can emit as atlas quads. The display-mode work takes forced blank, display-off and VRAM
 * display, and capture that does not need a 2D compose. Bitmap sprites,
 * mosaic sprites, semi-transparent ones (OAM mode 1), affine backgrounds and
 * the display FIFO still refuse; they are counted, not approximated.
 *
 * The registers are read the way the software renderer reads them, through the
 * same memoised translation and with the same masks, engine B's DISPCNT
 * write mask included, because half the fields this file tests do not exist on
 * that engine and reading them as if they did would put engine B in a subset
 * it is not in. Where a value's meaning is subtle the comment names
 * pc/hw/pc_gpu2d.c's own line, because that file is the oracle and a
 * disagreement between the two is a bug in this one.
 */

#include "3ds_gpu2d.h"

#include <stdio.h>
#include <string.h>

#include "armrec_rt.h"

#include "3ds_effect.h"
#include "3ds_gpu.h"
#include "3ds_hostmap.h"
#include "3ds_layer3d.h"
#include "3ds_tile.h"
#include "3ds_view.h"

/* ------------------------------------------------------------------ */
/* Guest state, read where it is                                       */
/* ------------------------------------------------------------------ */
/*
 * The same addresses pc_gpu2d.c names, and the same names for them. Repeated
 * here rather than shared: that file is on the DS SDK's include chain and this
 * one is on the port's, which is the same reason 3ds_view.c does not include
 * pc_video.h either.
 */
#define IO_A      0x04000000u
#define IO_B      0x04001000u
#define PAL_BASE  0x05000000u
#define PAL_MASK  0x000007FFu
#define OAM_BASE  0x07000000u
#define ABG_BASE  0x06000000u
#define ABG_MASK  0x0007FFFFu
#define BBG_BASE  0x06200000u
#define BBG_MASK  0x0001FFFFu
#define AOBJ_BASE 0x06400000u
#define AOBJ_MASK 0x0003FFFFu
#define BOBJ_BASE 0x06600000u
#define BOBJ_MASK 0x0001FFFFu
#define POWCNT1   0x04000304u
#define DISPCAPCNT      0x04000064u
#define DISPCAPCNT_MASK 0xEF3F1F1Fu

static inline uint16_t rd16(uint32_t a)
{
    return *(const volatile uint16_t *)hostmap_ptr(a);
}

static inline uint32_t rd32(uint32_t a)
{
    return *(const volatile uint32_t *)hostmap_ptr(a);
}

static inline void wr32(uint32_t a, uint32_t v)
{
    *(volatile uint32_t *)hostmap_ptr(a) = v;
}

static uint32_t dispmode_of(const struct bg_frame *f)
{
    return f->num ? ((f->dispcnt >> 16) & 1u) : ((f->dispcnt >> 16) & 3u);
}

/* ------------------------------------------------------------------ */
/* State, all of it                                                    */
/* ------------------------------------------------------------------ */

/* The texture the expanded tiles live in, as slots. Set by gpu2d_init(). */
static uint32_t *sPool;
static int sPoolSlots;
static int sAtlasWidth;
/*
 * One texel, in texture coordinates, and TWO of them, because the atlas is
 * 1024 by 512 and a single step would be right along one axis and half of what
 * it should be along the other. A square atlas hides this completely, which is
 * why the check in 3ds/tests/gpu2d_render.c uses one that is not square.
 */
static float sStepU, sStepV;

/*
 * Which slots were expanded this frame, so the flush is a few hundred bytes
 * rather than the whole megabyte. A frame that overflows this list flushes the
 * pool entire, which is correct and slow, and only happens on the first
 * frames of a scene, when nothing is cached yet.
 */
#define BG_DIRTY_MAX 512
static int sDirty[BG_DIRTY_MAX];
static int sDirtyN;
static int sDirtyAll;

/*
 * The 15-bit palettes this frame has already converted, keyed on the host
 * pointer they were read from. Cleared every frame: the tile cache tracks
 * guest writes by generation, but this is a table of colours in this process
 * and nothing would tell it that the halfwords behind it had moved.
 *
 * Two sizes because the two depths are not the same shape. 4bpp is what this
 * game asks for nearly everywhere (510 of 545 layer setups) so the small
 * table is the one with room to spare.
 */
#define BG_PAL4_SLOTS 32
#define BG_PAL8_SLOTS 8

/*
 * `key` is the palette's contents, folded down to a word: it is what the tile
 * cache is keyed on, because a palette's ADDRESS does not change when this
 * game fades one. See 3ds_tile.h.
 */
static struct {
    const void *src;
    uint32_t xform;             /* effect_id(): 11.6's transform, 0 for none */
    uint32_t key;
    uint32_t tab[TILE_PAL4];
} sPal4[BG_PAL4_SLOTS];
static int sPal4N;

static struct {
    const void *src;
    uint32_t xform;
    uint32_t key;
    uint32_t tab[TILE_PAL8];
} sPal8[BG_PAL8_SLOTS];
static int sPal8N;

/* An extended-palette slot with no bank behind it: the halfwords a read of an
 * unmapped bank gives, so it goes through the same conversion every other
 * palette does and cannot drift from it. */
static const uint16_t sZeroPal[TILE_PAL8];

static struct bg_frame sFrame[2];
static int sDraw[2], sClaim[2];
/*
 * The draws gpu2d_build() left, per engine. The effect pass asked for nine; the sprite path adds
 * one range per BG-relative priority, each of those able to split the way a
 * blended background does. The build refuses the frame rather than overrunning
 * the list, the same way it refuses a full vertex buffer.
 */
#define BG_DRAW_MAX 32
static struct bg_draw sDrawList[2][BG_DRAW_MAX];
static int sDrawN[2];
static int sMode = BG_SOFT;
/*
 * Whether the render targets have a stencil buffer under them. The effect pass needs one
 * for the window mask and for the "what is under this pixel" test a blend
 * turns on; without it those frames stay in software, which is the behaviour
 * this build had before the effect pass. gpu_init() says so once.
 */
static int sStencil;

/*
 * DISPCNT mode 2's texture: a 256x256 (or larger) RGBA8 pool in PICA swizzle
 * order. Set by gpu2d_set_vramtex(); without one, mode 2 stays BG_R_DISPMODE.
 */
static uint32_t *sVramPool;
static int sVramSide;
static uint8_t sVramSwizzle[TILE_TEXELS];
static int sVramSwizzleReady;
static int sVramDirty;

/* ------------------------------------------------------------------ */
/* Is a sprite actually on the screen, and can this path draw it       */
/* ------------------------------------------------------------------ */
/*
 * DISPCNT's OBJ bit is not the question. It is set for most of this game and
 * says only that the layer is enabled; what decides whether the software
 * renderer puts a sprite pixel down is whether any of the 128 attributes is
 * both undisabled and overlapping the 256x192 picture. Asking the cheaper
 * question would have blamed the sprite path for frames the background path could already draw.
 *
 * The sprite path draws a colour sprite as atlas quads. Three OAM modes stay in
 * software, because they are not a quad of tiles:
 *
 *   mode 3  a bitmap sprite, a halfword a texel, not a tile
 *   mode 1  semi-transparent: blends even when OBJ is not a target-1 layer
 *   mosaic  when MOSAIC's OBJ nibbles are non-zero and the sprite asks
 *
 * OBJ-window sprites contribute a mask rather than a picture; they are the sprite path's
 * only when DISPCNT bit 15 is on, and that bit is already BG_R_WINDOW. With
 * the bit off they are skipped here and do not keep the frame in software.
 *
 * The two tests are draw_sprites()' own, lifted rather than approximated:
 *
 *   Vertically, a sprite is visible on line L when ((L - Y) & 0xFF) < H, so
 *   its lines are [y, y + H) modulo 256 and the picture is [0, 192). Those
 *   miss only when Y is at or past 192 AND the run does not wrap past 256;
 *   which is exactly the sprite parked below the screen that does not come
 *   back round the top.
 *
 *   Horizontally, X is nine bits signed, so a sprite at 400 is at -112 and its
 *   right edge is what is on screen.
 *
 * The size tables are the hardware's 16 entries: shape 3 is not a shape and
 * all four of its sizes are 8x8.
 */
static const int32_t kSprW[16] = {
     8, 16,  8, 8,
    16, 32,  8, 8,
    32, 32, 16, 8,
    64, 64, 32, 8
};
static const int32_t kSprH[16] = {
     8,  8, 16, 8,
    16,  8, 32, 8,
    32, 16, 32, 8,
    64, 32, 64, 8
};

static unsigned long sObjLive;      /* colour sprites this path can draw */
static unsigned long sObjHard;      /* engine-frames a sprite still refuses */
static unsigned long sObj1d, sObj2d, sObj4, sObj8;
static unsigned long sObjRot, sObjBmp, sObjWin, sObjMos, sObjSemi, sObjWrap;

struct obj_kind {
    int live;                       /* a colour sprite is on screen      */
    int hard;                       /* one of them is a mode we skip     */
};

static int sprite_on_screen(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (y >= 192 && y + h <= 256) {
        return 0;
    }
    if (x <= -w || x >= 256) {
        return 0;
    }
    return 1;
}

static struct obj_kind survey_obj(int num, uint32_t dispcnt, uint16_t mosaic)
{
    struct obj_kind k = { 0, 0 };
    uint32_t oam = OAM_BASE + (num ? 0x400u : 0x000u);
    int mos = ((mosaic >> 8) & 0x0F) != 0 || ((mosaic >> 12) & 0x0F) != 0;
    int map1d = (dispcnt & (1u << 4)) != 0;
    int n;

    for (n = 0; n < 128; n++) {
        uint16_t a0 = rd16(oam + (uint32_t)n * 8u + 0u);
        uint16_t a1 = rd16(oam + (uint32_t)n * 8u + 2u);
        uint32_t sprtype = ((uint32_t)a0 >> 8) & 3u;
        uint32_t mode = ((uint32_t)a0 >> 10) & 3u;
        uint32_t sizeparam;
        int32_t w, h, x, y;
        int iswin, is8, wrap;

        if (sprtype == 2u) {
            continue;
        }
        sizeparam = ((uint32_t)a0 >> 14) | (((uint32_t)a1 & 0xC000u) >> 12);
        w = kSprW[sizeparam];
        h = kSprH[sizeparam];
        if (sprtype == 3u) {
            w <<= 1;
            h <<= 1;
        }
        y = (int32_t)(a0 & 0xFFu);
        x = (int32_t)(((uint32_t)a1 & 0x1FFu) ^ 0x100u) - 0x100;
        if (!sprite_on_screen(x, y, w, h)) {
            continue;
        }
        iswin = mode == 2u;
        is8 = (a0 & (1u << 13)) != 0;
        wrap = y + h > 256;
        if (iswin) {
            sObjWin++;
            continue;
        }
        k.live = 1;
        sObjLive++;
        if (sprtype & 1u) {
            sObjRot++;
        }
        if (is8) {
            sObj8++;
        } else {
            sObj4++;
        }
        if (map1d) {
            sObj1d++;
        } else {
            sObj2d++;
        }
        if (wrap) {
            sObjWrap++;
        }
        if (mode == 3u) {
            sObjBmp++;
            k.hard = 1;
        } else if (mode == 1u) {
            sObjSemi++;
            k.hard = 1;
        } else if ((a0 & (1u << 12)) && mos) {
            sObjMos++;
            k.hard = 1;
        }
    }
    return k;
}

/* ------------------------------------------------------------------ */
/* The effect pass's plan for one frame's colours                      */
/* ------------------------------------------------------------------ */
/*
 * The three units are one predicate because they are not independent. What a
 * colour effect does to a layer depends on the window's effect bit; whether
 * the master fade can be folded into a palette depends on whether a blend runs
 * after it; and whether the blend runs at all depends on which layers are
 * enabled, not only on which ones BLDCNT names.
 *
 * The survey is why this is shaped the way it is. Over the new-game replay
 * this game asks for exactly three BLDCNTs on the frames the effect pass is for: 0x0842
 * (BG1 blended over BG3) on 836 engine-frames, 0x0C43 (BG0 and BG1 over BG2
 * and BG3) on 262 where NEITHER target-1 layer is enabled, and 0x0000 on 100
 * that are here only for the fade. So the alpha blend is the whole of the
 * work, the brightness effects never fire, and a fifth of the refusals were a
 * predicate that read BLDCNT's effect field without asking whether it had
 * anything to apply itself to.
 */
struct bg_colour {
    unsigned effect;    /* EFFECT_*, resolved: NONE when it cannot fire  */
    unsigned t1;        /* the layer flags BLDCNT names as target 1...   */
    unsigned t2;        /* ...and as target 2                            */
    unsigned windows;   /* DISPCNT bits 13-14, the two rectangles        */
    int blend;          /* the GPU's blend unit runs on the target-1 layer */
    int fold;           /* the master fade goes into the palettes        */
    int fade;           /* ...or into a pass of its own, after the blend */
};

/* What the predicate made of this frame, kept because gpu2d_build() needs it
 * again once the frame has been accepted, and re-deriving it there would be
 * a second reading of registers the game may have moved since. */
static struct bg_colour sPlan[2];

/*
 * Fill it in, and return the reasons this frame is not the GPU path's. `f`
 * carries the effect pass's registers; `blendcnt` and `master` are passed in so the
 * self-test can put the unit in a state without a frame around it.
 */
static unsigned bg_colour_plan(const struct bg_frame *f, uint16_t blendcnt,
                               uint16_t master, unsigned live,
                               struct bg_colour *p)
{
    unsigned why = 0;
    unsigned mode = (unsigned)(master >> 14);
    unsigned eff;
    int fade;

    p->windows = (f->dispcnt >> 13) & 3u;
    p->effect = EFF_NONE;
    p->t1 = blendcnt & live;
    p->t2 = (blendcnt >> 8) & 0x3Fu;
    p->blend = 0;
    p->fold = 1;
    p->fade = 0;

    /*
     * The OBJ window is a per-pixel mask the sprite unit draws, so it is
     * The sprite path's and not this task's; the other two are rectangles and go into the
     * stencil. Without a stencil buffer under the target neither can be drawn
     * at all.
     */
    if (f->dispcnt & 0x8000u) {
        why |= BG_R_WINDOW;
    } else if (p->windows != 0u && !sStencil) {
        why |= BG_R_WINDOW;
    }

    eff = (blendcnt >> 6) & 3u;
    /*
     * An effect with nothing to apply itself to is the identity, whatever the
     * field says: color_composite_at() reaches its switch with coloreffect
     * still zero unless BLDCNT names the layer that produced the top pixel,
     * and a layer DISPCNT has not enabled produces none. This game leaves
     * 0x0C43 in the register on 262 engine-frames with neither BG0 nor BG1 on
     * screen.
     */
    if (eff != 0u && p->t1 != 0u) {
        /*
         * ...and the window's effect bit, which is bit 5 of whichever region
         * a pixel is in. All the regions in play agreeing is what makes the
         * effect a property of the frame rather than of the pixel: they
         * disagree and one layer needs two palettes, two sets of quads and a
         * stencil test between them, which is refused and counted rather than
         * approximated.
         */
        unsigned any = 0u, all = 0x20u;
        int w;

        if (p->windows == 0u) {
            p->effect = eff;            /* the unit is bypassed entirely */
        } else {
            any |= (unsigned)(f->wincnt[2] & 0x20u);
            all &= (unsigned)(f->wincnt[2] & 0x20u);
            for (w = 0; w < 2; w++) {
                if (p->windows & (1u << w)) {
                    any |= (unsigned)(f->wincnt[w] & 0x20u);
                    all &= (unsigned)(f->wincnt[w] & 0x20u);
                }
            }
            if (any != all) {
                why |= BG_R_EFFECT;
            } else if (all != 0u) {
                p->effect = eff;
            }
        }
    }

    if (p->effect == EFF_ALPHA) {
        /*
         * The one effect that is not A function of one colour, so it is the
         * one the GPU's blend unit has to run, and two things have to hold
         * for that to be the DS's answer.
         *
         * The second operand is whatever layer the target-1 layer covers, and
         * the DS blends only where BLDCNT admits that layer: a stencil bit
         * carries "what is here came from a target-2 layer" and the layer
         * draws once per side of it (3ds_gpu.c). No stencil, no blend.
         *
         * And exactly one enabled blender may be a target: with two, the
         * higher one would blend against a pixel the lower one had ALREADY
         * blended, where the DS blends against what it covered,
         * draw_pixel() keeps the covered pixel, not the composed one. This
         * game never asks for two, and a frame that did is refused rather
         * than drawn nearly right. OBJ is a blender since the sprite path, so a
         * background and a sprite both named as target 1 is the same case.
         *
         * The backdrop is not counted, because it can never be the blended
         * one: nothing is under it, so `blendcnt & target2` is zero for its
         * pixels and color_composite_at() calls that no effect at all.
         */
        unsigned blenders = p->t1 & (f->obj ? 0x1Fu : 0x0Fu);

        if (!sStencil || (blenders & (blenders - 1u)) != 0u) {
            why |= BG_R_EFFECT;
        } else {
            p->blend = 1;
        }
    }

    /*
     * MASTER_BRIGHT. Mode 3 does nothing and a factor of zero is the identity
     * in both directions, so a register that is merely non-zero is not by
     * itself a fade (master_bright()).
     */
    fade = (mode == 1u || mode == 2u) && (master & 0x1Fu) != 0u;
    if (fade) {
        /*
         * The fade is the last thing the DS does to a pixel, so it folds into
         * the palette a tile is expanded through, exactly, in six bits, on
         * the CPU, as long as nothing runs after it. A blend does run after
         * it, and blending two already-faded colours is not fading the blend
         * of them, so on those frames it becomes a pass of its own over the
         * finished picture and the arithmetic moves to the GPU with the rest
         * of that frame's.
         */
        p->fold = !p->blend;
        p->fade = p->blend;
    } else {
        p->fold = 0;
    }
    return why;
}

/*
 * One layer's palette transform: the blend-unit effect if BLDCNT names this
 * layer as a target and the effect is one of the two that read a single
 * colour, then the fade if it is being folded. `flag` is the layer's own bit:
 * 0x01 to 0x08 for the backgrounds and 0x20 for the backdrop, which is
 * color_composite_at()'s numbering.
 */
static struct effect layer_effect(const struct bg_frame *f,
                                  const struct bg_colour *p, unsigned flag)
{
    struct effect e;

    e.kind = EFF_NONE;
    e.evy = f->evy;
    e.master = p->fold ? f->master : 0u;
    if ((p->effect == EFF_UP || p->effect == EFF_DOWN)
        && (p->t1 & flag) != 0u) {
        e.kind = (uint8_t)p->effect;
    }
    return e;
}

/* ------------------------------------------------------------------ */
/* The predicate                                                       */
/* ------------------------------------------------------------------ */
/*
 * Pure: everything it needs is an argument, so the self-test can put an engine
 * in any state without guest memory under it. `f->dispcnt`, `f->bgcnt` and
 * `f->enable` are filled in by the caller because reading them is the part
 * that needs the translation.
 */
/*
 * Capture that still needs the software renderer: a FIFO source, a 3D source,
 * or source A as the 2D compose on a frame this path is not composing. A fill
 * (blank, display off, engine off) and source B alone are this path's; they
 * do not read a scanline the PICA never wrote.
 */
static int capture_needs_soft(const struct bg_frame *f, int engine_on,
                              uint32_t capcnt)
{
    uint32_t dispmode, src;
    int blank;

    if (f->num || (capcnt & 0x80000000u) == 0u) {
        return 0;
    }
    if ((capcnt & (1u << 25)) != 0u || (capcnt & (1u << 24)) != 0u) {
        return 1;
    }
    src = (capcnt >> 29) & 3u;
    if (src == 1u) {
        return 0;
    }
    dispmode = dispmode_of(f);
    blank = (f->dispcnt & (1u << 7)) != 0;
    if (!engine_on || blank || dispmode == 0u) {
        return 0;
    }
    return 1;
}

static unsigned bg_classify(const struct bg_frame *f, int engine_on,
                            uint32_t capcnt, uint16_t mosaic, uint16_t blendcnt,
                            uint16_t master, int sprites, unsigned layer3d,
                            struct bg_colour *planOut)
{
    unsigned why = 0;
    uint32_t mode = f->dispcnt & 7u;
    uint32_t dispmode = dispmode_of(f);
    int blank = (f->dispcnt & (1u << 7)) != 0;
    int compose;
    int b;

    if (!engine_on) {
        why |= BG_R_OFF;
    }
    /*
     * Mode 1 is the compose. 0 is a white fill MASTER_BRIGHT does not reach,
     * 2 is a VRAM bank scanned out, 3 is the main-memory FIFO this port has
     * not built. Blank is a white fill too, and software ignores it when the
     * display selector is not showing the compose; it fills, then replaces.
     */
    if (dispmode == 3u) {
        why |= BG_R_DISPMODE;
    } else if (dispmode == 2u && !f->num && sVramPool == NULL) {
        why |= BG_R_DISPMODE;
    }

    compose = engine_on && dispmode == 1u && !blank;
    if (compose) {
        /*
         * BG mode 0 is four text layers. Every other mode gives at least one
         * layer to the affine or extended path, and the ones this game uses
         * are counted as a whole frame here rather than per layer.
         */
        if (mode != 0u) {
            why |= BG_R_BGMODE;
        }
        /*
         * Engine A's BG0 can be the 3D engine's output, and the enable bit is
         * what decides whether that layer is drawn at all. `layer3d` is what
         * layer3d_produce() answered for this frame: without a texture
         * of the layer there is nothing to draw it with, and with one there is
         * still the blend to account for.
         *
         * The blend is the only part the 3D layer does not do. A 3D pixel whose alpha
         * is under 31 blends with the layer it covers whenever BLDCNT admits
         * that layer as a second operand, bits 8 to 13, any of them, because
         * the operand is whatever happens to be underneath, and it does so
         * WITHOUT consulting BLDCNT's effect field. The blend is six-bit with
         * its own rounding and the PICA's blend unit is eight-bit, so a frame
         * that needs one is not this path's until something reproduces it
         * exactly.
         */
        if (!f->num && (f->dispcnt & 8u) && (f->enable & 1u)) {
            if (!(layer3d & L3D_READY)) {
                why |= BG_R_3D;
            } else if ((layer3d & L3D_TRANSLUCENT) && (blendcnt & 0x3F00u)) {
                why |= BG_R_3DBLEND;
            }
        }
        if ((f->dispcnt & (1u << 12)) && sprites) {
            why |= BG_R_OBJ;
        }
        /*
         * The effect pass's three, as one plan: the window rectangles, BLDCNT's effect
         * and MASTER_BRIGHT. With no 3D pixel in the frame the two asymmetric
         * cases in color_composite() cannot arise, so the plan is a function
         * of the registers alone. OBJ is in `live` when the sprite path can draw a
         * colour sprite this frame (`f->obj`).
         */
        {
            struct bg_colour plan;
            unsigned live = (f->enable & 0x0Fu) | 0x20u;

            if (f->obj) {
                live |= 0x10u;
            }
            why |= bg_colour_plan(f, blendcnt, master, live, &plan);
            if (planOut != NULL) {
                *planOut = plan;
            }
        }
        /*
         * Mosaic, and only where it would be seen: the two halves are
         * separately conditioned (draw_bg_text), and BGxCNT bit 6 alone makes
         * a layer read the latched line even with the size nibbles at zero.
         */
        if ((mosaic & 0x00FFu) != 0u) {
            for (b = 0; b < 4; b++) {
                if ((f->enable & (1u << b)) && (f->bgcnt[b] & (1u << 6))) {
                    why |= BG_R_MOSAIC;
                }
            }
        }
    } else if (planOut != NULL) {
        memset(planOut, 0, sizeof *planOut);
    }
    if (capture_needs_soft(f, engine_on, capcnt)) {
        why |= BG_R_CAPTURE;
    }
    return why;
}

/* ------------------------------------------------------------------ */
/* The survey                                                          */
/* ------------------------------------------------------------------ */

static unsigned long sEngineFrames;
static unsigned long sEligible;
static unsigned long sDrawn;        /* engine-frames the PICA composed   */
static unsigned long sClaimed;      /* ...and the software renderer skipped */
static unsigned long sFailed;       /* ...and the ones it ran out during */
/* By cause, because "the build failed" is not a finding: [0] the atlas had no
 * slot for a tile this frame had not already asked for, [1] the vertex buffer
 * was full. */
static unsigned long sFailWhy[2];
/* Quads emitted, which is what a frame of this renderer costs the CPU: the map
 * read, the cache probe and six vertices, per cell. */
static unsigned long sQuads;
static unsigned long sReason[BG_R_COUNT];
/* Frames where the ONLY thing in the way was that one reason, the number
 * that says what a phase-11 task is worth, as against how often it fires. */
static unsigned long sSole[BG_R_COUNT];

static const char *const kReason[BG_R_COUNT] = {
    "off", "blank", "dispmode", "bgmode", "3d",
    "obj", "window", "effect", "mosaic", "bright", "capture", "3dblend"
};

/* ------------------------------------------------------------------ */
/* What the frames the effect pass is for actually ask for             */
/* ------------------------------------------------------------------ */
/*
 * The reason table above says a colour effect refuses 976 engine-frames and a
 * fade another 368. It does not say WHICH effect, and the three of them are
 * three different pieces of work: a brightness effect is a function of one
 * colour and folds into the palette a tile is expanded through, exactly; an
 * alpha blend is a function of two and needs the blend unit, which is eight
 * bits where the DS is six. So this counts, over the engine-frames where
 * The effect pass's own reasons are ALL that is in the way, what each of them was asked
 * for, and a frame is only in the total if handling all three would hand it
 * to the GPU path.
 */
#define BG_R_116 (BG_R_WINDOW | BG_R_EFFECT | BG_R_BRIGHT)

static unsigned long s116Total;
static unsigned long s116Effect[4];    /* BLDCNT bits 6-7: none/alpha/up/down */
static unsigned long s116Master[4];    /* MASTER_BRIGHT bits 14-15            */
static unsigned long s116Win[3];       /* WIN0 / WIN1 / the OBJ window        */
static unsigned long s116WinWrap;      /* ...with a rectangle that wraps      */
/* How many of the frame's enabled layers BLDCNT names as target 1, which is
 * how many of them a brightness effect changes the palette of. The backdrop
 * counts as one: it is a target like any other (color_composite_at). */
static unsigned long s116T1[6];
/* With an alpha blend, whether the second operand can only ever be the
 * backdrop, the one case where a blend is still a function of ONE varying
 * colour and folds into the palette like a brightness effect does. */
static unsigned long s116T2Backdrop;
static unsigned long s116T2Other;
/* The whole frame's colour pipeline is a per-palette-entry function, which is
 * the subset this task can draw with no arithmetic on the GPU at all and
 * therefore the subset a byte compare can still close on. */
static unsigned long s116Fold;
/*
 * The distinct BLDCNTs themselves, because the counts above say what KIND of
 * effect the game asks for and the design question is what it asks it OF: a
 * second operand naming every layer under the target is a test that is always
 * true and needs no stencil bit, and one naming a single layer is a test that
 * decides per pixel. Eight is more than this game has ever shown.
 */
#define BG_BLD_SEEN 8
static uint16_t s116Bld[BG_BLD_SEEN];
static unsigned long s116BldN[BG_BLD_SEEN];
static unsigned long s116BldLost;

/*
 * The count for one engine-frame, called only when the effect pass's reasons are the only
 * ones on it. `blendcnt` is already masked to 14 bits and `master` is raw.
 */
static void survey_116(const struct bg_frame *f, uint16_t blendcnt,
                       uint16_t master)
{
    unsigned effect = (blendcnt >> 6) & 3u;
    unsigned mode = (unsigned)(master >> 14);
    unsigned windows = (f->dispcnt >> 13) & 7u;
    unsigned t1 = 0;
    int b;

    if (mode == 3u || (master & 0x1Fu) == 0u) {
        mode = 0u;                  /* not a fade: see bg_classify() */
    }
    s116Total++;
    s116Effect[effect]++;
    s116Master[mode]++;
    for (b = 0; b < 3; b++) {
        if (windows & (1u << b)) {
            s116Win[b]++;
        }
    }
    for (b = 0; b < 2; b++) {
        /* X1 > X2 or Y1 > Y2 is a window that wraps round the edge of the
         * screen rather than an empty one, because neither latch is reset per
         * line (win_scan). Two rectangles, not one, if this ever fires. */
        if ((windows & (1u << b))
            && (((f->winh[b] >> 8) > (f->winh[b] & 0xFFu))
                || ((f->winv[b] >> 8) > (f->winv[b] & 0xFFu)))) {
            s116WinWrap++;
        }
    }
    for (b = 0; b < 4; b++) {
        if ((f->enable & (1u << b)) && (blendcnt & (1u << b))) {
            t1++;
        }
    }
    if (blendcnt & 0x20u) {
        t1++;                       /* the backdrop */
    }
    s116T1[t1 > 5u ? 5u : t1]++;
    if (effect == 1u) {
        if ((blendcnt & 0x3F00u) == 0x2000u) {
            s116T2Backdrop++;
        } else {
            s116T2Other++;
        }
    }
    /*
     * Foldable: every colour the frame shows is some source colour put through
     * one function of that colour alone. A brightness effect and a fade are
     * both that; an alpha blend is not, unless the only second operand it can
     * ever have is the backdrop, whose colour is a constant for the frame.
     * A window makes the effect's own bit vary across the screen, which does
     * not stop the fold; it makes the layer need two palettes and two draws,
     * which is a cost and not an inexactness, so it is not tested here.
     */
    if (effect != 1u || (blendcnt & 0x3F00u) == 0x2000u) {
        s116Fold++;
    }
    for (b = 0; b < BG_BLD_SEEN; b++) {
        if (s116BldN[b] == 0ul || s116Bld[b] == blendcnt) {
            s116Bld[b] = blendcnt;
            s116BldN[b]++;
            return;
        }
    }
    s116BldLost++;
}

int gpu2d_survey(struct bg_frame out[2])
{
    uint16_t powcnt = rd16(POWCNT1);
    uint32_t capcnt = rd32(DISPCAPCNT) & DISPCAPCNT_MASK;
    int eligible = 0;
    int n, b, i;

    for (n = 0; n < 2; n++) {
        struct bg_frame *f = &out[n];
        uint16_t mosaic, blendcnt, master;
        int engine_on, sprites;
        struct obj_kind obj;

        memset(f, 0, sizeof *f);
        f->num = n;
        f->io = n ? IO_B : IO_A;
        /* Engine B does not STORE the fields it has no use for, so the mask is
         * part of the read rather than a test at each use (engine_begin). */
        f->dispcnt = rd32(f->io + 0x000u) & (n ? 0xC0B1FFF7u : 0xFFFFFFFFu);
        f->enable = (f->dispcnt >> 8) & 0x0Fu;
        for (b = 0; b < 4; b++) {
            f->bgcnt[b] = rd16(f->io + 0x008u + (uint32_t)b * 2u);
            f->hofs[b] = rd16(f->io + 0x010u + (uint32_t)b * 4u);
            f->vofs[b] = rd16(f->io + 0x012u + (uint32_t)b * 4u);
        }
        f->vram_base = n ? BBG_BASE : ABG_BASE;
        f->vram_mask = n ? BBG_MASK : ABG_MASK;
        f->obj_base = n ? BOBJ_BASE : AOBJ_BASE;
        f->obj_mask = n ? BOBJ_MASK : AOBJ_MASK;
        f->oam = OAM_BASE + (n ? 0x400u : 0x000u);
        f->palbase = n ? 0x400u : 0x000u;
        f->backdrop = *(const volatile uint16_t *)
                          hostmap_ptr(PAL_BASE + (f->palbase & PAL_MASK));

        engine_on = (powcnt & (n ? (1u << 9) : (1u << 1))) != 0
                    && (powcnt & 1u) != 0;

        mosaic = rd16(f->io + 0x04Cu);
        blendcnt = (uint16_t)(rd16(f->io + 0x050u) & 0x3FFFu);
        master = rd16(f->io + 0x06Cu);
        obj.live = 0;
        obj.hard = 0;
        if (f->dispcnt & (1u << 12)) {
            obj = survey_obj(n, f->dispcnt, mosaic);
        }
        f->obj = obj.live && !obj.hard;
        sprites = obj.hard;
        if (sprites) {
            sObjHard++;
        }

        /* The effect pass's own registers, read here so the whole frame is one snapshot
         * (see struct bg_frame). The clamp is hardware's, at the write. */
        f->blendcnt = blendcnt;
        f->master = master;
        {
            uint16_t bldalpha = rd16(f->io + 0x052u);
            uint16_t bldy = rd16(f->io + 0x054u);

            f->eva = (uint8_t)(bldalpha & 0x1Fu);
            f->evb = (uint8_t)((bldalpha >> 8) & 0x1Fu);
            f->evy = (uint8_t)(bldy & 0x1Fu);
            if (f->eva > 16u) f->eva = 16u;
            if (f->evb > 16u) f->evb = 16u;
            if (f->evy > 16u) f->evy = 16u;
        }
        for (b = 0; b < 2; b++) {
            f->winh[b] = rd16(f->io + 0x040u + (uint32_t)b * 2u);
            f->winv[b] = rd16(f->io + 0x044u + (uint32_t)b * 2u);
        }
        f->wincnt[0] = (uint8_t)(rd16(f->io + 0x048u) & 0xFFu);
        f->wincnt[1] = (uint8_t)(rd16(f->io + 0x048u) >> 8);
        f->wincnt[2] = (uint8_t)(rd16(f->io + 0x04Au) & 0xFFu);
        f->wincnt[3] = (uint8_t)(rd16(f->io + 0x04Au) >> 8);

        f->reasons = bg_classify(f, engine_on, capcnt, mosaic, blendcnt,
                                 master, sprites, 0u, &sPlan[n]);
        /*
         * ...and then ask for the 3D layer, but only where the answer can
         * change this one: packing it walks 49,152 pixels, and a frame a
         * sprite is going to refuse anyway must not pay for that. So the
         * predicate runs once with no layer, and a second time only when the
         * layer is the SOLE thing in the way.
         *
         * Gated on the mode too, which skews one number on purpose. In `soft`
         * the pack would be pure cost (nothing draws it) and it would make
         * the software path look slower than it is in exactly the A/B the
         * mode file exists for. The price is that `bg-reason 3d` counts the
         * layer as an obstacle in that mode; `l3d-packed` is what says how
         * often the other mode got past it.
         */
        if (f->reasons == BG_R_3D && sMode == BG_GPU && sPool != NULL
            && gpu_ready()) {
            /*
             * ...and with the effect pass's transform, because the layer is packed once
             * and the fade has to be in the texels: the compositor draws that
             * quad out of a texture and has nowhere to apply a palette. A
             * producer that cannot apply it says so by refusing, and the frame
             * keeps BG_R_3D.
             */
            struct effect e = layer_effect(f, &sPlan[n], 0x01u);
            unsigned l3d = layer3d_produce(&e);

            if (l3d & L3D_READY) {
                f->reasons = bg_classify(f, engine_on, capcnt, mosaic,
                                         blendcnt, master, sprites, l3d,
                                         &sPlan[n]);
            }
        }

        sDraw[n] = 0;
        sClaim[n] = 0;
        if (f->reasons == 0u && sMode == BG_GPU && sPool != NULL
            && gpu_ready()) {
            sDraw[n] = 1;
            sDrawn++;
            /*
             * ...but the software renderer is only skipped when nothing is
             * going to ask it what the frame should have looked like.
             */
            sClaim[n] = gpu_mode() != PRESENT_VERIFY;
            sClaimed += (unsigned long)sClaim[n];
        }
        sFrame[n] = *f;

        sEngineFrames++;
        if (f->reasons != 0u && (f->reasons & ~(unsigned)BG_R_116) == 0u) {
            survey_116(f, blendcnt, master);
        }
        if (f->reasons == 0u) {
            sEligible++;
            eligible++;
        } else {
            for (i = 0; i < BG_R_COUNT; i++) {
                if (f->reasons & (1u << i)) {
                    sReason[i]++;
                    if (f->reasons == (1u << i)) {
                        sSole[i]++;
                    }
                }
            }
        }
    }
    return eligible;
}

unsigned long gpu2d_eligible_total(void)
{
    return sEligible;
}

unsigned long gpu2d_claimed_total(void)
{
    return sClaimed;
}

void gpu2d_report(FILE *f)
{
    struct tile_cache_stats ts;
    int i;

    fprintf(f, "bg-mode %s\n", gpu2d_mode_name());
    fprintf(f, "bg-engine-frames %lu\n", sEngineFrames);
    fprintf(f, "bg-eligible %lu\n", sEligible);
    fprintf(f, "bg-drawn %lu\n", sDrawn);
    /* Not zero means an engine-frame was handed to the GPU and the GPU could
     * not finish it, which shows the frame before's picture on that screen. */
    fprintf(f, "bg-build-failed %lu atlas %lu verts %lu\n", sFailed,
            sFailWhy[0], sFailWhy[1]);
    fprintf(f, "bg-atlas-slots %d\n", tile_cache_slots());
    fprintf(f, "bg-quads %lu\n", sQuads);
    tile_cache_stats(&ts);
    fprintf(f, "bg-tiles %lu hit %lu expanded %lu evicted %lu stale\n",
            ts.hits, ts.misses, ts.evictions, ts.stale);
    /* fired = engine-frames this reason was present on; sole = engine-frames
     * it was the only thing in the way, which is what taking it away would
     * hand to the GPU path. */
    fprintf(f, "# bg-reason name fired sole\n");
    for (i = 0; i < BG_R_COUNT; i++) {
        fprintf(f, "bg-reason %s %lu %lu\n", kReason[i], sReason[i], sSole[i]);
    }
    /* The effect pass's, over the engine-frames only the effect pass is in the way of. */
    fprintf(f, "bg-116-total %lu\n", s116Total);
    fprintf(f, "bg-116-effect none %lu alpha %lu up %lu down %lu\n",
            s116Effect[0], s116Effect[1], s116Effect[2], s116Effect[3]);
    fprintf(f, "bg-116-master none %lu up %lu down %lu\n",
            s116Master[0], s116Master[1], s116Master[2]);
    fprintf(f, "bg-116-window win0 %lu win1 %lu objwin %lu wrapped %lu\n",
            s116Win[0], s116Win[1], s116Win[2], s116WinWrap);
    fprintf(f, "bg-116-target1 %lu %lu %lu %lu %lu %lu\n",
            s116T1[0], s116T1[1], s116T1[2], s116T1[3], s116T1[4], s116T1[5]);
    fprintf(f, "bg-116-target2 backdrop-only %lu other %lu\n",
            s116T2Backdrop, s116T2Other);
    fprintf(f, "bg-116-foldable %lu\n", s116Fold);
    fprintf(f, "# bg-116-bldcnt <value> <target1> <target2> <effect> <frames>\n");
    for (i = 0; i < BG_BLD_SEEN; i++) {
        if (s116BldN[i] == 0ul) {
            continue;
        }
        fprintf(f, "bg-116-bldcnt %04X %02X %02X %u %lu\n", s116Bld[i],
                s116Bld[i] & 0x3Fu, (s116Bld[i] >> 8) & 0x3Fu,
                (unsigned)((s116Bld[i] >> 6) & 3u), s116BldN[i]);
    }
    fprintf(f, "bg-116-bldcnt-lost %lu\n", s116BldLost);
    /* The sprite path's, over every colour sprite the survey saw. `hard` is engine-frames
     * a sprite still keeps in software; the rest are counts of sprites. */
    fprintf(f, "obj-live %lu hard-frames %lu\n", sObjLive, sObjHard);
    fprintf(f, "obj-map 1d %lu 2d %lu\n", sObj1d, sObj2d);
    fprintf(f, "obj-depth 4 %lu 8 %lu\n", sObj4, sObj8);
    fprintf(f, "obj-rot %lu bitmap %lu window %lu mosaic %lu semi %lu wrap %lu\n",
            sObjRot, sObjBmp, sObjWin, sObjMos, sObjSemi, sObjWrap);
}

/* ------------------------------------------------------------------ */
/* The atlas, the palettes and the quads                               */
/* ------------------------------------------------------------------ */
/*
 * What A text layer is, as quads. The DS builds one from a map of 16-bit
 * entries and a set of 8x8 tiles: the map says which tile, which of the 16
 * sub-palettes, and whether it is mirrored. So the GPU form is one quad per
 * visible cell, textured from the tile's expanded texels in the atlas, and a
 * mirror is the quad's texture coordinates swapped rather than a second
 * expansion.
 *
 * 33 COLUMNS AND 25 ROWS, not 32 and 24: a layer scrolled by anything that is
 * not a multiple of eight shows part of a tile at each edge. Those edge quads
 * are TRIMMED here rather than scissored; the cut is on a texel boundary, so
 * moving the edge by k pixels and the texture coordinate by k texels is exact
 * under GPU_NEAREST, and it costs no state change between draws.
 *
 * The cost model, because the survey is the reason this file measures before it
 * believes: a full four-layer engine is about 3,300 cells, and each one is a
 * map read, a cache probe and six vertices. The vertices are the larger half
 * and the plan's answer to them is a geometry shader taking one vertex per
 * quad; this cut writes all six, because the first question is whether the
 * picture is right and the second is what it costs. Both are measured on the
 * console before either is optimised.
 */

/* ------------------------------------------------------------------ */
/* The mode file                                                       */
/* ------------------------------------------------------------------ */

static int mode_read(void)
{
    FILE *f = fopen(BG_PATH, "r");
    char word[16];
    int mode = BG_GPU;

    if (f == NULL) {
        return BG_GPU;
    }
    if (fscanf(f, "%15s", word) == 1) {
        if (strcmp(word, "soft") == 0) {
            mode = BG_SOFT;
        } else if (strcmp(word, "gpu") == 0) {
            mode = BG_GPU;
        } else {
            fprintf(stderr, "3ds-gpu2d: %s says \"%s\", which is not "
                            "soft/gpu, using gpu\n", BG_PATH, word);
        }
    }
    fclose(f);
    return mode;
}

int gpu2d_mode(void)
{
    return sMode;
}

const char *gpu2d_mode_name(void)
{
    return sMode == BG_GPU ? "gpu" : "soft";
}

int gpu2d_init(uint32_t *pool, int slots, int width)
{
    sMode = mode_read();
    if (pool == NULL || slots < 1 || width < TILE_SIDE) {
        return -1;
    }
    if (tile_cache_init(pool, slots) != 0) {
        return -1;
    }
    sPool = pool;
    sPoolSlots = slots;
    sAtlasWidth = width;
    sStepU = 1.0f / (float)width;
    sStepV = 1.0f / (float)(slots / (width / TILE_SIDE) * TILE_SIDE);
    return 0;
}

void gpu2d_set_vramtex(uint32_t *pool, int side)
{
    int x, y;

    if (pool == NULL || side < VIEW_DS_WIDTH || (side % TILE_SIDE) != 0) {
        sVramPool = NULL;
        sVramSide = 0;
        return;
    }
    sVramPool = pool;
    sVramSide = side;
    if (!sVramSwizzleReady) {
        for (y = 0; y < TILE_SIDE; y++) {
            for (x = 0; x < TILE_SIDE; x++) {
                sVramSwizzle[y * TILE_SIDE + x] = (uint8_t)tile_swizzle(x, y);
            }
        }
        sVramSwizzleReady = 1;
    }
}

const uint32_t *gpu2d_vram_tex(void)
{
    return sVramPool;
}

int gpu2d_vram_side(void)
{
    return sVramSide;
}

void gpu2d_set_stencil(int have)
{
    sStencil = have;
}

int gpu2d_draws(int engine)
{
    return sDraw[engine & 1];
}

int gpu2d_claims(int engine)
{
    return sClaim[engine & 1];
}

int gpu2d_drawlist(int engine, const struct bg_draw **out)
{
    *out = sDrawList[engine & 1];
    return sDrawN[engine & 1];
}

int gpu2d_exact(int engine)
{
    int e = engine & 1;
    int i;

    for (i = 0; i < sDrawN[e]; i++) {
        if (sDrawList[e][i].blend != BG_BLEND_NONE) {
            return 0;
        }
    }
    return 1;
}

int gpu2d_layer3d(int engine)
{
    int e = engine & 1;
    int i;

    for (i = 0; i < sDrawN[e]; i++) {
        if (sDrawList[e][i].tex == BG_TEX_LAYER3D) {
            return 1;
        }
    }
    return 0;
}


/* ------------------------------------------------------------------ */
/* Palettes                                                            */
/* ------------------------------------------------------------------ */
/*
 * The palette's contents as one word, which is what the tile cache is keyed
 * on, so two palettes that hash the same are one palette to it, and a layer
 * gets drawn in another layer's colours.
 *
 * Not fnv, and that is A bug this key had rather than A preference. Fnv's
 * step is `h = (h ^ word) * prime`, and a multiply carries a difference only
 * upward: two sub-palettes differing in red differ in the TOP byte of every
 * texel, because a texel is 0xRRGGBBAA, so every bit of the key below 24 is
 * identical between them and the top byte is an eight-bit running sum that
 * can come out equal. It does: sub-palettes 6 and 14 of the check's own
 * ramp (sixteen entries, fifteen of them different) hashed to the same
 * word both before and after an avalanche, because an avalanche cannot
 * separate two values that were already equal.
 *
 * So the mix rotates, which carries a difference in any bit into all of them
 * within one word. The effect pass is what made this show: a palette with a fade folded
 * into it is a fresh key on every frame of the fade, so a scene asks for
 * sixteen keys at once where it used to ask for one.
 */
static uint32_t pal_hash(const uint32_t *tab, int entries)
{
    uint32_t h = 2166136261u;
    int i;

    for (i = 0; i < entries; i++) {
        uint32_t k = tab[i] * 0xCC9E2D51u;

        k = ((k << 15) | (k >> 17)) * 0x1B873593u;
        h ^= k;
        h = ((h << 13) | (h >> 19)) * 5u + 0xE6546B64u;
    }
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

/*
 * The transform is part of the key, and that is the effect pass's whole trick in one
 * line. Two layers can read the same sixteen halfwords in one frame and want
 * two different tables out of them, because BLDCNT names one of them as a
 * target and not the other; and the tile cache below is keyed on the CONTENTS
 * of the table, so a faded palette expands into its own slots and the
 * compositor draws the fade without doing any arithmetic.
 */
static const uint32_t *palette4(const uint16_t *src, const struct effect *e,
                                uint32_t *keyOut)
{
    uint32_t xform = effect_id(e);
    int i;

    for (i = 0; i < sPal4N; i++) {
        if (sPal4[i].src == (const void *)src && sPal4[i].xform == xform) {
            *keyOut = sPal4[i].key;
            return sPal4[i].tab;
        }
    }
    /*
     * Full is not a failure. gpu2d_build() runs after the software renderer
     * has been skipped, so anything that can return -1 there is a stale
     * screen, and a table that recycles its oldest entry costs a second
     * conversion of sixteen halfwords and nothing else. A layer can name only
     * sixteen sub-palettes, so this cannot thrash within one layer.
     */
    if (sPal4N == BG_PAL4_SLOTS) {
        sPal4N = 0;
    }
    sPal4[sPal4N].src = src;
    sPal4[sPal4N].xform = xform;
    effect_palette(sPal4[sPal4N].tab, src, TILE_PAL4, e);
    sPal4[sPal4N].key = pal_hash(sPal4[sPal4N].tab, TILE_PAL4);
    *keyOut = sPal4[sPal4N].key;
    return sPal4[sPal4N++].tab;
}

static const uint32_t *palette8(const uint16_t *src, const struct effect *e,
                                uint32_t *keyOut)
{
    uint32_t xform = effect_id(e);
    int i;

    for (i = 0; i < sPal8N; i++) {
        if (sPal8[i].src == (const void *)src && sPal8[i].xform == xform) {
            *keyOut = sPal8[i].key;
            return sPal8[i].tab;
        }
    }
    if (sPal8N == BG_PAL8_SLOTS) {
        sPal8N = 0;                 /* see palette4() */
    }
    sPal8[sPal8N].src = src;
    sPal8[sPal8N].xform = xform;
    effect_palette(sPal8[sPal8N].tab, src, TILE_PAL8, e);
    sPal8[sPal8N].key = pal_hash(sPal8[sPal8N].tab, TILE_PAL8);
    *keyOut = sPal8[sPal8N].key;
    return sPal8[sPal8N++].tab;
}

/* ------------------------------------------------------------------ */
/* One cell                                                            */
/* ------------------------------------------------------------------ */

static void dirty_slot(int slot)
{
    if (sDirtyN < BG_DIRTY_MAX) {
        sDirty[sDirtyN++] = slot;
    } else {
        sDirtyAll = 1;
    }
}

/*
 * A quad, trimmed to the picture. `sx`/`sy` are the cell's top left corner in
 * the DS's own 256x192 coordinates and may be negative; `flipx`/`flipy` are
 * the map entry's mirror bits.
 */
static int cell_quad(float ox, float oy, int slot, int sx, int sy,
                     int flipx, int flipy)
{
    struct gpu_vertex *v;
    int bx = (slot % (sAtlasWidth / TILE_SIDE)) * TILE_SIDE;
    int by = (slot / (sAtlasWidth / TILE_SIDE)) * TILE_SIDE;
    int cut0x = sx < 0 ? -sx : 0;
    int cut0y = sy < 0 ? -sy : 0;
    int cut1x = sx + TILE_SIDE > VIEW_DS_WIDTH ? sx + TILE_SIDE - VIEW_DS_WIDTH : 0;
    int cut1y = sy + TILE_SIDE > VIEW_DS_HEIGHT ? sy + TILE_SIDE - VIEW_DS_HEIGHT : 0;
    int w = TILE_SIDE - cut0x - cut1x;
    int h = TILE_SIDE - cut0y - cut1y;
    float x0, y0, x1, y1, u0, u1, v0, v1;

    /*
     * A tile that misses the picture is not a failure: a sprite parked at
     * x = -12 has its first 8x8 off screen and the rest on. Allocate AFTER
     * that test, otherwise the six vertices are taken from the previous
     * scene and left unwritten.
     */
    if (w <= 0 || h <= 0) {
        return 0;
    }
    v = gpu_vertex_alloc(6);
    if (v == NULL) {
        return -1;
    }

    x0 = ox + (float)(sx + cut0x);
    y0 = oy + (float)(sy + cut0y);
    x1 = x0 + (float)w;
    y1 = y0 + (float)h;

    /*
     * The texels this quad keeps, as texture coordinates. A trim takes them
     * off the same side of the tile that was cut off the screen, and off the
     * OTHER side when the tile is mirrored, which is the whole of what makes a
     * flip a coordinate swap rather than a second expansion.
     */
    u0 = (float)(bx + (flipx ? cut1x : cut0x)) * sStepU;
    u1 = (float)(bx + TILE_SIDE - (flipx ? cut0x : cut1x)) * sStepU;
    /*
     * v runs the other way: memory row 0 of the texture is v = 1, which is the
     * mapping the GPU present established with a byte compare and not from a document.
     * So the tile's first row (the top one on the DS) is the largest v.
     */
    v0 = 1.0f - (float)(by + (flipy ? cut1y : cut0y)) * sStepV;
    v1 = 1.0f - (float)(by + TILE_SIDE - (flipy ? cut0y : cut1y)) * sStepV;

    if (flipx) {
        float t = u0; u0 = u1; u1 = t;
    }
    if (flipy) {
        float t = v0; v0 = v1; v1 = t;
    }

    v[0] = (struct gpu_vertex){ x0, y0, 0.5f, u0, v0 };
    v[1] = (struct gpu_vertex){ x0, y1, 0.5f, u0, v1 };
    v[2] = (struct gpu_vertex){ x1, y1, 0.5f, u1, v1 };
    v[3] = (struct gpu_vertex){ x0, y0, 0.5f, u0, v0 };
    v[4] = (struct gpu_vertex){ x1, y1, 0.5f, u1, v1 };
    v[5] = (struct gpu_vertex){ x1, y0, 0.5f, u1, v0 };
    return 0;
}

/* ------------------------------------------------------------------ */
/* One layer                                                           */
/* ------------------------------------------------------------------ */
/*
 * The address arithmetic is draw_bg_text()'s, cell by cell instead of pixel by
 * pixel. Every line of it that looks arbitrary is that function's, the two
 * 64 KB bases that belong to engine A alone, the second screen block at
 * +0x800 rather than as an index, the extended-palette slot that is only
 * BGxCNT bit 13 on BG0 and BG1, and where the two disagree, this one is
 * wrong.
 */
static int layer_quads(const struct bg_frame *f, int bgnum, unsigned frame,
                       float ox, float oy, const struct effect *e)
{
    uint16_t bgcnt = f->bgcnt[bgnum];
    uint32_t tileset, tilemap;
    uint32_t widexmask = (bgcnt & (1 << 14)) ? 0x100u : 0u;
    int is8 = (bgcnt & (1 << 7)) != 0;
    int extpal = is8 && (f->dispcnt & (1u << 30)) != 0;
    int extslot = (bgnum < 2 && (bgcnt & 0x2000)) ? (2 + bgnum) : bgnum;
    int row, col;
    int quads = 0;

    if (f->num) {
        tileset = (uint32_t)(bgcnt & 0x003C) << 12;
        tilemap = (uint32_t)(bgcnt & 0x1F00) << 3;
    } else {
        tileset = ((f->dispcnt & 0x07000000u) >> 8)
                + ((uint32_t)(bgcnt & 0x003C) << 12);
        tilemap = ((f->dispcnt & 0x38000000u) >> 11)
                + ((uint32_t)(bgcnt & 0x1F00) << 3);
    }

    for (row = 0; ; row++) {
        int sy = row * TILE_SIDE - (int)(f->vofs[bgnum] & 7u);
        uint16_t yy = (uint16_t)(f->vofs[bgnum] + (uint16_t)sy);
        uint32_t mapaddr = tilemap;
        const volatile uint16_t *map0, *map1;

        if (sy >= VIEW_DS_HEIGHT) {
            break;
        }
        if (bgcnt & (1 << 15)) {
            mapaddr += ((uint32_t)yy & 0x1F8u) << 3;
            if (bgcnt & (1 << 14)) {
                mapaddr += ((uint32_t)yy & 0x100u) << 3;
            }
        } else {
            mapaddr += ((uint32_t)yy & 0xF8u) << 3;
        }
        map0 = (const volatile uint16_t *)
                   hostmap_ptr(f->vram_base + (mapaddr & f->vram_mask));
        map1 = widexmask
                   ? (const volatile uint16_t *)
                         hostmap_ptr(f->vram_base
                                     + ((mapaddr + 0x800u) & f->vram_mask))
                   : map0;

        for (col = 0; ; col++) {
            int sx = col * TILE_SIDE - (int)(f->hofs[bgnum] & 7u);
            uint16_t xx = (uint16_t)(f->hofs[bgnum] + (uint16_t)sx);
            uint16_t entry;
            uint32_t tileaddr, palkey = 0;
            const uint32_t *pal;
            const uint8_t *src;
            int slot, expanded = 0;

            if (sx >= VIEW_DS_WIDTH) {
                break;
            }
            entry = ((uint32_t)xx & widexmask) ? map1[((uint32_t)xx & 0xF8u) >> 3]
                                              : map0[((uint32_t)xx & 0xF8u) >> 3];

            if (is8) {
                tileaddr = tileset + ((uint32_t)(entry & 0x03FF) << 6);
                if (extpal) {
                    const uint16_t *p = (const uint16_t *)
                        armrec_vram_extpal(f->num ? ARMREC_EXTPAL_BBG
                                                  : ARMREC_EXTPAL_ABG,
                                           extslot);
                    if (p == NULL) {
                        /*
                         * No bank mapped is not "do not draw": every index
                         * reads zero, so index 0 is still transparent and
                         * every other index is black (bg_extpal). Read out of
                         * a table of zeros rather than out of a table of
                         * expanded black, so that the effect pass's transform reaches it
                         * the same way it reaches every other palette, a
                         * faded screen fades this one too.
                         */
                        p = sZeroPal;
                    } else {
                        p += (uint32_t)(entry >> 12) * 256u;
                    }
                    pal = palette8(p, e, &palkey);
                } else {
                    pal = palette8((const uint16_t *)
                                       hostmap_ptr(PAL_BASE + f->palbase),
                                   e, &palkey);
                }
            } else {
                tileaddr = tileset + ((uint32_t)(entry & 0x03FF) << 5);
                pal = palette4((const uint16_t *)
                                   hostmap_ptr(PAL_BASE
                                               + ((f->palbase
                                                   + ((uint32_t)(entry >> 12)
                                                      * 32u))
                                                  & PAL_MASK)),
                               e, &palkey);
            }
            if (pal == NULL) {
                return -1;
            }

            tileaddr = f->vram_base + (tileaddr & f->vram_mask);
            src = (const uint8_t *)hostmap_ptr(tileaddr);
            slot = tile_cache_get(tileaddr, palkey, is8 ? 8 : 4, src, pal,
                                  frame, &expanded);
            if (slot < 0) {
                sFailWhy[0]++;
                return -1;
            }
            if (expanded) {
                dirty_slot(slot);
            }
            if (cell_quad(ox, oy, slot, sx, sy, (entry & (1 << 10)) != 0,
                          (entry & (1 << 11)) != 0) != 0) {
                sFailWhy[1]++;
                return -1;
            }
            quads++;
        }
    }
    return quads;
}

/* ------------------------------------------------------------------ */
/* Sprites, one atlas quad per tile                                  */
/* ------------------------------------------------------------------ */
/*
 * The hardware draws the OBJ layer into A scanline buffer first, then
 * interleaves it between the backgrounds at the sprite's BG-relative
 * priority, last within that priority so a sprite covers every background
 * that shares it (draw_scanline). Here that is the same walk in reverse
 * OAM order: last quad wins, which is lower index wins, which is the
 * sprite-against-sprite rule.
 *
 * One quad per tile, not per sprite. The tile cache's atlas is 8x8 slots; a 16x16
 * sprite is four of them. 1D mapping makes those tiles consecutive, 2D
 * mapping puts the next row 32 tile-numbers on, and both are an address
 * the tile cache already keys on. Rot/scale is the four corners of each
 * tile pushed through the inverse of OAM's 8.8 matrix, one quad of a
 * parallelogram, still out of the same atlas, so a rotated 16x16 is still
 * four quads whose shared edges agree to the bit.
 */

static uint32_t sprite_tile_addr(const struct bg_frame *f, uint32_t tilenum,
                                 int col, int row, int is8, int width)
{
    uint32_t t = tilenum;

    if (f->dispcnt & (1u << 4)) {
        t <<= (f->dispcnt >> 20) & 3u;
        t += ((uint32_t)row * ((uint32_t)width >> 3) + (uint32_t)col)
             << (is8 ? 1 : 0);
    } else {
        t += (uint32_t)row * 0x20u;
        t += (uint32_t)col << (is8 ? 1 : 0);
    }
    return f->obj_base + ((t << 5) & f->obj_mask);
}

static int affine_cell_quad(float ox, float oy, int slot,
                            float x0, float y0, float x1, float y1,
                            float x2, float y2, float x3, float y3)
{
    struct gpu_vertex *v = gpu_vertex_alloc(6);
    int bx = (slot % (sAtlasWidth / TILE_SIDE)) * TILE_SIDE;
    int by = (slot / (sAtlasWidth / TILE_SIDE)) * TILE_SIDE;
    float u0 = (float)bx * sStepU;
    float u1 = (float)(bx + TILE_SIDE) * sStepU;
    float v0 = 1.0f - (float)by * sStepV;
    float v1 = 1.0f - (float)(by + TILE_SIDE) * sStepV;

    if (v == NULL) {
        return -1;
    }
    v[0] = (struct gpu_vertex){ ox + x0, oy + y0, 0.5f, u0, v0 };
    v[1] = (struct gpu_vertex){ ox + x3, oy + y3, 0.5f, u0, v1 };
    v[2] = (struct gpu_vertex){ ox + x2, oy + y2, 0.5f, u1, v1 };
    v[3] = (struct gpu_vertex){ ox + x0, oy + y0, 0.5f, u0, v0 };
    v[4] = (struct gpu_vertex){ ox + x2, oy + y2, 0.5f, u1, v1 };
    v[5] = (struct gpu_vertex){ ox + x1, oy + y1, 0.5f, u1, v0 };
    return 0;
}

static void affine_map(float A, float B, float C, float D, float det,
                       float cx, float cy, float hw, float hh,
                       float tx, float ty, float *sx, float *sy)
{
    float dx = tx - hw;
    float dy = ty - hh;

    *sx = cx + (dx * D - dy * B) / det;
    *sy = cy + (dy * A - dx * C) / det;
}

static int sprite_emit_tile(unsigned frame, float ox, float oy,
                            uint32_t tileaddr, const uint32_t *pal,
                            uint32_t palkey, int is8, int sx, int sy,
                            int flipx, int flipy, int affine,
                            float ax0, float ay0, float ax1, float ay1,
                            float ax2, float ay2, float ax3, float ay3)
{
    const uint8_t *src = (const uint8_t *)hostmap_ptr(tileaddr);
    int expanded = 0;
    int slot = tile_cache_get(tileaddr, palkey, is8 ? 8 : 4, src, pal,
                              frame, &expanded);

    if (slot < 0) {
        sFailWhy[0]++;
        return -1;
    }
    if (expanded) {
        dirty_slot(slot);
    }
    if (affine) {
        if (affine_cell_quad(ox, oy, slot, ax0, ay0, ax1, ay1,
                             ax2, ay2, ax3, ay3) != 0) {
            sFailWhy[1]++;
            return -1;
        }
        return 0;
    }
    if (cell_quad(ox, oy, slot, sx, sy, flipx, flipy) != 0) {
        sFailWhy[1]++;
        return -1;
    }
    return 0;
}

static int sprite_tiles(const struct bg_frame *f, unsigned frame,
                        float ox, float oy, uint32_t tilenum, int is8,
                        int width, int height, int xpos, int ypos,
                        int flipx, int flipy, const uint32_t *pal,
                        uint32_t palkey, int affine, float A, float B,
                        float C, float D, float det, int boundw, int boundh)
{
    int cols = width / TILE_SIDE;
    int rows = height / TILE_SIDE;
    int row, col;
    int n = 0;
    float hw = (float)width * 0.5f;
    float hh = (float)height * 0.5f;
    float cx = (float)boundw * 0.5f;
    float cy = (float)boundh * 0.5f;

    for (row = 0; row < rows; row++) {
        int sr = flipy ? (rows - 1 - row) : row;

        for (col = 0; col < cols; col++) {
            int sc = flipx ? (cols - 1 - col) : col;
            uint32_t addr = sprite_tile_addr(f, tilenum, sc, sr, is8, width);
            int sx = xpos + col * TILE_SIDE;
            int sy = ypos + row * TILE_SIDE;
            float x0, y0, x1, y1, x2, y2, x3, y3;

            if (affine) {
                float tx0 = (float)(col * TILE_SIDE);
                float ty0 = (float)(row * TILE_SIDE);
                float tx1 = tx0 + (float)TILE_SIDE;
                float ty1 = ty0 + (float)TILE_SIDE;

                affine_map(A, B, C, D, det, cx, cy, hw, hh, tx0, ty0, &x0, &y0);
                affine_map(A, B, C, D, det, cx, cy, hw, hh, tx1, ty0, &x1, &y1);
                affine_map(A, B, C, D, det, cx, cy, hw, hh, tx1, ty1, &x2, &y2);
                affine_map(A, B, C, D, det, cx, cy, hw, hh, tx0, ty1, &x3, &y3);
                x0 += (float)xpos; y0 += (float)ypos;
                x1 += (float)xpos; y1 += (float)ypos;
                x2 += (float)xpos; y2 += (float)ypos;
                x3 += (float)xpos; y3 += (float)ypos;
            } else {
                x0 = y0 = x1 = y1 = x2 = y2 = x3 = y3 = 0.0f;
            }
            if (sprite_emit_tile(frame, ox, oy, addr, pal, palkey, is8,
                                 sx, sy, flipx, flipy, affine,
                                 x0, y0, x1, y1, x2, y2, x3, y3) != 0) {
                return -1;
            }
            n++;
        }
    }
    return n;
}

static int sprite_one(const struct bg_frame *f, unsigned frame,
                      float ox, float oy, int sprnum, int prio,
                      const struct effect *e)
{
    uint16_t a0 = rd16(f->oam + (uint32_t)sprnum * 8u + 0u);
    uint16_t a1 = rd16(f->oam + (uint32_t)sprnum * 8u + 2u);
    uint16_t a2 = rd16(f->oam + (uint32_t)sprnum * 8u + 4u);
    uint32_t sprtype = ((uint32_t)a0 >> 8) & 3u;
    uint32_t mode = ((uint32_t)a0 >> 10) & 3u;
    uint32_t sizeparam;
    int32_t width, height, boundw, boundh, xpos, ypos;
    int is8, flipx, flipy, affine;
    uint32_t tilenum, palkey = 0;
    const uint32_t *pal;
    float A = 1.0f, B = 0.0f, C = 0.0f, D = 1.0f, det = 1.0f;
    int copies, c, n = 0;

    if (sprtype == 2u || mode == 1u || mode == 2u || mode == 3u) {
        return 0;
    }
    if (((a2 >> 10) & 3u) != (uint32_t)prio) {
        return 0;
    }
    sizeparam = ((uint32_t)a0 >> 14) | (((uint32_t)a1 & 0xC000u) >> 12);
    width = kSprW[sizeparam];
    height = kSprH[sizeparam];
    boundw = width;
    boundh = height;
    if (sprtype == 3u) {
        boundw <<= 1;
        boundh <<= 1;
    }
    ypos = (int32_t)(a0 & 0xFFu);
    xpos = (int32_t)(((uint32_t)a1 & 0x1FFu) ^ 0x100u) - 0x100;
    if (!sprite_on_screen(xpos, ypos, boundw, boundh)) {
        return 0;
    }

    is8 = (a0 & (1u << 13)) != 0;
    affine = (sprtype & 1u) != 0;
    flipx = !affine && (a1 & (1u << 12)) != 0;
    flipy = !affine && (a1 & (1u << 13)) != 0;
    tilenum = a2 & 0x03FFu;

    if (affine) {
        uint32_t rp = ((uint32_t)(a1 >> 9) & 0x1Fu) * 16u + 3u;
        int16_t rotA = (int16_t)rd16(f->oam + rp * 2u + 0u);
        int16_t rotB = (int16_t)rd16(f->oam + rp * 2u + 8u);
        int16_t rotC = (int16_t)rd16(f->oam + rp * 2u + 16u);
        int16_t rotD = (int16_t)rd16(f->oam + rp * 2u + 24u);

        A = (float)rotA / 256.0f;
        B = (float)rotB / 256.0f;
        C = (float)rotC / 256.0f;
        D = (float)rotD / 256.0f;
        det = A * D - B * C;
        if (det > -1.0e-6f && det < 1.0e-6f) {
            return 0;
        }
    }

    if (is8) {
        if (f->dispcnt & (1u << 31)) {
            const uint16_t *p = (const uint16_t *)
                armrec_vram_extpal(f->num ? ARMREC_EXTPAL_BOBJ
                                          : ARMREC_EXTPAL_AOBJ, 0);

            if (p == NULL) {
                p = sZeroPal;
            } else {
                p += (uint32_t)(a2 >> 12) * 256u;
            }
            pal = palette8(p, e, &palkey);
        } else {
            pal = palette8((const uint16_t *)
                               hostmap_ptr(PAL_BASE
                                           + ((f->palbase + 0x200u) & PAL_MASK)),
                           e, &palkey);
        }
    } else {
        pal = palette4((const uint16_t *)
                           hostmap_ptr(PAL_BASE
                                       + ((f->palbase + 0x200u
                                           + (uint32_t)(a2 >> 12) * 32u)
                                          & PAL_MASK)),
                       e, &palkey);
    }
    if (pal == NULL) {
        return -1;
    }

    copies = (ypos + boundh > 256) ? 2 : 1;
    for (c = 0; c < copies; c++) {
        int y = ypos - c * 256;
        int got = sprite_tiles(f, frame, ox, oy, tilenum, is8, width, height,
                               xpos, y, flipx, flipy, pal, palkey, affine,
                               A, B, C, D, det, boundw, boundh);

        if (got < 0) {
            return -1;
        }
        n += got;
    }
    return n;
}

static int sprite_quads(const struct bg_frame *f, int prio, unsigned frame,
                        float ox, float oy, const struct effect *e)
{
    int sprnum;
    int n = 0;

    if (!f->obj) {
        return 0;
    }
    /* Reverse OAM order so the last quad is the lowest index, which is the
     * sprite that wins a tie. */
    for (sprnum = 127; sprnum >= 0; sprnum--) {
        int got = sprite_one(f, frame, ox, oy, sprnum, prio, e);

        if (got < 0) {
            return -1;
        }
        n += got;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* The window mask, as rectangles                                      */
/* ------------------------------------------------------------------ */
/*
 * What the window unit produces is one byte per pixel saying which layers may
 * draw there and whether the colour effect may run, and it is built by two
 * latches rather than by a rectangle test: the vertical one flips at the two
 * scanlines WINxV names and the horizontal one at the two pixels WINxH names,
 * and NEITHER is reset (win_scan). So X1 > X2 is a window that wraps round the
 * edge of the screen and X1 == X2 is one that is off; the clear is tested
 * first, so a coordinate that is both bounds clears.
 *
 * Held still for a frame, which is how this port reads every 2D register, the
 * latches settle and the region is one rectangle or two. That is what goes
 * into the stencil: WINOUT over the picture, then WIN1's rectangles, then
 * WIN0's, weakest first, which is calculate_window_mask()'s own order.
 */
/* A quad with no texture behind it: the window rectangles, the backdrop and
 * the fade. Its texture coordinates are never read, the stage that draws it
 * takes a constant colour, or writes no colour at all. */
static int plain_quad(float x, float y, float w, float h)
{
    struct gpu_vertex *v = gpu_vertex_alloc(6);

    if (v == NULL) {
        return -1;
    }
    v[0] = (struct gpu_vertex){ x,     y,     0.5f, 0.0f, 0.0f };
    v[1] = (struct gpu_vertex){ x,     y + h, 0.5f, 0.0f, 1.0f };
    v[2] = (struct gpu_vertex){ x + w, y + h, 0.5f, 1.0f, 1.0f };
    v[3] = (struct gpu_vertex){ x,     y,     0.5f, 0.0f, 0.0f };
    v[4] = (struct gpu_vertex){ x + w, y + h, 0.5f, 1.0f, 1.0f };
    v[5] = (struct gpu_vertex){ x + w, y,     0.5f, 1.0f, 0.0f };
    return 0;
}

static int win_span(unsigned lo, unsigned hi, unsigned limit,
                    unsigned *a0, unsigned *a1, unsigned *b0, unsigned *b1)
{
    if (lo == hi) {
        return 0;               /* the clear wins: never inside */
    }
    if (hi > limit) {
        hi = limit;
    }
    if (lo < hi) {
        *a0 = lo;
        *a1 = hi;
        return 1;
    }
    /* Wrapped: the latch is left set at the end of a line or a frame and
     * carries into the next one. */
    *a0 = 0u;
    *a1 = hi;
    *b0 = lo > limit ? limit : lo;
    *b1 = limit;
    return 2;
}

static int win_quads(const struct bg_frame *f, int w, float ox, float oy)
{
    unsigned x[2][2], y[2][2];
    int nx, ny, i, j;

    nx = win_span((unsigned)(f->winh[w] >> 8), (unsigned)(f->winh[w] & 0xFFu),
                  VIEW_DS_WIDTH, &x[0][0], &x[0][1], &x[1][0], &x[1][1]);
    ny = win_span((unsigned)(f->winv[w] >> 8), (unsigned)(f->winv[w] & 0xFFu),
                  VIEW_DS_HEIGHT, &y[0][0], &y[0][1], &y[1][0], &y[1][1]);
    for (i = 0; i < ny; i++) {
        for (j = 0; j < nx; j++) {
            if (x[j][1] <= x[j][0] || y[i][1] <= y[i][0]) {
                continue;
            }
            if (plain_quad(ox + (float)x[j][0], oy + (float)y[i][0],
                           (float)(x[j][1] - x[j][0]),
                           (float)(y[i][1] - y[i][0])) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The build                                                           */
/* ------------------------------------------------------------------ */

static struct bg_draw *draw_add(int e, int base)
{
    struct bg_draw *d;

    if (sDrawN[e] == BG_DRAW_MAX) {
        return NULL;
    }
    d = &sDrawList[e][sDrawN[e]++];
    memset(d, 0, sizeof *d);
    d->base = base;
    d->count = gpu_vertex_count() - base;
    d->tex = BG_TEX_ATLAS;
    return d;
}

/*
 * One vertex range, as the compositor will draw it: window stencil, then
 * the blend split if this layer is a target-1 alpha blender. Sprites use
 * the same helper with flag 0x10, WININ's OBJ bit, and BLDCNT's.
 */
static int draw_range(int e, int base, uint8_t tex, unsigned flag,
                      const struct bg_frame *f, const struct bg_colour *p)
{
    struct bg_draw *d;

    if (gpu_vertex_count() == base) {
        return 0;
    }
    d = draw_add(e, base);
    if (d == NULL) {
        return -1;
    }
    d->tex = tex;
    d->test = (uint8_t)(p->windows != 0u ? flag : 0u);
    d->ref = (uint8_t)(p->windows != 0u ? flag : 0u);
    if (p->blend) {
        unsigned mine = (p->t2 & flag) ? BG_ST_T2 : 0u;

        if (p->t1 & flag) {
            struct bg_draw *plain, *mark;

            d->test |= BG_ST_T2;
            d->ref |= BG_ST_T2;
            d->blend = BG_BLEND_ALPHA;
            d->eva = f->eva;
            d->evb = f->evb;
            plain = draw_add(e, base);
            if (plain == NULL) {
                return -1;
            }
            *plain = *d;
            plain->blend = BG_BLEND_NONE;
            plain->ref = (uint8_t)(d->ref & ~BG_ST_T2);
            mark = draw_add(e, base);
            if (mark == NULL) {
                return -1;
            }
            *mark = *d;
            mark->blend = BG_BLEND_NONE;
            mark->test = (uint8_t)(d->test & ~BG_ST_T2);
            mark->ref = (uint8_t)((d->ref & ~BG_ST_T2) | mine);
            mark->write = BG_ST_T2;
            mark->nocolour = 1;
        } else {
            d->ref |= (uint8_t)mine;
            d->write = BG_ST_T2;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The display-mode work, a fill, a VRAM bank, and the capture unit  */
/* ------------------------------------------------------------------ */

static int display_fill(int e, const struct bg_frame *f, float ox, float oy,
                        uint32_t bgr6, int fade)
{
    struct effect fx;
    struct bg_draw *d;
    int base;

    fx.kind = EFF_NONE;
    fx.evy = 0u;
    fx.master = fade ? f->master : 0u;
    base = gpu_vertex_count();
    if (plain_quad(ox, oy, (float)VIEW_DS_WIDTH, (float)VIEW_DS_HEIGHT) != 0) {
        sFailWhy[1]++;
        sFailed++;
        return -1;
    }
    d = draw_add(e, base);
    if (d == NULL) {
        sFailed++;
        return -1;
    }
    d->tex = BG_TEX_NONE;
    d->colour = 1;
    d->constant = effect_texel6(&fx, bgr6);
    sQuads++;
    return 1;
}

static int vram_pack(const struct bg_frame *f, const struct effect *e)
{
    int bank = (int)((f->dispcnt >> 18) & 3u);
    const uint16_t *src;
    int by, bx, y, x;
    int cols;

    if (sVramPool == NULL || sVramSide < VIEW_DS_WIDTH) {
        return -1;
    }
    cols = sVramSide / TILE_SIDE;
    src = armrec_vram_bank_in_lcdc(bank)
        ? (const uint16_t *)armrec_vram_bank_ptr(bank)
        : NULL;
    for (by = 0; by < VIEW_DS_HEIGHT / TILE_SIDE; by++) {
        for (bx = 0; bx < VIEW_DS_WIDTH / TILE_SIDE; bx++) {
            uint32_t *dst = sVramPool
                          + (size_t)(by * cols + bx) * TILE_TEXELS;

            for (y = 0; y < TILE_SIDE; y++) {
                unsigned line = (unsigned)(by * TILE_SIDE + y);

                for (x = 0; x < TILE_SIDE; x++) {
                    uint16_t pix = 0;

                    if (src != NULL) {
                        unsigned i = line * (unsigned)VIEW_DS_WIDTH
                                   + (unsigned)(bx * TILE_SIDE + x);

                        pix = src[i & 0xFFFFu];
                    }
                    dst[sVramSwizzle[y * TILE_SIDE + x]] =
                        effect_texel_flat(e, pix);
                }
            }
        }
    }
    sVramDirty = 1;
    return 0;
}

static int vram_quad(float ox, float oy)
{
    struct gpu_vertex *v = gpu_vertex_alloc(6);
    float u0 = 0.0f;
    float u1 = (float)VIEW_DS_WIDTH / (float)sVramSide;
    float v0 = 1.0f;
    float v1 = 1.0f - (float)VIEW_DS_HEIGHT / (float)sVramSide;
    float x0 = ox, y0 = oy;
    float x1 = ox + (float)VIEW_DS_WIDTH, y1 = oy + (float)VIEW_DS_HEIGHT;

    if (v == NULL || sVramPool == NULL) {
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

static int display_vram(int e, const struct bg_frame *f, float ox, float oy)
{
    struct effect fx;
    struct bg_draw *d;
    int base;

    fx.kind = EFF_NONE;
    fx.evy = 0u;
    fx.master = f->master;
    if (vram_pack(f, &fx) != 0) {
        sFailed++;
        return -1;
    }
    base = gpu_vertex_count();
    if (vram_quad(ox, oy) != 0) {
        sFailWhy[1]++;
        sFailed++;
        return -1;
    }
    d = draw_add(e, base);
    if (d == NULL) {
        sFailed++;
        return -1;
    }
    d->tex = BG_TEX_VRAM;
    sQuads++;
    return 1;
}

/*
 * DISPCAPCNT, lifted from pc_gpu2d.c's do_capture(). The GPU path claims the
 * engine, so that function never runs; this is the write it would have made.
 * Source A is a 6-bit scanline with a flag byte, or NULL when the unit is
 * reading source B alone.
 */
static void do_capture(uint32_t line, const uint32_t *src2d, uint32_t capcnt)
{
    uint32_t width, height, sz = (capcnt >> 20) & 3u;
    uint32_t dstbank, dstaddr, i;
    volatile uint16_t *dst;
    const uint32_t *srcA = src2d;
    const uint16_t *srcB = NULL;

    if (sz == 0u) { width = 128u; height = 128u; }
    else          { width = 256u; height = 64u * sz; }
    if (line >= height) {
        return;
    }

    dstbank = (capcnt >> 16) & 3u;
    if (!armrec_vram_bank_in_lcdc((int)dstbank)) {
        return;
    }
    dst = (volatile uint16_t *)armrec_vram_bank_ptr((int)dstbank);
    dstaddr = (((capcnt >> 18) & 3u) << 14) + line * width;
    dst += (dstaddr & 0xFFFFu);

    if ((capcnt & (1u << 25)) == 0u) {
        uint32_t dispcnt = sFrame[0].dispcnt;
        uint32_t srcvram = (dispcnt >> 18) & 3u;

        if (armrec_vram_bank_in_lcdc((int)srcvram)) {
            uint32_t offset = line * 256u;

            if (((dispcnt >> 16) & 3u) != 2u) {
                offset += ((capcnt >> 26) & 3u) << 14;
            }
            srcB = (const uint16_t *)armrec_vram_bank_ptr((int)srcvram)
                 + (offset & 0xFFFFu);
        }
    }

    switch ((capcnt >> 29) & 3u) {
    case 0:
        if (srcA == NULL) {
            return;
        }
        for (i = 0; i < width; i++) {
            uint32_t v = srcA[i];

            dst[i] = (uint16_t)(((v >> 1) & 0x1Fu)
                                | (((v >> 9) & 0x1Fu) << 5)
                                | (((v >> 17) & 0x1Fu) << 10)
                                | ((v >> 24) ? 0x8000u : 0u));
        }
        break;
    case 1:
        for (i = 0; i < width; i++) {
            dst[i] = srcB ? srcB[i] : 0u;
        }
        break;
    default:
        {
            uint32_t eva = capcnt & 0x1Fu, evb = (capcnt >> 8) & 0x1Fu;

            if (eva > 16u) eva = 16u;
            if (evb > 16u) evb = 16u;
            for (i = 0; i < width; i++) {
                uint32_t v = srcA ? srcA[i] : 0u;
                uint32_t rA = (v >> 1) & 0x1Fu, gA = (v >> 9) & 0x1Fu;
                uint32_t bA = (v >> 17) & 0x1Fu, aA = (v >> 24) ? 1u : 0u;
                uint32_t rB = 0, gB = 0, bB = 0, aB = 0;
                uint32_t rD, gD, bD, aD;

                if (srcB) {
                    uint32_t w = srcB[i];

                    rB = w & 0x1Fu;
                    gB = (w >> 5) & 0x1Fu;
                    bB = (w >> 10) & 0x1Fu;
                    aB = w >> 15;
                }
                rD = ((rA * aA * eva) + (rB * aB * evb) + 8u) >> 4;
                gD = ((gA * aA * eva) + (gB * aB * evb) + 8u) >> 4;
                bD = ((bA * aA * eva) + (bB * aB * evb) + 8u) >> 4;
                aD = (eva ? aA : 0u) | (evb ? aB : 0u);
                if (rD > 0x1Fu) rD = 0x1Fu;
                if (gD > 0x1Fu) gD = 0x1Fu;
                if (bD > 0x1Fu) bD = 0x1Fu;
                dst[i] = (uint16_t)(rD | (gD << 5) | (bD << 10) | (aD << 15));
            }
        }
        break;
    }
}

void gpu2d_capture(void)
{
    const struct bg_frame *f = &sFrame[0];
    uint32_t capcnt = rd32(DISPCAPCNT) & DISPCAPCNT_MASK;
    uint16_t powcnt;
    uint32_t dispmode, fill, line, i;
    int engine_on, blank, srcA_fill;
    static uint32_t blank_line[256];

    if (!sClaim[0] || (capcnt & 0x80000000u) == 0u) {
        return;
    }
    powcnt = rd16(POWCNT1);
    engine_on = (powcnt & (1u << 1)) != 0 && (powcnt & 1u) != 0;
    dispmode = dispmode_of(f);
    blank = (f->dispcnt & (1u << 7)) != 0;
    srcA_fill = !engine_on || blank || dispmode == 0u;
    fill = (!engine_on) ? 0u : 0x3F3F3Fu;
    if (dispmode == 0u) {
        fill = 0x3F3F3Fu;
    }
    if (srcA_fill) {
        for (i = 0; i < 256u; i++) {
            blank_line[i] = fill | 0xFF000000u;
        }
    }
    for (line = 0; line < (uint32_t)VIEW_DS_HEIGHT; line++) {
        do_capture(line, srcA_fill ? blank_line : NULL, capcnt);
    }
    wr32(DISPCAPCNT, capcnt & ~0x80000000u);
}

int gpu2d_build(int engine, unsigned frame, float ox, float oy)
{
    int e = engine & 1;
    const struct bg_frame *f = &sFrame[e];
    const struct bg_colour *p = &sPlan[e];
    struct effect fx;
    struct bg_draw *d;
    int quads = 0;
    int base;
    int prio, b, w;
    uint32_t dispmode;
    int blank;

    sPal4N = 0;
    sPal8N = 0;
    sDrawN[e] = 0;

    dispmode = dispmode_of(f);
    blank = (f->dispcnt & (1u << 7)) != 0;
    if (dispmode == 0u) {
        return display_fill(e, f, ox, oy, 0x3F3F3Fu, 0);
    }
    if (dispmode == 2u) {
        return display_vram(e, f, ox, oy);
    }
    if (blank) {
        return display_fill(e, f, ox, oy, 0x3F3F3Fu, 1);
    }

    /*
     * The window mask first, because every draw after it tests it. Colour
     * writes are off for these three (BG_TEX_NONE with no constant) so
     * what they leave is six bits of stencil and nothing else.
     */
    if (p->windows != 0u) {
        base = gpu_vertex_count();
        if (plain_quad(ox, oy, (float)VIEW_DS_WIDTH, (float)VIEW_DS_HEIGHT)
            != 0) {
            sFailWhy[1]++;
            sFailed++;
            return -1;
        }
        d = draw_add(e, base);
        if (d == NULL) {
            sFailed++;
            return -1;
        }
        d->tex = BG_TEX_NONE;
        d->ref = f->wincnt[2];              /* WINOUT */
        d->write = BG_ST_WIN;
        for (w = 1; w >= 0; w--) {
            if (!(p->windows & (1u << w))) {
                continue;
            }
            base = gpu_vertex_count();
            if (win_quads(f, w, ox, oy) != 0) {
                sFailWhy[1]++;
                sFailed++;
                return -1;
            }
            if (gpu_vertex_count() == base) {
                continue;                   /* the window is off */
            }
            d = draw_add(e, base);
            if (d == NULL) {
                sFailed++;
                return -1;
            }
            d->tex = BG_TEX_NONE;
            d->ref = f->wincnt[w];
            d->write = BG_ST_WIN;
        }
    }

    /*
     * The backdrop, which is a layer to the blend unit and not a clear: it has
     * a target bit of its own in BLDCNT, so a translucent layer over it blends
     * with it wherever nothing else drew, and the stencil bit this draw
     * writes is what says so.
     */
    base = gpu_vertex_count();
    if (plain_quad(ox, oy, (float)VIEW_DS_WIDTH, (float)VIEW_DS_HEIGHT) != 0) {
        sFailWhy[1]++;
        sFailed++;
        return -1;
    }
    d = draw_add(e, base);
    if (d == NULL) {
        sFailed++;
        return -1;
    }
    fx = layer_effect(f, p, 0x20u);
    d->tex = BG_TEX_NONE;
    d->colour = 1;
    d->constant = effect_texel(&fx, f->backdrop);
    if (p->blend) {
        d->ref = (p->t2 & 0x20u) ? BG_ST_T2 : 0u;
        d->write = BG_ST_T2;
    }
    quads++;

    /*
     * Back to front, and inside one priority the higher layer number first,
     * draw_scanline()'s own order, and the reason composition there is a store
     * rather than a comparison. Here the last quad over a pixel wins for the
     * same reason: no depth test, and the blend unit is source-times-one.
     */
    for (prio = 3; prio >= 0; prio--) {
        for (b = 3; b >= 0; b--) {
            unsigned flag = 1u << b;
            uint8_t tex;
            int n;

            if ((f->bgcnt[b] & 3) != (uint32_t)prio
                || !(f->enable & flag)) {
                continue;
            }
            base = gpu_vertex_count();
            fx = layer_effect(f, p, flag);
            /*
             * BG0 as the 3D engine, which is one quad out of another texture
             * and not tiles at all, draw_scanline()'s own branch, at the
             * same point in the same order. The survey has already
             * packed the layer, with this frame's transform already in its
             * texels, and would not have called this frame ours if it could
             * not, so a failure here is the vertex buffer and nothing else.
             */
            if (b == 0 && !f->num && (f->dispcnt & 8u)) {
                if (layer3d_quad(ox, oy) != 0) {
                    sFailWhy[1]++;
                    sFailed++;
                    return -1;
                }
                n = 1;
                tex = BG_TEX_LAYER3D;
            } else {
                n = layer_quads(f, b, frame, ox, oy, &fx);
                if (n < 0) {
                    sFailed++;
                    return -1;
                }
                tex = BG_TEX_ATLAS;
            }
            quads += n;
            if (draw_range(e, base, tex, flag, f, p) != 0) {
                sFailed++;
                return -1;
            }
        }
        /* OBJ last within the priority, so a sprite covers every background
         * that shares it, draw_scanline()'s own order. */
        if (f->obj) {
            int n;

            base = gpu_vertex_count();
            fx = layer_effect(f, p, 0x10u);
            n = sprite_quads(f, prio, frame, ox, oy, &fx);
            if (n < 0) {
                sFailed++;
                return -1;
            }
            quads += n;
            if (draw_range(e, base, BG_TEX_ATLAS, 0x10u, f, p) != 0) {
                sFailed++;
                return -1;
            }
        }
    }

    /*
     * The fade, when it could not be folded. MASTER_BRIGHT is the last thing
     * the DS does to a pixel and a blend runs after the palette, so on a frame
     * with both it cannot be in the palette: fading two colours and blending
     * them is not blending them and fading the result. So it becomes what it
     * is on hardware, a pass over the finished picture, towards white or
     * towards black by EVY sixteenths.
     */
    if (p->fade) {
        base = gpu_vertex_count();
        if (plain_quad(ox, oy, (float)VIEW_DS_WIDTH, (float)VIEW_DS_HEIGHT)
            != 0) {
            sFailWhy[1]++;
            sFailed++;
            return -1;
        }
        d = draw_add(e, base);
        if (d == NULL) {
            sFailed++;
            return -1;
        }
        d->tex = BG_TEX_NONE;
        d->colour = 1;
        d->constant = (f->master >> 14) == 1u ? 0xFFFFFFFFu : 0x000000FFu;
        d->blend = BG_BLEND_FADE;
        d->eva = (uint8_t)(f->master & 0x1Fu);
        if (d->eva > 16u) {
            d->eva = 16u;
        }
        quads++;
    }

    sQuads += (unsigned long)quads;
    return quads;
}

void gpu2d_end_frame(void)
{
    sDraw[0] = 0;
    sDraw[1] = 0;
    sClaim[0] = 0;
    sClaim[1] = 0;
}

void gpu2d_flush(void)
{
    int i;

    if (sPool != NULL) {
        if (sDirtyAll) {
            gpu_cache_flush(sPool, (size_t)sPoolSlots * TILE_TEXELS
                                       * sizeof *sPool);
        } else {
            for (i = 0; i < sDirtyN; i++) {
                gpu_cache_flush(sPool + (size_t)sDirty[i] * TILE_TEXELS,
                                TILE_TEXELS * sizeof *sPool);
            }
        }
        sDirtyN = 0;
        sDirtyAll = 0;
    }
    if (sVramDirty && sVramPool != NULL) {
        gpu_cache_flush(sVramPool, (size_t)sVramSide * (size_t)sVramSide
                                       * sizeof *sVramPool);
        sVramDirty = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#define CHECK(cond)                                                          \
    do {                                                                     \
        ran++;                                                               \
        if (!(cond)) {                                                       \
            fprintf(stderr, "  3ds_gpu2d.c:%d failed\n", __LINE__);           \
            bad++;                                                           \
        }                                                                    \
    } while (0)

int gpu2d_selftest(int *ranOut)
{
    int ran = 0, bad = 0;
    int stencil = sStencil;
    struct bg_frame f;

    /* Engine A, mode 0, one text layer, nothing else on: the state the whole
     * of the background path exists for. */
    memset(&f, 0, sizeof f);
    f.num = 0;
    f.dispcnt = (1u << 16)          /* display mode 1                     */
              | (1u << 8);          /* BG0 enabled                        */
    f.enable = 1u;
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == 0u);

    /* ...and each reason on its own, so a bit that stopped being tested shows
     * up here rather than as a subset that quietly grew. */
    CHECK(bg_classify(&f, 0, 0, 0, 0, 0, 0, 0u, NULL) == BG_R_OFF);
    CHECK(bg_classify(&f, 1, 0x80000000u, 0, 0, 0, 0, 0u, NULL)
          == BG_R_CAPTURE);
    CHECK(bg_classify(&f, 1, 0, 0x0011u, 0, 0, 0, 0u, NULL) == 0u);      /* not on BG0 */

    /*
     * The effect pass, and the first of these is the one worth having: an effect field
     * that names no ENABLED layer as a target is the identity over every
     * pixel, whatever the field says, and this game leaves one in the
     * register on 262 engine-frames. Only BG0 is on here, so a BLDCNT naming
     * nothing else changes nothing.
     */
    CHECK(bg_classify(&f, 1, 0, 0, 0x0042u, 0, 0, 0u, NULL) == 0u);
    /* ...and one that does name it needs the blend unit, which needs the
     * stencil bit that says what a pixel covers. */
    sStencil = 0;
    CHECK(bg_classify(&f, 1, 0, 0, 0x0241u, 0, 0, 0u, NULL) == BG_R_EFFECT);
    sStencil = 1;
    CHECK(bg_classify(&f, 1, 0, 0, 0x0241u, 0, 0, 0u, NULL) == 0u);
    /* Two enabled target-1 layers would blend one against the other's
     * already-blended pixel, which is not what draw_pixel() keeps. */
    f.enable = 3u;
    f.dispcnt |= 1u << 9;
    CHECK(bg_classify(&f, 1, 0, 0, 0x0243u, 0, 0, 0u, NULL) == BG_R_EFFECT);
    CHECK(bg_classify(&f, 1, 0, 0, 0x0242u, 0, 0, 0u, NULL) == 0u);
    f.enable = 1u;
    f.dispcnt &= ~(1u << 9);
    /* The two brightness effects fold into the palette, so they are not a
     * reason at all, and neither is the fade. */
    CHECK(bg_classify(&f, 1, 0, 0, 0x0081u, 0, 0, 0u, NULL) == 0u);
    CHECK(bg_classify(&f, 1, 0, 0, 0x00C1u, 0, 0, 0u, NULL) == 0u);
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0x4010u, 0, 0u, NULL) == 0u);
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0xC010u, 0, 0u, NULL) == 0u);      /* mode 3     */
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0x4000u, 0, 0u, NULL) == 0u);      /* factor 0   */

    f.bgcnt[0] = 1u << 6;
    CHECK(bg_classify(&f, 1, 0, 0x0011u, 0, 0, 0, 0u, NULL) == BG_R_MOSAIC);
    CHECK(bg_classify(&f, 1, 0, 0x0000u, 0, 0, 0, 0u, NULL) == 0u);      /* size zero  */
    f.bgcnt[0] = 0;

    f.dispcnt |= 1u << 12;                                     /* OBJ on     */
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 1, 0u, NULL) == BG_R_OBJ);
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == 0u);            /* none shown */
    /* The sprite path: a colour sprite this path can draw is not a reason, and it is a
     * blender, so OBJ and BG0 both named as target 1 is the two-blender
     * case, and OBJ as target 2 is not. */
    f.obj = 1;
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == 0u);
    CHECK(bg_classify(&f, 1, 0, 0, 0x0251u, 0, 0, 0u, NULL) == BG_R_EFFECT);
    CHECK(bg_classify(&f, 1, 0, 0, 0x0241u, 0, 0, 0u, NULL) == 0u);
    f.obj = 0;
    f.dispcnt &= ~(1u << 12);

    /*
     * The window rectangles go into the stencil, so without one they are a
     * reason and with one they are not. The OBJ window is neither: it is a
     * per-pixel mask the sprite unit draws, and the sprite path's.
     */
    f.dispcnt |= 0x2000u;
    sStencil = 0;
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == BG_R_WINDOW);
    sStencil = 1;
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == 0u);
    /* ...and one that runs the colour effect on part of the screen only is
     * refused: WININ says yes, WINOUT says no, and one layer would need two
     * palettes and two sets of quads. */
    f.wincnt[0] = 0x2Fu;
    f.wincnt[2] = 0x0Fu;
    CHECK(bg_classify(&f, 1, 0, 0, 0x0081u, 0, 0, 0u, NULL) == BG_R_EFFECT);
    f.wincnt[2] = 0x2Fu;
    CHECK(bg_classify(&f, 1, 0, 0, 0x0081u, 0, 0, 0u, NULL) == 0u);
    /* ...and with both regions refusing the effect it does not run, which is
     * not a reason either. */
    f.wincnt[0] = 0x0Fu;
    f.wincnt[2] = 0x0Fu;
    CHECK(bg_classify(&f, 1, 0, 0, 0x0081u, 0, 0, 0u, NULL) == 0u);
    f.wincnt[0] = 0u;
    f.wincnt[2] = 0u;
    f.dispcnt &= ~0x2000u;
    f.dispcnt |= 0x8000u;
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == BG_R_WINDOW);
    f.dispcnt &= ~0x8000u;

    f.dispcnt |= 8u;                                           /* BG0 is 3D  */
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == BG_R_3D);
    /*
     * The 3D layer: with a texture of the layer the reason goes away, and the one
     * thing that brings it back is the blend; which needs BOTH a translucent
     * pixel and a BLDCNT that admits the layer underneath. Neither alone is
     * one, and the effect field is not consulted, so 0x0100 with no effect
     * bits set is the case a test of BLDCNT's top two bits would miss.
     */
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, L3D_READY, NULL) == 0u);
    CHECK(bg_classify(&f, 1, 0, 0, 0x0100u, 0, 0, L3D_READY, NULL) == 0u);
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0,
                      L3D_READY | L3D_TRANSLUCENT, NULL) == 0u);
    CHECK(bg_classify(&f, 1, 0, 0, 0x0100u, 0, 0,
                      L3D_READY | L3D_TRANSLUCENT, NULL) == BG_R_3DBLEND);
    CHECK(bg_classify(&f, 1, 0, 0, 0x2000u, 0, 0,
                      L3D_READY | L3D_TRANSLUCENT, NULL) == BG_R_3DBLEND);
    /* ...and a target-1 bit is not a target-2 one. */
    CHECK(bg_classify(&f, 1, 0, 0, 0x0001u, 0, 0,
                      L3D_READY | L3D_TRANSLUCENT, NULL) == 0u);
    f.enable = 0u;                                             /* ...unshown */
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == 0u);
    f.enable = 1u;
    f.dispcnt &= ~8u;

    f.dispcnt |= 1u;                                           /* mode 1     */
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == BG_R_BGMODE);
    f.dispcnt &= ~7u;

    f.dispcnt |= 1u << 7;
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == 0u);            /* blank is a fill */
    CHECK(bg_classify(&f, 1, 0x80000000u, 0, 0, 0, 0, 0u, NULL)
          == 0u);                                                      /* ...and capture of one */
    f.dispcnt &= ~(1u << 7);
    f.dispcnt &= ~(3u << 16);                                          /* display off */
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == 0u);
    f.dispcnt |= 2u << 16;                                             /* VRAM display */
    {
        uint32_t *saved = sVramPool;

        CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == 0u);
        sVramPool = NULL;
        CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == BG_R_DISPMODE);
        sVramPool = saved;
    }
    f.dispcnt &= ~(3u << 16);
    f.dispcnt |= 3u << 16;                                             /* FIFO */
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == BG_R_DISPMODE);
    f.dispcnt &= ~(3u << 16);
    f.dispcnt |= 1u << 16;

    /* Engine B has neither the 3D layer nor the capture unit, and its display
     * mode field is one bit wide, so DISPCNT bit 17, which would be mode 2
     * or 3 on engine A, is not a mode on B at all. */
    f.num = 1;
    f.dispcnt |= 1u << 17;
    CHECK(bg_classify(&f, 1, 0x80000000u, 0, 0, 0, 1, 0u, NULL) == 0u);
    f.dispcnt &= ~(1u << 17);
    f.dispcnt |= 8u;
    CHECK(bg_classify(&f, 1, 0, 0, 0, 0, 0, 0u, NULL) == 0u);

    sStencil = stencil;
    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return bad;
}
