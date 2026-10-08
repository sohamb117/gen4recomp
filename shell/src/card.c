/*
 * Trainer Card / Pokedex diploma renderer (see card.h). Everything is drawn
 * with a handful of primitives (rectangles, frames, discs, scaled 8x8 text)
 * straight into the caller's buffer, so exporting needs no renderer, no
 * window and no allocation beyond that buffer.
 */
#include "card.h"

#include <stdio.h>
#include <string.h>

#include "font8x8.h"

typedef struct canvas {
    uint32_t *px;
} canvas;

static void fill(canvas *c, int x, int y, int w, int h, uint32_t color)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > NP_CARD_W ? NP_CARD_W : x + w, y1 = y + h > NP_CARD_H ? NP_CARD_H : y + h;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            c->px[yy * NP_CARD_W + xx] = color;
}

static void frame(canvas *c, int x, int y, int w, int h, int t, uint32_t color)
{
    fill(c, x, y, w, t, color);
    fill(c, x, y + h - t, w, t, color);
    fill(c, x, y, t, h, color);
    fill(c, x + w - t, y, t, h, color);
}

/* A panel with corners cut by `r` pixels, which reads as rounded at this size. */
static void panel(canvas *c, int x, int y, int w, int h, int r, uint32_t color)
{
    for (int i = 0; i < r; i++) {
        int inset = r - i;
        fill(c, x + inset, y + i, w - 2 * inset, 1, color);
        fill(c, x + inset, y + h - 1 - i, w - 2 * inset, 1, color);
    }
    fill(c, x, y + r, w, h - 2 * r, color);
}

static void disc(canvas *c, int cx, int cy, int r, uint32_t color)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                fill(c, cx + dx, cy + dy, 1, 1, color);
}

/* Next character of a UTF-8 string mapped into the font's ASCII range: the
 * accented letters the games use become their base letter, the rest '?'. */
static unsigned char next_char(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned char ch = *p++;
    if (ch >= 0x80) {
        unsigned cp = 0;
        int extra = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : 1;
        cp = ch & (0x3F >> extra);
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; i++)
            cp = cp << 6 | (*p++ & 0x3F);
        if (cp >= 0xE8 && cp <= 0xEB)
            ch = 'e';
        else if (cp >= 0xC8 && cp <= 0xCB)
            ch = 'E';
        else if (cp == 0x2642)
            ch = 'M'; /* male sign */
        else if (cp == 0x2640)
            ch = 'F'; /* female sign */
        else
            ch = '?';
    }
    *s = (const char *)p;
    return ch;
}

static int text_len(const char *s)
{
    int n = 0;
    while (*s) {
        next_char(&s);
        n++;
    }
    return n;
}

/* Draws at most `max` characters of `s` with glyphs `scale` pixels per font
 * pixel and a one-pixel-per-scale drop shadow when `shadow` is not 0. */
static void text(canvas *c, int x, int y, const char *s, int scale, uint32_t color, uint32_t shadow, int max)
{
    for (int i = 0; *s && i < max; i++, x += 8 * scale) {
        const uint8_t *g = np_font_glyph(next_char(&s));
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] >> col & 1) {
                    if (shadow)
                        fill(c, x + col * scale + scale / 2 + 1, y + row * scale + scale / 2 + 1, scale, scale,
                             shadow);
                    fill(c, x + col * scale, y + row * scale, scale, scale, color);
                }
    }
}

static void text_centered(canvas *c, int cx, int y, const char *s, int scale, uint32_t color, uint32_t shadow)
{
    text(c, cx - text_len(s) * 4 * scale, y, s, scale, color, shadow, 96);
}

static void text_right(canvas *c, int right, int y, const char *s, int scale, uint32_t color, uint32_t shadow)
{
    text(c, right - text_len(s) * 8 * scale, y, s, scale, color, shadow, 96);
}

/* ---- trainer card ----------------------------------------------------------- */

/* Card colour by badges earned, our own progression from slate to gold. */
static void tier_colors(int badges, uint32_t *base, uint32_t *light)
{
    static const uint32_t bases[5] = {0x3E5A86, 0x2F7A55, 0x8A5A2E, 0x6F7A88, 0xA88420};
    static const uint32_t lights[5] = {0x7D9CCB, 0x6DB892, 0xC9945E, 0xB4BECB, 0xE6C65A};
    int t = badges >= 8 ? 4 : badges / 2;
    *base = bases[t];
    *light = lights[t];
}

