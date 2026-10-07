/*
 * See fx.h for the design. Pattern textures (one DS pixel each):
 *   lcd    4x4, dark right column and bottom row: the gaps between LCD cells
 *   scan   1x4, dark bottom row: the gap between scanlines
 *   mask   4x1, red/green/blue stripes and a dim gap, multiplied in:
 *          aperture grille (4 texels so WRAP works on every renderer)
 *   vign   64x64 radial falloff, multiplied in
 * Overlay strength is the texture's alpha modulation; MUL blending honours
 * alpha, so 0% intensity leaves the picture untouched.
 */
#include "fx.h"

#include <math.h>

#include "app.h"
#include "scale2x.h"

#define GRID_X 16
#define GRID_Y 12
#define AUTO_SLOW_MS 12.0  /* work per frame that counts as struggling */
#define AUTO_FAST_MS 5.0   /* ...and as comfortable again */
#define AUTO_WINDOW 120    /* frames per decision */

typedef struct np_fx_state {
    SDL_Texture *lcd, *scan, *mask, *vign;
    SDL_Texture *smooth[2]; /* 512x384, for SMOOTH */
    uint32_t *buf;          /* Scale2x output */
    int smooth_valid;
    /* Auto preset */
    int auto_low;
    double auto_sum;
    int auto_n;
} np_fx_state;

static SDL_Texture *make_tex(SDL_Renderer *r, int w, int h, const uint32_t *px, SDL_BlendMode blend)
{
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t)
        return NULL;
    SDL_UpdateTexture(t, NULL, px, w * 4);
    SDL_SetTextureBlendMode(t, blend);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    return t;
}

int np_fx_init(np_app *app)
{
    np_fx_state *s = SDL_calloc(1, sizeof *s);
    if (!s)
        return -1;
    app->fx = s;
    SDL_Renderer *r = app->renderer;

    uint32_t lcd[16];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            lcd[y * 4 + x] = (x == 3 || y == 3) ? 0xFF000000u : 0x00000000u;
    uint32_t scan[4] = {0, 0, 0, 0xFF000000u};
    uint32_t mask[4] = {0xFFFF9090u, 0xFF90FF90u, 0xFF9090FFu, 0xFFB0B0B0u};
    static uint32_t vign[64 * 64];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float dx = (x + 0.5f) / 32.0f - 1.0f, dy = (y + 0.5f) / 32.0f - 1.0f;
            float d = SDL_min(1.0f, sqrtf(dx * dx * 0.8f + dy * dy * 0.8f));
            uint32_t v = (uint32_t)(255.0f * (1.0f - 0.75f * d * d * d));
            vign[y * 64 + x] = 0xFF000000u | v << 16 | v << 8 | v;
        }
    s->lcd = make_tex(r, 4, 4, lcd, SDL_BLENDMODE_BLEND);
    s->scan = make_tex(r, 1, 4, scan, SDL_BLENDMODE_BLEND);
    s->mask = make_tex(r, 4, 1, mask, SDL_BLENDMODE_MUL);
    s->vign = make_tex(r, 64, 64, vign, SDL_BLENDMODE_MUL);
    if (s->vign)
        SDL_SetTextureScaleMode(s->vign, SDL_SCALEMODE_LINEAR);
    for (int i = 0; i < 2; i++) {
        s->smooth[i] = SDL_CreateTexture(r, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 2 * NP_SCREEN_W,
                                         2 * NP_SCREEN_H);
        if (s->smooth[i])
            SDL_SetTextureScaleMode(s->smooth[i], SDL_SCALEMODE_LINEAR);
    }
    s->buf = SDL_malloc((size_t)4 * NP_SCREEN_W * NP_SCREEN_H * 4);
    if (!s->lcd || !s->scan || !s->mask || !s->vign || !s->smooth[0] || !s->smooth[1] || !s->buf)
        return -1;
    return 0;
}

void np_fx_destroy(np_app *app)
{
    np_fx_state *s = app->fx;
    if (!s)
        return;
    SDL_Texture *t[] = {s->lcd, s->scan, s->mask, s->vign, s->smooth[0], s->smooth[1]};
    for (size_t i = 0; i < SDL_arraysize(t); i++)
        if (t[i])
            SDL_DestroyTexture(t[i]);
    SDL_free(s->buf);
    SDL_free(s);
    app->fx = NULL;
}

