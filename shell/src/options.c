/*
 * options.ini format: "[section]" headers and "key = value" lines; '#' or
 * ';' start comments. Bindings use SDL's scancode names and gamepad button
 * strings so the file stays readable and editable by hand:
 *   [keys]    a = Z, Return, Space
 *   [gamepad] a = b
 */
#include "options.h"

#include <SDL3/SDL.h>
#include <string.h>

#include "storage.h"

const int np_speeds[NP_SPEED_COUNT] = {1, 2, 3, 4, 8, 0};
static const char *const np_game_ids[NP_GAME_COUNT] = {"diamond", "pearl", "platinum", "black", "white"};
const int np_fps_caps[NP_FPS_CAP_COUNT] = {0, 30, 60, 120, 144, 240};
const char *const np_fx_ids[NP_FX_COUNT] = {"off", "lcd", "scanlines", "crt", "smooth"};
const char *const np_perf_ids[NP_PERF_COUNT] = {"custom", "high", "balanced", "low", "auto"};

static const char *const action_ids[NP_ACT_COUNT] = {
    "a", "b", "x", "y", "l", "r", "start", "select", "up", "down", "left", "right", "ff_hold", "ff_toggle", "rewind",
};

static const char *const action_names[NP_ACT_COUNT] = {
    "A",  "B",    "X",    "Y",     "L",    "R", "Start", "Select", "Up", "Down", "Left", "Right",
    "Fast-forward (hold)", "Fast-forward (toggle)", "Rewind (hold)",
};

static const char *const layout_ids[NP_LAYOUT_COUNT] = {"vertical", "horizontal", "hybrid", "top", "bottom"};
static const char *const touch_ids[NP_TOUCH_MODE_COUNT] = {"auto", "on", "off"};

const char *np_action_name(np_action a) { return (unsigned)a < NP_ACT_COUNT ? action_names[a] : "?"; }

void np_bindings_defaults(np_bindings *b)
{
    static const int keys[NP_ACT_COUNT][NP_KEY_SLOTS] = {
        [NP_ACT_A] = {SDL_SCANCODE_Z, SDL_SCANCODE_RETURN, SDL_SCANCODE_SPACE},
        [NP_ACT_B] = {SDL_SCANCODE_X, SDL_SCANCODE_BACKSPACE},
        [NP_ACT_X] = {SDL_SCANCODE_C},
        [NP_ACT_Y] = {SDL_SCANCODE_V},
        [NP_ACT_L] = {SDL_SCANCODE_Q},
        [NP_ACT_R] = {SDL_SCANCODE_E},
        [NP_ACT_START] = {SDL_SCANCODE_ESCAPE},
        [NP_ACT_SELECT] = {SDL_SCANCODE_TAB, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT},
        [NP_ACT_UP] = {SDL_SCANCODE_UP, SDL_SCANCODE_W},
        [NP_ACT_DOWN] = {SDL_SCANCODE_DOWN, SDL_SCANCODE_S},
        [NP_ACT_LEFT] = {SDL_SCANCODE_LEFT, SDL_SCANCODE_A},
        [NP_ACT_RIGHT] = {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D},
        [NP_ACT_FF_HOLD] = {SDL_SCANCODE_F},
        [NP_ACT_FF_TOGGLE] = {SDL_SCANCODE_G},
        [NP_ACT_REWIND] = {SDL_SCANCODE_R},
    };
    /* Positional, Nintendo style: DS A is the right face button. */
    static const int pads[NP_ACT_COUNT] = {
        [NP_ACT_A] = SDL_GAMEPAD_BUTTON_EAST,
        [NP_ACT_B] = SDL_GAMEPAD_BUTTON_SOUTH,
        [NP_ACT_X] = SDL_GAMEPAD_BUTTON_NORTH,
        [NP_ACT_Y] = SDL_GAMEPAD_BUTTON_WEST,
        [NP_ACT_L] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
        [NP_ACT_R] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        [NP_ACT_START] = SDL_GAMEPAD_BUTTON_START,
        [NP_ACT_SELECT] = SDL_GAMEPAD_BUTTON_BACK,
        [NP_ACT_UP] = SDL_GAMEPAD_BUTTON_DPAD_UP,
        [NP_ACT_DOWN] = SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        [NP_ACT_LEFT] = SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        [NP_ACT_RIGHT] = SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        [NP_ACT_FF_HOLD] = NP_PAD_RIGHT_TRIGGER,
        [NP_ACT_FF_TOGGLE] = NP_PAD_LEFT_TRIGGER,
        [NP_ACT_REWIND] = SDL_GAMEPAD_BUTTON_LEFT_STICK,
    };
    memcpy(b->key, keys, sizeof keys);
    memcpy(b->pad, pads, sizeof pads);
}

