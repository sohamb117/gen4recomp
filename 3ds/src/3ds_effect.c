/*
 * 3ds/src/3ds_effect.c: see 3ds_effect.h.
 *
 * Every expression here is pc/hw/pc_gpu2d.c's, COPIED. color_bright_up(),
 * color_bright_down(), color_blend4() and master_bright() are that file's
 * names for the four of them, and the packed-channel arithmetic with its
 * masks after the shift is what keeps three channels in one word without a
 * carry crossing between them. Copied rather than shared for the reason
 * 3ds_gpu2d.c gives for the register reads: that file is on the DS SDK's
 * include chain and this one is on the port's. Where the two disagree, this
 * one is wrong, and the check that says so is 3ds/tests/gpu2d_render.c;
 * which runs the real renderer over a scene with a fade on it and compares
 * every pixel.
 *
 * The two brightness functions are not the same function, which is the trap
 * in this file. The blend unit's rounds with a bias of 8 upward and 7
 * downward; the master fade rounds with no bias upward and +15 downward. They
 * differ on half of all inputs, and a frame with both a fade and a brightness
 * effect on it goes through them in that order.
 */

#include "3ds_effect.h"

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* The three of them                                                   */
/* ------------------------------------------------------------------ */

static uint32_t bright_up(uint32_t v, uint32_t factor, uint32_t bias)
{
    uint32_t rb = v & 0x3F003Fu, g = v & 0x003F00u;

    rb += ((((0x3F003Fu - rb) * factor) + (bias * 0x010001u)) >> 4) & 0x3F003Fu;
    g  += ((((0x003F00u - g)  * factor) + (bias * 0x000100u)) >> 4) & 0x003F00u;
    return rb | g;
}

static uint32_t bright_down(uint32_t v, uint32_t factor, uint32_t bias)
{
    uint32_t rb = v & 0x3F003Fu, g = v & 0x003F00u;

    rb -= (((rb * factor) + (bias * 0x010001u)) >> 4) & 0x3F003Fu;
    g  -= (((g  * factor) + (bias * 0x000100u)) >> 4) & 0x003F00u;
    return rb | g;
}

/* MASTER_BRIGHT, which is the display's and not the blend unit's: a different
 * pair of biases, and a factor clamped at the use rather than at the write. */
static uint32_t master_bright(uint32_t c, uint16_t reg)
{
    uint32_t mode = (uint32_t)reg >> 14, factor = reg & 0x1Fu;
    uint32_t r = c & 0x3Fu, g = (c >> 8) & 0x3Fu, b = (c >> 16) & 0x3Fu;

    if (factor > 16u) factor = 16u;
    if (mode == 1u) {
        r += ((0x3Fu - r) * factor) >> 4;
        g += ((0x3Fu - g) * factor) >> 4;
        b += ((0x3Fu - b) * factor) >> 4;
    } else if (mode == 2u) {
        /* The +15 is not symmetry with the other direction and is not
         * roundable away: without it a full-strength fade to black leaves
         * every channel one step short of 0. */
        r -= ((r * factor) + 15u) >> 4;
        g -= ((g * factor) + 15u) >> 4;
        b -= ((b * factor) + 15u) >> 4;
    } else {
        return c;
    }
    return r | (g << 8) | (b << 16);
}

/* ------------------------------------------------------------------ */
/* One colour, all the way through                                     */
/* ------------------------------------------------------------------ */

uint32_t effect_rgb6(uint16_t c15)
{
    uint32_t r = (uint32_t)(c15 & 0x001Fu) << 1;
    uint32_t g = ((uint32_t)(c15 & 0x03E0u) >> 4)
                 | ((uint32_t)(c15 & 0x8000u) >> 15);
    uint32_t b = (uint32_t)(c15 & 0x7C00u) >> 9;

    return r | (g << 8) | (b << 16);
}

uint32_t effect_rgb6_flat(uint16_t c15)
{
    uint32_t r = (uint32_t)(c15 & 0x001Fu) << 1;
    uint32_t g = (uint32_t)(c15 & 0x03E0u) >> 4;
    uint32_t b = (uint32_t)(c15 & 0x7C00u) >> 9;

    return r | (g << 8) | (b << 16);
}

int effect_is_identity(const struct effect *e)
{
    uint32_t mode = (uint32_t)e->master >> 14;

    if (e->kind != EFF_NONE) {
        return 0;
    }
    /* Mode 3 does nothing and a factor of zero is the identity in both
     * directions, so a register that is merely non-zero is not a fade. */
    return !((mode == 1u || mode == 2u) && (e->master & 0x1Fu) != 0u);
}

