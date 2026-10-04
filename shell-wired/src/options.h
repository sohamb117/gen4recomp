/*
 * Player settings and input bindings, persisted as options.ini in the user
 * data root. Unknown keys are ignored and missing keys keep their defaults,
 * so files from older or newer builds still load.
 */
#ifndef NP_OPTIONS_H
#define NP_OPTIONS_H

#include "layout.h"
#include "np_core.h"
#include "slots.h"

/* Bindable actions: the twelve DS keys, then shell actions. */
typedef enum np_action {
    NP_ACT_A,
    NP_ACT_B,
    NP_ACT_X,
    NP_ACT_Y,
    NP_ACT_L,
    NP_ACT_R,
    NP_ACT_START,
    NP_ACT_SELECT,
    NP_ACT_UP,
    NP_ACT_DOWN,
    NP_ACT_LEFT,
    NP_ACT_RIGHT,
    NP_ACT_FF_HOLD,
    NP_ACT_FF_TOGGLE,
    NP_ACT_REWIND, /* hold to rewind */
    NP_ACT_COUNT
} np_action;

#define NP_KEY_SLOTS 3
/* Gamepad binding values: an SDL_GamepadButton, or one of these. */
#define NP_PAD_NONE (-1)
#define NP_PAD_LEFT_TRIGGER 100
#define NP_PAD_RIGHT_TRIGGER 101

typedef struct np_bindings {
    int key[NP_ACT_COUNT][NP_KEY_SLOTS]; /* SDL_Scancode, 0 = unbound */
    int pad[NP_ACT_COUNT];
} np_bindings;

enum { NP_TOUCH_AUTO, NP_TOUCH_ON, NP_TOUCH_OFF, NP_TOUCH_MODE_COUNT };

#define NP_SPEED_COUNT 6
extern const int np_speeds[NP_SPEED_COUNT]; /* 1,2,3,4,8 and 0 = uncapped */
#define NP_FPS_CAP_COUNT 6
extern const int np_fps_caps[NP_FPS_CAP_COUNT]; /* 0 = off */

/* Display effects, two chained slots (fx.c). */
enum { NP_FX_OFF, NP_FX_LCD, NP_FX_SCANLINES, NP_FX_CRT, NP_FX_SMOOTH, NP_FX_COUNT };
extern const char *const np_fx_ids[NP_FX_COUNT]; /* "off", "lcd", ... for options.ini */
/* Presentation presets: Custom uses the VSync/FPS cap/effect options as set. */
enum { NP_PERF_CUSTOM, NP_PERF_HIGH, NP_PERF_BALANCED, NP_PERF_LOW, NP_PERF_AUTO, NP_PERF_COUNT };
extern const char *const np_perf_ids[NP_PERF_COUNT];

typedef struct np_options {
    np_layout_mode layout;
    int swap;
    int rotation; /* quarter turns clockwise */
    np_scale_mode scale;
    int linear_filter;
    int fullscreen;
    int vsync;
    int fx[2];           /* NP_FX_* in chain order */
    int fx_intensity[2]; /* 0..100 */
    int crt_curvature;   /* CRT bends the picture */
    int perf;            /* NP_PERF_* */
    int fps_cap_index;
    int logic_clock_60; /* 1: run the guest at exactly 60 Hz instead of 59.8261 */
    int speed_index;    /* base speed */
    int ff_speed_index; /* speed while fast-forward is held or toggled */
    int volume;         /* 0..100 */
    int mute_unfocused;
    int touch_controls; /* NP_TOUCH_* */
    int real_clock;     /* 1: RTC from the device's local time; 0: the port's deterministic clock */
    int startup_continue; /* 1: boot straight into the last game and slot */
    int last_game;      /* np_game last played, or -1 */
    char last_slot[NP_GAME_COUNT][NP_SLOT_NAME_MAX + 1]; /* "" when none */
    /* Game options the core applies live (np_guest_abi.h NP_OPT_*). */
    int bgm_volume, se_volume; /* 0..100 % of the cartridge's mix */
    int render_scale;          /* 1..4, internal 3D resolution */
    int widescreen;
    int camera_zoom;  /* NP_OPT_CAMERA_ZOOM: 256 = the game's camera */
    int camera_tilt;  /* NP_OPT_CAMERA_TILT: 1/16 degree, + toward the horizon */
    int text_instant;
    int fix_bugs;       /* NP_RULE_FIX_BUGS */
    int rewind_seconds; /* history kept for hold-to-rewind, 0 = off */
    char sync_folder[1024]; /* folder sync target (sync.c), "" = off */
    /* Local wireless (net.c). station_id is generated once and kept: the
     * game derives the console's MAC from it and stores that in saves. */
    int lan_enabled;
    int lan_port;
    char lan_peer[64]; /* "host:port" to join beyond LAN discovery, or "" */
    char lan_relay[96]; /* "host:port" of an internet relay (net.c), "" = LAN */
    char lan_pin[33];   /* relay room PIN */
    uint32_t station_id;
    np_bindings bind;
} np_options;

void np_options_defaults(np_options *o);
void np_bindings_defaults(np_bindings *b);
/* Missing file -> defaults, returns 0. */
int np_options_load(np_options *o, const char *path);
int np_options_save(const np_options *o, const char *path);

const char *np_action_name(np_action a); /* "A", "Fast-forward (hold)" */

/* Short display name for a gamepad binding value ("South", "L1", "RT"). */
const char *np_pad_binding_name(int pad);

/* Binds `scancode` to action slot, removing it from any other slot so one
 * key never drives two actions. Same for gamepad buttons. */
void np_bind_key(np_bindings *b, np_action a, int slot, int scancode);
void np_bind_pad(np_bindings *b, np_action a, int pad);

#endif
