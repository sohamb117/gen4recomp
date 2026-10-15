/*
 * On-screen controls for touch devices: d-pad, A/B/X/Y, L/R, Start/Select,
 * a fast-forward toggle and a menu button (the only way into Options on a
 * phone without a keyboard or controller), drawn and hit-tested from the
 * layout for the window's orientation (touchlayout.c).
 *
 * The layout editor (Options > Edit touch controls...) moves, resizes and
 * fades each control for the current orientation; landscape and portrait
 * are kept apart because the same arrangement rarely suits both. Layouts
 * the player has not edited follow the built-in arrangement, so they keep
 * adapting to the window. Edited ones are saved to touch-controls.ini.
 */
#include "app.h"

#include <math.h>

#include "romdb.h"
#include "touchlayout.h"

typedef struct touch_state {
    np_tc_layout layout[2]; /* [0] landscape, [1] portrait */
    int custom[2];          /* edited (else the defaults for the window) */
    /* editor */
    int sel;
    int drag; /* 0 none, 1 move, 2 resize */
    float grab_x, grab_y;
    int dirty;
    SDL_FRect bar[8];
    int nbar;
} touch_state;

static touch_state tp;

static int portrait(const np_app *app) { return app->out_h > app->out_w; }

static void touch_path(char *out, size_t n) { np_storage_path(out, n, "touch-controls.ini"); }