uint32_t effect_colour6(const struct effect *e, uint32_t bgr6)
{
    switch (e->kind) {
    case EFF_UP:
        bgr6 = bright_up(bgr6, e->evy, 8u);
        break;
    case EFF_DOWN:
        bgr6 = bright_down(bgr6, e->evy, 7u);
        break;
    default:
        break;
    }
    return master_bright(bgr6, e->master);
}

/* Six bits a channel out to eight, replicating the top two, pc_gpu2d.c's
 * expand(), and tile_colour()'s. */
static uint32_t expand(uint32_t c)
{
    uint32_t r = c & 0x3Fu, g = (c >> 8) & 0x3Fu, b = (c >> 16) & 0x3Fu;

    r = (r << 2) | (r >> 4);
    g = (g << 2) | (g >> 4);
    b = (b << 2) | (b >> 4);
    return (r << 16) | (g << 8) | b;
}

uint32_t effect_texel(const struct effect *e, uint16_t c15)
{
    return (expand(effect_colour6(e, effect_rgb6(c15))) << 8) | 0xFFu;
}

uint32_t effect_texel6(const struct effect *e, uint32_t bgr6)
{
    return (expand(effect_colour6(e, bgr6)) << 8) | 0xFFu;
}

uint32_t effect_texel_flat(const struct effect *e, uint16_t c15)
{
    return effect_texel6(e, effect_rgb6_flat(c15));
}

void effect_palette(uint32_t *out, const uint16_t *pal15, int entries,
                    const struct effect *e)
{
    int i;

    /* Index 0 is transparent in every tiled mode and the effect never sees it:
     * The DS skips the pixel rather than composing it, so there is nothing for
     * the blend unit to be applied to. */
    out[0] = 0u;
    for (i = 1; i < entries; i++) {
        out[i] = effect_texel(e, pal15[i]);
    }
}

