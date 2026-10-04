/*
 * On-screen controls for touch devices: d-pad, A/B/X/Y, L/R, Start/Select,
 * a fast-forward toggle and a menu button (the only way into Options on a
 * phone without a keyboard or controller). Geometry derives from the window
 * size on every call, so rotation and resizing need no bookkeeping, and the
 * same function serves drawing and hit-testing.
 */
#include "app.h"

#include <math.h>

typedef struct pad_geom {
    float u; /* size unit */
    float dcx, dcy, drad;      /* d-pad */
    float fcx, fcy, foff, frad; /* face buttons */
    SDL_FRect l, r, select, start, ff, menu;
} pad_geom;

static void geometry(const np_app *app, pad_geom *g)
{
    float w = app->out_w, h = app->out_h;
    float u = SDL_min(w, h) / 7.0f;
    float m = u * 0.3f;
    g->u = u;
    g->drad = 1.5f * u;
    g->dcx = m + g->drad;
    g->dcy = h - m - g->drad;
    g->foff = 0.95f * u;
    g->frad = 0.55f * u;
    g->fcx = w - m - g->drad;
    g->fcy = g->dcy;
    g->l = (SDL_FRect){m, m, 1.8f * u, 0.7f * u};
    g->r = (SDL_FRect){w - m - 1.8f * u, m, 1.8f * u, 0.7f * u};
    g->select = (SDL_FRect){w * 0.5f - 1.7f * u, h - m - 0.6f * u, 1.5f * u, 0.6f * u};
    g->start = (SDL_FRect){w * 0.5f + 0.2f * u, h - m - 0.6f * u, 1.5f * u, 0.6f * u};
    g->ff = (SDL_FRect){w * 0.5f - 1.7f * u, m, 1.5f * u, 0.6f * u};
    g->menu = (SDL_FRect){w * 0.5f + 0.2f * u, m, 1.5f * u, 0.6f * u};
}

/* Face buttons in diamond order: east, south, north, west. */
static const uint16_t face_keys[4] = {NP_KEY_A, NP_KEY_B, NP_KEY_X, NP_KEY_Y};
static const char *const face_labels[4] = {"A", "B", "X", "Y"};
static const float face_dx[4] = {1, 0, 0, -1};
static const float face_dy[4] = {0, 1, -1, 0};

int np_touchpad_visible(const np_app *app)
{
    if (app->view != NP_VIEW_GAME || app->page != NP_PAGE_NONE)
        return 0;
    return app->opt.touch_controls == NP_TOUCH_ON || (app->opt.touch_controls == NP_TOUCH_AUTO && app->touch_seen);
}

static int in_rect(const SDL_FRect *r, float x, float y, float slop)
{
    return x >= r->x - slop && x < r->x + r->w + slop && y >= r->y - slop && y < r->y + r->h + slop;
}

uint16_t np_touchpad_hit(const np_app *app, float x, float y, int *ff, int *menu, int *any)
{
    pad_geom g;
    geometry(app, &g);
    uint16_t keys = 0;
    *ff = *menu = *any = 0;
    float dx = x - g.dcx, dy = y - g.dcy, d = sqrtf(dx * dx + dy * dy);
    if (d <= g.drad * 1.15f) {
        *any = 1;
        /* 8-way: a component counts when it is over ~22 degrees off-axis. */
        if (d > g.drad * 0.2f) {
            if (dx < -0.4f * d)
                keys |= NP_KEY_LEFT;
            if (dx > 0.4f * d)
                keys |= NP_KEY_RIGHT;
            if (dy < -0.4f * d)
                keys |= NP_KEY_UP;
            if (dy > 0.4f * d)
                keys |= NP_KEY_DOWN;
        }
        return keys;
    }
    for (int i = 0; i < 4; i++) {
        float bx = g.fcx + face_dx[i] * g.foff - x, by = g.fcy + face_dy[i] * g.foff - y;
        if (sqrtf(bx * bx + by * by) < g.frad * 1.2f) {
            keys |= face_keys[i];
            *any = 1;
        }
    }
    float slop = g.u * 0.1f;
    if (in_rect(&g.l, x, y, slop))
        keys |= NP_KEY_L;
    if (in_rect(&g.r, x, y, slop))
        keys |= NP_KEY_R;
    if (in_rect(&g.select, x, y, slop))
        keys |= NP_KEY_SELECT;
    if (in_rect(&g.start, x, y, slop))
        keys |= NP_KEY_START;
    *ff = in_rect(&g.ff, x, y, slop);
    *menu = in_rect(&g.menu, x, y, slop);
    *any |= keys != 0 || *ff || *menu;
    return keys;
}