static const char *const sinnoh_badges[8] = {"Coal", "Forest", "Cobble", "Fen", "Relic", "Mine", "Icicle", "Beacon"};
static const uint32_t badge_colors[8] = {0x5A5A5A, 0x3FA34D, 0xC9853A, 0x6FA8DC,
                                         0xC9A23A, 0x8C6E4E, 0x9AD7F0, 0xF0D040};

static void info_row(canvas *c, int y, const char *label, const char *value)
{
    text(c, 60, y, label, 2, 0xFFFFFF, 0x202020, 20);
    text(c, 240, y, value, 2, 0xFFFFFF, 0x202020, 20);
}

static void render_trainer(canvas *c, const np_card_info *in)
{
    int nbadges = 0;
    for (int b = 0; b < 8; b++)
        nbadges += in->badges >> b & 1;
    uint32_t base, light;
    tier_colors(nbadges, &base, &light);

    fill(c, 0, 0, NP_CARD_W, NP_CARD_H, 0x181C26);
    panel(c, 24, 24, 720, 528, 18, 0x0E1016);
    panel(c, 28, 28, 712, 520, 16, base);
    fill(c, 28, 104, 712, 4, light);
    text(c, 52, 48, "TRAINER CARD", 4, 0xFFFFFF, 0x101010, 12);
    char game[48];
    snprintf(game, sizeof game, "Pokemon %s", in->game ? in->game : "");
    text_right(c, 716, 62, game, 2, light, 0x101010);

    /* Details on the left, an initial "portrait" on the right. */
    panel(c, 44, 124, 456, 212, 10, (base & 0xFEFEFE) >> 1);
    char v[64];
    info_row(c, 140, "NAME", in->name);
    snprintf(v, sizeof v, "%05u", in->tid);
    info_row(c, 180, "ID No.", v);
    snprintf(v, sizeof v, "$%u", (unsigned)in->money);
    info_row(c, 220, "MONEY", v);
    snprintf(v, sizeof v, "%u (seen %u)", in->dex_caught, in->dex_seen);
    info_row(c, 260, "POKEDEX", v);
    snprintf(v, sizeof v, "%u:%02u", in->play_hours, in->play_minutes);
    info_row(c, 300, "TIME", v);

    uint32_t gender = in->female ? 0xE0607A : 0x4A86E0;
    panel(c, 524, 124, 196, 212, 10, gender);
    panel(c, 532, 132, 180, 196, 8, 0xF4F4F4);
    const char *name = in->name;
    char initial[2] = {(char)next_char(&name), 0};
    if (!initial[0])
        initial[0] = '?';
    text_centered(c, 622, 172, initial, 12, gender, 0xC8C8C8);

    /* Badge case. */
    panel(c, 44, 352, 676, 92, 10, (base & 0xFEFEFE) >> 1);
    const char *const *badge_names = in->badge_names ? in->badge_names : sinnoh_badges;
    for (int b = 0; b < 8; b++) {
        int cx = 86 + b * 84, cy = 386;
        bool have = in->badges >> b & 1;
        disc(c, cx, cy, 26, have ? 0x101010 : (base & 0xFCFCFC) >> 2);
        disc(c, cx, cy, 23, have ? badge_colors[b] : (base & 0xFEFEFE) >> 1);
        if (have)
            disc(c, cx - 8, cy - 8, 6, 0xFFFFFF);
        char l[2] = {badge_names[b][0], 0};
        text_centered(c, cx, cy - 8, l, 2, have ? 0xFFFFFF : light, have ? 0x101010 : 0);
        text_centered(c, cx, 420, badge_names[b], 1, have ? 0xFFFFFF : light, 0);
    }

    /* Party, two rows of three. */
    text(c, 48, 458, "PARTY", 2, light, 0x101010, 5);
    for (int i = 0; i < in->party_count && i < NP_CARD_PARTY_MAX; i++) {
        int x = 48 + (i % 3) * 228, y = 484 + (i / 3) * 28;
        disc(c, x + 8, y + 7, 7, 0xF0F0F0);
        fill(c, x + 1, y + 6, 15, 2, 0x101010);
        disc(c, x + 8, y + 7, 3, 0x101010);
        disc(c, x + 8, y + 7, 2, 0xF0F0F0);
        /* Names fit 10 characters; the level goes after in small type. */
        text(c, x + 22, y, in->party[i], 2, 0xFFFFFF, 0x101010, 10);
        if (in->party_level[i]) {
            char lv[8];
            snprintf(lv, sizeof lv, "Lv%u", in->party_level[i]);
            int len = text_len(in->party[i]);
            text(c, x + 22 + (len < 10 ? len : 10) * 16 + 4, y + 7, lv, 1, light, 0x101010, 5);
        }
    }
    if (!in->party_count)
        text(c, 48, 484, "(none yet)", 2, light, 0, 12);
    text_right(c, 728, 534, "nativeplat", 1, light, 0);
}