uint32_t effect_id(const struct effect *e)
{
    if (effect_is_identity(e)) {
        return 0u;
    }
    /*
     * FNV-1a over every field that changes a colour. It is a cache key, not a
     * hash of GPU state: two effects with the same id produce the same
     * palette out of the same halfwords, and the identity has id zero so a
     * frame with no effect on it cannot collide with one that has.
     */
    {
        uint32_t h = 2166136261u;
        uint32_t f[3];
        int i;

        f[0] = e->kind;
        f[1] = e->evy;
        f[2] = e->master;
        for (i = 0; i < 3; i++) {
            h = (h ^ f[i]) * 16777619u;
        }
        return h != 0u ? h : 1u;
    }
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */
/*
 * The oracle is the renderer itself where one exists. Pc/hw/pc_gpu2d.c
 * exports pc_gpu2d_present_px(), the fade and the expansion, in one call,
 * for the wide margins outside the composed picture, and that is exactly
 * this file's job for a frame with no blend-unit effect on it. So the fade is
 * checked against the software renderer over every colour a palette can hold
 * rather than against a table written by hand. It is weak because the
 * diagnostic build links no pc/hw at all; there the sweep does not run and
 * the checks below still do.
 */
extern uint32_t pc_gpu2d_present_px(uint32_t bgr6, uint16_t reg) __attribute__((weak));

#define CHECK(cond)                                                        \
    do {                                                                   \
        ran++;                                                             \
        if (!(cond)) {                                                     \
            bad++;                                                         \
        }                                                                  \
    } while (0)

int effect_selftest(int *ranOut)
{
    static const uint16_t kFade[] = {
        0x0000u, 0x4001u, 0x4008u, 0x4010u, 0x8001u, 0x8008u, 0x8010u,
        0xC010u, 0x401Fu
    };
    struct effect e;
    int ran = 0, bad = 0;
    int i;

    /* The halfword, as six bits a channel. White is 62 and not 63: the low
     * bit of red and blue does not exist, and green's comes from bit 15. */
    CHECK(effect_rgb6(0x7FFFu) == (62u | (62u << 8) | (62u << 16)));
    CHECK(effect_rgb6(0xFFFFu) == (62u | (63u << 8) | (62u << 16)));
    CHECK(effect_rgb6(0x0000u) == 0u);
    CHECK(effect_rgb6(0x001Fu) == 62u);
    CHECK(effect_rgb6(0x7C00u) == (62u << 16));
    /* VRAM display drops bit 15, so 0xFFFF and 0x7FFF are the same colour. */
    CHECK(effect_rgb6_flat(0xFFFFu) == effect_rgb6(0x7FFFu));
    CHECK(effect_rgb6_flat(0x7FFFu) == effect_rgb6(0x7FFFu));

    /* The identity has to be tile_texel() exactly, or a frame with no effect
     * on it would differ from one this file drew. */
    e.kind = EFF_NONE;
    e.evy = 0u;
    e.master = 0u;
    CHECK(effect_is_identity(&e));
    CHECK(effect_id(&e) == 0u);
    CHECK(effect_texel(&e, 0x7FFFu) == 0xFBFBFBFFu);
    CHECK(effect_texel(&e, 0x0000u) == 0x000000FFu);
    /* A forced blank is 6-bit 0x3F, which expands to 0xFF, not a palette
     * 0x7FFF's 0xFB. */
    CHECK(effect_texel6(&e, 0x3F3F3Fu) == 0xFFFFFFFFu);

    /* Mode 3 is not a fade and a factor of zero is not one either. */
    e.master = 0xC010u;
    CHECK(effect_is_identity(&e));
    e.master = 0x4000u;
    CHECK(effect_is_identity(&e));
    e.master = 0x4001u;
    CHECK(!effect_is_identity(&e));

    /* Both ends of the fade, where the answer is a colour and not a rounding
     * argument: all the way up is white and all the way down is black. */
    e.master = 0x4010u;
    CHECK(effect_texel(&e, 0x0000u) == 0xFFFFFFFFu);
    e.master = 0x8010u;
    CHECK(effect_texel(&e, 0x7FFFu) == 0x000000FFu);
    /* ...and the +15 the downward one carries, which is what makes that true:
     * without it a full fade leaves every channel one step short. */
    e.master = 0x8010u;
    CHECK(effect_texel(&e, 0x0421u) == 0x000000FFu);

    /* The blend unit's two brightness effects, at their ends. */
    e.master = 0u;
    e.kind = EFF_UP;
    e.evy = 16u;
    CHECK(effect_texel(&e, 0x0000u) == 0xFFFFFFFFu);
    e.evy = 0u;
    CHECK(effect_texel(&e, 0x3DEFu) == effect_texel(&(struct effect){
              EFF_NONE, 0u, 0u }, 0x3DEFu));
    e.kind = EFF_DOWN;
    e.evy = 16u;
    CHECK(effect_texel(&e, 0x7FFFu) == 0x000000FFu);
    e.evy = 0u;
    CHECK(effect_texel(&e, 0x3DEFu) == effect_texel(&(struct effect){
              EFF_NONE, 0u, 0u }, 0x3DEFu));

    /* A transform is only the same transform if every field is: two ids that
     * agreed would put one layer's colours in another layer's palette. */
    {
        struct effect a = { EFF_UP, 6u, 0x4008u };
        struct effect b = { EFF_DOWN, 6u, 0x4008u };
        struct effect c = { EFF_UP, 7u, 0x4008u };
        struct effect d = { EFF_UP, 6u, 0x4009u };

        CHECK(effect_id(&a) != effect_id(&b));
        CHECK(effect_id(&a) != effect_id(&c));
        CHECK(effect_id(&a) != effect_id(&d));
        CHECK(effect_id(&a) == effect_id(&a));
    }

    /*
     * And the fade against the renderer's own, over every colour a palette
     * can hold and every fade this game can write. 32,768 halfwords times
     * nine registers is one check, because a count of 294,912 would drown the
     * others; what matters is that the first disagreement fails it.
     */
    if (pc_gpu2d_present_px != NULL) {
        int wrong = 0;
        unsigned c;

        for (i = 0; i < (int)(sizeof kFade / sizeof kFade[0]); i++) {
            struct effect f;

            f.kind = EFF_NONE;
            f.evy = 0u;
            f.master = kFade[i];
            for (c = 0u; c < 0x10000u; c += 1u) {
                uint32_t mine = effect_texel(&f, (uint16_t)c) >> 8;
                uint32_t theirs = pc_gpu2d_present_px(effect_rgb6((uint16_t)c),
                                                      kFade[i]);

                if (mine != theirs) {
                    wrong++;
                    break;
                }
            }
        }
        CHECK(wrong == 0);
    }

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return bad;
}
