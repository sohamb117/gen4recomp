/*
 * See layout.h for the model. Content-space arrangement per mode (A is the
 * primary screen: top, or bottom when swapped):
 *   vertical    A at (0,0), B at (0,192)              256x384
 *   horizontal  A at (0,0), B at (256,0)              512x192
 *   hybrid      A at (0,0) scale 2, B at (512,192)    768x384
 *   top/bottom  the one screen at (0,0)               256x192
 */
#include "layout.h"

#include <math.h>

#define DS_W 256.0f
#define SH 192.0f

static void place(np_layout *l, int which, float x, float y, float s)
{
    np_screen_place *sp = &l->screen[which];
    sp->visible = 1;
    sp->content_x = x;
    sp->content_y = y;
    sp->content_scale = s;
}

/* Content point -> point in the rotated (unscaled) content box. */
static void rotate_point(const np_layout *l, float x, float y, float *rx, float *ry)
{
    float cw = l->content_w, ch = l->content_h;
    switch (l->rotation) {
    case 1:
        *rx = ch - y;
        *ry = x;
        break;
    case 2:
        *rx = cw - x;
        *ry = ch - y;
        break;
    case 3:
        *rx = y;
        *ry = cw - x;
        break;
    default:
        *rx = x;
        *ry = y;
        break;
    }
}

static void unrotate_point(const np_layout *l, float rx, float ry, float *x, float *y)
{
    float cw = l->content_w, ch = l->content_h;
    switch (l->rotation) {
    case 1:
        *x = ry;
        *y = ch - rx;
        break;
    case 2:
        *x = cw - rx;
        *y = ch - ry;
        break;
    case 3:
        *x = cw - ry;
        *y = rx;
        break;
    default:
        *x = rx;
        *y = ry;
        break;
    }
}

void np_layout_compute(np_layout *l, const np_layout_params *p, float win_w, float win_h)
{
    *l = (np_layout){0};
    l->rotation = ((p->rotation % 4) + 4) % 4;
    int a = p->swap ? 1 : 0, b = 1 - a;
    const float SW = p->screen_w > 0 ? (float)p->screen_w : DS_W;
    l->screen_w = SW;
    switch (p->mode) {
    case NP_LAYOUT_HORIZONTAL:
        place(l, a, 0, 0, 1);
        place(l, b, SW, 0, 1);
        l->content_w = 2 * SW;
        l->content_h = SH;
        break;
    case NP_LAYOUT_HYBRID:
        place(l, a, 0, 0, 2);
        place(l, b, 2 * SW, SH, 1);
        l->content_w = 3 * SW;
        l->content_h = 2 * SH;
        break;
    case NP_LAYOUT_TOP_ONLY:
        place(l, 0, 0, 0, 1);
        l->content_w = SW;
        l->content_h = SH;
        break;
    case NP_LAYOUT_BOTTOM_ONLY:
        place(l, 1, 0, 0, 1);
        l->content_w = SW;
        l->content_h = SH;
        break;
    default:
        place(l, a, 0, 0, 1);
        place(l, b, 0, SH, 1);
        l->content_w = SW;
        l->content_h = 2 * SH;
        break;
    }

    int odd = l->rotation & 1;
    float rw = odd ? l->content_h : l->content_w;
    float rh = odd ? l->content_w : l->content_h;
    float s = fminf(win_w / rw, win_h / rh);
    if (p->scale == NP_SCALE_INTEGER && s >= 1.0f)
        s = floorf(s);
    if (!(s > 0.0f))
        s = 0.0f;
    l->scale = s;
    /* Round, not floor: a fit-scaled box can exceed the window by a float
     * epsilon, and flooring that would shift it a whole pixel off-screen. */
    l->origin_x = floorf((win_w - rw * s) * 0.5f + 0.5f);
    l->origin_y = floorf((win_h - rh * s) * 0.5f + 0.5f);

    if (p->frames) {
        /* A skin places each screen itself, in window pixels. */
        l->rotation = 0;
        odd = 0;
        s = l->scale = 1.0f;
        l->origin_x = l->origin_y = 0.0f;
        l->content_w = win_w;
        l->content_h = win_h;
        for (int i = 0; i < 2; i++) {
            const float *f = p->frames + 4 * i;
            l->screen[i].visible = 0;
            if (!(f[2] > 0.0f) || !(f[3] > 0.0f))
                continue;
            float fs = fminf(f[2] / SW, f[3] / SH);
            place(l, i, f[0] + (f[2] - SW * fs) * 0.5f, f[1] + (f[3] - SH * fs) * 0.5f, fs);
        }
    }

    for (int i = 0; i < 2; i++) {
        np_screen_place *sp = &l->screen[i];
        if (!sp->visible)
            continue;
        float w = SW * sp->content_scale, h = SH * sp->content_scale;
        float rx, ry;
        rotate_point(l, sp->content_x + w * 0.5f, sp->content_y + h * 0.5f, &rx, &ry);
        sp->cx = l->origin_x + rx * s;
        sp->cy = l->origin_y + ry * s;
        sp->w = w * s;
        sp->h = h * s;
        sp->bw = odd ? sp->h : sp->w;
        sp->bh = odd ? sp->w : sp->h;
        sp->bx = sp->cx - sp->bw * 0.5f;
        sp->by = sp->cy - sp->bh * 0.5f;
    }
}

int np_layout_touch(const np_layout *l, float wx, float wy, int clamp, int *tx, int *ty)
{
    const np_screen_place *sp = &l->screen[1];
    if (!sp->visible || l->scale <= 0.0f)
        return 0;
    float x, y;
    unrotate_point(l, (wx - l->origin_x) / l->scale, (wy - l->origin_y) / l->scale, &x, &y);
    float u = floorf((x - sp->content_x) / sp->content_scale) - floorf((l->screen_w - DS_W) * 0.5f);
    float v = floorf((y - sp->content_y) / sp->content_scale);
    int inside = u >= 0.0f && u < DS_W && v >= 0.0f && v < SH;
    if (!inside && !clamp)
        return 0;
    *tx = (int)fminf(fmaxf(u, 0.0f), DS_W - 1);
    *ty = (int)fminf(fmaxf(v, 0.0f), SH - 1);
    return 1;
}