static void fill_circle(np_app *app, float cx, float cy, float r, SDL_Color c)
{
    enum { SEG = 32 };
    SDL_Vertex v[SEG + 1];
    int idx[SEG * 3];
    SDL_FColor fc = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
    v[0] = (SDL_Vertex){{cx, cy}, fc, {0, 0}};
    for (int i = 0; i < SEG; i++) {
        float a = (float)i * 6.2831853f / SEG;
        v[i + 1] = (SDL_Vertex){{cx + r * cosf(a), cy + r * sinf(a)}, fc, {0, 0}};
        idx[3 * i] = 0;
        idx[3 * i + 1] = i + 1;
        idx[3 * i + 2] = (i + 1) % SEG + 1;
    }
    SDL_RenderGeometry(app->renderer, NULL, v, SEG + 1, idx, SEG * 3);
}

static void label_centered(np_app *app, float cx, float cy, float scale, const char *s, SDL_Color c)
{
    float w = (float)SDL_strlen(s) * 8.0f * scale;
    np_ui_text(app, cx - w * 0.5f, cy - 4.0f * scale, scale, s, c);
}

static void button_rect(np_app *app, const SDL_FRect *r, const char *label, int lit, float ts)
{
    SDL_Color fill = {255, 255, 255, (Uint8)(lit ? 150 : 60)};
    np_ui_fill(app, *r, fill);
    np_ui_frame(app, *r, SDL_max(1.0f, ts), (SDL_Color){255, 255, 255, 140});
    label_centered(app, r->x + r->w * 0.5f, r->y + r->h * 0.5f, ts, label, (SDL_Color){255, 255, 255, 230});
}

void np_touchpad_draw(np_app *app)
{
    if (!np_touchpad_visible(app))
        return;
    pad_geom g;
    geometry(app, &g);
    uint16_t lit = app->control_keys;
    float ts = SDL_max(1.0f, floorf(g.u / 40.0f)); /* label scale: "SELECT" fits 1.5u */
    SDL_Color base = {255, 255, 255, 60}, on = {255, 255, 255, 150};

    float arm = g.drad * 0.62f;
    fill_circle(app, g.dcx, g.dcy, g.drad, (SDL_Color){0, 0, 0, 50});
    np_ui_fill(app, (SDL_FRect){g.dcx - g.drad * 0.9f, g.dcy - arm * 0.5f, g.drad * 1.8f, arm}, base);
    np_ui_fill(app, (SDL_FRect){g.dcx - arm * 0.5f, g.dcy - g.drad * 0.9f, arm, g.drad * 1.8f}, base);
    float half = g.drad * 0.9f - arm * 0.5f;
    if (lit & NP_KEY_LEFT)
        np_ui_fill(app, (SDL_FRect){g.dcx - g.drad * 0.9f, g.dcy - arm * 0.5f, half, arm}, on);
    if (lit & NP_KEY_RIGHT)
        np_ui_fill(app, (SDL_FRect){g.dcx + arm * 0.5f, g.dcy - arm * 0.5f, half, arm}, on);
    if (lit & NP_KEY_UP)
        np_ui_fill(app, (SDL_FRect){g.dcx - arm * 0.5f, g.dcy - g.drad * 0.9f, arm, half}, on);
    if (lit & NP_KEY_DOWN)
        np_ui_fill(app, (SDL_FRect){g.dcx - arm * 0.5f, g.dcy + arm * 0.5f, arm, half}, on);

    for (int i = 0; i < 4; i++) {
        float bx = g.fcx + face_dx[i] * g.foff, by = g.fcy + face_dy[i] * g.foff;
        fill_circle(app, bx, by, g.frad, (lit & face_keys[i]) ? on : base);
        label_centered(app, bx, by, ts * 1.5f, face_labels[i], (SDL_Color){255, 255, 255, 230});
    }
    button_rect(app, &g.l, "L", lit & NP_KEY_L, ts);
    button_rect(app, &g.r, "R", lit & NP_KEY_R, ts);
    button_rect(app, &g.select, "SELECT", lit & NP_KEY_SELECT, ts);
    button_rect(app, &g.start, "START", lit & NP_KEY_START, ts);
    button_rect(app, &g.ff, app->ff_toggle ? "FF ON" : "FF", app->ff_toggle, ts);
    button_rect(app, &g.menu, "MENU", 0, ts);
}