void np_options_defaults(np_options *o)
{
    memset(o, 0, sizeof *o);
    o->layout = NP_LAYOUT_VERTICAL;
    o->scale = NP_SCALE_FIT;
    o->vsync = 1;
    o->fx_intensity[0] = o->fx_intensity[1] = 60;
    o->crt_curvature = 1;
    o->ff_speed_index = 3; /* 4x */
    o->volume = 80;
    o->mute_unfocused = 1;
    o->touch_controls = NP_TOUCH_AUTO;
    o->real_clock = 1;
    o->last_game = -1;
    o->lan_port = 2009; /* NP_NET_DEFAULT_PORT */
    o->bgm_volume = o->se_volume = 100;
    o->render_scale = 1;
    o->camera_zoom = 256;
    o->rewind_seconds = 30;
    np_bindings_defaults(&o->bind);
}

const char *np_pad_binding_name(int pad)
{
    switch (pad) {
    case NP_PAD_NONE: return "-";
    case NP_PAD_LEFT_TRIGGER: return "LT";
    case NP_PAD_RIGHT_TRIGGER: return "RT";
    case SDL_GAMEPAD_BUTTON_SOUTH: return "South";
    case SDL_GAMEPAD_BUTTON_EAST: return "East";
    case SDL_GAMEPAD_BUTTON_WEST: return "West";
    case SDL_GAMEPAD_BUTTON_NORTH: return "North";
    case SDL_GAMEPAD_BUTTON_BACK: return "Back";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
    case SDL_GAMEPAD_BUTTON_START: return "Start";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "L3";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "R3";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-Up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-Down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-Left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-Right";
    case SDL_GAMEPAD_BUTTON_MISC1: return "Misc";
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
    default: return "Paddle";
    }
}

void np_bind_key(np_bindings *b, np_action a, int slot, int scancode)
{
    if (scancode)
        for (int i = 0; i < NP_ACT_COUNT; i++)
            for (int s = 0; s < NP_KEY_SLOTS; s++)
                if (b->key[i][s] == scancode)
                    b->key[i][s] = 0;
    b->key[a][slot] = scancode;
}

void np_bind_pad(np_bindings *b, np_action a, int pad)
{
    if (pad != NP_PAD_NONE)
        for (int i = 0; i < NP_ACT_COUNT; i++)
            if (b->pad[i] == pad)
                b->pad[i] = NP_PAD_NONE;
    b->pad[a] = pad;
}

static int lookup(const char *const *ids, int n, const char *v)
{
    for (int i = 0; i < n; i++)
        if (!SDL_strcasecmp(ids[i], v))
            return i;
    return -1;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int pad_from_name(const char *v)
{
    if (!*v || !strcmp(v, "-"))
        return NP_PAD_NONE;
    if (!SDL_strcasecmp(v, SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)))
        return NP_PAD_LEFT_TRIGGER;
    if (!SDL_strcasecmp(v, SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)))
        return NP_PAD_RIGHT_TRIGGER;
    SDL_GamepadButton btn = SDL_GetGamepadButtonFromString(v);
    return btn == SDL_GAMEPAD_BUTTON_INVALID ? NP_PAD_NONE : (int)btn;
}

static const char *pad_to_name(int pad)
{
    if (pad == NP_PAD_LEFT_TRIGGER)
        return SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    if (pad == NP_PAD_RIGHT_TRIGGER)
        return SDL_GetGamepadStringForAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    const char *s = pad >= 0 ? SDL_GetGamepadStringForButton((SDL_GamepadButton)pad) : NULL;
    return s ? s : "-";
}