void np_fx_resolve(const np_app *app, np_present *out)
{
    const np_options *o = &app->opt;
    int perf = o->perf;
    if (perf == NP_PERF_AUTO)
        perf = app->fx && app->fx->auto_low ? NP_PERF_LOW : NP_PERF_HIGH;
    for (int i = 0; i < 2; i++) {
        out->fx[i] = perf == NP_PERF_LOW ? NP_FX_OFF : o->fx[i];
        out->intensity[i] = o->fx_intensity[i];
    }
    out->curvature = o->crt_curvature && perf != NP_PERF_BALANCED && perf != NP_PERF_LOW;
    switch (perf) {
    case NP_PERF_HIGH:
        out->vsync = 1;
        out->fps_cap = 0;
        break;
    case NP_PERF_BALANCED:
        out->vsync = 1;
        out->fps_cap = 60;
        break;
    case NP_PERF_LOW:
        out->vsync = 0;
        out->fps_cap = 30;
        break;
    default:
        out->vsync = o->vsync;
        out->fps_cap = np_fps_caps[o->fps_cap_index];
        break;
    }
}

static int wants_smooth(const np_present *p) { return p->fx[0] == NP_FX_SMOOTH || p->fx[1] == NP_FX_SMOOTH; }

/* The core may change its frame size on any frame (render scale,
 * widescreen; np_core.h): follow it. */
static void fit_screen_textures(np_app *app)
{
    for (int i = 0; i < 2; i++) {
        SDL_Texture *t = app->screen_tex[i];
        if (t && t->w == (int)app->frame.width && t->h == (int)app->frame.height)
            continue;
        SDL_Texture *n = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                           (int)app->frame.width, (int)app->frame.height);
        if (!n) {
            SDL_Log("screen texture %ux%u: %s", app->frame.width, app->frame.height, SDL_GetError());
            continue;
        }
        SDL_SetTextureScaleMode(n, app->opt.linear_filter ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        if (t)
            SDL_DestroyTexture(t);
        app->screen_tex[i] = n;
    }
}

void np_fx_upload(np_app *app)
{
    fit_screen_textures(app);
    for (int i = 0; i < 2; i++)
        if (app->frame.screen[i] && app->screen_tex[i]->w == (int)app->frame.width)
            SDL_UpdateTexture(app->screen_tex[i], NULL, app->frame.screen[i], (int)(app->frame.stride * 4));
    np_fx_state *s = app->fx;
    np_present p;
    np_fx_resolve(app, &p);
    s->smooth_valid = 0;
    if (!wants_smooth(&p) || app->frame.width != NP_SCREEN_W || app->frame.height != NP_SCREEN_H ||
        !app->frame.screen[1])
        return;
    for (int i = 0; i < 2; i++) {
        np_scale2x(app->frame.screen[i], NP_SCREEN_W, NP_SCREEN_H, app->frame.stride, s->buf);
        SDL_UpdateTexture(s->smooth[i], NULL, s->buf, 2 * NP_SCREEN_W * 4);
    }
    s->smooth_valid = 1;
}

/* A screen's mesh: (gx+1) x (gy+1) vertices, positions in window space. */
typedef struct mesh {
    int gx, gy;
    SDL_Vertex v[(GRID_X + 1) * (GRID_Y + 1)];
    int idx[GRID_X * GRID_Y * 6];
    int nidx;
} mesh;

static void build_mesh(mesh *m, const np_screen_place *sp, int rotation, float curve)
{
    m->gx = curve > 0 ? GRID_X : 1;
    m->gy = curve > 0 ? GRID_Y : 1;
    /* Quarter turns clockwise: (x, y) -> (-y, x) per turn. */
    static const float rc[4] = {1, 0, -1, 0}, rs[4] = {0, 1, 0, -1};
    float c = rc[rotation & 3], s = rs[rotation & 3];
    for (int j = 0; j <= m->gy; j++) {
        for (int i = 0; i <= m->gx; i++) {
            float u = (float)i / (float)m->gx, v = (float)j / (float)m->gy;
            float nx = 2 * u - 1, ny = 2 * v - 1;
            float px = nx * (1.0f - curve * ny * ny) * sp->w * 0.5f;
            float py = ny * (1.0f - curve * nx * nx) * sp->h * 0.5f;
            SDL_Vertex *vx = &m->v[j * (m->gx + 1) + i];
            vx->position.x = sp->cx + px * c - py * s;
            vx->position.y = sp->cy + px * s + py * c;
            vx->color = (SDL_FColor){1, 1, 1, 1};
            vx->tex_coord.x = u;
            vx->tex_coord.y = v;
        }
    }
    m->nidx = 0;
    for (int j = 0; j < m->gy; j++)
        for (int i = 0; i < m->gx; i++) {
            int a = j * (m->gx + 1) + i, b = a + 1, d = a + m->gx + 1, e = d + 1;
            int q[6] = {a, b, d, b, e, d};
            for (int k = 0; k < 6; k++)
                m->idx[m->nidx++] = q[k];
        }
}