/* ---- diploma ----------------------------------------------------------------- */

static void render_diploma(canvas *c, const np_card_info *in)
{
    const uint32_t paper = 0xF5EDD6, ink = 0x3A2A1A, gold = 0xC79A2C, red = 0xB8333A;
    fill(c, 0, 0, NP_CARD_W, NP_CARD_H, 0x2A2118);
    fill(c, 16, 16, NP_CARD_W - 32, NP_CARD_H - 32, paper);
    frame(c, 28, 28, NP_CARD_W - 56, NP_CARD_H - 56, 6, gold);
    frame(c, 40, 40, NP_CARD_W - 80, NP_CARD_H - 80, 2, ink);
    for (int i = 0; i < 4; i++) /* corner ornaments */
        disc(c, i & 1 ? NP_CARD_W - 40 : 40, i & 2 ? NP_CARD_H - 40 : 40, 12, gold);

    const int mid = NP_CARD_W / 2;
    text_centered(c, mid, 70, "POKEDEX DIPLOMA", 4, ink, 0xD8CBAA);
    fill(c, mid - 220, 112, 440, 3, gold);
    char line[96];
    snprintf(line, sizeof line, "Pokemon %s", in->game ? in->game : "");
    text_centered(c, mid, 126, line, 2, gold, 0);

    text_centered(c, mid, 176, "This certifies that", 2, ink, 0);
    text_centered(c, mid, 206, in->name, 5, ink, 0xD8CBAA);
    snprintf(line, sizeof line, "(ID No. %05u)", in->tid);
    text_centered(c, mid, 256, line, 2, ink, 0);
    snprintf(line, sizeof line, "has seen %u and caught %u Pokemon", in->dex_seen, in->dex_caught);
    text_centered(c, mid, 296, line, 2, ink, 0);
    text_centered(c, mid, 322, in->national_dex ? "in the National Pokedex." : "on the way to the National Pokedex.", 2,
                  ink, 0);

    /* Caught out of the generation's species (493 in Gen IV, 649 in Gen V). */
    unsigned total = in->dex_total ? in->dex_total : 493;
    int bar_w = 520, filled = in->dex_caught > total ? bar_w : bar_w * (int)in->dex_caught / (int)total;
    frame(c, mid - bar_w / 2 - 4, 360, bar_w + 8, 28, 2, ink);
    fill(c, mid - bar_w / 2, 364, filled, 20, red);
    snprintf(line, sizeof line, "%u / %u", in->dex_caught, total);
    text_centered(c, mid, 400, line, 2, ink, 0);

    snprintf(line, sizeof line, "Awarded %04d-%02d-%02d", in->year, in->month, in->day);
    text(c, 76, 470, line, 2, ink, 0, 40);
    fill(c, 76, 500, 260, 2, ink);
    text(c, 76, 508, "nativeplat", 2, ink, 0, 20);

    disc(c, 640, 470, 52, gold); /* seal */
    disc(c, 640, 470, 44, red);
    disc(c, 640, 470, 40, gold);
    disc(c, 640, 470, 36, red);
    snprintf(line, sizeof line, "%u", in->dex_caught);
    text_centered(c, 640, 452, "CAUGHT", 1, paper, 0);
    text_centered(c, 640, 466, line, 3, paper, 0x6E1A1F);
}

void np_card_render(np_card_kind kind, const np_card_info *info, uint32_t *px)
{
    canvas c = {px};
    if (kind == NP_CARD_DIPLOMA)
        render_diploma(&c, info);
    else
        render_trainer(&c, info);
}
