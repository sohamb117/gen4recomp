/* Touch control layouts (see touchlayout.h). */
#include "touchlayout.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "np_core.h"

const char *const np_tc_ids[NP_TC_COUNT] = {"dpad", "a", "b", "x", "y", "l", "r", "select", "start", "ff", "menu"};
const char *const np_tc_names[NP_TC_COUNT] = {"D-pad", "A", "B", "X", "Y", "L", "R", "Select", "Start",
                                              "Fast-forward", "Menu"};
static const uint16_t tc_keys[NP_TC_COUNT] = {0,        NP_KEY_A,      NP_KEY_B,     NP_KEY_X, NP_KEY_Y, NP_KEY_L,
                                              NP_KEY_R, NP_KEY_SELECT, NP_KEY_START, 0,        0};

int np_tc_round(int id) { return id <= NP_TC_Y; }

static float minf(float a, float b) { return a < b ? a : b; }

/* Unit u = short side / 7 and margin 0.3u, as the arrangement was first
 * drawn: d-pad bottom left, face diamond bottom right, L/R in the top
 * corners, FF/Menu top centre, Select/Start bottom centre. */
void np_tc_default(np_tc_layout *l, float W, float H)
{
    float s = minf(W, H), u = s / 7.0f, m = 0.3f * u;
    struct {
        float cx, cy, w, h;
    } px[NP_TC_COUNT];
    float dcy = H - m - 1.5f * u, fcx = W - m - 1.5f * u;
    px[NP_TC_DPAD].cx = m + 1.5f * u, px[NP_TC_DPAD].cy = dcy, px[NP_TC_DPAD].w = px[NP_TC_DPAD].h = 3.0f * u;
    static const float fdx[4] = {1, 0, 0, -1}, fdy[4] = {0, 1, -1, 0}; /* A east, B south, X north, Y west */
    for (int i = 0; i < 4; i++) {
        px[NP_TC_A + i].cx = fcx + fdx[i] * 0.95f * u;
        px[NP_TC_A + i].cy = dcy + fdy[i] * 0.95f * u;
        px[NP_TC_A + i].w = px[NP_TC_A + i].h = 1.1f * u;
    }
    px[NP_TC_L].cx = m + 0.9f * u, px[NP_TC_L].cy = m + 0.35f * u;
    px[NP_TC_R].cx = W - m - 0.9f * u, px[NP_TC_R].cy = m + 0.35f * u;
    px[NP_TC_L].w = px[NP_TC_R].w = 1.8f * u, px[NP_TC_L].h = px[NP_TC_R].h = 0.7f * u;
    px[NP_TC_SELECT].cx = W * 0.5f - 0.95f * u, px[NP_TC_START].cx = W * 0.5f + 0.95f * u;
    px[NP_TC_SELECT].cy = px[NP_TC_START].cy = H - m - 0.3f * u;
    px[NP_TC_FF].cx = W * 0.5f - 0.95f * u, px[NP_TC_MENU].cx = W * 0.5f + 0.95f * u;
    px[NP_TC_FF].cy = px[NP_TC_MENU].cy = m + 0.3f * u;
    for (int i = NP_TC_SELECT; i <= NP_TC_MENU; i++)
        px[i].w = 1.5f * u, px[i].h = 0.6f * u;
    for (int i = 0; i < NP_TC_COUNT; i++)
        l->item[i] = (np_tc_item){px[i].cx / W, px[i].cy / H, px[i].w / s, px[i].h / s, 1.0f};
}

void np_tc_rect(const np_tc_item *it, float W, float H, float r[4])
{
    float s = minf(W, H);
    r[2] = it->w * s;
    r[3] = it->h * s;
    r[0] = it->cx * W - r[2] * 0.5f;
    r[1] = it->cy * H - r[3] * 0.5f;
}

/* Inside item `id`, with the slop the original controls had: round items
 * 15-20% beyond their circle, rectangles a tenth of the unit. */
