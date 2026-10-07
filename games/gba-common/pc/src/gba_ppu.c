/*
 * GBA picture processing, one scanline at a time from the registers, VRAM,
 * palette and OAM as they stand when the line is drawn (gba_step_line):
 * text and affine backgrounds, bitmap modes 3-5, regular and affine
 * sprites, windows 0/1/OBJ, colour special effects and mosaic.
 *
 * Written for the nativeplat GBA guest after the GBATEK description of the
 * hardware; the same BG/OBJ/window/blend model as the DS 2D engine
 * (games/platinum/pc/hw/pc_gpu2d.c) with the GBA's register layout.
 */
#include <string.h>

#include "gba_port.h"

uint32_t gba_screen[GBA_H * GBA_W];

#define VRAM8 ((const uint8_t *)(uintptr_t)GBA_VRAM)
#define PAL16 ((const uint16_t *)(uintptr_t)GBA_PAL)
#define OAM16 ((const uint16_t *)(uintptr_t)GBA_OAM)

/* a layer pixel: BGR555 in bits 0-14, OPAQUE set when drawn */
#define OPAQUE 0x8000u
#define SEMI 0x10000u /* OBJ pixel of a semi-transparent sprite */

static uint32_t bgline[4][GBA_W];
static uint32_t objline[GBA_W];
static uint8_t objprio[GBA_W];
static uint8_t objwin[GBA_W];

/* internal affine reference points (BG2, BG3), 20.8 fixed */
static int32_t aff_x[2], aff_y[2];

static int32_t sext28(uint32_t v) { return (int32_t)(v << 4) >> 4; }

void gba_ppu_reload_affine(int bg) {
    uint32_t base = bg == 2 ? R_BG2X : R_BG3X;
    aff_x[bg - 2] = sext28(IO32(base));
    aff_y[bg - 2] = sext28(IO32(base + 4));
}

void gba_ppu_vblank(void) {
    gba_ppu_reload_affine(2);
    gba_ppu_reload_affine(3);
}

static uint32_t rgb(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return r << 16 | g << 8 | b;
}

/* ------------------------------------------------------------ backgrounds */

static void bg_text(int bg, int y) {
    uint16_t cnt = IO16(R_BG0CNT + 2 * bg);
    uint32_t hofs = IO16(R_BG0HOFS + 4 * bg) & 0x1FF, vofs = IO16(R_BG0HOFS + 4 * bg + 2) & 0x1FF;
    uint32_t chr = ((cnt >> 2) & 3) * 0x4000u, scr = ((cnt >> 8) & 31) * 0x800u;
    int bpp8 = (cnt >> 7) & 1, size = cnt >> 14;
    uint32_t wmask = (size & 1) ? 511 : 255, hmask = (size & 2) ? 511 : 255;
    uint32_t *out = bgline[bg];
    if (cnt & 0x40) {
        uint32_t mv = ((IO16(R_MOSAIC) >> 4) & 15) + 1;
        y -= y % (int)mv;
    }
    uint32_t py = ((uint32_t)y + vofs) & hmask;
    for (int x = 0; x < GBA_W;) {
        uint32_t px = ((uint32_t)x + hofs) & wmask;
        uint32_t sb = (px >> 8) + ((py >> 8) * ((size & 1) ? 2 : 1));
        uint32_t entry_addr = scr + sb * 0x800u + ((py & 255) >> 3) * 64 + ((px & 255) >> 3) * 2;
        uint16_t e = *(const uint16_t *)(VRAM8 + entry_addr);
        uint32_t tile = e & 0x3FF, ty = py & 7;
        if (e & 0x800) ty = 7 - ty;
        int run = 8 - (int)(px & 7);
        for (int k = 0; k < run && x < GBA_W; k++, x++) {
            uint32_t tx = (px + (uint32_t)k) & 7;
            if (e & 0x400) tx = 7 - tx;
            uint32_t ci;
            if (bpp8) {
                uint32_t a = chr + tile * 64 + ty * 8 + tx;
                ci = a < 0x10000 ? VRAM8[a] : 0;
                out[x] = ci ? (PAL16[ci] | OPAQUE) : 0;
            } else {
                uint32_t a = chr + tile * 32 + ty * 4 + (tx >> 1);
                ci = a < 0x10000 ? (VRAM8[a] >> ((tx & 1) * 4)) & 15 : 0;
                out[x] = ci ? (PAL16[(e >> 12) * 16 + ci] | OPAQUE) : 0;
            }
        }
    }
    if (cnt & 0x40) {
        uint32_t mh = (IO16(R_MOSAIC) & 15) + 1;
        if (mh > 1)
            for (int x = 0; x < GBA_W; x++) out[x] = out[x - x % (int)mh];
    }
}

