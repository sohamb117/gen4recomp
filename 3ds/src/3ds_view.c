/*
 * 3ds/src/3ds_view.c: surface -> LCD, and the debug font that rides in the
 * letterbox.
 *
 * See 3ds_view.h for the rotation rule and why every entry point takes the
 * *display* width and height. Everything except `view_present` is pure C over
 * caller buffers so `3ds/tests/view_dump.c` can link it natively and dump the
 * same picture as a PNG; which is the gate while there is no emulator on
 * this machine.
 *
 * THE FONT is 5x7 in a 6x8 cell, hand-drawn here rather than borrowed: the
 * only alternative in reach was libctru's console, and a console owns the
 * whole screen (it reformats it to RGB565 and clears it), which is exactly
 * what a blit target cannot have. Rows are five bits, bit 4 leftmost, drawn
 * top to bottom. Binary literals are a GNU extension that both compilers on
 * this port accept; a hand-drawn font written in hex is a font nobody can
 * check by reading.
 *
 * Glyphs exist for digits, uppercase and a little punctuation, what a hex
 * dump of a register needs. Lowercase folds to uppercase. Anything else draws
 * a hollow box, so a character this font is missing shows up in a photograph
 * instead of vanishing.
 */

#include "3ds_view.h"

#include <string.h>

#ifdef __3DS__
#include <3ds.h>
#endif

/* ------------------------------------------------------------------ */
/* 5x7 font                                                            */
/* ------------------------------------------------------------------ */

#define FONT_FIRST 0x20
#define FONT_LAST  0x7E
#define FONT_COUNT (FONT_LAST - FONT_FIRST + 1)

#define G(c) [(c) - FONT_FIRST]

static const uint8_t kFont[FONT_COUNT][VIEW_GLYPH_HEIGHT] = {
    G(' ') = { 0, 0, 0, 0, 0, 0, 0 },

    G('0') = { 0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110 },
    G('1') = { 0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 },
    G('2') = { 0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111 },
    G('3') = { 0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110 },
    G('4') = { 0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010 },
    G('5') = { 0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110 },
    G('6') = { 0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110 },
    G('7') = { 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000 },
    G('8') = { 0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110 },
    G('9') = { 0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100 },

    G('A') = { 0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001 },
    G('B') = { 0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110 },
    G('C') = { 0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110 },
    G('D') = { 0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100 },
    G('E') = { 0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111 },
    G('F') = { 0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000 },
    G('G') = { 0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111 },
    G('H') = { 0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001 },
    G('I') = { 0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 },
    G('J') = { 0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100 },
    G('K') = { 0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001 },
    G('L') = { 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111 },
    G('M') = { 0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001 },
    G('N') = { 0b10001, 0b11001, 0b11001, 0b10101, 0b10011, 0b10011, 0b10001 },
    G('O') = { 0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 },
    G('P') = { 0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000 },
    G('Q') = { 0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101 },
    G('R') = { 0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001 },
    G('S') = { 0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110 },
    G('T') = { 0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100 },
    G('U') = { 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 },
    G('V') = { 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100 },
    G('W') = { 0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010 },
    G('X') = { 0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001 },
    G('Y') = { 0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100 },
    G('Z') = { 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111 },

    G('-') = { 0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000 },
    G('+') = { 0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000 },
    G('=') = { 0b00000, 0b00000, 0b11111, 0b00000, 0b11111, 0b00000, 0b00000 },
    G('.') = { 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100 },
    G(',') = { 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b00100, 0b01000 },
    G(':') = { 0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000 },
    G('/') = { 0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000 },
    G('(') = { 0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010 },
    G(')') = { 0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000 },
    G('*') = { 0b00000, 0b10101, 0b01110, 0b11111, 0b01110, 0b10101, 0b00000 },
    G('!') = { 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100 },
    G('?') = { 0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100 },
    G('#') = { 0b01010, 0b11111, 0b01010, 0b01010, 0b01010, 0b11111, 0b01010 },
    G('<') = { 0b00010, 0b00100, 0b01000, 0b10000, 0b01000, 0b00100, 0b00010 },
    G('>') = { 0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000 },
    G('[') = { 0b01110, 0b01000, 0b01000, 0b01000, 0b01000, 0b01000, 0b01110 },
    G(']') = { 0b01110, 0b00010, 0b00010, 0b00010, 0b00010, 0b00010, 0b01110 },
    G('_') = { 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11111 },
};