static void apply(np_options *o, const char *section, const char *key, char *val)
{
    int iv = SDL_atoi(val);
    if (!strcmp(section, "video")) {
        if (!strcmp(key, "layout")) {
            int m = lookup(layout_ids, NP_LAYOUT_COUNT, val);
            if (m >= 0)
                o->layout = (np_layout_mode)m;
        } else if (!strcmp(key, "swap"))
            o->swap = iv != 0;
        else if (!strcmp(key, "battle_layout")) {
            int m = lookup(layout_ids, NP_LAYOUT_COUNT, val);
            o->battle_layout = m >= 0 ? m + 1 : 0; /* "off" or unknown: keep the layout */
        }
        else if (!strcmp(key, "rotation"))
            o->rotation = clampi(iv / 90, 0, 3);
        else if (!strcmp(key, "scale"))
            o->scale = !SDL_strcasecmp(val, "integer") ? NP_SCALE_INTEGER : NP_SCALE_FIT;
        else if (!strcmp(key, "filter"))
            o->linear_filter = !SDL_strcasecmp(val, "linear");
        else if (!strcmp(key, "fullscreen"))
            o->fullscreen = iv != 0;
        else if (!strcmp(key, "vsync"))
            o->vsync = iv != 0;
        else if (!strcmp(key, "effect1") || !strcmp(key, "effect2")) {
            int m = lookup(np_fx_ids, NP_FX_COUNT, val);
            if (m >= 0)
                o->fx[key[6] - '1'] = m;
        } else if (!strcmp(key, "effect1_intensity") || !strcmp(key, "effect2_intensity"))
            o->fx_intensity[key[6] - '1'] = clampi(iv, 0, 100);
        else if (!strcmp(key, "crt_curvature"))
            o->crt_curvature = iv != 0;
        else if (!strcmp(key, "performance")) {
            int m = lookup(np_perf_ids, NP_PERF_COUNT, val);
            if (m >= 0)
                o->perf = m;
        }
        else if (!strcmp(key, "fps_cap")) {
            for (int i = 0; i < NP_FPS_CAP_COUNT; i++)
                if (np_fps_caps[i] == iv)
                    o->fps_cap_index = i;
        }
    } else if (!strcmp(section, "emulation")) {
        if (!strcmp(key, "real_clock"))
            o->real_clock = iv != 0;
        else if (!strcmp(key, "logic_clock"))
            o->logic_clock_60 = !strcmp(val, "60");
        else if (!strcmp(key, "speed") || !strcmp(key, "ff_speed")) {
            int *dst = key[0] == 's' ? &o->speed_index : &o->ff_speed_index;
            for (int i = 0; i < NP_SPEED_COUNT; i++)
                if (np_speeds[i] == iv)
                    *dst = i;
        }
    } else if (!strcmp(section, "audio")) {
        if (!strcmp(key, "volume"))
            o->volume = clampi(iv, 0, 100);
        else if (!strcmp(key, "mute_unfocused"))
            o->mute_unfocused = iv != 0;
        else if (!strcmp(key, "music_filter"))
            o->music_filter = clampi(iv, 0, 3);
    } else if (!strcmp(section, "interface")) {
        if (!strcmp(key, "ui_scale"))
            o->ui_scale = clampi(iv, 0, 6);
        else if (!strcmp(key, "reduce_motion"))
            o->reduce_motion = iv != 0;
    } else if (!strcmp(section, "session")) {
        const char *const *games = np_game_ids;
        if (!strcmp(key, "startup"))
            o->startup_continue = !SDL_strcasecmp(val, "continue");
        else if (!strcmp(key, "last_game"))
            o->last_game = lookup(games, NP_GAME_COUNT, val);
        else if (!strncmp(key, "last_slot_", 10)) {
            int g = lookup(games, NP_GAME_COUNT, key + 10);
            if (g >= 0 && !np_slot_name_problem(val))
                SDL_strlcpy(o->last_slot[g], val, sizeof o->last_slot[g]);
        }
    } else if (!strcmp(section, "game")) {
        if (!strcmp(key, "bgm_volume"))
            o->bgm_volume = clampi(iv, 0, 100);
        else if (!strcmp(key, "se_volume"))
            o->se_volume = clampi(iv, 0, 100);
        else if (!strcmp(key, "render_scale"))
            o->render_scale = clampi(iv, 1, 4);
        else if (!strcmp(key, "widescreen"))
            o->widescreen = iv != 0;
        else if (!strcmp(key, "camera_zoom"))
            o->camera_zoom = clampi(iv, 64, 1024);
        else if (!strcmp(key, "camera_tilt"))
            o->camera_tilt = clampi(iv, -720, 720);
        else if (!strcmp(key, "instant_text"))
            o->text_instant = iv != 0;
        else if (!strcmp(key, "fix_bugs"))
            o->fix_bugs = iv != 0;
        else if (!strcmp(key, "rewind_seconds"))
            o->rewind_seconds = clampi(iv, 0, 120);
    } else if (!strcmp(section, "updates")) {
        if (!strcmp(key, "repo"))
            SDL_strlcpy(o->update_repo, val, sizeof o->update_repo);
        else if (!strcmp(key, "api"))
            SDL_strlcpy(o->update_api, val, sizeof o->update_api);
    } else if (!strcmp(section, "gba")) {
        if (!strcmp(key, "rom"))
            SDL_strlcpy(o->gba_rom, val, sizeof o->gba_rom);
        else if (!strcmp(key, "save"))
            SDL_strlcpy(o->gba_save, val, sizeof o->gba_save);
    } else if (!strcmp(section, "sync")) {
        if (!strcmp(key, "folder"))
            SDL_strlcpy(o->sync_folder, val, sizeof o->sync_folder);
    } else if (!strcmp(section, "wireless")) {
        if (!strcmp(key, "enabled"))
            o->lan_enabled = iv != 0;
        else if (!strcmp(key, "port"))
            o->lan_port = clampi(iv, 1024, 65531);
        else if (!strcmp(key, "peer"))
            SDL_strlcpy(o->lan_peer, val, sizeof o->lan_peer);
        else if (!strcmp(key, "relay"))
            SDL_strlcpy(o->lan_relay, val, sizeof o->lan_relay);
        else if (!strcmp(key, "pin"))
            SDL_strlcpy(o->lan_pin, val, sizeof o->lan_pin);
        else if (!strcmp(key, "station_id"))
            o->station_id = (uint32_t)SDL_strtoul(val, NULL, 16) & 0xFFFFFFu;
    } else if (!strcmp(section, "input")) {
        if (!strcmp(key, "touch_controls")) {
            int m = lookup(touch_ids, NP_TOUCH_MODE_COUNT, val);
            if (m >= 0)
                o->touch_controls = m;
        } else if (!strcmp(key, "rumble"))
            o->rumble = iv != 0;
        else if (!strcmp(key, "skin") && !np_slot_name_problem(val))
            SDL_strlcpy(o->skin, val, sizeof o->skin);
    } else if (!strcmp(section, "keys")) {
        int a = lookup(action_ids, NP_ACT_COUNT, key);
        if (a < 0)
            return;
        int slot = 0;
        char *save = NULL;
        for (int s = 0; s < NP_KEY_SLOTS; s++)
            o->bind.key[a][s] = 0;
        for (char *tok = SDL_strtok_r(val, ",", &save); tok && slot < NP_KEY_SLOTS;
             tok = SDL_strtok_r(NULL, ",", &save)) {
            /* "-" keeps an empty slot so slots stay where the player put them. */
            char *name = trim(tok);
            SDL_Scancode sc = strcmp(name, "-") ? SDL_GetScancodeFromName(name) : SDL_SCANCODE_UNKNOWN;
            o->bind.key[a][slot++] = sc;
        }
    } else if (!strcmp(section, "gamepad")) {
        int a = lookup(action_ids, NP_ACT_COUNT, key);
        if (a >= 0)
            o->bind.pad[a] = pad_from_name(val);
    }
}