static void bg_affine(int bg, int mode) {
    uint16_t cnt = IO16(R_BG0CNT + 2 * bg);
    uint32_t base = bg == 2 ? R_BG2PA : R_BG3PA;
    int32_t pa = (int16_t)IO16(base), pc = (int16_t)IO16(base + 4);
    int32_t pb = (int16_t)IO16(base + 2), pd = (int16_t)IO16(base + 6);
    int32_t cx = aff_x[bg - 2], cy = aff_y[bg - 2];
    uint32_t *out = bgline[bg];
    if (mode >= 3) {
        /* bitmaps: 3 = 240x160 direct, 4 = 240x160 paletted (two frames),
         * 5 = 160x128 direct (two frames) */
        int w = mode == 5 ? 160 : 240, h = mode == 5 ? 128 : 160;
        uint32_t frame = (mode != 3 && (IO16(R_DISPCNT) & 0x10)) ? 0xA000u : 0;
        for (int x = 0; x < GBA_W; x++, cx += pa, cy += pc) {
            int32_t tx = cx >> 8, ty = cy >> 8;
            if (tx < 0 || ty < 0 || tx >= w || ty >= h) {
                out[x] = 0;
                continue;
            }
            if (mode == 4) {
                uint8_t ci = VRAM8[frame + (uint32_t)(ty * w + tx)];
                out[x] = ci ? (PAL16[ci] | OPAQUE) : 0;
            } else {
                out[x] = (*(const uint16_t *)(VRAM8 + frame + (uint32_t)(ty * w + tx) * 2) & 0x7FFF) | OPAQUE;
            }
        }
    } else {
        uint32_t chr = ((cnt >> 2) & 3) * 0x4000u, scr = ((cnt >> 8) & 31) * 0x800u;
        int32_t size = 128 << (cnt >> 14);
        int wrap = (cnt >> 13) & 1;
        for (int x = 0; x < GBA_W; x++, cx += pa, cy += pc) {
            int32_t tx = cx >> 8, ty = cy >> 8;
            if (wrap) {
                tx &= size - 1;
                ty &= size - 1;
            } else if (tx < 0 || ty < 0 || tx >= size || ty >= size) {
                out[x] = 0;
                continue;
            }
            uint32_t tile = VRAM8[scr + (uint32_t)((ty >> 3) * (size >> 3) + (tx >> 3))];
            uint32_t a = chr + tile * 64 + (uint32_t)((ty & 7) * 8 + (tx & 7));
            uint8_t ci = a < 0x10000 ? VRAM8[a] : 0;
            out[x] = ci ? (PAL16[ci] | OPAQUE) : 0;
        }
    }
    if (cnt & 0x40) {
        uint32_t mh = (IO16(R_MOSAIC) & 15) + 1;
        if (mh > 1)
            for (int x = 0; x < GBA_W; x++) out[x] = out[x - x % (int)mh];
    }
    aff_x[bg - 2] += pb;
    aff_y[bg - 2] += pd;
}

/* ---------------------------------------------------------------- sprites */

static const uint8_t k_obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const uint8_t k_obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

