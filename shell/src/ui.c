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
void np_ui_text_clip(np_app *app, float x, float y, float s, const char *str, int max_cols, SDL_Color c)
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
int np_ui_text_wrap(np_app *app, float x, float y, float s, float line_h, int cols, const char *str, SDL_Color c,
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

void np_ui_hit(np_app *app, SDL_FRect r, int id)
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

void np_ui_button(np_app *app, SDL_FRect r, const char *label, int selected, int id, float s)
{
    np_ui_fill(app, r, selected ? (SDL_Color){255, 205, 80, 60} : (SDL_Color){255, 255, 255, 18});
    np_ui_frame(app, r, s, selected ? accent : (SDL_Color){255, 255, 255, 70});
    float tw = (float)SDL_strlen(label) * 8.0f * s;
    np_ui_text(app, r.x + (r.w - tw) * 0.5f, r.y + (r.h - 8.0f * s) * 0.5f, s, label, selected ? accent : white);
    np_ui_hit(app, r, id);
}

/* ---- options page ------------------------------------------------------ */

enum opt_item {
    OPT_LAYOUT,
    OPT_SWAP,
    OPT_ROTATION,
    OPT_SCALE,
    OPT_FILTER,
    OPT_FULLSCREEN,
    OPT_FX1,
    OPT_FX1_INT,
    OPT_FX2,
    OPT_FX2_INT,
    OPT_CURVATURE,
    OPT_PERF,
    OPT_VSYNC,
    OPT_FPS_CAP,
    OPT_LOGIC_CLOCK,
    OPT_REAL_CLOCK,
    OPT_STARTUP,
    OPT_SPEED,
    OPT_FF_SPEED,
    OPT_VOLUME,
    OPT_MUTE,
    OPT_BGM,
    OPT_SE,
    OPT_RENDER_SCALE,
    OPT_WIDESCREEN,
    OPT_CAMERA_ZOOM,
    OPT_CAMERA_TILT,
    OPT_TEXT_INSTANT,
    OPT_FIX_BUGS,
    OPT_REWIND,
    OPT_GBA_ROM,
    OPT_GBA_SAVE,
    OPT_TOUCH,
    OPT_TOUCH_EDIT,
    OPT_RUMBLE,
    OPT_LAN,
    OPT_LAN_PORT,
    OPT_LAN_PEER,
    OPT_LAN_RELAY,
    OPT_LAN_PIN,
    OPT_LAN_STATUS,
    OPT_SYNC_FOLDER,
    OPT_SYNC_NOW,
    OPT_SYNC_STATUS,
    OPT_CONTROLS,
    OPT_MODS,
    OPT_UPDATES,
    OPT_ABOUT,
    OPT_QUIT_GAME,
    OPT_RESUME,
    OPT_COUNT
};

static const char *const opt_labels[OPT_COUNT] = {
    "Screen layout", "Swap screens", "Rotation", "Scaling", "Filter", "Fullscreen", "Effect 1", "Effect 1 intensity",
    "Effect 2", "Effect 2 intensity", "CRT curvature", "Performance", "VSync", "Display FPS cap",
    "Logic clock", "Real-time clock", "On startup", "Speed", "Fast-forward speed", "Volume", "Mute when unfocused",
    "Music volume", "Sound effects volume", "3D render scale", "Widescreen 3D", "Camera zoom", "Camera tilt",
    "Instant text", "Fix cartridge bugs", "Rewind history", "GBA cartridge (Pal Park)", "GBA save",
    "Touch controls", "Edit touch controls...", "Rumble on press (gamepad)", "Local wireless (LAN)", "LAN port",
    "Join by IP:port", "Internet relay host:port", "Room PIN",
    "Wireless status", "Sync folder", "Sync now", "Sync status",
    "Controls...", "Mods...", "Updates...", "About...", "Quit to launcher", "Close",
};

static int options_items(const np_app *app, int *items)
{
    int n = 0;
    for (int i = 0; i < OPT_COUNT; i++) {
        if (i == OPT_QUIT_GAME && app->view != NP_VIEW_GAME)
            continue;
        if ((i == OPT_SYNC_NOW || i == OPT_SYNC_STATUS) && !app->opt.sync_folder[0])
            continue;
        if (i == OPT_UPDATES && !np_update_enabled(app))
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
    static const char *const fx_names[NP_FX_COUNT] = {"Off", "LCD grid", "Scanlines", "CRT", "Smooth"};
    static const char *const perf_names[NP_PERF_COUNT] = {"Custom", "High", "Balanced", "Low", "Auto"};
    const np_options *o = &app->opt;
    switch (item) {
    case OPT_LAYOUT: SDL_strlcpy(buf, layouts[o->layout], n); break;
    case OPT_SWAP: SDL_strlcpy(buf, o->swap ? "On" : "Off", n); break;
    case OPT_ROTATION: SDL_strlcpy(buf, rotations[o->rotation], n); break;
    case OPT_SCALE: SDL_strlcpy(buf, o->scale == NP_SCALE_INTEGER ? "Integer" : "Fit", n); break;
    case OPT_FILTER: SDL_strlcpy(buf, o->linear_filter ? "Linear" : "Nearest", n); break;
    case OPT_FULLSCREEN: SDL_strlcpy(buf, o->fullscreen ? "On" : "Off", n); break;
    case OPT_FX1:
    case OPT_FX2: SDL_strlcpy(buf, fx_names[o->fx[item == OPT_FX2]], n); break;
    case OPT_FX1_INT:
    case OPT_FX2_INT: SDL_snprintf(buf, n, "%d%%", o->fx_intensity[item == OPT_FX2_INT]); break;
    case OPT_CURVATURE: SDL_strlcpy(buf, o->crt_curvature ? "On" : "Off", n); break;
    case OPT_PERF: SDL_strlcpy(buf, perf_names[o->perf], n); break;
    case OPT_VSYNC:
        SDL_strlcpy(buf, o->vsync ? "On" : "Off", n);
        if (o->perf != NP_PERF_CUSTOM)
            SDL_strlcat(buf, " (preset decides)", n);
        break;
    case OPT_FPS_CAP:
        if (np_fps_caps[o->fps_cap_index])
            SDL_snprintf(buf, n, "%d", np_fps_caps[o->fps_cap_index]);
        else
            SDL_strlcpy(buf, "Off", n);
        break;
    case OPT_LOGIC_CLOCK: SDL_strlcpy(buf, o->logic_clock_60 ? "Exact 60 Hz" : "DS 59.83 Hz", n); break;
    case OPT_REAL_CLOCK:
        SDL_strlcpy(buf, o->real_clock ? "Device time" : "Fixed (2009-03-22)", n);
        if (app->view == NP_VIEW_GAME && (app->host.rtc_now != NULL) != o->real_clock)
            SDL_strlcat(buf, ", next boot", n);
        break;
    case OPT_STARTUP: SDL_strlcpy(buf, o->startup_continue ? "Continue last game" : "Launcher", n); break;
    case OPT_SPEED: SDL_strlcpy(buf, speed_name(o->speed_index), n); break;
    case OPT_FF_SPEED: SDL_strlcpy(buf, speed_name(o->ff_speed_index), n); break;
    case OPT_VOLUME: SDL_snprintf(buf, n, "%d%%", o->volume); break;
    case OPT_MUTE: SDL_strlcpy(buf, o->mute_unfocused ? "On" : "Off", n); break;
    case OPT_BGM: SDL_snprintf(buf, n, "%d%%", o->bgm_volume); break;
    case OPT_SE: SDL_snprintf(buf, n, "%d%%", o->se_volume); break;
    case OPT_RENDER_SCALE: SDL_snprintf(buf, n, "%dx (%dx%d)", o->render_scale, 256 * o->render_scale,
                                        192 * o->render_scale); break;
    case OPT_WIDESCREEN: SDL_strlcpy(buf, o->widescreen ? "On" : "Off", n); break;
    case OPT_CAMERA_ZOOM:
        SDL_snprintf(buf, n, "%d%%%s (- / =)", o->camera_zoom * 100 / 256, o->camera_zoom == 256 ? " original" : "");
        break;
    case OPT_CAMERA_TILT: SDL_snprintf(buf, n, "%+d deg (3 / 4)", o->camera_tilt / 16); break;
    case OPT_TEXT_INSTANT: SDL_strlcpy(buf, o->text_instant ? "On" : "Off", n); break;
    case OPT_FIX_BUGS: SDL_strlcpy(buf, o->fix_bugs ? "On" : "Off (as the cartridge)", n); break;
    case OPT_REWIND:
        if (o->rewind_seconds)
            SDL_snprintf(buf, n, "%d s (hold R / L3)", o->rewind_seconds);
        else
            SDL_strlcpy(buf, "Off", n);
        break;
    case OPT_GBA_ROM: {
        const char *slash = SDL_strrchr(o->gba_rom, '/');
        if (!o->gba_rom[0])
            SDL_strlcpy(buf, "Empty (Enter: insert)", n);
        else
            SDL_snprintf(buf, n, "%s%s", slash ? slash + 1 : o->gba_rom,
                         app->core && !np_app_gba_inserted(app) ? " (next boot)" : "");
        break;
    }
    case OPT_GBA_SAVE: {
        const char *slash = SDL_strrchr(o->gba_save, '/');
        SDL_strlcpy(buf, o->gba_save[0] ? (slash ? slash + 1 : o->gba_save) : "<cartridge name>.sav", n);
        break;
    }
    case OPT_TOUCH: SDL_strlcpy(buf, touch[o->touch_controls], n); break;
    case OPT_RUMBLE: SDL_strlcpy(buf, o->rumble ? "On" : "Off", n); break;
    case OPT_LAN: SDL_strlcpy(buf, o->lan_enabled ? "On" : "Off", n); break;
    case OPT_LAN_PORT: SDL_snprintf(buf, n, "%d", o->lan_port); break;
    case OPT_LAN_PEER: SDL_strlcpy(buf, o->lan_peer[0] ? o->lan_peer : "(LAN discovery only)", n); break;
    case OPT_LAN_RELAY: SDL_strlcpy(buf, o->lan_relay[0] ? o->lan_relay : "(none: LAN)", n); break;
    case OPT_LAN_PIN: SDL_strlcpy(buf, o->lan_pin[0] ? o->lan_pin : "(none)", n); break;
    case OPT_LAN_STATUS: {
        int peers = np_app_net_peers(app);
        if (app->net_error[0])
            SDL_strlcpy(buf, app->net_error, n);
        else if (peers < 0)
            SDL_strlcpy(buf, "Off", n);
        else
            SDL_snprintf(buf, n, "%d in range, id %06X", peers, (unsigned)o->station_id);
        break;
    }
    case OPT_SYNC_FOLDER: {
        /* The folder's tail: the start of a long path says little. */
        size_t len = SDL_strlen(o->sync_folder);
        if (!len)
            SDL_strlcpy(buf, "Off (Enter: choose)", n);
        else if (len < n)
            SDL_strlcpy(buf, o->sync_folder, n);
        else
            SDL_snprintf(buf, n, "...%s", o->sync_folder + len - (n - 4));
        break;
    }
    case OPT_SYNC_NOW: SDL_strlcpy(buf, "Enter", n); break;
    case OPT_SYNC_STATUS: {
        int c = np_sync_conflicts(app);
        if (c)
            SDL_snprintf(buf, n, "%d conflict%s: Enter to choose", c, c == 1 ? "" : "s");
        else
            SDL_strlcpy(buf, app->sync_status[0] ? app->sync_status : "Not synced yet", n);
        break;
    }
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
    case OPT_FX1:
    case OPT_FX2: o->fx[item == OPT_FX2] = wrapi(o->fx[item == OPT_FX2] + dir, NP_FX_COUNT); break;
    case OPT_FX1_INT:
    case OPT_FX2_INT: {
        int *v = &o->fx_intensity[item == OPT_FX2_INT];
        *v = SDL_clamp(*v + dir * 10, 0, 100);
        break;
    }
    case OPT_CURVATURE: o->crt_curvature = !o->crt_curvature; break;
    case OPT_PERF: o->perf = wrapi(o->perf + dir, NP_PERF_COUNT); break;
    case OPT_VSYNC: o->vsync = !o->vsync; break;
    case OPT_FPS_CAP: o->fps_cap_index = wrapi(o->fps_cap_index + dir, NP_FPS_CAP_COUNT); break;
    case OPT_LOGIC_CLOCK: o->logic_clock_60 = !o->logic_clock_60; break;
    case OPT_REAL_CLOCK: o->real_clock = !o->real_clock; break;
    case OPT_STARTUP: o->startup_continue = !o->startup_continue; break;
    case OPT_SPEED: o->speed_index = wrapi(o->speed_index + dir, NP_SPEED_COUNT); break;
    case OPT_FF_SPEED: o->ff_speed_index = wrapi(o->ff_speed_index + dir, NP_SPEED_COUNT); break;
    case OPT_VOLUME: o->volume = SDL_clamp(o->volume + dir * 10, 0, 100); break;
    case OPT_MUTE: o->mute_unfocused = !o->mute_unfocused; break;
    case OPT_BGM: o->bgm_volume = SDL_clamp(o->bgm_volume + dir * 10, 0, 100); break;
    case OPT_SE: o->se_volume = SDL_clamp(o->se_volume + dir * 10, 0, 100); break;
    case OPT_RENDER_SCALE: o->render_scale = wrapi(o->render_scale - 1 + dir, 4) + 1; break;
    case OPT_WIDESCREEN: o->widescreen = !o->widescreen; break;
    case OPT_CAMERA_ZOOM: o->camera_zoom = SDL_clamp(o->camera_zoom + dir * 32, 64, 1024); break;
    case OPT_CAMERA_TILT: o->camera_tilt = SDL_clamp(o->camera_tilt + dir * 80, -720, 720); break;
    case OPT_TEXT_INSTANT: o->text_instant = !o->text_instant; break;
    case OPT_FIX_BUGS: o->fix_bugs = !o->fix_bugs; break;
    case OPT_REWIND: {
        static const int secs[] = {0, 10, 30, 60};
        int k = 0;
        for (int i = 0; i < 4; i++)
            if (secs[i] == o->rewind_seconds)
                k = i;
        o->rewind_seconds = secs[wrapi(k + dir, 4)];
        break;
    }
    case OPT_TOUCH: o->touch_controls = wrapi(o->touch_controls + dir, NP_TOUCH_MODE_COUNT); break;
    case OPT_RUMBLE:
        o->rumble = !o->rumble;
        np_input_rumble(app); /* feel it */
        break;
    case OPT_LAN:
        o->lan_enabled = !o->lan_enabled;
        np_app_net_apply(app);
        break;
    case OPT_LAN_PORT:
        o->lan_port = SDL_clamp(o->lan_port + dir, 1024, 65531);
        if (o->lan_enabled)
            np_app_net_apply(app);
        break;
    case OPT_LAN_STATUS: return;
    case OPT_SYNC_FOLDER:
        if (dir < 0) { /* Left turns sync off */
            o->sync_folder[0] = '\0';
            app->sync_status[0] = '\0';
            break;
        }
        np_app_open_sync_folder_dialog(app);
        return;
    case OPT_SYNC_NOW: np_sync_all(app, 0); return;
    case OPT_GBA_ROM:
        if (dir < 0) { /* Left ejects */
            o->gba_rom[0] = o->gba_save[0] = '\0';
            np_app_toast(app, "GBA slot empty%s", app->core ? " from the next boot" : "");
            break;
        }
        np_app_open_gba_dialog(app, 0);
        return;
    case OPT_GBA_SAVE:
        if (dir < 0) {
            o->gba_save[0] = '\0';
            break;
        }
        np_app_open_gba_dialog(app, 1);
        return;
    case OPT_SYNC_STATUS:
        if (np_sync_conflicts(app))
            np_app_open_page(app, NP_PAGE_SYNC);
        return;
    default: return;
    }
    app->options_dirty = 1;
    np_app_apply_video_options(app);
    np_audio_update_gain(app);
    np_session_apply_options(app);
}

static void opt_activate(np_app *app, int item, int dir)
{
    switch (item) {
    case OPT_LAN_PEER: np_ui_open_text(app, NP_TEXT_LAN_PEER, app->opt.lan_peer, 32); break;
    case OPT_LAN_RELAY: np_ui_open_text(app, NP_TEXT_LAN_RELAY, app->opt.lan_relay, 64); break;
    case OPT_LAN_PIN: np_ui_open_text(app, NP_TEXT_LAN_PIN, app->opt.lan_pin, 32); break;
    case OPT_CONTROLS: np_app_open_page(app, NP_PAGE_CONTROLS); break;
    case OPT_MODS: np_mods_open(app, NULL); break;
    case OPT_UPDATES: np_update_open(app); break;
    case OPT_TOUCH_EDIT: np_touchedit_open(app); break;
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

static int text_capture_event(np_app *app, const SDL_Event *e);

int np_ui_capture_event(np_app *app, const SDL_Event *e, int pad)
{
    if (app->page == NP_PAGE_EDITOR)
        return np_editor_event(app, e);
    if (app->page == NP_PAGE_TOUCH_EDIT)
        return e->type == SDL_EVENT_KEY_DOWN && np_touchedit_key(app, &e->key);
    if (app->page == NP_PAGE_TEXT)
        return text_capture_event(app, e);
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
            SDL_snprintf(app->status, sizeof app->status, "The %s core is not included in this build.",
                         np_game_title(g));
        else if (np_storage_rom_present(g))
            np_app_open_slots(app, g);
        else
            np_app_open_rom_dialog(app);
        return;
    }
    switch (id) {
    case LB_IMPORT: np_app_open_rom_dialog(app); break;
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
    y += lh * (float)np_ui_text_wrap(app, m, y, s, lh, (int)((W - 2 * m) / cw),
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
        np_game game = (np_game)g;
        int avail = np_core_available(game), present = np_storage_rom_present(game);
        const char *state;
        SDL_Color sc = dim;
        if (avail && present) {
            state = "Ready";
            sc = (SDL_Color){140, 230, 140, 255};
        } else if (present) {
            state = "Imported; core not included in this build";
            sc = warn;
        } else if (avail) {
            state = "Not imported";
        } else {
            state = "Core not included in this build";
            sc = warn;
        }
        int state_lines = 1;
        if (wide)
            state_lines = np_ui_text_wrap(app, r.x + cw, r.y + 2.8f * lh, s, lh, cols, state, sc, 1);
        else
            np_ui_text_clip(app, r.x + cw, r.y + 2.8f * lh, s, state, cols, sc);
        const np_rom_entry *e = np_romdb_accepted(game);
        if (wide && e && state_lines == 1)
            np_ui_text_wrap(app, r.x + cw, r.y + 4.2f * lh, s, lh, cols, e->label, dim, 1);
        char hint[64];
        if (avail && present && app->opt.last_slot[g][0])
            SDL_snprintf(hint, sizeof hint, "Continue: %s", app->opt.last_slot[g]);
        else
            SDL_strlcpy(hint, avail && present ? "Choose a save slot" : avail ? "Import..." : "", sizeof hint);
        if (hint[0])
            np_ui_text_clip(app, r.x + cw, r.y + r.h - 1.5f * lh, s, hint, cols, selected ? accent : dim);
        np_ui_hit(app, r, g);
    }
    y += wide ? card_h + 1.5f * lh : 3 * (card_h + gap) + 0.5f * lh;

    static const char *const labels[4] = {"Import ROM", "Options", "About", "Quit"};
    int nb = launcher_buttons();
    float bw = SDL_min(14 * cw, (W - 2 * m - (float)(nb - 1) * gap) / (float)nb), bh = 2 * lh;
    for (int i = 0; i < nb; i++)
        np_ui_button(app, (SDL_FRect){m + (float)i * (bw + gap), y, bw, bh}, labels[i], app->launcher_sel == LB_IMPORT + i,
               LB_IMPORT + i, s);
    y += bh + lh;

    int cols = (int)((W - 2 * m) / cw);
    if (app->status[0])
        y += lh * (float)np_ui_text_wrap(app, m, y, s, lh, cols, app->status, accent, 1) + 0.5f * lh;
    if (y < H - 3 * lh) {
        np_ui_text_clip(app, m, H - m - 2 * lh, s, "Drop a .nds file on this window to import it.", cols, dim);
        char where[1200];
        SDL_snprintf(where, sizeof where, "%s data: %s", np_storage_is_portable() ? "Portable" : "User",
                     np_storage_root());
        np_ui_text_clip(app, m, H - m - lh, s, where, cols, dim);
    }
}

/* ---- pages ------------------------------------------------------------- */

void np_ui_begin_page(np_app *app, np_page_frame *f, const char *title)
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
void np_ui_end_page(np_app *app, const np_page_frame *f, const char *hint, int total)
{
    float s = f->s, cw = f->cw, lh = f->lh;
    SDL_FRect p = f->panel;
    float y = p.y + p.h - 3 * lh;
    np_ui_text_clip(app, p.x + 2 * cw, y - 0.5f * lh, s, hint, f->cols, dim);
    float bh = 1.8f * lh, by = p.y + p.h - bh - 0.6f * lh;
    np_ui_button(app, (SDL_FRect){p.x + 2 * cw, by, 8 * cw, bh}, "Back", 0, HIT_BACK, s);
    if (total > f->rows) {
        np_ui_button(app, (SDL_FRect){p.x + p.w - 2 * cw - 14 * cw, by, 6 * cw, bh}, "Up", 0, HIT_SCROLL_UP, s);
        np_ui_button(app, (SDL_FRect){p.x + p.w - 2 * cw - 7 * cw, by, 6 * cw, bh}, "Down", 0, HIT_SCROLL_DOWN, s);
    }
}

void np_ui_keep_visible(np_app *app, int sel, int rows, int total)
{
    if (sel < app->scroll)
        app->scroll = sel;
    if (sel >= app->scroll + rows)
        app->scroll = sel - rows + 1;
    app->scroll = SDL_clamp(app->scroll, 0, SDL_max(0, total - rows));
}

static void draw_options(np_app *app)
{
    np_page_frame f;
    np_ui_begin_page(app, &f, "Options");
    int items[OPT_COUNT];
    int n = options_items(app, items);
    app->sel = SDL_clamp(app->sel, 0, n - 1);
    np_ui_keep_visible(app, app->sel, f.rows, n);
    float x = f.panel.x + 2 * f.cw, vx = f.panel.x + f.panel.w * 0.5f;
    int vcols = (int)((f.panel.x + f.panel.w - 2 * f.cw - vx) / f.cw);
    for (int r = 0; r < f.rows && app->scroll + r < n; r++) {
        int i = app->scroll + r, item = items[i], selected = i == app->sel;
        float y = f.list_y + (float)r * f.lh;
        SDL_FRect row = {f.panel.x + f.cw, y - 2 * f.s, f.panel.w - 2 * f.cw, f.lh};
        if (selected)
            np_ui_fill(app, row, (SDL_Color){255, 205, 80, 40});
        np_ui_text_clip(app, x, y, f.s, opt_labels[item], (int)((vx - x) / f.cw) - 1, selected ? accent : white);
        char v[64], shown[80];
        opt_value(app, item, v, sizeof v);
        if (*v) {
            SDL_snprintf(shown, sizeof shown, selected ? "< %s >" : "  %s", v);
            np_ui_text_clip(app, vx, y, f.s, shown, vcols, selected ? accent : dim);
        }
        np_ui_hit(app, row, i);
    }
    np_ui_end_page(app, &f, "Left/Right: change  Enter/A: select  Esc/B: back", n);
}

static void draw_controls(np_app *app)
{
    np_page_frame f;
    np_ui_begin_page(app, &f, "Controls");
    app->sel = SDL_clamp(app->sel, 0, CTRL_ROWS - 1);
    np_ui_keep_visible(app, app->sel, f.rows, CTRL_ROWS);
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
            np_ui_hit(app, rr, row * 4);
            continue;
        }
        const char *name = np_action_name((np_action)row);
        /* Long shell action names get their short form in the grid. */
        if (row == NP_ACT_FF_HOLD)
            name = "FF hold";
        else if (row == NP_ACT_FF_TOGGLE)
            name = "FF toggle";
        else if (row == NP_ACT_REWIND)
            name = "Rewind";
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
            np_ui_text_clip(app, cell.x + f.cw * 0.5f, y, f.s, label, slot_cols,
                      cell_sel ? accent : (c == NP_KEY_SLOTS ? (SDL_Color){150, 200, 255, 255} : white));
            np_ui_hit(app, cell, row * 4 + c);
        }
    }
    np_ui_end_page(app, &f,
             app->capture ? "Press a key/button. Esc: cancel  Delete: clear"
                          : "Enter/A: rebind  Right-click: clear  Esc/B: back",
             CTRL_ROWS);
}

static void draw_about(np_app *app)
{
    np_page_frame f;
    np_ui_begin_page(app, &f, "About");
    /* Lay the text out once per frame into wrapped lines, then window it. */
    int total = 0;
    for (size_t i = 0; i < SDL_arraysize(about_text); i++)
        total += *about_text[i] ? np_ui_text_wrap(app, 0, 0, f.s, f.lh, f.cols, about_text[i], white, 0) : 1;
    app->scroll = SDL_clamp(app->scroll, 0, SDL_max(0, total - f.rows));
    int line = 0;
    float x = f.panel.x + 2 * f.cw;
    for (size_t i = 0; i < SDL_arraysize(about_text); i++) {
        const char *t = about_text[i];
        int is_heading = i == 0 || !SDL_strcmp(t, "LICENSE") || !SDL_strcmp(t, "CREDITS");
        int n = *t ? np_ui_text_wrap(app, 0, 0, f.s, f.lh, f.cols, t, white, 0) : 1;
        /* Draw only paragraphs fully inside the window. */
        if (*t && line >= app->scroll && line + n <= app->scroll + f.rows)
            np_ui_text_wrap(app, x, f.list_y + (float)(line - app->scroll) * f.lh, f.s, f.lh, f.cols, t,
                      is_heading ? accent : white, 1);
        line += n;
    }
    np_ui_end_page(app, &f, "Up/Down scroll, Esc/B back.", total);
}

/* ---- save slots ---------------------------------------------------------- */

#define HIT_OSK 2000

enum { SROW_CONTINUE, SROW_NEW, SROW_SLOT, SROW_IMPORT };
typedef struct slot_row {
    int kind, slot;
} slot_row;

static int slots_rows(const np_app *app, slot_row *rows)
{
    int n = 0;
    if (np_slot_list_find(&app->slots, app->opt.last_slot[app->slots_game]) >= 0)
        rows[n++] = (slot_row){SROW_CONTINUE, -1};
    rows[n++] = (slot_row){SROW_NEW, -1};
    for (int i = 0; i < app->slots.count; i++)
        rows[n++] = (slot_row){SROW_SLOT, i};
    rows[n++] = (slot_row){SROW_IMPORT, -1};
    return n;
}

static int slots_row_of(const np_app *app, int slot)
{
    slot_row rows[NP_MAX_SLOTS + 3];
    int n = slots_rows(app, rows);
    for (int i = 0; i < n; i++)
        if (rows[i].kind == SROW_SLOT && rows[i].slot == slot)
            return i;
    return 0;
}

static void format_time(int64_t t, char *buf, size_t n)
{
    SDL_DateTime dt;
    if (t && SDL_TimeToDateTime(t, &dt, true))
        SDL_snprintf(buf, n, "%04d-%02d-%02d %02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute);
    else
        SDL_strlcpy(buf, "?", n);
}

static const np_slot_info *cur_slot(const np_app *app)
{
    return app->slot_sel >= 0 && app->slot_sel < app->slots.count ? &app->slots.slot[app->slot_sel] : NULL;
}

static void open_slots_at(np_app *app, int slot)
{
    np_app_open_page(app, NP_PAGE_SLOTS);
    if (slot >= 0)
        app->sel = slots_row_of(app, slot);
}

void np_ui_open_text(np_app *app, np_text_purpose purpose, const char *initial, int max)
{
    app->text_purpose = purpose;
    app->text_max = SDL_clamp(max, 1, NP_SLOT_NAME_MAX);
    SDL_strlcpy(app->text, initial, sizeof app->text);
    app->text[app->text_max] = '\0';
    app->text_error[0] = '\0';
    np_app_open_page(app, NP_PAGE_TEXT);
    app->osk_sel = 0;
}

static void open_text(np_app *app, np_text_purpose purpose, const char *initial)
{
    np_ui_open_text(app, purpose, initial, NP_SLOT_NAME_MAX);
}

static int text_for_editor(const np_app *app)
{
    return app->text_purpose == NP_TEXT_TRAINER_NAME || app->text_purpose == NP_TEXT_NICKNAME;
}

/* Wireless settings typed on the text page; they go back to Options. */
static char *text_option_target(np_app *app, size_t *cap)
{
    switch (app->text_purpose) {
    case NP_TEXT_LAN_PEER: *cap = sizeof app->opt.lan_peer; return app->opt.lan_peer;
    case NP_TEXT_LAN_RELAY: *cap = sizeof app->opt.lan_relay; return app->opt.lan_relay;
    case NP_TEXT_LAN_PIN: *cap = sizeof app->opt.lan_pin; return app->opt.lan_pin;
    default: return NULL;
    }
}

static void slots_activate(np_app *app, int row)
{
    slot_row rows[NP_MAX_SLOTS + 3];
    int n = slots_rows(app, rows);
    if (row < 0 || row >= n)
        return;
    switch (rows[row].kind) {
    case SROW_CONTINUE: np_app_start_game(app, app->slots_game, app->opt.last_slot[app->slots_game]); break;
    case SROW_NEW: {
        if (app->slots.count >= NP_MAX_SLOTS || app->slots.truncated) {
            np_app_toast(app, "Too many save slots: delete one first");
            break;
        }
        const char *taken[NP_MAX_SLOTS];
        for (int i = 0; i < app->slots.count; i++)
            taken[i] = app->slots.slot[i].name;
        char name[NP_SLOT_NAME_MAX + 1];
        np_slot_default_name(taken, app->slots.count, name);
        open_text(app, NP_TEXT_NEW_SLOT, name);
        break;
    }
    case SROW_SLOT:
        app->slot_sel = rows[row].slot;
        np_app_open_page(app, NP_PAGE_SLOT_MENU);
        break;
    case SROW_IMPORT: np_app_open_sav_import_dialog(app, app->slots_game); break;
    default: break;
    }
}

static void draw_slots(np_app *app)
{
    np_page_frame f;
    char title[64];
    SDL_snprintf(title, sizeof title, "%s saves", np_game_title(app->slots_game));
    np_ui_begin_page(app, &f, title);
    slot_row rows[NP_MAX_SLOTS + 3];
    int n = slots_rows(app, rows);
    app->sel = SDL_clamp(app->sel, 0, n - 1);
    np_ui_keep_visible(app, app->sel, f.rows, n);
    float x = f.panel.x + 2 * f.cw, vx = f.panel.x + f.panel.w - 2 * f.cw - 18 * f.cw;
    for (int r = 0; r < f.rows && app->scroll + r < n; r++) {
        int i = app->scroll + r, selected = i == app->sel;
        float y = f.list_y + (float)r * f.lh;
        SDL_FRect row = {f.panel.x + f.cw, y - 2 * f.s, f.panel.w - 2 * f.cw, f.lh};
        if (selected)
            np_ui_fill(app, row, (SDL_Color){255, 205, 80, 40});
        SDL_Color c = selected ? accent : white;
        char label[96], when[32] = "";
        switch (rows[i].kind) {
        case SROW_CONTINUE:
            SDL_snprintf(label, sizeof label, "Continue: %s", app->opt.last_slot[app->slots_game]);
            break;
        case SROW_NEW: SDL_strlcpy(label, "New save slot...", sizeof label); break;
        case SROW_IMPORT: SDL_strlcpy(label, "Import .sav...", sizeof label); break;
        default: {
            const np_slot_info *s = &app->slots.slot[rows[i].slot];
            SDL_snprintf(label, sizeof label, "  %s", s->name);
            if (s->size)
                format_time(s->mtime, when, sizeof when);
            else
                SDL_strlcpy(when, "not saved yet", sizeof when);
            break;
        }
        }
        np_ui_text_clip(app, x, y, f.s, label, (int)((vx - x) / f.cw) - 1, c);
        if (when[0])
            np_ui_text_clip(app, vx, y, f.s, when, 18, selected ? accent : dim);
        np_ui_hit(app, row, i);
    }
    np_ui_end_page(app, &f, "Enter/A: open  Esc/B: back  Drop .sav: import", n);
}

enum { SM_PLAY, SM_EDIT, SM_RENAME, SM_DUPLICATE, SM_EXPORT, SM_DELETE, SM_COUNT };

static void slot_menu_activate(np_app *app, int item)
{
    const np_slot_info *s = cur_slot(app);
    if (!s)
        return;
    char name[NP_SLOT_NAME_MAX + 1];
    SDL_strlcpy(name, s->name, sizeof name);
    np_game g = app->slots_game;
    switch (item) {
    case SM_PLAY: np_app_start_game(app, g, name); break;
    case SM_EDIT:
        if (!s->size)
            np_app_toast(app, "\"%s\" has no save yet", name);
        else
            np_editor_open(app, g, name);
        break;
    case SM_RENAME: open_text(app, NP_TEXT_RENAME_SLOT, name); break;
    case SM_DUPLICATE: {
        const char *taken[NP_MAX_SLOTS];
        for (int i = 0; i < app->slots.count; i++)
            taken[i] = app->slots.slot[i].name;
        char copy[NP_SLOT_NAME_MAX + 1];
        if (app->slots.count >= NP_MAX_SLOTS || np_slot_unique(name, taken, app->slots.count, copy)) {
            np_app_toast(app, "Too many save slots: delete one first");
        } else if (np_storage_slot_duplicate(g, name, copy)) {
            np_app_toast(app, "Duplicate failed: %s", SDL_GetError());
        } else {
            np_app_refresh_slots(app, copy);
            np_app_toast(app, "Copied \"%s\" to \"%s\"", name, copy);
            open_slots_at(app, app->slot_sel);
        }
        break;
    }
    case SM_EXPORT:
        if (!s->size)
            np_app_toast(app, "\"%s\" has no save yet", name);
        else
            np_app_open_sav_export_dialog(app, g, name);
        break;
    case SM_DELETE: np_app_open_page(app, NP_PAGE_CONFIRM); break;
    default: break;
    }
}

static void draw_slot_menu(np_app *app)
{
    const np_slot_info *s = cur_slot(app);
    if (!s) {
        open_slots_at(app, -1);
        return;
    }
    np_page_frame f;
    np_ui_begin_page(app, &f, s->name);
    static const char *const labels[SM_COUNT] = {"Play",      "Edit save...",   "Rename...",
                                                 "Duplicate", "Export .sav...", "Delete..."};
    char info[96], when[32];
    format_time(s->mtime, when, sizeof when);
    if (s->size)
        SDL_snprintf(info, sizeof info, "%s, last saved %s", np_game_title(app->slots_game), when);
    else
        SDL_snprintf(info, sizeof info, "%s, not saved yet", np_game_title(app->slots_game));
    np_ui_text_clip(app, f.panel.x + 2 * f.cw, f.list_y, f.s, info, f.cols, dim);
    app->sel = SDL_clamp(app->sel, 0, SM_COUNT - 1);
    for (int i = 0; i < SM_COUNT; i++) {
        float y = f.list_y + (float)(i + 2) * f.lh;
        SDL_FRect row = {f.panel.x + f.cw, y - 2 * f.s, f.panel.w - 2 * f.cw, f.lh};
        if (i == app->sel)
            np_ui_fill(app, row, (SDL_Color){255, 205, 80, 40});
        np_ui_text(app, f.panel.x + 2 * f.cw, y, f.s, labels[i], i == app->sel ? (i == SM_DELETE ? warn : accent) : white);
        np_ui_hit(app, row, i);
    }
    np_ui_end_page(app, &f, "Enter/A: select  Esc/B: back", 0);
}

static void confirm_activate(np_app *app, int yes)
{
    const np_slot_info *s = cur_slot(app);
    if (!yes || !s) {
        np_app_open_page(app, NP_PAGE_SLOT_MENU);
        app->sel = SM_DELETE;
        return;
    }
    char name[NP_SLOT_NAME_MAX + 1];
    SDL_strlcpy(name, s->name, sizeof name);
    if (np_storage_slot_delete(app->slots_game, name)) {
        np_app_toast(app, "Delete failed: %s", SDL_GetError());
        np_app_open_page(app, NP_PAGE_SLOT_MENU);
        return;
    }
    np_sync_forget(app, app->slots_game, name);
    if (np_slot_name_eq(app->opt.last_slot[app->slots_game], name)) {
        app->opt.last_slot[app->slots_game][0] = '\0';
        app->options_dirty = 1;
    }
    np_app_refresh_slots(app, NULL);
    np_app_toast(app, "Deleted \"%s\"", name);
    open_slots_at(app, app->slots.count ? app->slot_sel : -1);
}

static void draw_confirm(np_app *app)
{
    const np_slot_info *s = cur_slot(app);
    np_page_frame f;
    np_ui_begin_page(app, &f, "Delete save slot");
    char q[160];
    SDL_snprintf(q, sizeof q, "Delete \"%s\" from %s? Its save is erased and cannot be recovered.", s ? s->name : "?",
                 np_game_title(app->slots_game));
    int lines = np_ui_text_wrap(app, f.panel.x + 2 * f.cw, f.list_y, f.s, f.lh, f.cols, q, white, 1);
    static const char *const labels[2] = {"No, keep it", "Yes, delete it"};
    app->sel = SDL_clamp(app->sel, 0, 1);
    float bw = 18 * f.cw, bh = 2 * f.lh, y = f.list_y + (float)(lines + 1) * f.lh;
    for (int i = 0; i < 2; i++)
        np_ui_button(app, (SDL_FRect){f.panel.x + 2 * f.cw + (float)i * (bw + 2 * f.cw), y, bw, bh}, labels[i], app->sel == i,
               i, f.s);
    np_ui_end_page(app, &f, "Left/Right: choose  Enter/A: confirm  Esc/B: back", 0);
}

/* On-screen keyboard for name entry without a physical keyboard (gamepad,
 * mouse; iOS also raises its own keyboard). 13 cells per row; the last row
 * holds punctuation and the wide Space/Del/OK/Cancel keys. */
enum { OSK_COLS = 13, OSK_ROWS = 6, OSK_SPACE = 256, OSK_DEL, OSK_OK, OSK_CANCEL };
typedef struct osk_key {
    int row, col, span, code;
} osk_key;

static int osk_keys(osk_key *k)
{
    static const char *const rows[5] = {"ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "abcdefghijklm", "nopqrstuvwxyz",
                                        "0123456789-_."};
    int n = 0;
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < OSK_COLS; c++)
            k[n++] = (osk_key){r, c, 1, (unsigned char)rows[r][c]};
    static const char punct[] = "()!'#";
    for (int c = 0; c < 5; c++)
        k[n++] = (osk_key){5, c, 1, (unsigned char)punct[c]};
    k[n++] = (osk_key){5, 5, 2, OSK_SPACE};
    k[n++] = (osk_key){5, 7, 2, OSK_DEL};
    k[n++] = (osk_key){5, 9, 2, OSK_OK};
    k[n++] = (osk_key){5, 11, 2, OSK_CANCEL};
    return n;
}

static void osk_move(np_app *app, int dr, int dc)
{
    osk_key k[80];
    int n = osk_keys(k);
    osk_key cur = k[SDL_clamp(app->osk_sel, 0, n - 1)];
    int row = cur.row, col = cur.col;
    if (dc) {
        int idx = app->osk_sel;
        do
            idx = wrapi(idx + dc, n);
        while (k[idx].row != row);
        app->osk_sel = idx;
        return;
    }
    row = wrapi(row + dr, OSK_ROWS);
    for (int i = 0; i < n; i++)
        if (k[i].row == row && col >= k[i].col && col < k[i].col + k[i].span) {
            app->osk_sel = i;
            return;
        }
    for (int i = n - 1; i >= 0; i--) /* past the row's last key */
        if (k[i].row == row) {
            app->osk_sel = i;
            return;
        }
}

static void text_cancel(np_app *app)
{
    size_t cap;
    if (text_option_target(app, &cap)) {
        np_app_open_page(app, NP_PAGE_OPTIONS);
    } else if (text_for_editor(app)) {
        np_editor_text_cancel(app);
    } else if (app->text_purpose == NP_TEXT_RENAME_SLOT) {
        np_app_open_page(app, NP_PAGE_SLOT_MENU);
        app->sel = SM_RENAME;
    } else {
        open_slots_at(app, -1);
    }
}

static void text_commit(np_app *app)
{
    size_t cap;
    char *target = text_option_target(app, &cap);
    if (target) {
        SDL_strlcpy(target, app->text, cap);
        app->options_dirty = 1;
        np_app_net_apply(app);
        np_app_open_page(app, NP_PAGE_OPTIONS);
        return;
    }
    if (text_for_editor(app)) {
        const char *err = np_editor_text_done(app, app->text);
        if (err)
            SDL_strlcpy(app->text_error, err, sizeof app->text_error);
        return;
    }
    const char *problem = np_slot_name_problem(app->text);
    if (problem) {
        SDL_strlcpy(app->text_error, problem, sizeof app->text_error);
        return;
    }
    np_game g = app->slots_game;
    int existing = np_slot_list_find(&app->slots, app->text);
    char name[NP_SLOT_NAME_MAX + 1];
    SDL_strlcpy(name, app->text, sizeof name);
    if (app->text_purpose == NP_TEXT_NEW_SLOT) {
        if (existing >= 0) {
            SDL_strlcpy(app->text_error, "A slot with that name already exists.", sizeof app->text_error);
            return;
        }
        if (np_storage_slot_create(g, name)) {
            SDL_snprintf(app->text_error, sizeof app->text_error, "Cannot create it: %s", SDL_GetError());
            return;
        }
        SDL_Log("created save slot \"%s\" for %s", name, np_game_id(g));
        np_app_refresh_slots(app, name);
        np_app_start_game(app, g, name);
        return;
    }
    const np_slot_info *s = cur_slot(app);
    if (!s)
        return;
    char old[NP_SLOT_NAME_MAX + 1];
    SDL_strlcpy(old, s->name, sizeof old);
    if (existing >= 0 && existing != app->slot_sel) {
        SDL_strlcpy(app->text_error, "A slot with that name already exists.", sizeof app->text_error);
        return;
    }
    if (SDL_strcmp(old, name)) {
        if (np_storage_slot_rename(g, old, name)) {
            SDL_snprintf(app->text_error, sizeof app->text_error, "Cannot rename it: %s", SDL_GetError());
            return;
        }
        if (np_slot_name_eq(app->opt.last_slot[g], old)) {
            SDL_strlcpy(app->opt.last_slot[g], name, sizeof app->opt.last_slot[g]);
            app->options_dirty = 1;
        }
        SDL_Log("renamed save slot \"%s\" to \"%s\"", old, name);
        np_sync_forget(app, g, old);
        np_sync_slot(app, g, name);
    }
    np_app_refresh_slots(app, name);
    np_app_open_page(app, NP_PAGE_SLOT_MENU);
}

static void text_type(np_app *app, int code)
{
    size_t len = SDL_strlen(app->text);
    app->text_error[0] = '\0';
    switch (code) {
    case OSK_DEL:
        if (len)
            app->text[len - 1] = '\0';
        break;
    case OSK_OK: text_commit(app); break;
    case OSK_CANCEL: text_cancel(app); break;
    default:
        if (code == OSK_SPACE)
            code = ' ';
        /* Slot names are file names; game names are checked by the game's
         * charset when committed. */
        size_t cap;
        int ok = text_for_editor(app) || text_option_target(app, &cap) ? code >= 0x20 && code < 0x7F
                                                                       : np_slot_char_ok((unsigned char)code);
        if ((int)len < app->text_max && ok) {
            app->text[len] = (char)code;
            app->text[len + 1] = '\0';
        }
        break;
    }
}

static void draw_text_page(np_app *app)
{
    np_page_frame f;
    static const char *const titles[] = {"New save slot",   "Rename save slot",         "Trainer name", "Nickname",
                                         "Join by IP:port", "Internet relay host:port", "Room PIN"};
    np_ui_begin_page(app, &f, titles[app->text_purpose]);
    float x = f.panel.x + 2 * f.cw, y = f.list_y;
    SDL_FRect box = {x, y - 4 * f.s, (float)(app->text_max + 2) * f.cw, f.lh + 4 * f.s};
    box.w = SDL_min(box.w, f.panel.w - 4 * f.cw);
    np_ui_fill(app, box, (SDL_Color){0, 0, 0, 160});
    np_ui_frame(app, box, f.s, accent);
    char shown[sizeof app->text + 1];
    int blink = (SDL_GetTicks() / 500) % 2 == 0;
    SDL_snprintf(shown, sizeof shown, "%s%s", app->text, blink ? "_" : "");
    np_ui_text_clip(app, x + f.cw * 0.5f, y, f.s, shown, (int)(box.w / f.cw) - 1, white);
    y += 1.5f * f.lh;
    if (app->text_error[0])
        np_ui_text_clip(app, x, y, f.s, app->text_error, f.cols, warn);
    y += 1.5f * f.lh;

    osk_key k[80];
    int n = osk_keys(k);
    app->osk_sel = SDL_clamp(app->osk_sel, 0, n - 1);
    float avail_h = f.panel.y + f.panel.h - 4 * f.lh - y;
    float cell = SDL_min((f.panel.w - 4 * f.cw) / OSK_COLS, avail_h / OSK_ROWS);
    float ts = SDL_max(1.0f, floorf(cell / 16.0f));
    for (int i = 0; i < n; i++) {
        SDL_FRect r = {x + (float)k[i].col * cell, y + (float)k[i].row * cell, (float)k[i].span * cell - 2 * f.s,
                       cell - 2 * f.s};
        char label[8];
        switch (k[i].code) {
        case OSK_SPACE: SDL_strlcpy(label, "Spc", sizeof label); break;
        case OSK_DEL: SDL_strlcpy(label, "Del", sizeof label); break;
        case OSK_OK: SDL_strlcpy(label, "OK", sizeof label); break;
        case OSK_CANCEL: SDL_strlcpy(label, "Esc", sizeof label); break;
        default: label[0] = (char)k[i].code, label[1] = '\0'; break;
        }
        float tw = (float)SDL_strlen(label) * 8 * ts;
        int sel = i == app->osk_sel;
        np_ui_fill(app, r, sel ? (SDL_Color){255, 205, 80, 70} : (SDL_Color){255, 255, 255, 18});
        if (sel)
            np_ui_frame(app, r, f.s, accent);
        np_ui_text(app, r.x + (r.w - tw) * 0.5f, r.y + (r.h - 8 * ts) * 0.5f, ts, label, sel ? accent : white);
        np_ui_hit(app, r, HIT_OSK + i);
    }
    np_ui_end_page(app, &f, "Type or pick keys. Enter: OK  Esc: cancel", 0);
}

/* Keyboard while naming a slot: typed text arrives as SDL_EVENT_TEXT_INPUT;
 * every key press is consumed so bindings and hotkeys stay out of the way. */
static int text_capture_event(np_app *app, const SDL_Event *e)
{
    if (e->type == SDL_EVENT_TEXT_INPUT) {
        for (const char *p = e->text.text; *p; p++)
            text_type(app, (unsigned char)*p);
        return 1;
    }
    if (e->type != SDL_EVENT_KEY_DOWN)
        return e->type == SDL_EVENT_KEY_UP;
    switch (e->key.scancode) {
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: text_commit(app); break;
    case SDL_SCANCODE_ESCAPE: text_cancel(app); break;
    case SDL_SCANCODE_BACKSPACE: text_type(app, OSK_DEL); break;
    case SDL_SCANCODE_UP: osk_move(app, -1, 0); break;
    case SDL_SCANCODE_DOWN: osk_move(app, 1, 0); break;
    case SDL_SCANCODE_LEFT: osk_move(app, 0, -1); break;
    case SDL_SCANCODE_RIGHT: osk_move(app, 0, 1); break;
    default: break;
    }
    return 1;
}

/* ---- dispatch ---------------------------------------------------------- */

static void page_back(np_app *app);

void np_ui_back(np_app *app) { page_back(app); }

static void page_back(np_app *app)
{
    np_page from = app->page;
    switch (from) {
    case NP_PAGE_SLOTS: np_app_open_page(app, NP_PAGE_NONE); return;
    case NP_PAGE_SLOT_MENU: open_slots_at(app, app->slot_sel); return;
    case NP_PAGE_CONFIRM: confirm_activate(app, 0); return;
    case NP_PAGE_TEXT: text_cancel(app); return;
    default: break;
    }
    np_page to = from == NP_PAGE_OPTIONS ? NP_PAGE_NONE : app->page_parent;
    np_app_open_page(app, to);
    if (to == NP_PAGE_OPTIONS) {
        int back_to = from == NP_PAGE_CONTROLS ? OPT_CONTROLS
                      : from == NP_PAGE_MODS  ? OPT_MODS
                      : from == NP_PAGE_UPDATES ? OPT_UPDATES
                      : from == NP_PAGE_TOUCH_EDIT ? OPT_TOUCH_EDIT
                                                : OPT_ABOUT;
        int items[OPT_COUNT], n = options_items(app, items);
        for (int i = 0; i < n; i++)
            if (items[i] == back_to)
                app->sel = i;
    }
}

static void slot_pages_command(np_app *app, np_menu_cmd cmd)
{
    slot_row rows[NP_MAX_SLOTS + 3];
    switch (app->page) {
    case NP_PAGE_SLOTS: {
        int n = slots_rows(app, rows);
        if (cmd == NP_CMD_UP || cmd == NP_CMD_DOWN)
            app->sel = wrapi(app->sel + (cmd == NP_CMD_UP ? -1 : 1), n);
        else if (cmd == NP_CMD_CONFIRM)
            slots_activate(app, app->sel);
        break;
    }
    case NP_PAGE_SLOT_MENU:
        if (cmd == NP_CMD_UP || cmd == NP_CMD_DOWN)
            app->sel = wrapi(app->sel + (cmd == NP_CMD_UP ? -1 : 1), SM_COUNT);
        else if (cmd == NP_CMD_CONFIRM)
            slot_menu_activate(app, app->sel);
        break;
    case NP_PAGE_CONFIRM:
        if (cmd == NP_CMD_LEFT || cmd == NP_CMD_RIGHT || cmd == NP_CMD_UP || cmd == NP_CMD_DOWN)
            app->sel = !app->sel;
        else if (cmd == NP_CMD_CONFIRM)
            confirm_activate(app, app->sel == 1);
        break;
    case NP_PAGE_TEXT: {
        osk_key k[80];
        int n = osk_keys(k);
        switch (cmd) {
        case NP_CMD_UP: osk_move(app, -1, 0); break;
        case NP_CMD_DOWN: osk_move(app, 1, 0); break;
        case NP_CMD_LEFT: osk_move(app, 0, -1); break;
        case NP_CMD_RIGHT: osk_move(app, 0, 1); break;
        case NP_CMD_CONFIRM: text_type(app, k[SDL_clamp(app->osk_sel, 0, n - 1)].code); break;
        default: break;
        }
        break;
    }
    default: break;
    }
}

void np_ui_command(np_app *app, np_menu_cmd cmd)
{
    if (cmd == NP_CMD_NONE)
        return;
    if (app->page == NP_PAGE_TOUCH_EDIT && cmd != NP_CMD_BACK && cmd != NP_CMD_CLOSE) {
        np_touchedit_command(app, cmd);
        return;
    }
    if (app->page == NP_PAGE_EDITOR) {
        np_editor_command(app, cmd);
        return;
    }
    if (app->page == NP_PAGE_MODS && cmd >= NP_CMD_TAB_PREV) {
        np_mods_command(app, cmd); /* X deletes a package */
        return;
    }
    if (cmd >= NP_CMD_TAB_PREV)
        return; /* only the editor has tabs and secondary actions */
    if (app->page == NP_PAGE_NONE) {
        if (app->view == NP_VIEW_LAUNCHER)
            launcher_command(app, cmd);
        return;
    }
    if (cmd == NP_CMD_CLOSE && app->page == NP_PAGE_TEXT) {
        text_commit(app); /* Start on a gamepad: done typing */
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
    if (app->page == NP_PAGE_UPDATES) {
        np_update_command(app, cmd);
        return;
    }
    if (app->page == NP_PAGE_MODS) {
        np_mods_command(app, cmd);
        return;
    }
    if (app->page == NP_PAGE_SYNC) {
        np_sync_command(app, cmd);
        return;
    }
    if (app->page >= NP_PAGE_SLOTS) {
        slot_pages_command(app, cmd);
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
    if (id >= NP_EDITOR_HIT_BASE) {
        np_editor_hit(app, id, 0, 0);
        return;
    }
    if (id >= HIT_OSK) {
        app->osk_sel = id - HIT_OSK;
        return;
    }
    if (id >= HIT_SCROLL_UP)
        return;
    if (app->page == NP_PAGE_NONE)
        app->launcher_sel = id;
    else if (app->page == NP_PAGE_OPTIONS || app->page >= NP_PAGE_SLOTS)
        app->sel = id;
    else if (app->page == NP_PAGE_CONTROLS) {
        app->sel = id / 4;
        if (id / 4 < NP_ACT_COUNT)
            app->col = id % 4;
    }
}

static void activate_hit(np_app *app, int id, int dir)
{
    if (id >= NP_EDITOR_HIT_BASE) {
        np_editor_hit(app, id, 1, dir);
        return;
    }
    if (id == HIT_BACK) {
        page_back(app);
        return;
    }
    if (app->page == NP_PAGE_UPDATES) {
        np_update_hit(app, id);
        return;
    }
    if (app->page == NP_PAGE_MODS) {
        np_mods_hit(app, id);
        return;
    }
    if (app->page == NP_PAGE_SYNC) {
        np_sync_hit(app, id);
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
    if (id >= HIT_OSK) {
        osk_key k[80];
        int n = osk_keys(k);
        if (id - HIT_OSK < n)
            text_type(app, k[id - HIT_OSK].code);
    } else if (app->page == NP_PAGE_NONE) {
        launcher_activate(app, id);
    } else if (app->page == NP_PAGE_SLOTS) {
        slots_activate(app, id);
    } else if (app->page == NP_PAGE_SLOT_MENU) {
        slot_menu_activate(app, id);
    } else if (app->page == NP_PAGE_CONFIRM) {
        confirm_activate(app, id == 1);
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
    if (app->page == NP_PAGE_TOUCH_EDIT) {
        np_touchedit_pointer(app, x, y, pressed, released);
        return;
    }
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
    case NP_PAGE_SLOTS: draw_slots(app); break;
    case NP_PAGE_SLOT_MENU: draw_slot_menu(app); break;
    case NP_PAGE_CONFIRM: draw_confirm(app); break;
    case NP_PAGE_TEXT: draw_text_page(app); break;
    case NP_PAGE_EDITOR: np_editor_draw(app); break;
    case NP_PAGE_SYNC: np_sync_draw(app); break;
    case NP_PAGE_MODS: np_mods_draw(app); break;
    case NP_PAGE_UPDATES: np_update_draw(app); break;
    case NP_PAGE_TOUCH_EDIT: np_touchedit_draw(app); break;
    default: break;
    }
    if (app->toast[0] && SDL_GetTicksNS() < app->toast_until) {
        float s = np_ui_scale(app);
        int cols = (int)((app->out_w - 32 * s) / (8 * s));
        float w = (float)SDL_min((int)SDL_strlen(app->toast), cols) * 8 * s + 16 * s;
        SDL_FRect r = {floorf((app->out_w - w) * 0.5f), 8 * s, w, 16 * s};
        np_ui_fill(app, r, (SDL_Color){0, 0, 0, 190});
        np_ui_text_clip(app, r.x + 8 * s, r.y + 4 * s, s, app->toast, cols, white);
    }
}