int np_options_load(np_options *o, const char *path)
{
    np_options_defaults(o);
    size_t size;
    char *text = SDL_LoadFile(path, &size);
    if (!text)
        return np_storage_exists(path) ? -1 : 0;
    char section[32] = "";
    char *save = NULL;
    for (char *line = SDL_strtok_r(text, "\n", &save); line; line = SDL_strtok_r(NULL, "\n", &save)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                SDL_strlcpy(section, s + 1, sizeof section);
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        apply(o, section, trim(s), trim(eq + 1));
    }
    SDL_free(text);
    return 0;
}

typedef struct strbuf {
    char data[8192];
    size_t len;
} strbuf;

static void put(strbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = SDL_vsnprintf(b->data + b->len, sizeof b->data - b->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        b->len = SDL_min(b->len + (size_t)n, sizeof b->data - 1);
}

int np_options_save(const np_options *o, const char *path)
{
    strbuf *b = SDL_malloc(sizeof *b);
    if (!b)
        return -1;
    b->len = 0;
    put(b, "# nativeplat options. Edit while the app is closed.\n\n[video]\n");
    put(b, "layout = %s\nswap = %d\nrotation = %d\n", layout_ids[o->layout], o->swap, o->rotation * 90);
    put(b, "battle_layout = %s\n", o->battle_layout ? layout_ids[o->battle_layout - 1] : "off");
    put(b, "scale = %s\nfilter = %s\n", o->scale == NP_SCALE_INTEGER ? "integer" : "fit",
        o->linear_filter ? "linear" : "nearest");
    put(b, "fullscreen = %d\nvsync = %d\nfps_cap = %d\n", o->fullscreen, o->vsync, np_fps_caps[o->fps_cap_index]);
    put(b, "effect1 = %s\neffect1_intensity = %d\neffect2 = %s\neffect2_intensity = %d\ncrt_curvature = %d\n",
        np_fx_ids[o->fx[0]], o->fx_intensity[0], np_fx_ids[o->fx[1]], o->fx_intensity[1], o->crt_curvature);
    put(b, "performance = %s\n\n", np_perf_ids[o->perf]);
    put(b, "[emulation]\nlogic_clock = %s\nspeed = %d\nff_speed = %d\nreal_clock = %d\n\n",
        o->logic_clock_60 ? "60" : "ds", np_speeds[o->speed_index], np_speeds[o->ff_speed_index], o->real_clock);
    put(b, "[audio]\nvolume = %d\nmute_unfocused = %d\nmusic_filter = %d\n\n", o->volume, o->mute_unfocused,
        o->music_filter);
    put(b, "[interface]\nui_scale = %d\nreduce_motion = %d\n\n", o->ui_scale, o->reduce_motion);
    put(b, "[game]\nbgm_volume = %d\nse_volume = %d\nrender_scale = %d\nwidescreen = %d\n", o->bgm_volume,
        o->se_volume, o->render_scale, o->widescreen);
    put(b, "camera_zoom = %d\ncamera_tilt = %d\ninstant_text = %d\nfix_bugs = %d\nrewind_seconds = %d\n\n",
        o->camera_zoom, o->camera_tilt, o->text_instant, o->fix_bugs, o->rewind_seconds);
    put(b, "[session]\nstartup = %s\n", o->startup_continue ? "continue" : "launcher");
    if (o->last_game >= 0 && o->last_game < NP_GAME_COUNT)
        put(b, "last_game = %s\n", np_game_ids[o->last_game]);
    for (int g = 0; g < NP_GAME_COUNT; g++)
        if (o->last_slot[g][0])
            put(b, "last_slot_%s = %s\n", np_game_ids[g], o->last_slot[g]);
    put(b, "\n[sync]\nfolder = %s\n", o->sync_folder);
    put(b, "\n[gba]\nrom = %s\nsave = %s\n", o->gba_rom, o->gba_save);
    if (o->update_repo[0] || o->update_api[0])
        put(b, "\n[updates]\nrepo = %s\napi = %s\n", o->update_repo, o->update_api);
    put(b, "\n[wireless]\nenabled = %d\nport = %d\npeer = %s\nrelay = %s\npin = %s\nstation_id = %06X\n",
        o->lan_enabled, o->lan_port, o->lan_peer, o->lan_relay, o->lan_pin, (unsigned)o->station_id);
    put(b, "\n[input]\ntouch_controls = %s\nrumble = %d\nskin = %s\n\n[keys]\n", touch_ids[o->touch_controls],
        o->rumble, o->skin);
    for (int a = 0; a < NP_ACT_COUNT; a++) {
        put(b, "%s =", action_ids[a]);
        int last = -1;
        for (int s = 0; s < NP_KEY_SLOTS; s++)
            if (o->bind.key[a][s])
                last = s;
        for (int s = 0; s <= last; s++) {
            const char *name = o->bind.key[a][s] ? SDL_GetScancodeName((SDL_Scancode)o->bind.key[a][s]) : "-";
            put(b, "%s %s", s ? "," : "", name);
        }
        put(b, "\n");
    }
    put(b, "\n[gamepad]\n");
    for (int a = 0; a < NP_ACT_COUNT; a++)
        put(b, "%s = %s\n", action_ids[a], pad_to_name(o->bind.pad[a]));
    int r = np_storage_write_atomic(path, b->data, b->len, 0);
    SDL_free(b);
    return r;
}