void np_touchpad_load(np_app *app)
{
    (void)app;
    char path[1100];
    touch_path(path, sizeof path);
    char *text = SDL_LoadFile(path, NULL);
    if (!text)
        return;
    int o = -1;
    char *save = NULL;
    for (char *line = SDL_strtok_r(text, "\r\n", &save); line; line = SDL_strtok_r(NULL, "\r\n", &save)) {
        while (*line == ' ' || *line == '\t')
            line++;
        if (*line == '#' || !*line)
            continue;
        if (*line == '[') {
            o = !SDL_strncmp(line, "[landscape]", 11) ? 0 : !SDL_strncmp(line, "[portrait]", 10) ? 1 : -1;
            if (o >= 0 && !tp.custom[o]) {
                /* Start from the defaults of a typical window of that shape,
                 * so keys missing from the file still have a place. */
                np_tc_default(&tp.layout[o], o ? 390.0f : 844.0f, o ? 844.0f : 390.0f);
                tp.custom[o] = 1;
            }
            continue;
        }
        char *eq = SDL_strchr(line, '=');
        if (o < 0 || !eq)
            continue;
        *eq = '\0';
        char *key = line, *end = eq;
        while (end > key && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (np_tc_parse(&tp.layout[o], key, eq + 1))
            SDL_Log("touch-controls.ini: ignoring \"%s\"", key);
    }
    SDL_free(text);
}

static void save_layouts(np_app *app)
{
    if (app->autotest.active && !app->autotest.storage)
        return; /* autotests write only to portable storage */
    char buf[2048];
    size_t len = (size_t)SDL_snprintf(buf, sizeof buf, "# nativeplat touch controls: centre x y (fraction of the window), "
                                                        "width height (fraction of its short side), opacity\n");
    for (int o = 0; o < 2; o++) {
        if (!tp.custom[o])
            continue;
        len += (size_t)SDL_snprintf(buf + len, sizeof buf - len, "\n[%s]\n", o ? "portrait" : "landscape");
        int n = np_tc_format(&tp.layout[o], buf + len, sizeof buf - len);
        if (n > 0 && (size_t)n < sizeof buf - len)
            len += (size_t)n;
    }
    char path[1100];
    touch_path(path, sizeof path);
    if (!tp.custom[0] && !tp.custom[1])
        SDL_RemovePath(path); /* everything back to the defaults */
    else if (np_storage_write_atomic(path, buf, len, 0))
        np_app_toast(app, "Cannot save the touch controls: %s", SDL_GetError());
}

/* The layout for the current window: the edited one, or the defaults. */
static const np_tc_layout *current(const np_app *app, np_tc_layout *scratch)
{
    int o = portrait(app);
    if (tp.custom[o])
        return &tp.layout[o];
    np_tc_default(scratch, app->out_w, app->out_h);
    return scratch;
}

/* The controls the running console has none of (a GBA: X and Y). */
static unsigned hidden(const np_app *app) { return app->core && np_game_is_gba(app->game) ? NP_TC_HIDE_GBA : 0; }

/* A skin is a deliberate choice, so it shows whenever a game does; the
 * built-in controls follow Options > Touch controls. */
int np_touchpad_visible(const np_app *app)
{
    if (app->view != NP_VIEW_GAME || app->page != NP_PAGE_NONE)
        return 0;
    if (np_skin_active(app))
        return 1;
    return app->opt.touch_controls == NP_TOUCH_ON || (app->opt.touch_controls == NP_TOUCH_AUTO && app->touch_seen);
}

void np_touchpad_hit(const np_app *app, float x, float y, np_touch_hit *h)
{
    SDL_zerop(h);
    if (np_skin_hit(app, x, y, h))
        return;
    np_tc_layout scratch;
    h->any = np_tc_hit(current(app, &scratch), hidden(app), app->out_w, app->out_h, x, y, &h->keys, &h->ff_toggle,
                       &h->menu);
}

/* ---- drawing ------------------------------------------------------------------ */

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

static SDL_Color white_a(float alpha) { return (SDL_Color){255, 255, 255, (Uint8)SDL_clamp(alpha, 0.0f, 255.0f)}; }

/* One control; `lit` highlights it, opacity scales every alpha. */
static void draw_item(np_app *app, int id, const np_tc_item *it, uint16_t lit, int lit_flag)
{
    static const uint16_t face_keys[4] = {NP_KEY_A, NP_KEY_B, NP_KEY_X, NP_KEY_Y};
    static const char *const labels[NP_TC_COUNT] = {"", "A", "B", "X", "Y", "L", "R", "SELECT", "START", "FF", "MENU"};
    float r[4];
    np_tc_rect(it, app->out_w, app->out_h, r);
    float op = it->opacity, cx = r[0] + r[2] * 0.5f, cy = r[1] + r[3] * 0.5f;
    float u = SDL_min(app->out_w, app->out_h) / 7.0f;
    float ts = SDL_max(1.0f, floorf(u / 40.0f)); /* label scale: "SELECT" fits 1.5u */
    SDL_Color base = white_a(60 * op), on = white_a(150 * op), text = white_a(230 * op);
    if (id == NP_TC_DPAD) {
        float rad = SDL_min(r[2], r[3]) * 0.5f, arm = rad * 0.62f, half = rad * 0.9f - arm * 0.5f;
        fill_circle(app, cx, cy, rad, (SDL_Color){0, 0, 0, (Uint8)(50 * op)});
        np_ui_fill(app, (SDL_FRect){cx - rad * 0.9f, cy - arm * 0.5f, rad * 1.8f, arm}, base);
        np_ui_fill(app, (SDL_FRect){cx - arm * 0.5f, cy - rad * 0.9f, arm, rad * 1.8f}, base);
        if (lit & NP_KEY_LEFT)
            np_ui_fill(app, (SDL_FRect){cx - rad * 0.9f, cy - arm * 0.5f, half, arm}, on);
        if (lit & NP_KEY_RIGHT)
            np_ui_fill(app, (SDL_FRect){cx + arm * 0.5f, cy - arm * 0.5f, half, arm}, on);
        if (lit & NP_KEY_UP)
            np_ui_fill(app, (SDL_FRect){cx - arm * 0.5f, cy - rad * 0.9f, arm, half}, on);
        if (lit & NP_KEY_DOWN)
            np_ui_fill(app, (SDL_FRect){cx - arm * 0.5f, cy + arm * 0.5f, arm, half}, on);
    } else if (np_tc_round(id)) {
        fill_circle(app, cx, cy, SDL_min(r[2], r[3]) * 0.5f, (lit & face_keys[id - NP_TC_A]) ? on : base);
        label_centered(app, cx, cy, SDL_max(1.0f, floorf(SDL_min(r[2], r[3]) / 26.0f)), labels[id], text);
    } else {
        SDL_FRect fr = {r[0], r[1], r[2], r[3]};
        np_ui_fill(app, fr, lit_flag ? on : base);
        np_ui_frame(app, fr, SDL_max(1.0f, ts), white_a(140 * op));
        const char *label = id == NP_TC_FF && app->ff_toggle ? "FF ON" : labels[id];
        float fit = floorf(r[2] / ((float)SDL_strlen(label) * 8.0f + 8.0f));
        label_centered(app, cx, cy, SDL_clamp(fit, 1.0f, ts * 2), label, text);
    }
}

static int lit_flag(const np_app *app, int id, uint16_t lit)
{
    switch (id) {
    case NP_TC_L: return (lit & NP_KEY_L) != 0;
    case NP_TC_R: return (lit & NP_KEY_R) != 0;
    case NP_TC_SELECT: return (lit & NP_KEY_SELECT) != 0;
    case NP_TC_START: return (lit & NP_KEY_START) != 0;
    case NP_TC_FF: return app->ff_toggle;
    default: return 0;
    }
}

void np_touchpad_draw(np_app *app)
{
    if (!np_touchpad_visible(app))
        return;
    if (np_skin_active(app)) {
        np_skin_draw_pressed(app, app->control_keys); /* the art itself is drawn under the screens */
        return;
    }
    np_tc_layout scratch;
    const np_tc_layout *l = current(app, &scratch);
    unsigned hide = hidden(app);
    for (int i = 0; i < NP_TC_COUNT; i++)
        if (!(hide >> i & 1))
            draw_item(app, i, &l->item[i], app->control_keys, lit_flag(app, i, app->control_keys));
}

/* ---- editor --------------------------------------------------------------------- */

enum { BAR_PREV, BAR_NEXT, BAR_SMALLER, BAR_BIGGER, BAR_FAINTER, BAR_BOLDER, BAR_RESET, BAR_DONE, BAR_COUNT };

void np_touchedit_open(np_app *app)
{
    int o = portrait(app);
    if (!tp.custom[o])
        np_tc_default(&tp.layout[o], app->out_w, app->out_h);
    tp.sel = 0; /* the d-pad: never hidden */
    tp.drag = 0;
    np_app_open_page(app, NP_PAGE_TOUCH_EDIT);
}

/* Applies an edit to the selected control of this orientation. */
static void edit(np_app *app, float dx, float dy, float dsize, float dop)
{
    int o = portrait(app);
    np_tc_item *it = &tp.layout[o].item[tp.sel];
    it->cx += dx;
    it->cy += dy;
    it->w *= 1.0f + dsize;
    it->h *= 1.0f + dsize;
    it->opacity += dop;
    np_tc_clamp(it);
    tp.custom[o] = 1;
    tp.dirty = 1;
}

static void bar_action(np_app *app, int a)
{
    int o = portrait(app);
    unsigned hide = hidden(app);
    switch (a) {
    case BAR_PREV:
        do
            tp.sel = (tp.sel + NP_TC_COUNT - 1) % NP_TC_COUNT;
        while (hide >> tp.sel & 1);
        break;
    case BAR_NEXT:
        do
            tp.sel = (tp.sel + 1) % NP_TC_COUNT;
        while (hide >> tp.sel & 1);
        break;
    case BAR_SMALLER: edit(app, 0, 0, -0.1f, 0); break;
    case BAR_BIGGER: edit(app, 0, 0, 0.1f, 0); break;
    case BAR_FAINTER: edit(app, 0, 0, 0, -0.1f); break;
    case BAR_BOLDER: edit(app, 0, 0, 0, 0.1f); break;
    case BAR_RESET:
        tp.custom[o] = 0;
        np_tc_default(&tp.layout[o], app->out_w, app->out_h);
        tp.dirty = 1;
        np_app_toast(app, "%s controls reset", o ? "Portrait" : "Landscape");
        break;
    case BAR_DONE: np_ui_back(app); break;
    default: break;
    }
}

void np_touchedit_close(np_app *app)
{
    if (tp.dirty)
        save_layouts(app);
    tp.dirty = 0;
    tp.drag = 0;
}

static int in_frect(const SDL_FRect *r, float x, float y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* The resize handle: a square on the selected control's lower right corner. */
static SDL_FRect handle(const np_app *app)
{
    float r[4], s = SDL_min(app->out_w, app->out_h) * 0.045f;
    np_tc_rect(&tp.layout[portrait(app)].item[tp.sel], app->out_w, app->out_h, r);
    return (SDL_FRect){r[0] + r[2] - s * 0.5f, r[1] + r[3] - s * 0.5f, s, s};
}

void np_touchedit_pointer(np_app *app, float x, float y, int pressed, int released)
{
    int o = portrait(app);
    np_tc_layout *l = &tp.layout[o];
    if (pressed) {
        for (int i = 0; i < tp.nbar; i++)
            if (in_frect(&tp.bar[i], x, y)) {
                bar_action(app, i);
                return;
            }
        SDL_FRect h = handle(app);
        if (in_frect(&h, x, y)) {
            tp.drag = 2;
            return;
        }
        int hit = np_tc_pick(l, hidden(app), app->out_w, app->out_h, x, y);
        if (hit >= 0) {
            tp.sel = hit;
            tp.drag = 1;
            tp.grab_x = x - l->item[hit].cx * app->out_w;
            tp.grab_y = y - l->item[hit].cy * app->out_h;
        }
        return;
    }
    if (released) {
        tp.drag = 0;
        return;
    }
    np_tc_item *it = &l->item[tp.sel];
    float s = SDL_min(app->out_w, app->out_h);
    if (tp.drag == 1) {
        it->cx = (x - tp.grab_x) / app->out_w;
        it->cy = (y - tp.grab_y) / app->out_h;
    } else if (tp.drag == 2) {
        /* The corner follows the pointer; round controls stay round. */
        float r[4];
        np_tc_rect(it, app->out_w, app->out_h, r);
        float w = (x - r[0]) / s, h = (y - r[1]) / s;
        if (np_tc_round(tp.sel))
            w = h = SDL_max(w, h);
        it->cx += (w - it->w) * s * 0.5f / app->out_w;
        it->cy += (h - it->h) * s * 0.5f / app->out_h;
        it->w = w;
        it->h = h;
    } else {
        return;
    }
    np_tc_clamp(it);
    tp.custom[o] = 1;
    tp.dirty = 1;
}

/* Keys the editor takes itself (others reach the menu commands). */
int np_touchedit_key(np_app *app, const SDL_KeyboardEvent *k)
{
    float step = k->mod & SDL_KMOD_SHIFT ? 0.05f : 0.01f;
    switch (k->scancode) {
    case SDL_SCANCODE_TAB: bar_action(app, k->mod & SDL_KMOD_SHIFT ? BAR_PREV : BAR_NEXT); return 1;
    case SDL_SCANCODE_LEFT: edit(app, -step, 0, 0, 0); return 1;
    case SDL_SCANCODE_RIGHT: edit(app, step, 0, 0, 0); return 1;
    case SDL_SCANCODE_UP: edit(app, 0, -step, 0, 0); return 1;
    case SDL_SCANCODE_DOWN: edit(app, 0, step, 0, 0); return 1;
    case SDL_SCANCODE_MINUS: bar_action(app, BAR_SMALLER); return 1;
    case SDL_SCANCODE_EQUALS: bar_action(app, BAR_BIGGER); return 1;
    case SDL_SCANCODE_LEFTBRACKET: bar_action(app, BAR_FAINTER); return 1;
    case SDL_SCANCODE_RIGHTBRACKET: bar_action(app, BAR_BOLDER); return 1;
    default: return 0;
    }
}

void np_touchedit_command(np_app *app, np_menu_cmd cmd)
{
    switch (cmd) {
    case NP_CMD_UP: edit(app, 0, -0.01f, 0, 0); break;
    case NP_CMD_DOWN: edit(app, 0, 0.01f, 0, 0); break;
    case NP_CMD_LEFT: edit(app, -0.01f, 0, 0, 0); break;
    case NP_CMD_RIGHT: edit(app, 0.01f, 0, 0, 0); break;
    case NP_CMD_TAB_PREV: bar_action(app, BAR_PREV); break;
    case NP_CMD_TAB_NEXT: bar_action(app, BAR_NEXT); break;
    case NP_CMD_X: bar_action(app, BAR_SMALLER); break;
    case NP_CMD_Y: bar_action(app, BAR_BIGGER); break;
    case NP_CMD_CONFIRM: { /* A cycles opacity 100 -> 75 -> 50 -> 25 -> 100 % */
        np_tc_item *it = &tp.layout[portrait(app)].item[tp.sel];
        edit(app, 0, 0, 0, it->opacity <= 0.3f ? 1.0f : -0.25f);
        break;
    }
    default: break;
    }
}

void np_touchedit_draw(np_app *app)
{
    static const SDL_Color accent = {255, 205, 80, 255}, white = {235, 235, 235, 255}, dim = {170, 170, 180, 255};
    int o = portrait(app);
    const np_tc_layout *l = &tp.layout[o];
    np_ui_fill(app, (SDL_FRect){0, 0, app->out_w, app->out_h}, (SDL_Color){0, 0, 0, 120});
    /* Where the DS screens are, so controls can be kept off them. */
    for (int i = 0; app->view == NP_VIEW_GAME && i < 2; i++) {
        const np_screen_place *sp = &app->layout.screen[i];
        if (sp->visible)
            np_ui_frame(app, (SDL_FRect){sp->bx, sp->by, sp->bw, sp->bh}, 2, (SDL_Color){120, 170, 255, 200});
    }
    unsigned hide = hidden(app);
    for (int i = 0; i < NP_TC_COUNT; i++)
        if (!(hide >> i & 1))
            draw_item(app, i, &l->item[i], 0, 0);
    float r[4];
    np_tc_rect(&l->item[tp.sel], app->out_w, app->out_h, r);
    /* Toolbar text at least 2x once the window allows: it sits over the
     * game and must stay readable at arm's length on a phone. */
    float s = SDL_max(np_ui_scale(app), floorf(SDL_min(app->out_w, app->out_h) / 270.0f));
    np_ui_frame(app, (SDL_FRect){r[0], r[1], r[2], r[3]}, 2 * s, accent);
    np_ui_fill(app, handle(app), accent);

    /* Toolbar in the middle of the window. */
    static const char *const labels[BAR_COUNT] = {"<", ">", "Smaller", "Bigger", "Fainter", "Bolder", "Reset", "Done"};
    float cw = 8 * s, bh = 14 * s, bw = 9 * cw, gap = cw * 0.5f;
    float pw = 4 * bw + 5 * gap, ph = 2 * bh + 4 * gap + 3 * 10 * s;
    float px = floorf((app->out_w - pw) * 0.5f), py = floorf((app->out_h - ph) * 0.5f);
    np_ui_fill(app, (SDL_FRect){px, py, pw, ph}, (SDL_Color){20, 22, 30, 225});
    np_ui_frame(app, (SDL_FRect){px, py, pw, ph}, s, dim);
    char line[96];
    const np_tc_item *it = &l->item[tp.sel];
    SDL_snprintf(line, sizeof line, "%s controls: %s", o ? "Portrait" : "Landscape", np_tc_names[tp.sel]);
    np_ui_text_clip(app, px + gap, py + gap, s, line, (int)((pw - 2 * gap) / cw), accent);
    SDL_snprintf(line, sizeof line, "size %d%%  opacity %d%%", (int)(it->w * 100 + 0.5f),
                 (int)(it->opacity * 100 + 0.5f));
    np_ui_text_clip(app, px + gap, py + gap + 10 * s, s, line, (int)((pw - 2 * gap) / cw), white);
    tp.nbar = BAR_COUNT;
    for (int i = 0; i < BAR_COUNT; i++) {
        tp.bar[i] = (SDL_FRect){px + gap + (float)(i % 4) * (bw + gap), py + 2 * gap + 20 * s + (float)(i / 4) * (bh + gap),
                                bw, bh};
        np_ui_button(app, tp.bar[i], labels[i], 0, -1, s);
    }
    np_ui_text_clip(app, px + gap, py + ph - gap - 8 * s, s, "Drag: move. Corner: resize.",
                    (int)((pw - 2 * gap) / cw), dim);
}
