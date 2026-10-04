/*
 * Embedded public-domain 8x8 font (see font8x8.c for provenance). Shared by
 * the shell UI and the stub core so neither needs a font library.
 */
#ifndef NP_FONT8X8_H
#define NP_FONT8X8_H

#include <stdint.h>

#define NP_FONT_FIRST 0x20
#define NP_FONT_GLYPHS 96

extern const uint8_t np_font8x8[NP_FONT_GLYPHS][8];

/* Glyph rows for `c`; characters outside the table render as '?'. */
static inline const uint8_t *np_font_glyph(unsigned char c)
{
    if (c < NP_FONT_FIRST || c >= NP_FONT_FIRST + NP_FONT_GLYPHS)
        c = '?';
    return np_font8x8[c - NP_FONT_FIRST];
}

#endif