/* Drawn for any character the table has no glyph for. */
static const uint8_t kMissing[VIEW_GLYPH_HEIGHT] = {
    0b11111, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11111
};

static const uint8_t *font_glyph(unsigned char c)
{
    const uint8_t *g;
    int i;

    if (c >= 'a' && c <= 'z') {
        c = (unsigned char)(c - 'a' + 'A');
    }
    if (c < FONT_FIRST || c > FONT_LAST) {
        return kMissing;
    }

    g = kFont[c - FONT_FIRST];
    if (c == ' ') {
        return g;
    }

    /* An undefined entry is all zeroes, which would draw nothing at all. */
    for (i = 0; i < VIEW_GLYPH_HEIGHT; i++) {
        if (g[i] != 0) {
            return g;
        }
    }
    return kMissing;
}

/* ------------------------------------------------------------------ */
/* Display buffer                                                      */
/* ------------------------------------------------------------------ */

void view_put(uint8_t *fb, int sw, int sh, int x, int y, uint32_t rgb)
{
    uint8_t *p;

    if (x < 0 || y < 0 || x >= sw || y >= sh) {
        return;
    }

    p = fb + ((size_t)x * (size_t)sh + (size_t)(sh - 1 - y)) * 3;
    p[0] = (uint8_t)(rgb & 0xFFu);
    p[1] = (uint8_t)((rgb >> 8) & 0xFFu);
    p[2] = (uint8_t)((rgb >> 16) & 0xFFu);
}

/*
 * view_put's inverse. It exists for the font sheet in 3ds_gpu.c, which is
 * built by drawing every glyph with view_text() and then reading the pixels
 * back out, one font in this port, not two.
 */
