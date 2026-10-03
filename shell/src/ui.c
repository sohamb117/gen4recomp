/*
 * The shell's own UI: launcher, Options, Controls (rebinding) and About,
 * drawn with SDL_Renderer and the embedded 8x8 font. Every page is driven
 * by the same abstract commands (np_menu_cmd) from keyboard and gamepad, and
 * by pointer hits (mouse, touch) recorded while drawing, so each page has one
 * selection model for all input devices.
 *
 * Text is scaled by whole numbers only, keeping the bitmap font crisp.
 */
#include "app.h"

#include <math.h>

#include "font8x8.h"
#include "romdb.h"
#include "storage.h"

#define HIT_SCROLL_UP 1000
#define HIT_SCROLL_DOWN 1001
#define HIT_BACK 1002

static const SDL_Color white = {235, 238, 245, 255};
static const SDL_Color dim = {150, 158, 175, 255};
static const SDL_Color accent = {255, 205, 80, 255};
static const SDL_Color warn = {255, 140, 120, 255};
static const SDL_Color game_colors[NP_GAME_COUNT] = {{64, 110, 210, 255}, {200, 104, 150, 255}, {150, 140, 120, 255}};

/* ---- drawing primitives ---------------------------------------------- */

int np_ui_init(np_app *app)
{
    enum { COLS = 16, ROWS = NP_FONT_GLYPHS / COLS };
    static uint32_t pixels[ROWS * 8][COLS * 8];
    for (int g = 0; g < NP_FONT_GLYPHS; g++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                pixels[(g / COLS) * 8 + y][(g % COLS) * 8 + x] =
                    ((np_font8x8[g][y] >> x) & 1) ? 0xFFFFFFFFu : 0x00FFFFFFu;
    app->font_tex = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                                      COLS * 8, ROWS * 8);
    if (!app->font_tex)
        return -1;
    SDL_UpdateTexture(app->font_tex, NULL, pixels, COLS * 8 * 4);
    SDL_SetTextureBlendMode(app->font_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(app->font_tex, SDL_SCALEMODE_NEAREST);
    app->ui_press_hit = -1;
    return 0;
}

void np_ui_destroy(np_app *app)
{
    if (app->font_tex)
        SDL_DestroyTexture(app->font_tex);
    app->font_tex = NULL;
}

float np_ui_scale(const np_app *app)
{
    float s = floorf(SDL_min(app->out_w / 480.0f, app->out_h / 300.0f));
    return s < 1.0f ? 1.0f : s;
}

void np_ui_text(np_app *app, float x, float y, float scale, const char *s, SDL_Color c)
{
    SDL_SetTextureColorMod(app->font_tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(app->font_tex, c.a);
    float step = 8.0f * scale;
    for (; *s; s++, x += step) {
        unsigned char ch = (unsigned char)*s;
        if (ch == ' ')
            continue;
        if (ch < NP_FONT_FIRST || ch >= NP_FONT_FIRST + NP_FONT_GLYPHS)
            ch = '?';
        int g = ch - NP_FONT_FIRST;
        SDL_FRect src = {(float)(g % 16) * 8.0f, (float)(g / 16) * 8.0f, 8, 8};
        SDL_FRect dst = {x, y, step, step};
        SDL_RenderTexture(app->renderer, app->font_tex, &src, &dst);
    }
}

void np_ui_fill(np_app *app, SDL_FRect r, SDL_Color c)
{
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(app->renderer, &r);
}

void np_ui_frame(np_app *app, SDL_FRect r, float t, SDL_Color c)
{
    np_ui_fill(app, (SDL_FRect){r.x, r.y, r.w, t}, c);
    np_ui_fill(app, (SDL_FRect){r.x, r.y + r.h - t, r.w, t}, c);
    np_ui_fill(app, (SDL_FRect){r.x, r.y + t, t, r.h - 2 * t}, c);
    np_ui_fill(app, (SDL_FRect){r.x + r.w - t, r.y + t, t, r.h - 2 * t}, c);
}

/* Text clipped to `max_cols` characters. */
static void text_clip(np_app *app, float x, float y, float s, const char *str, int max_cols, SDL_Color c)
{
    char buf[256];
    int n = (int)SDL_strlen(str);
    if (max_cols < 1)
        return;
    if (n > max_cols && max_cols < (int)sizeof buf) {
        SDL_memcpy(buf, str, (size_t)max_cols);
        buf[max_cols] = '\0';
        if (max_cols > 3)
            buf[max_cols - 1] = buf[max_cols - 2] = buf[max_cols - 3] = '.';
        str = buf;
    }
    np_ui_text(app, x, y, s, str, c);
}

/* Word-wrapped text; returns the number of lines used. */
static int text_wrap(np_app *app, float x, float y, float s, float line_h, int cols, const char *str, SDL_Color c,
                     int draw)
{
    int lines = 0;
    char buf[256];
    if (cols < 8)
        cols = 8;
    if (cols > (int)sizeof buf - 1)
        cols = (int)sizeof buf - 1;
    while (*str) {
        int n = (int)SDL_strlen(str);
        int take = n;
        const char *nl = SDL_strchr(str, '\n');
        if (nl && nl - str < take)
            take = (int)(nl - str);
        if (take > cols) {
            take = cols;
            while (take > 0 && str[take] != ' ')
                take--;
            if (take == 0)
                take = cols; /* one long word, e.g. a hash or path */
        }
        if (draw) {
            SDL_memcpy(buf, str, (size_t)take);
            buf[take] = '\0';
            np_ui_text(app, x, y + (float)lines * line_h, s, buf, c);
        }
        lines++;
        str += take;
        while (*str == ' ' || *str == '\n')
            str++;
    }
    return lines;
}

static void hit_add(np_app *app, SDL_FRect r, int id)
{
    if (app->nhits < NP_MAX_HITS)
        app->hits[app->nhits++] = (np_hit){r, id};
}

static int hit_at(const np_app *app, float x, float y)
{
    for (int i = app->nhits - 1; i >= 0; i--) {
        const SDL_FRect *r = &app->hits[i].r;
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return app->hits[i].id;
    }
    return -1;
}

static void button(np_app *app, SDL_FRect r, const char *label, int selected, int id, float s)
{
    np_ui_fill(app, r, selected ? (SDL_Color){255, 205, 80, 60} : (SDL_Color){255, 255, 255, 18});
    np_ui_frame(app, r, s, selected ? accent : (SDL_Color){255, 255, 255, 70});
    float tw = (float)SDL_strlen(label) * 8.0f * s;
    np_ui_text(app, r.x + (r.w - tw) * 0.5f, r.y + (r.h - 8.0f * s) * 0.5f, s, label, selected ? accent : white);
    hit_add(app, r, id);
}

/* ---- options page ------------------------------------------------------ */

enum opt_item {
    OPT_LAYOUT,
    OPT_SWAP,
    OPT_ROTATION,
    OPT_SCALE,
    OPT_FILTER,
    OPT_FULLSCREEN,
    OPT_VSYNC,
    OPT_FPS_CAP,
    OPT_LOGIC_CLOCK,
    OPT_SPEED,
    OPT_FF_SPEED,
    OPT_VOLUME,
    OPT_MUTE,
    OPT_TOUCH,
    OPT_CONTROLS,
    OPT_ABOUT,
    OPT_QUIT_GAME,
    OPT_RESUME,
    OPT_COUNT
};

static const char *const opt_labels[OPT_COUNT] = {
    "Screen layout", "Swap screens", "Rotation", "Scaling", "Filter", "Fullscreen", "VSync", "Display FPS cap",
    "Logic clock", "Speed", "Fast-forward speed", "Volume", "Mute when unfocused", "Touch controls",
    "Controls...", "About...", "Quit to launcher", "Close",
};

static int options_items(const np_app *app, int *items)
{
    int n = 0;
    for (int i = 0; i < OPT_COUNT; i++) {
        if (i == OPT_QUIT_GAME && app->view != NP_VIEW_GAME)
            continue;
        items[n++] = i;
    }
    return n;
}

static int opt_is_action(int item) { return item >= OPT_CONTROLS; }

static const char *speed_name(int index)
{
    static const char *const names[NP_SPEED_COUNT] = {"1x", "2x", "3x", "4x", "8x", "Uncapped"};
    return names[index];
}

static void opt_value(const np_app *app, int item, char *buf, size_t n)
{
    static const char *const layouts[NP_LAYOUT_COUNT] = {"Vertical", "Side by side", "Hybrid", "Top only",
                                                         "Bottom only"};
    static const char *const rotations[4] = {"None", "90 (portrait)", "180", "270 (portrait)"};
    static const char *const touch[NP_TOUCH_MODE_COUNT] = {"Auto", "On", "Off"};
    const np_options *o = &app->opt;
    switch (item) {
    case OPT_LAYOUT: SDL_strlcpy(buf, layouts[o->layout], n); break;
    case OPT_SWAP: SDL_strlcpy(buf, o->swap ? "On" : "Off", n); break;
    case OPT_ROTATION: SDL_strlcpy(buf, rotations[o->rotation], n); break;
    case OPT_SCALE: SDL_strlcpy(buf, o->scale == NP_SCALE_INTEGER ? "Integer" : "Fit", n); break;
    case OPT_FILTER: SDL_strlcpy(buf, o->linear_filter ? "Linear" : "Nearest", n); break;
    case OPT_FULLSCREEN: SDL_strlcpy(buf, o->fullscreen ? "On" : "Off", n); break;
    case OPT_VSYNC: SDL_strlcpy(buf, o->vsync ? "On" : "Off", n); break;
    case OPT_FPS_CAP:
        if (np_fps_caps[o->fps_cap_index])
            SDL_snprintf(buf, n, "%d", np_fps_caps[o->fps_cap_index]);
        else
            SDL_strlcpy(buf, "Off", n);
        break;
    case OPT_LOGIC_CLOCK: SDL_strlcpy(buf, o->logic_clock_60 ? "Exact 60 Hz" : "DS 59.83 Hz", n); break;
    case OPT_SPEED: SDL_strlcpy(buf, speed_name(o->speed_index), n); break;
    case OPT_FF_SPEED: SDL_strlcpy(buf, speed_name(o->ff_speed_index), n); break;
    case OPT_VOLUME: SDL_snprintf(buf, n, "%d%%", o->volume); break;
    case OPT_MUTE: SDL_strlcpy(buf, o->mute_unfocused ? "On" : "Off", n); break;
    case OPT_TOUCH: SDL_strlcpy(buf, touch[o->touch_controls], n); break;
    default: buf[0] = '\0'; break;
    }
}

static int wrapi(int v, int n) { return ((v % n) + n) % n; }

static void opt_adjust(np_app *app, int item, int dir)
{
    np_options *o = &app->opt;
    switch (item) {
    case OPT_LAYOUT: o->layout = (np_layout_mode)wrapi((int)o->layout + dir, NP_LAYOUT_COUNT); break;
    case OPT_SWAP: o->swap = !o->swap; break;
    case OPT_ROTATION: o->rotation = wrapi(o->rotation + dir, 4); break;
    case OPT_SCALE: o->scale = o->scale == NP_SCALE_FIT ? NP_SCALE_INTEGER : NP_SCALE_FIT; break;
    case OPT_FILTER: o->linear_filter = !o->linear_filter; break;
    case OPT_FULLSCREEN: o->fullscreen = !o->fullscreen; break;
    case OPT_VSYNC: o->vsync = !o->vsync; break;
    case OPT_FPS_CAP: o->fps_cap_index = wrapi(o->fps_cap_index + dir, NP_FPS_CAP_COUNT); break;
    case OPT_LOGIC_CLOCK: o->logic_clock_60 = !o->logic_clock_60; break;
    case OPT_SPEED: o->speed_index = wrapi(o->speed_index + dir, NP_SPEED_COUNT); break;
    case OPT_FF_SPEED: o->ff_speed_index = wrapi(o->ff_speed_index + dir, NP_SPEED_COUNT); break;
    case OPT_VOLUME: o->volume = SDL_clamp(o->volume + dir * 10, 0, 100); break;
    case OPT_MUTE: o->mute_unfocused = !o->mute_unfocused; break;
    case OPT_TOUCH: o->touch_controls = wrapi(o->touch_controls + dir, NP_TOUCH_MODE_COUNT); break;
    default: return;
    }
    app->options_dirty = 1;
    np_app_apply_video_options(app);
    np_audio_update_gain(app);
}

static void opt_activate(np_app *app, int item, int dir)
{
    switch (item) {
    case OPT_CONTROLS: np_app_open_page(app, NP_PAGE_CONTROLS); break;
    case OPT_ABOUT: np_app_open_page(app, NP_PAGE_ABOUT); break;
    case OPT_QUIT_GAME:
        np_app_open_page(app, NP_PAGE_NONE);
        np_app_stop_game(app);
        break;
    case OPT_RESUME: np_app_open_page(app, NP_PAGE_NONE); break;
    default: opt_adjust(app, item, dir); break;
    }
}

/* ---- controls page ----------------------------------------------------- */

#define CTRL_RESET NP_ACT_COUNT
#define CTRL_BACK (NP_ACT_COUNT + 1)
#define CTRL_ROWS (NP_ACT_COUNT + 2)

static void key_name(const np_app *app, int act, int slot, char *buf, size_t n)
{
    int sc = app->opt.bind.key[act][slot];
    const char *name = sc ? SDL_GetScancodeName((SDL_Scancode)sc) : "";
    SDL_strlcpy(buf, *name ? name : "-", n);
}

static void controls_activate(np_app *app, int row)
{
    if (row == CTRL_RESET) {
        np_bindings_defaults(&app->opt.bind);
        app->options_dirty = 1;
        np_app_toast(app, "Controls reset to defaults");
    } else if (row == CTRL_BACK) {
        np_app_open_page(app, app->page_parent);
    } else {
        app->capture = 1;
        app->capture_deadline = SDL_GetTicksNS() + 6 * SDL_NS_PER_SECOND;
    }
}

int np_ui_capture_event(np_app *app, const SDL_Event *e, int pad)
{
    if (!app->capture || app->page != NP_PAGE_CONTROLS)
        return 0;
    int act = app->sel, col = app->col;
    if (e->type == SDL_EVENT_KEY_DOWN) {
        if (e->key.repeat)
            return 1;
        SDL_Scancode sc = e->key.scancode;
        if (sc == SDL_SCANCODE_ESCAPE) {
            /* cancel */
        } else if (sc == SDL_SCANCODE_DELETE) {
            if (col < NP_KEY_SLOTS)
                app->opt.bind.key[act][col] = 0;
            else
                app->opt.bind.pad[act] = NP_PAD_NONE;
            app->options_dirty = 1;
        } else if (col < NP_KEY_SLOTS) {
            np_bind_key(&app->opt.bind, (np_action)act, col, sc);
            app->options_dirty = 1;
        } else {
            return 1; /* a key while waiting for a button: keep waiting */
        }
        app->capture = 0;
        return 1;
    }
    if (pad != NP_PAD_NONE) {
        if (col == NP_KEY_SLOTS) {
            np_bind_pad(&app->opt.bind, (np_action)act, pad);
            app->options_dirty = 1;
        }
        app->capture = 0;
        return 1;
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_FINGER_DOWN) {
        app->capture = 0;
        app->ui_press_hit = -1;
        return 1;
    }
    return e->type == SDL_EVENT_GAMEPAD_BUTTON_UP || e->type == SDL_EVENT_MOUSE_BUTTON_UP ||
           e->type == SDL_EVENT_FINGER_UP;
}

/* ---- about page -------------------------------------------------------- */

static const char *const about_text[] = {
    "nativeplat",
    "Native ports of Pokemon Diamond, Pearl and Platinum (US) for macOS, iOS and Windows.",
    "",
    "The game is your own cartridge: nativeplat ships no game data. It reads the ROM dump you "
    "imported, which never leaves this device's app data folder.",
    "",
    "LICENSE",
    "nativeplat is free software, licensed under the GNU General Public License version 3 or (at your "
    "option) any later version (GPL-3.0-or-later). It comes with ABSOLUTELY NO WARRANTY.",
    "",
    "CREDITS",
    "pret - the pokeplatinum and pokediamond decompilation projects.",
    "wize-logic - pokeplatinum-port: the native PC port layer and armrec.",
    "melonDS - Nintendo DS hardware models (GPLv3+).",
    "WebAssembly Binary Toolkit (wabt) - wasm2c, which turns the game back into portable C.",
    "wasi-sdk - the clang toolchain that compiles the games to WebAssembly.",
    "SDL 3 - windowing, rendering, audio and input (zlib license).",
    "font8x8 by Daniel Hepper, from IBM's VGA fonts - public domain.",
    "",
    "Pokemon is a trademark of Nintendo, Creatures Inc. and GAME FREAK inc. This project is not "
    "affiliated with or endorsed by them.",
};

/* ---- launcher ---------------------------------------------------------- */

#define LB_IMPORT 10
#define LB_OPTIONS 11
#define LB_ABOUT 12
#define LB_QUIT 13

static int launcher_buttons(void)
{
#if defined(SDL_PLATFORM_IOS)
    return 3; /* iOS apps do not quit themselves */
#else
    return 4;
#endif
}

static void launcher_activate(np_app *app, int id)
{
    if (id >= 0 && id < NP_GAME_COUNT) {
        np_game g = (np_game)id;
        if (!np_core_available(g))
            SDL_snprintf(app->status, sizeof app->status, "This build does not include the %s core.",
                         np_game_title(g));
        else if (np_storage_rom_present(g))
            np_app_start_game(app, g);
        else
            np_app_open_import_dialog(app);
        return;
    }
    switch (id) {
    case LB_IMPORT: np_app_open_import_dialog(app); break;
    case LB_OPTIONS: np_app_open_page(app, NP_PAGE_OPTIONS); break;
    case LB_ABOUT: np_app_open_page(app, NP_PAGE_ABOUT); break;
    case LB_QUIT: {
        SDL_Event quit = {.type = SDL_EVENT_QUIT};
        SDL_PushEvent(&quit);
        break;
    }
    default: break;
    }
}

static void launcher_command(np_app *app, np_menu_cmd cmd)
{
    int sel = app->launcher_sel;
    int nb = launcher_buttons();
    int on_cards = sel < NP_GAME_COUNT;
    switch (cmd) {
    case NP_CMD_LEFT: sel = on_cards ? wrapi(sel - 1, NP_GAME_COUNT) : LB_IMPORT + wrapi(sel - LB_IMPORT - 1, nb); break;
    case NP_CMD_RIGHT: sel = on_cards ? wrapi(sel + 1, NP_GAME_COUNT) : LB_IMPORT + wrapi(sel - LB_IMPORT + 1, nb); break;
    case NP_CMD_UP:
    case NP_CMD_DOWN: sel = on_cards ? LB_IMPORT + SDL_min(sel, nb - 1) : SDL_min(sel - LB_IMPORT, NP_GAME_COUNT - 1); break;
    case NP_CMD_CONFIRM:
    case NP_CMD_CLOSE: launcher_activate(app, sel); return;
    default: return;
    }
    app->launcher_sel = sel;
}

static void draw_launcher(np_app *app)
{
    float s = np_ui_scale(app), cw = 8 * s, lh = 12 * s;
    float W = app->out_w, H = app->out_h, m = 2 * cw;
    np_ui_fill(app, (SDL_FRect){0, 0, W, H}, (SDL_Color){18, 20, 28, 255});

    float y = m;
    np_ui_text(app, m, y, 3 * s, "nativeplat", white);
    y += 3 * 8 * s + lh * 0.5f;
    y += lh * (float)text_wrap(app, m, y, s, lh, (int)((W - 2 * m) / cw),
                               "Pokemon Diamond, Pearl and Platinum. Bring your own cartridge.", dim, 1);
    y += lh;

    int wide = W >= H;
    float gap = cw;
    float card_w = wide ? (W - 2 * m - 2 * gap) / 3 : W - 2 * m;
    float card_h = wide ? 9 * lh : 5 * lh;
    for (int g = 0; g < NP_GAME_COUNT; g++) {
        SDL_FRect r = wide ? (SDL_FRect){m + (float)g * (card_w + gap), y, card_w, card_h}
                           : (SDL_FRect){m, y + (float)g * (card_h + gap), card_w, card_h};
        int selected = app->launcher_sel == g;
        SDL_Color c = game_colors[g];
        np_ui_fill(app, r, (SDL_Color){(Uint8)(c.r / 4), (Uint8)(c.g / 4), (Uint8)(c.b / 4), 255});
        np_ui_fill(app, (SDL_FRect){r.x, r.y, r.w, 2.2f * lh}, c);
        np_ui_frame(app, r, s, selected ? accent : (SDL_Color){255, 255, 255, 40});
        int cols = (int)((r.w - 2 * cw) / cw);
        float ts = (float)SDL_strlen(np_game_title((np_game)g)) * 16 * s <= r.w - 2 * cw ? 2 * s : s;
        np_ui_text(app, r.x + cw, r.y + (2.2f * lh - 8 * ts) * 0.5f, ts, np_game_title((np_game)g), white);
        const char *state;
        SDL_Color sc;
        if (!np_core_available((np_game)g)) {
            state = "Core not in this build";
            sc = warn;
        } else if (np_storage_rom_present((np_game)g)) {
            state = "Ready";
            sc = (SDL_Color){140, 230, 140, 255};
        } else {
            state = "Not imported";
            sc = dim;
        }
        text_clip(app, r.x + cw, r.y + 2.8f * lh, s, state, cols, sc);
        const np_rom_entry *e = np_romdb_accepted((np_game)g);
        if (wide && e)
            text_wrap(app, r.x + cw, r.y + 4.2f * lh, s, lh, cols, e->label, dim, 1);
        const char *hint = np_storage_rom_present((np_game)g) ? "Play" : "Import...";
        text_clip(app, r.x + cw, r.y + r.h - 1.5f * lh, s, hint, cols, selected ? accent : dim);
        hit_add(app, r, g);
    }
    y += wide ? card_h + 1.5f * lh : 3 * (card_h + gap) + 0.5f * lh;

    static const char *const labels[4] = {"Import ROM", "Options", "About", "Quit"};
    int nb = launcher_buttons();
    float bw = SDL_min(14 * cw, (W - 2 * m - (float)(nb - 1) * gap) / (float)nb), bh = 2 * lh;
    for (int i = 0; i < nb; i++)
        button(app, (SDL_FRect){m + (float)i * (bw + gap), y, bw, bh}, labels[i], app->launcher_sel == LB_IMPORT + i,
               LB_IMPORT + i, s);
    y += bh + lh;

    int cols = (int)((W - 2 * m) / cw);
    if (app->status[0])
        y += lh * (float)text_wrap(app, m, y, s, lh, cols, app->status, accent, 1) + 0.5f * lh;
    if (y < H - 3 * lh) {
        text_clip(app, m, H - m - 2 * lh, s, "Drop a .nds file on this window to import it.", cols, dim);
        char where[1200];
        SDL_snprintf(where, sizeof where, "%s data: %s", np_storage_is_portable() ? "Portable" : "User",
                     np_storage_root());
        text_clip(app, m, H - m - lh, s, where, cols, dim);
    }
}

/* ---- pages ------------------------------------------------------------- */

typedef struct page_frame {
    float s, cw, lh;
    SDL_FRect panel;
    float list_y; /* first row */
    int rows;     /* visible rows */
    int cols;     /* text columns inside the panel */
} page_frame;

static void begin_page(np_app *app, page_frame *f, const char *title)
{
    float s = np_ui_scale(app), cw = 8 * s, lh = 12 * s;
    float W = app->out_w, H = app->out_h;
    np_ui_fill(app, (SDL_FRect){0, 0, W, H}, (SDL_Color){0, 0, 0, 170});
    float pw = SDL_min(W - 2 * cw, 60 * cw), ph = SDL_min(H - 2 * cw, 36 * lh);
    SDL_FRect p = {floorf((W - pw) * 0.5f), floorf((H - ph) * 0.5f), pw, ph};
    np_ui_fill(app, p, (SDL_Color){24, 27, 38, 245});
    np_ui_frame(app, p, s, (SDL_Color){255, 255, 255, 60});
    np_ui_text(app, p.x + 2 * cw, p.y + lh, 2 * s, title, accent);
    f->s = s;
    f->cw = cw;
    f->lh = lh;
    f->panel = p;
    f->cols = (int)((pw - 4 * cw) / cw);
    f->list_y = p.y + lh + 16 * s + lh;
    f->rows = (int)((p.y + ph - 4 * lh - f->list_y) / lh);
    if (f->rows < 1)
        f->rows = 1;
}

/* Footer with a hint, Back, and scroll arrows when the list overflows. */
static void end_page(np_app *app, const page_frame *f, const char *hint, int total)
{
    float s = f->s, cw = f->cw, lh = f->lh;
    SDL_FRect p = f->panel;
    float y = p.y + p.h - 3 * lh;
    text_clip(app, p.x + 2 * cw, y - 0.5f * lh, s, hint, f->cols, dim);
    float bh = 1.8f * lh, by = p.y + p.h - bh - 0.6f * lh;
    button(app, (SDL_FRect){p.x + 2 * cw, by, 8 * cw, bh}, "Back", 0, HIT_BACK, s);
    if (total > f->rows) {
        button(app, (SDL_FRect){p.x + p.w - 2 * cw - 14 * cw, by, 6 * cw, bh}, "Up", 0, HIT_SCROLL_UP, s);
        button(app, (SDL_FRect){p.x + p.w - 2 * cw - 7 * cw, by, 6 * cw, bh}, "Down", 0, HIT_SCROLL_DOWN, s);
    }
}

static void keep_visible(np_app *app, int sel, int rows, int total)
{
    if (sel < app->scroll)
        app->scroll = sel;
    if (sel >= app->scroll + rows)
        app->scroll = sel - rows + 1;
    app->scroll = SDL_clamp(app->scroll, 0, SDL_max(0, total - rows));
}

static void draw_options(np_app *app)
{
    page_frame f;
    begin_page(app, &f, "Options");
    int items[OPT_COUNT];
    int n = options_items(app, items);
    app->sel = SDL_clamp(app->sel, 0, n - 1);
    keep_visible(app, app->sel, f.rows, n);
    float x = f.panel.x + 2 * f.cw, vx = f.panel.x + f.panel.w * 0.5f;
    int vcols = (int)((f.panel.x + f.panel.w - 2 * f.cw - vx) / f.cw);
    for (int r = 0; r < f.rows && app->scroll + r < n; r++) {
        int i = app->scroll + r, item = items[i], selected = i == app->sel;
        float y = f.list_y + (float)r * f.lh;
        SDL_FRect row = {f.panel.x + f.cw, y - 2 * f.s, f.panel.w - 2 * f.cw, f.lh};
        if (selected)
            np_ui_fill(app, row, (SDL_Color){255, 205, 80, 40});
        text_clip(app, x, y, f.s, opt_labels[item], (int)((vx - x) / f.cw) - 1, selected ? accent : white);
        char v[64], shown[80];
        opt_value(app, item, v, sizeof v);
        if (*v) {
            SDL_snprintf(shown, sizeof shown, selected ? "< %s >" : "  %s", v);
            text_clip(app, vx, y, f.s, shown, vcols, selected ? accent : dim);
        }
        hit_add(app, row, i);
    }
    end_page(app, &f, "Left/Right: change  Enter/A: select  Esc/B: back", n);
}

static void draw_controls(np_app *app)
{
    page_frame f;
    begin_page(app, &f, "Controls");
    app->sel = SDL_clamp(app->sel, 0, CTRL_ROWS - 1);
    keep_visible(app, app->sel, f.rows, CTRL_ROWS);
    if (app->capture && SDL_GetTicksNS() > app->capture_deadline)
        app->capture = 0;
    float x = f.panel.x + 2 * f.cw;
    float name_w = 12 * f.cw;
    float slot_w = (f.panel.w - 4 * f.cw - name_w) / 4.0f;
    int slot_cols = (int)(slot_w / f.cw) - 1;
    for (int r = 0; r < f.rows && app->scroll + r < CTRL_ROWS; r++) {
        int row = app->scroll + r;
        float y = f.list_y + (float)r * f.lh;
        int row_sel = row == app->sel;
        if (row >= NP_ACT_COUNT) {
            SDL_FRect rr = {f.panel.x + f.cw, y - 2 * f.s, f.panel.w - 2 * f.cw, f.lh};
            if (row_sel)
                np_ui_fill(app, rr, (SDL_Color){255, 205, 80, 40});
            np_ui_text(app, x, y, f.s, row == CTRL_RESET ? "Reset to defaults" : "Back",
                       row_sel ? accent : white);
            hit_add(app, rr, row * 4);
            continue;
        }
        const char *name = np_action_name((np_action)row);
        /* Long shell action names get their short form in the grid. */
        if (row == NP_ACT_FF_HOLD)
            name = "FF hold";
        else if (row == NP_ACT_FF_TOGGLE)
            name = "FF toggle";
        np_ui_text(app, x, y, f.s, name, row_sel ? accent : white);
        for (int c = 0; c < 4; c++) {
            SDL_FRect cell = {x + name_w + (float)c * slot_w, y - 2 * f.s, slot_w - f.cw * 0.5f, f.lh};
            int cell_sel = row_sel && c == app->col;
            char label[64];
            if (cell_sel && app->capture)
                SDL_strlcpy(label, c < NP_KEY_SLOTS ? "press key" : "press btn", sizeof label);
            else if (c < NP_KEY_SLOTS)
                key_name(app, row, c, label, sizeof label);
            else
                SDL_strlcpy(label, np_pad_binding_name(app->opt.bind.pad[row]), sizeof label);
            if (cell_sel)
                np_ui_fill(app, cell, app->capture ? (SDL_Color){255, 120, 80, 90} : (SDL_Color){255, 205, 80, 50});
            text_clip(app, cell.x + f.cw * 0.5f, y, f.s, label, slot_cols,
                      cell_sel ? accent : (c == NP_KEY_SLOTS ? (SDL_Color){150, 200, 255, 255} : white));
            hit_add(app, cell, row * 4 + c);
        }
    }
    end_page(app, &f,
             app->capture ? "Press a key/button. Esc: cancel  Delete: clear"
                          : "Enter/A: rebind  Right-click: clear  Esc/B: back",
             CTRL_ROWS);
}

static void draw_about(np_app *app)
{
    page_frame f;
    begin_page(app, &f, "About");
    /* Lay the text out once per frame into wrapped lines, then window it. */
    int total = 0;
    for (size_t i = 0; i < SDL_arraysize(about_text); i++)
        total += *about_text[i] ? text_wrap(app, 0, 0, f.s, f.lh, f.cols, about_text[i], white, 0) : 1;
    app->scroll = SDL_clamp(app->scroll, 0, SDL_max(0, total - f.rows));
    int line = 0;
    float x = f.panel.x + 2 * f.cw;
    for (size_t i = 0; i < SDL_arraysize(about_text); i++) {
        const char *t = about_text[i];
        int is_heading = i == 0 || !SDL_strcmp(t, "LICENSE") || !SDL_strcmp(t, "CREDITS");
        int n = *t ? text_wrap(app, 0, 0, f.s, f.lh, f.cols, t, white, 0) : 1;
        /* Draw only paragraphs fully inside the window. */
        if (*t && line >= app->scroll && line + n <= app->scroll + f.rows)
            text_wrap(app, x, f.list_y + (float)(line - app->scroll) * f.lh, f.s, f.lh, f.cols, t,
                      is_heading ? accent : white, 1);
        line += n;
    }
    end_page(app, &f, "Up/Down scroll, Esc/B back.", total);
}

/* ---- dispatch ---------------------------------------------------------- */

static void page_back(np_app *app)
{
    np_page from = app->page;
    np_page to = from == NP_PAGE_OPTIONS ? NP_PAGE_NONE : app->page_parent;
    np_app_open_page(app, to);
    if (to == NP_PAGE_OPTIONS) {
        int items[OPT_COUNT], n = options_items(app, items);
        for (int i = 0; i < n; i++)
            if (items[i] == (from == NP_PAGE_CONTROLS ? OPT_CONTROLS : OPT_ABOUT))
                app->sel = i;
    }
}

void np_ui_command(np_app *app, np_menu_cmd cmd)
{
    if (cmd == NP_CMD_NONE)
        return;
    if (app->page == NP_PAGE_NONE) {
        if (app->view == NP_VIEW_LAUNCHER)
            launcher_command(app, cmd);
        return;
    }
    if (cmd == NP_CMD_CLOSE) {
        np_app_open_page(app, NP_PAGE_NONE);
        return;
    }
    if (cmd == NP_CMD_BACK) {
        page_back(app);
        return;
    }
    if (app->page == NP_PAGE_OPTIONS) {
        int items[OPT_COUNT], n = options_items(app, items);
        int item = items[SDL_clamp(app->sel, 0, n - 1)];
        switch (cmd) {
        case NP_CMD_UP: app->sel = wrapi(app->sel - 1, n); break;
        case NP_CMD_DOWN: app->sel = wrapi(app->sel + 1, n); break;
        case NP_CMD_LEFT:
            if (!opt_is_action(item))
                opt_adjust(app, item, -1);
            break;
        case NP_CMD_RIGHT:
            if (!opt_is_action(item))
                opt_adjust(app, item, 1);
            break;
        case NP_CMD_CONFIRM: opt_activate(app, item, 1); break;
        default: break;
        }
    } else if (app->page == NP_PAGE_CONTROLS) {
        switch (cmd) {
        case NP_CMD_UP: app->sel = wrapi(app->sel - 1, CTRL_ROWS); break;
        case NP_CMD_DOWN: app->sel = wrapi(app->sel + 1, CTRL_ROWS); break;
        case NP_CMD_LEFT: app->col = wrapi(app->col - 1, 4); break;
        case NP_CMD_RIGHT: app->col = wrapi(app->col + 1, 4); break;
        case NP_CMD_CONFIRM: controls_activate(app, app->sel); break;
        default: break;
        }
    } else if (app->page == NP_PAGE_ABOUT) {
        if (cmd == NP_CMD_UP)
            app->scroll--;
        else if (cmd == NP_CMD_DOWN)
            app->scroll++;
        else if (cmd == NP_CMD_CONFIRM)
            page_back(app);
        if (app->scroll < 0)
            app->scroll = 0;
    }
}

static void select_hit(np_app *app, int id)
{
    if (id >= HIT_SCROLL_UP)
        return;
    if (app->page == NP_PAGE_NONE)
        app->launcher_sel = id;
    else if (app->page == NP_PAGE_OPTIONS)
        app->sel = id;
    else if (app->page == NP_PAGE_CONTROLS) {
        app->sel = id / 4;
        if (id / 4 < NP_ACT_COUNT)
            app->col = id % 4;
    }
}

static void activate_hit(np_app *app, int id, int dir)
{
    if (id == HIT_BACK) {
        page_back(app);
        return;
    }
    if (id == HIT_SCROLL_UP || id == HIT_SCROLL_DOWN) {
        np_menu_cmd cmd = id == HIT_SCROLL_UP ? NP_CMD_UP : NP_CMD_DOWN;
        if (app->page == NP_PAGE_ABOUT) {
            for (int i = 0; i < 4; i++)
                np_ui_command(app, cmd);
        } else {
            app->scroll += id == HIT_SCROLL_UP ? -4 : 4;
            if (app->scroll < 0)
                app->scroll = 0;
            /* Keep the selection on screen so the next draw does not
             * scroll straight back to it. */
            app->sel = app->scroll;
        }
        return;
    }
    select_hit(app, id);
    if (app->page == NP_PAGE_NONE) {
        launcher_activate(app, id);
    } else if (app->page == NP_PAGE_OPTIONS) {
        int items[OPT_COUNT], n = options_items(app, items);
        if (id < n)
            opt_activate(app, items[id], dir);
    } else if (app->page == NP_PAGE_CONTROLS) {
        if (dir < 0 && id / 4 < NP_ACT_COUNT) {
            /* Secondary click clears the slot. */
            if (id % 4 < NP_KEY_SLOTS)
                app->opt.bind.key[id / 4][id % 4] = 0;
            else
                app->opt.bind.pad[id / 4] = NP_PAD_NONE;
            app->options_dirty = 1;
        } else {
            controls_activate(app, id / 4);
        }
    }
}

void np_ui_pointer(np_app *app, float x, float y, int pressed, int released, int button)
{
    int id = hit_at(app, x, y);
    if (pressed) {
        app->ui_press_hit = id;
        if (id >= 0)
            select_hit(app, id);
        return;
    }
    if (released) {
        int was = app->ui_press_hit;
        app->ui_press_hit = -1;
        if (id >= 0 && id == was)
            activate_hit(app, id, button == 3 ? -1 : 1);
        return;
    }
    if (id >= 0 && !app->capture)
        select_hit(app, id);
}

void np_ui_draw(np_app *app)
{
    app->nhits = 0;
    if (app->view == NP_VIEW_LAUNCHER)
        draw_launcher(app);
    if (app->page != NP_PAGE_NONE)
        app->nhits = 0; /* the page is modal: launcher hits underneath are dead */
    switch (app->page) {
    case NP_PAGE_OPTIONS: draw_options(app); break;
    case NP_PAGE_CONTROLS: draw_controls(app); break;
    case NP_PAGE_ABOUT: draw_about(app); break;
    default: break;
    }
    if (app->toast[0] && SDL_GetTicksNS() < app->toast_until) {
        float s = np_ui_scale(app);
        float w = (float)SDL_strlen(app->toast) * 8 * s + 16 * s;
        SDL_FRect r = {floorf((app->out_w - w) * 0.5f), 8 * s, w, 16 * s};
        np_ui_fill(app, r, (SDL_Color){0, 0, 0, 190});
        np_ui_text(app, r.x + 8 * s, r.y + 4 * s, s, app->toast, white);
    }
}