static void objs(int y, int bitmap_mode) {
    uint16_t dispcnt = IO16(R_DISPCNT);
    int map1d = (dispcnt >> 6) & 1;
    uint32_t mos = IO16(R_MOSAIC);
    uint32_t mh = ((mos >> 8) & 15) + 1, mv = ((mos >> 12) & 15) + 1;
    for (int x = 0; x < GBA_W; x++) {
        objline[x] = 0;
        objprio[x] = 4;
        objwin[x] = 0;
    }
    for (int i = 127; i >= 0; i--) {
        uint16_t a0 = OAM16[i * 4], a1 = OAM16[i * 4 + 1], a2 = OAM16[i * 4 + 2];
        int affine = (a0 >> 8) & 1;
        if (!affine && (a0 & 0x200)) continue; /* disabled */
        int shape = a0 >> 14, size = a1 >> 14;
        if (shape == 3) continue;
        int w = k_obj_w[shape][size], h = k_obj_h[shape][size];
        int dbl = affine && (a0 & 0x200);
        int bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
        int oy = a0 & 255, ox = a1 & 511;
        if (oy >= GBA_H) oy -= 256;
        if (ox >= GBA_W) ox -= 512;
        int ly = y - oy;
        if (ly < 0 || ly >= bh) continue;
        int mode = (a0 >> 10) & 3;
        if (mode == 3) continue;
        int bpp8 = (a0 >> 13) & 1;
        uint32_t tile = a2 & 0x3FF;
        if (bitmap_mode && tile < 512) continue;
        int prio = (a2 >> 10) & 3;
        uint32_t pal = bpp8 ? 0 : (a2 >> 12) * 16;
        int mosaic = (a0 >> 12) & 1;
        if (mosaic) ly -= ly % (int)mv;
        /* row stride in 32-byte tile units */
        uint32_t row_units = map1d ? (uint32_t)(w / 8) * (bpp8 ? 2 : 1) : 32;
        int32_t pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            int p = (a1 >> 9) & 31;
            pa = (int16_t)OAM16[p * 16 + 3];
            pb = (int16_t)OAM16[p * 16 + 7];
            pc = (int16_t)OAM16[p * 16 + 11];
            pd = (int16_t)OAM16[p * 16 + 15];
        }
        for (int lx = 0; lx < bw; lx++) {
            int sx = ox + lx;
            if (sx < 0 || sx >= GBA_W) continue;
            int tx, ty;
            if (affine) {
                int32_t dx = lx - bw / 2, dy = ly - bh / 2;
                tx = ((pa * dx + pb * dy) >> 8) + w / 2;
                ty = ((pc * dx + pd * dy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = lx;
                ty = ly;
                if (a1 & 0x1000) tx = w - 1 - tx;
                if (a1 & 0x2000) ty = h - 1 - ty;
            }
            if (mosaic) tx -= tx % (int)mh;
            uint32_t t = tile + (uint32_t)(ty >> 3) * row_units + (uint32_t)(tx >> 3) * (bpp8 ? 2 : 1);
            uint32_t ci;
            if (bpp8) {
                uint32_t addr = 0x10000u + ((t & 0x3FF) * 32) + (uint32_t)((ty & 7) * 8 + (tx & 7));
                ci = VRAM8[addr & 0x17FFF];
            } else {
                uint32_t addr = 0x10000u + ((t & 0x3FF) * 32) + (uint32_t)((ty & 7) * 4 + ((tx & 7) >> 1));
                ci = (VRAM8[addr & 0x17FFF] >> ((tx & 1) * 4)) & 15;
            }
            if (!ci) continue;
            if (mode == 2) {
                objwin[sx] = 1;
                continue;
            }
            if (prio <= objprio[sx]) {
                objline[sx] = PAL16[256 + pal + ci] | OPAQUE | (mode == 1 ? SEMI : 0);
                objprio[sx] = (uint8_t)prio;
            }
        }
    }
}

/* ------------------------------------------------------------- composition */

static int in_win(int x, int y, uint32_t hreg, uint32_t vreg) {
    int x1 = (int)(hreg >> 8), x2 = (int)(hreg & 255), y1 = (int)(vreg >> 8), y2 = (int)(vreg & 255);
    int inx = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
    int iny = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
    if (x2 > GBA_W && x1 <= x2) inx = x >= x1;
    if (y2 > GBA_H && y1 <= y2) iny = y >= y1;
    return inx && iny;
}

static uint16_t blend_alpha(uint16_t a, uint16_t b, uint32_t ea, uint32_t eb) {
    uint32_t r = ((a & 31) * ea + (b & 31) * eb) >> 4;
    uint32_t g = (((a >> 5) & 31) * ea + ((b >> 5) & 31) * eb) >> 4;
    uint32_t bl = (((a >> 10) & 31) * ea + ((b >> 10) & 31) * eb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return (uint16_t)(r | g << 5 | bl << 10);
}

static uint16_t bright(uint16_t a, uint32_t ey, int up) {
    uint32_t r = a & 31, g = (a >> 5) & 31, b = (a >> 10) & 31;
    if (up) {
        r += ((31 - r) * ey) >> 4;
        g += ((31 - g) * ey) >> 4;
        b += ((31 - b) * ey) >> 4;
    } else {
        r -= (r * ey) >> 4;
        g -= (g * ey) >> 4;
        b -= (b * ey) >> 4;
    }
    return (uint16_t)(r | g << 5 | b << 10);
}

void gba_ppu_line(int y) {
    uint16_t dispcnt = IO16(R_DISPCNT);
    uint32_t *dst = gba_screen + y * GBA_W;
    if (dispcnt & 0x80) { /* forced blank */
        for (int x = 0; x < GBA_W; x++) dst[x] = 0xFFFFFF;
        return;
    }
    int mode = dispcnt & 7;
    int bg_on[4] = {0, 0, 0, 0};
    for (int bg = 0; bg < 4; bg++) {
        if (!(dispcnt & (0x100 << bg))) continue;
        if (mode == 0 || (mode == 1 && bg < 2)) {
            bg_text(bg, y);
            bg_on[bg] = 1;
        } else if ((mode == 1 && bg == 2) || (mode == 2 && bg >= 2) || (mode >= 3 && bg == 2)) {
            bg_affine(bg, mode);
            bg_on[bg] = 1;
        }
    }
    /* affine references advance on every line whether or not shown */
    if (!bg_on[2] && (mode >= 1)) {
        aff_x[0] += (int16_t)IO16(R_BG2PA + 2);
        aff_y[0] += (int16_t)IO16(R_BG2PA + 6);
    }
    if (!bg_on[3] && mode == 2) {
        aff_x[1] += (int16_t)IO16(R_BG3PA + 2);
        aff_y[1] += (int16_t)IO16(R_BG3PA + 6);
    }
    int obj_on = (dispcnt >> 12) & 1;
    if (obj_on || (dispcnt & 0x8000)) objs(y, mode >= 3);

    int prio[4];
    for (int bg = 0; bg < 4; bg++) prio[bg] = IO16(R_BG0CNT + 2 * bg) & 3;
    uint16_t bldcnt = IO16(R_BLDCNT), bldalpha = IO16(R_BLDALPHA);
    uint32_t eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = IO16(R_BLDY) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    int effect = (bldcnt >> 6) & 3;
    int wins = dispcnt >> 13; /* bit0 WIN0, bit1 WIN1, bit2 OBJWIN */
    uint16_t winin = IO16(R_WININ), winout = IO16(R_WINOUT);
    uint16_t backdrop = PAL16[0] & 0x7FFF;

    for (int x = 0; x < GBA_W; x++) {
        uint32_t ctl = 0x3F;
        if (wins) {
            if ((wins & 1) && in_win(x, y, IO16(R_WIN0H), IO16(R_WIN0V)))
                ctl = winin & 0x3F;
            else if ((wins & 2) && in_win(x, y, IO16(R_WIN1H), IO16(R_WIN1V)))
                ctl = (winin >> 8) & 0x3F;
            else if ((wins & 4) && objwin[x])
                ctl = (winout >> 8) & 0x3F;
            else
                ctl = winout & 0x3F;
        }
        /* top two layers: id 0-3 BG, 4 OBJ, 5 backdrop */
        int top = 5, second = 5;
        uint32_t ctop = backdrop, csec = backdrop;
        int ptop = 5, psec = 5;
        for (int p = 0; p < 4; p++) {
            if (obj_on && (ctl & 0x10) && (objline[x] & OPAQUE) && objprio[x] == p) {
                if (top == 5) {
                    top = 4, ctop = objline[x], ptop = p;
                } else if (second == 5) {
                    second = 4, csec = objline[x], psec = p;
                }
            }
            for (int bg = 0; bg < 4; bg++) {
                if (!bg_on[bg] || prio[bg] != p || !(ctl & (1u << bg)) || !(bgline[bg][x] & OPAQUE)) continue;
                if (top == 5) {
                    top = bg, ctop = bgline[bg][x], ptop = p;
                } else if (second == 5) {
                    second = bg, csec = bgline[bg][x], psec = p;
                }
            }
            if (second != 5) break;
        }
        (void)ptop;
        (void)psec;
        uint16_t c = (uint16_t)(ctop & 0x7FFF);
        int sfx = (ctl >> 5) & 1;
        int first_ok = (bldcnt >> top) & 1, second_ok = (bldcnt >> (8 + second)) & 1;
        if (top == 4 && (ctop & SEMI) && second_ok) {
            c = blend_alpha(c, (uint16_t)(csec & 0x7FFF), eva, evb);
        } else if (sfx && first_ok) {
            if (effect == 1 && second_ok)
                c = blend_alpha(c, (uint16_t)(csec & 0x7FFF), eva, evb);
            else if (effect == 2)
                c = bright(c, evy, 1);
            else if (effect == 3)
                c = bright(c, evy, 0);
        }
        dst[x] = rgb(c);
    }
}