static int inside(const np_tc_item *it, int id, float W, float H, float x, float y, float *dx, float *dy, float *rad)
{
    float r[4];
    np_tc_rect(it, W, H, r);
    if (np_tc_round(id)) {
        *rad = minf(r[2], r[3]) * 0.5f;
        *dx = x - (r[0] + r[2] * 0.5f);
        *dy = y - (r[1] + r[3] * 0.5f);
        float slop = id == NP_TC_DPAD ? 1.15f : 1.2f;
        return sqrtf(*dx * *dx + *dy * *dy) <= *rad * slop;
    }
    float slop = minf(W, H) / 70.0f;
    return x >= r[0] - slop && x < r[0] + r[2] + slop && y >= r[1] - slop && y < r[1] + r[3] + slop;
}

int np_tc_hit(const np_tc_layout *l, unsigned hide, float W, float H, float x, float y, uint16_t *keys, int *ff,
              int *menu)
{
    int any = 0;
    *keys = 0;
    *ff = *menu = 0;
    for (int i = 0; i < NP_TC_COUNT; i++) {
        float dx, dy, rad;
        if ((hide >> i & 1) || !inside(&l->item[i], i, W, H, x, y, &dx, &dy, &rad))
            continue;
        any = 1;
        if (i == NP_TC_DPAD) {
            /* 8-way: a component counts when it is over ~22 degrees off-axis;
             * the centre fifth is neutral. */
            float d = sqrtf(dx * dx + dy * dy);
            if (d > rad * 0.2f) {
                if (dx < -0.4f * d)
                    *keys |= NP_KEY_LEFT;
                if (dx > 0.4f * d)
                    *keys |= NP_KEY_RIGHT;
                if (dy < -0.4f * d)
                    *keys |= NP_KEY_UP;
                if (dy > 0.4f * d)
                    *keys |= NP_KEY_DOWN;
            }
        } else if (i == NP_TC_FF) {
            *ff = 1;
        } else if (i == NP_TC_MENU) {
            *menu = 1;
        } else {
            *keys |= tc_keys[i];
        }
    }
    return any;
}

int np_tc_pick(const np_tc_layout *l, unsigned hide, float W, float H, float x, float y)
{
    int best = -1;
    float best_area = 0;
    for (int i = 0; i < NP_TC_COUNT; i++) {
        if (hide >> i & 1)
            continue;
        float r[4];
        np_tc_rect(&l->item[i], W, H, r);
        if (x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3] && (best < 0 || r[2] * r[3] < best_area)) {
            best = i;
            best_area = r[2] * r[3];
        }
    }
    return best;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void np_tc_clamp(np_tc_item *it)
{
    it->w = clampf(it->w, 0.05f, 1.0f);
    it->h = clampf(it->h, 0.05f, 1.0f);
    it->cx = clampf(it->cx, 0.0f, 1.0f);
    it->cy = clampf(it->cy, 0.0f, 1.0f);
    it->opacity = clampf(it->opacity, 0.1f, 1.0f);
}

int np_tc_format(const np_tc_layout *l, char *out, size_t n)
{
    int len = 0;
    for (int i = 0; i < NP_TC_COUNT; i++) {
        const np_tc_item *it = &l->item[i];
        int k = snprintf(out + len, (size_t)len < n ? n - (size_t)len : 0, "%s = %.4f %.4f %.4f %.4f %.2f\n",
                         np_tc_ids[i], it->cx, it->cy, it->w, it->h, it->opacity);
        if (k < 0)
            return -1;
        len += k;
    }
    return len;
}

int np_tc_parse(np_tc_layout *l, const char *key, const char *value)
{
    for (int i = 0; i < NP_TC_COUNT; i++) {
        if (strcmp(key, np_tc_ids[i]))
            continue;
        np_tc_item it;
        char extra;
        if (sscanf(value, "%f %f %f %f %f %c", &it.cx, &it.cy, &it.w, &it.h, &it.opacity, &extra) != 5 ||
            !isfinite(it.cx) || !isfinite(it.cy) || !isfinite(it.w) || !isfinite(it.h) || !isfinite(it.opacity))
            return -1;
        np_tc_clamp(&it);
        l->item[i] = it;
        return 0;
    }
    return -1;
}
