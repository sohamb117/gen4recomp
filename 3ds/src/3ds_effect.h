/*
 * 3ds/src/3ds_effect.h: the DS's colour effects, as a function of one colour.
 *
 * What the blend unit does to a pixel, and the whole reason this file is
 * shaped like a palette transform rather than like GPU state. pc_gpu2d.c
 * composes a scanline by storing the top pixel and the one it covered, then
 * runs color_composite() over the pair. Three of its four effects, brightness
 * up, brightness down, and the master fade every frame ends with, read only
 * the top pixel. A function of one colour applied to every pixel a layer draws
 * is a function of that layer's palette, and a palette is something the tile
 * cache already keys on: give it a different table and it expands the tile
 * again, into a different slot, and the compositor draws the effect without
 * doing any arithmetic at all.
 *
 * That is not a shortcut, it is the only exact route. The DS works in six bits
 * a channel and rounds at six; the PICA's blend unit and its TEV stages work
 * in eight, and 63/16 of a step is not representable there. A fade done on the
 * GPU is off by a step on a good fraction of all pixels, small enough to
 * photograph as correct and far too large for a byte compare. Done through the
 * palette it is not off at all, because the arithmetic is the software
 * renderer's own, on the CPU, in six bits.
 *
 * The cost is re-expansion, and it is real: a fade changes the palette every
 * frame, so every tile on screen is expanded again on every frame of it. That
 * is the trade this file makes on purpose, and `bg-tiles expanded` in the
 * frame-time report says what it cost.
 *
 * Not here: the alpha blend between two layers, whose second operand is
 * another layer's pixel and therefore not a constant. That runs on the PICA's
 * blend unit, where a stencil bit says whether the pixel underneath is a layer
 * BLDCNT admits, and where the coefficients are 255ths rather than sixteenths,
 * which is the one part a byte compare cannot close on. The survey says it is
 * also the only effect this game asks for: over the new-game replay, BLDCNT's
 * field is 1 on every engine-frame that has one at all, and the two this file
 * exists for never fire. What does fire is the fade.
 */

#ifndef POKEPLATINUM_3DS_EFFECT_H
#define POKEPLATINUM_3DS_EFFECT_H

#include <stdint.h>

/*
 * BLDCNT bits 6-7, and the same numbering color_composite_at() switches on.
 * EFF_ALPHA is 1 and is not in this file: its second operand is another
 * layer's pixel, so it is not a function of one colour and it is the PICA's
 * blend unit that runs it (3ds_gpu.c). The value is named so that a BLDCNT
 * field can be compared against it.
 */
enum {
    EFF_NONE  = 0,
    EFF_ALPHA = 1,
    EFF_UP    = 2,
    EFF_DOWN  = 3
};

/*
 * One layer's colour pipeline for one frame: at most one blend-unit effect,
 * then the master fade the whole panel gets. Both may be identities, in which
 * case effect_is_identity() is true and the caller must take the plain path,
 * not because this file would give a different answer, but because a frame
 * that needs no transform must not pay for a second set of palettes.
 */
struct effect {
    uint8_t kind;       /* EFF_NONE, EFF_UP or EFF_DOWN, resolved already */
    uint8_t evy;        /* ...its factor, 0 to 16                         */
    uint16_t master;    /* MASTER_BRIGHT, raw, applied last               */
};

int effect_is_identity(const struct effect *e);

/*
 * pc_gpu2d.c's rgb15to18(): a palette halfword as six bits a channel, packed
 * r | g << 8 | b << 16, with the halfword's top bit as green's low bit.
 */
uint32_t effect_rgb6(uint16_t c15);

/*
 * The same conversion without the extra green bit. DISPCNT's VRAM display
 * mode scans a bank as a picture, and there bit 15 is unused, rgb15to18_flat
 * in pc_gpu2d.c, which differs by one step on half of all halfwords.
 */
uint32_t effect_rgb6_flat(uint16_t c15);

/*
 * The effect, then the master fade, over one six-bit colour. This is
 * color_composite_at() and master_bright() for the two effects that read one
 * colour, and it must agree with them to the step.
 */
uint32_t effect_colour6(const struct effect *e, uint32_t bgr6);

/* ...and the whole way from a palette halfword to a texel, 0xRRGGBBAA opaque:
 * tile_texel() with the effect in the middle. */
uint32_t effect_texel(const struct effect *e, uint16_t c15);

/* A six-bit colour that did not come from a palette halfword, the forced
 * blank's 0x3F3F3F, which is full white rather than a 5-bit 0x7FFF. */
uint32_t effect_texel6(const struct effect *e, uint32_t bgr6);

/* VRAM display: bit 15 unused, then the fade, then the eight-bit texel. */
uint32_t effect_texel_flat(const struct effect *e, uint16_t c15);

/* ...and over a whole palette, index 0 transparent, which is tile_palette()
 * when the effect is the identity. */
void effect_palette(uint32_t *out, const uint16_t *pal15, int entries,
                    const struct effect *e);

/*
 * The transform as a number, so a palette table cached under a source address
 * can be told apart from the same source read for a layer the effect does not
 * apply to. Zero for the identity.
 */
uint32_t effect_id(const struct effect *e);

/* Self-test, the same shape every model in this port carries. */
int effect_selftest(int *ranOut);

#endif /* POKEPLATINUM_3DS_EFFECT_H */