/* Draws `tex` over the mesh with texture coordinates scaled by (su, sv):
 * 1,1 stretches it over the screen; 256,192 repeats it once per DS pixel. */
static void draw_mesh(SDL_Renderer *r, mesh *m, SDL_Texture *tex, float su, float sv, float alpha)
{
    int n = (m->gx + 1) * (m->gy + 1);
    SDL_Vertex v[(GRID_X + 1) * (GRID_Y + 1)];
    for (int i = 0; i < n; i++) {
        v[i] = m->v[i];
        v[i].tex_coord.x *= su;
        v[i].tex_coord.y *= sv;
        v[i].color.a = alpha;
    }
    SDL_RenderGeometry(r, tex, v, n, m->idx, m->nidx);
}

static void overlay(SDL_Renderer *r, mesh *m, SDL_Texture *tex, float su, float sv, float alpha, int wrap)
{
    if (alpha <= 0.0f)
        return;
    SDL_TextureAddressMode mode = wrap ? SDL_TEXTURE_ADDRESS_WRAP : SDL_TEXTURE_ADDRESS_CLAMP;
    SDL_SetRenderTextureAddressMode(r, mode, mode);
    draw_mesh(r, m, tex, su, sv, alpha);
    SDL_SetRenderTextureAddressMode(r, SDL_TEXTURE_ADDRESS_AUTO, SDL_TEXTURE_ADDRESS_AUTO);
}

void np_fx_draw_screen(np_app *app, int which)
{
    const np_screen_place *sp = &app->layout.screen[which];
    if (!sp->visible)
        return;
    np_fx_state *s = app->fx;
    np_present p;
    np_fx_resolve(app, &p);
    float curve = 0.0f;
    for (int k = 0; k < 2; k++)
        if (p.fx[k] == NP_FX_CRT && p.curvature)
            curve = SDL_max(curve, 0.08f * (float)p.intensity[k] / 100.0f);
    static mesh m;
    build_mesh(&m, sp, app->layout.rotation, curve);

    SDL_Texture *src = app->screen_tex[which];
    if (wants_smooth(&p) && s->smooth_valid)
        src = s->smooth[which];
    draw_mesh(app->renderer, &m, src, 1, 1, 1);

    /* Patterns finer than ~2 window pixels per DS pixel only alias. Wide
     * screens hold more than 256 DS pixel columns. */
    float cols = app->layout.screen_w;
    float cell = sp->w / cols;
    for (int k = 0; k < 2; k++) {
        float a = (float)p.intensity[k] / 100.0f;
        switch (p.fx[k]) {
        case NP_FX_LCD:
            if (cell >= 2.0f)
                overlay(app->renderer, &m, s->lcd, cols, NP_SCREEN_H, 0.6f * a, 1);
            break;
        case NP_FX_SCANLINES:
            if (cell >= 2.0f)
                overlay(app->renderer, &m, s->scan, 1, NP_SCREEN_H, 0.75f * a, 1);
            break;
        case NP_FX_CRT:
            if (cell >= 2.0f) {
                overlay(app->renderer, &m, s->scan, 1, NP_SCREEN_H, 0.55f * a, 1);
                overlay(app->renderer, &m, s->mask, cols, 1, 0.8f * a, 1);
            }
            overlay(app->renderer, &m, s->vign, 1, 1, a, 0);
            break;
        default: break;
        }
    }
}

int np_fx_auto_sample(np_app *app, double work_ms)
{
    np_fx_state *s = app->fx;
    if (!s || app->opt.perf != NP_PERF_AUTO)
        return 0;
    s->auto_sum += work_ms;
    if (++s->auto_n < AUTO_WINDOW)
        return 0;
    double avg = s->auto_sum / s->auto_n;
    s->auto_sum = 0;
    s->auto_n = 0;
    int low = s->auto_low ? avg > AUTO_FAST_MS : avg > AUTO_SLOW_MS;
    if (low == s->auto_low)
        return 0;
    s->auto_low = low;
    SDL_Log("performance: auto switched to %s (%.1f ms of work per frame)", low ? "low" : "high", avg);
    return 1;
}
