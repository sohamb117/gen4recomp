/*
 * Player settings and input bindings, persisted as options.ini in the user
 * data root. Unknown keys are ignored and missing keys keep their defaults,
 * so files from older or newer builds still load.
 */
#ifndef NP_OPTIONS_H
#define NP_OPTIONS_H

#include "layout.h"

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

typedef struct np_options {
    np_layout_mode layout;
    int swap;
    int rotation; /* quarter turns clockwise */
    np_scale_mode scale;
    int linear_filter;
    int fullscreen;
    int vsync;
    int fps_cap_index;
    int logic_clock_60; /* 1: run the guest at exactly 60 Hz instead of 59.8261 */
    int speed_index;    /* base speed */
    int ff_speed_index; /* speed while fast-forward is held or toggled */
    int volume;         /* 0..100 */
    int mute_unfocused;
    int touch_controls; /* NP_TOUCH_* */
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