uint32_t view_get(const uint8_t *fb, int sw, int sh, int x, int y)
{
    const uint8_t *p;

    if (x < 0 || y < 0 || x >= sw || y >= sh) {
        return 0;
    }

    p = fb + ((size_t)x * (size_t)sh + (size_t)(sh - 1 - y)) * 3;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

void view_clear(uint8_t *fb, int sw, int sh, uint32_t rgb)
{
    size_t n = (size_t)sw * (size_t)sh;
    uint8_t b = (uint8_t)(rgb & 0xFFu);
    uint8_t g = (uint8_t)((rgb >> 8) & 0xFFu);
    uint8_t r = (uint8_t)((rgb >> 16) & 0xFFu);
    size_t i;

    /* The rotation does not matter for a flat fill: every byte gets written. */
    if (b == g && g == r) {
        memset(fb, b, n * 3);
        return;
    }
    for (i = 0; i < n; i++) {
        fb[i * 3 + 0] = b;
        fb[i * 3 + 1] = g;
        fb[i * 3 + 2] = r;
    }
}

void view_blit(uint8_t *fb, int sw, int sh,
               const uint32_t *px, int w, int h)
{
    int ox, oy, sx, sy;

    /* No surface is a screen with nothing on it but its letterbox: the hang
     * detector presents its message that way, and reading through the NULL
     * would be a fault delivered while diagnosing a fault. */
    if (px == NULL || w > sw || h > sh || w <= 0 || h <= 0) {
        return;
    }

    ox = (sw - w) / 2;
    oy = (sh - h) / 2;

    /*
     * Column-major, because the framebuffer is: one source column is one run
     * of descending addresses, so the writes stay together even though the
     * reads stride by `w`.
     */
    for (sx = 0; sx < w; sx++) {
        uint8_t *dst = fb + ((size_t)(ox + sx) * (size_t)sh
                             + (size_t)(sh - 1 - oy)) * 3;
        const uint32_t *src = px + sx;

        for (sy = 0; sy < h; sy++) {
            uint32_t c = src[(size_t)sy * (size_t)w];

            dst[0] = (uint8_t)(c & 0xFFu);
            dst[1] = (uint8_t)((c >> 8) & 0xFFu);
            dst[2] = (uint8_t)((c >> 16) & 0xFFu);
            dst -= 3;
        }
    }
}

void view_text(uint8_t *fb, int sw, int sh, int x, int y,
               const char *s, uint32_t rgb)
{
    if (s == NULL) {
        return;
    }

    for (; *s != '\0'; s++, x += VIEW_GLYPH_ADVANCE) {
        const uint8_t *g = font_glyph((unsigned char)*s);
        int row, col;

        if (x >= sw) {
            return;
        }
        for (row = 0; row < VIEW_GLYPH_HEIGHT; row++) {
            for (col = 0; col < VIEW_GLYPH_WIDTH; col++) {
                if (g[row] & (0x10u >> col)) {
                    view_put(fb, sw, sh, x + col, y + row, rgb);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Test pattern                                                        */
/* ------------------------------------------------------------------ */

void view_test_pattern(uint32_t *px, int w, int h,
                       uint32_t body, uint32_t edge, uint32_t mark)
{
    int x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            px[(size_t)y * (size_t)w + (size_t)x] = body;
        }
    }

    for (x = 0; x < w; x++) {
        px[x] = edge;
        px[(size_t)(h - 1) * (size_t)w + (size_t)x] = edge;
    }
    for (y = 0; y < h; y++) {
        px[(size_t)y * (size_t)w] = edge;
        px[(size_t)y * (size_t)w + (size_t)(w - 1)] = edge;
    }

    /* The orientation marker sits at the surface's own (0,0). */
    for (y = 0; y < VIEW_MARK_SIZE && y < h; y++) {
        for (x = 0; x < VIEW_MARK_SIZE && x < w; x++) {
            px[(size_t)y * (size_t)w + (size_t)x] = mark;
        }
    }
}

/* ------------------------------------------------------------------ */
/* libctru side                                                        */
/* ------------------------------------------------------------------ */

/*
 * One screen, the whole of it: letterbox, picture, and the two debug lines in
 * the bands. It is the CPU present path, and since the GPU present it is also the oracle
 * that path is checked against, PRESENT_VERIFY in 3ds_gpu.c composes a
 * frame with this function and compares it with what the PICA left in the
 * framebuffer. Pure C over a caller's buffer, so the host harness can call it
 * too.
 */
void view_compose(uint8_t *fb, int sw, int sh, const uint32_t *px,
                  const char *line_top, const char *line_bottom)
{
    int tx = (sw - VIEW_DS_WIDTH) / 2;

    view_clear(fb, sw, sh, VIEW_COLOUR_LETTERBOX);
    view_blit(fb, sw, sh, px, VIEW_DS_WIDTH, VIEW_DS_HEIGHT);
    view_text(fb, sw, sh, tx, VIEW_TEXT_TOP_Y, line_top, VIEW_COLOUR_TEXT);
    view_text(fb, sw, sh, tx, VIEW_TEXT_BOTTOM_Y, line_bottom, VIEW_COLOUR_TEXT);
}

#ifdef __3DS__

void view_present(const uint32_t *top, const uint32_t *bottom,
                  const char *line_top, const char *line_bottom)
{
    u16 fbw, fbh;
    u8 *fb;

    /*
     * The one place the swap happens: gfxGetFramebuffer's "width" is the
     * short axis (240) and its "height" is the long one (400 / 320); the
     * display is the other way round.
     */
    fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbw, &fbh);
    view_compose(fb, fbh, fbw, top, line_top, line_bottom);

    fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbw, &fbh);
    view_compose(fb, fbh, fbw, bottom, line_top, line_bottom);
}

#endif /* __3DS__ */
