/*
 * Input: bound keys and gamepad buttons are polled once per emulated batch
 * (held state is what the DS samples), while presses that trigger shell
 * actions (fast-forward toggle, menu navigation) are taken from events so
 * they fire exactly once.
 *
 * Pointers: every finger and the mouse's left button are tracked as
 * "fingers". What a finger does is decided when it lands: on a menu item it
 * taps the UI, on an on-screen control it holds that control (sliding across
 * the d-pad changes direction), on the bottom screen it becomes the stylus.
 * Only one finger is the stylus at a time; it stays down when dragged off
 * the screen, clamped to the edge, like a real stylus leaving the panel.
 */
#include "app.h"

#define STICK_THRESHOLD 16000
#define TRIGGER_THRESHOLD 16000

static const uint16_t action_keys[12] = {
    NP_KEY_A,     NP_KEY_B,      NP_KEY_X,  NP_KEY_Y,    NP_KEY_L,    NP_KEY_R,
    NP_KEY_START, NP_KEY_SELECT, NP_KEY_UP, NP_KEY_DOWN, NP_KEY_LEFT, NP_KEY_RIGHT,
};

void np_input_gamepad_added(np_app *app, SDL_JoystickID id)
{
    for (int i = 0; i < NP_MAX_PADS; i++)
        if (app->pads[i] && SDL_GetGamepadID(app->pads[i]) == id)
            return;
    for (int i = 0; i < NP_MAX_PADS; i++) {
        if (!app->pads[i]) {
            app->pads[i] = SDL_OpenGamepad(id);
            app->trigger_down[i][0] = app->trigger_down[i][1] = 0;
            if (app->pads[i]) {
                const char *name = SDL_GetGamepadName(app->pads[i]);
                np_app_toast(app, "Controller connected: %s", name ? name : "gamepad");
            }
            return;
        }
    }
}

void np_input_gamepad_removed(np_app *app, SDL_JoystickID id)
{
    for (int i = 0; i < NP_MAX_PADS; i++) {
        if (app->pads[i] && SDL_GetGamepadID(app->pads[i]) == id) {
            SDL_CloseGamepad(app->pads[i]);
            app->pads[i] = NULL;
            np_app_toast(app, "Controller disconnected");
        }
    }
}

void np_input_close_gamepads(np_app *app)
{
    for (int i = 0; i < NP_MAX_PADS; i++) {
        if (app->pads[i])
            SDL_CloseGamepad(app->pads[i]);
        app->pads[i] = NULL;
    }
}

static int pad_held(SDL_Gamepad *pad, int binding)
{
    if (binding == NP_PAD_LEFT_TRIGGER)
        return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_THRESHOLD;
    if (binding == NP_PAD_RIGHT_TRIGGER)
        return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_THRESHOLD;
    return binding >= 0 && SDL_GetGamepadButton(pad, (SDL_GamepadButton)binding);
}

uint16_t np_input_poll_keys(np_app *app, int *ff_hold)
{
    const bool *ks = SDL_GetKeyboardState(NULL);
    /* Alt/Cmd chords are shell hotkeys (Alt+Enter, Cmd+Q), not game input. */
    int keyboard = !(SDL_GetModState() & (SDL_KMOD_ALT | SDL_KMOD_GUI));
    const np_bindings *b = &app->opt.bind;
    int held[NP_ACT_COUNT] = {0};
    for (int a = 0; a < NP_ACT_COUNT; a++) {
        for (int s = 0; keyboard && s < NP_KEY_SLOTS && !held[a]; s++)
            held[a] = b->key[a][s] && ks[b->key[a][s]];
        for (int p = 0; p < NP_MAX_PADS && !held[a]; p++)
            held[a] = app->pads[p] && pad_held(app->pads[p], b->pad[a]);
    }
    uint16_t keys = app->control_keys;
    for (int a = 0; a < 12; a++)
        if (held[a])
            keys |= action_keys[a];
    for (int p = 0; p < NP_MAX_PADS; p++) {
        if (!app->pads[p])
            continue;
        int x = SDL_GetGamepadAxis(app->pads[p], SDL_GAMEPAD_AXIS_LEFTX);
        int y = SDL_GetGamepadAxis(app->pads[p], SDL_GAMEPAD_AXIS_LEFTY);
        if (x < -STICK_THRESHOLD)
            keys |= NP_KEY_LEFT;
        if (x > STICK_THRESHOLD)
            keys |= NP_KEY_RIGHT;
        if (y < -STICK_THRESHOLD)
            keys |= NP_KEY_UP;
        if (y > STICK_THRESHOLD)
            keys |= NP_KEY_DOWN;
    }
    *ff_hold = held[NP_ACT_FF_HOLD];
    app->rewind_hold = held[NP_ACT_REWIND];
    return keys;
}

int np_input_pad_press(np_app *app, const SDL_Event *e)
{
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        return e->gbutton.button;
    if (e->type != SDL_EVENT_GAMEPAD_AXIS_MOTION)
        return NP_PAD_NONE;
    int which;
    if (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
        which = 0;
    else if (e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
        which = 1;
    else
        return NP_PAD_NONE;
    for (int p = 0; p < NP_MAX_PADS; p++) {
        if (!app->pads[p] || SDL_GetGamepadID(app->pads[p]) != e->gaxis.which)
            continue;
        int down = e->gaxis.value > TRIGGER_THRESHOLD;
        int rising = down && !app->trigger_down[p][which];
        app->trigger_down[p][which] = (uint8_t)down;
        if (rising)
            return which ? NP_PAD_RIGHT_TRIGGER : NP_PAD_LEFT_TRIGGER;
    }
    return NP_PAD_NONE;
}

int np_input_action_for(const np_app *app, int scancode, int pad)
{
    const np_bindings *b = &app->opt.bind;
    for (int a = 0; a < NP_ACT_COUNT; a++) {
        if (scancode)
            for (int s = 0; s < NP_KEY_SLOTS; s++)
                if (b->key[a][s] == scancode)
                    return a;
        if (pad != NP_PAD_NONE && b->pad[a] == pad)
            return a;
    }
    return -1;
}

static np_menu_cmd action_cmd(int act)
{
    switch (act) {
    case NP_ACT_UP: return NP_CMD_UP;
    case NP_ACT_DOWN: return NP_CMD_DOWN;
    case NP_ACT_LEFT: return NP_CMD_LEFT;
    case NP_ACT_RIGHT: return NP_CMD_RIGHT;
    case NP_ACT_A: return NP_CMD_CONFIRM;
    case NP_ACT_B: return NP_CMD_BACK;
    case NP_ACT_START: return NP_CMD_CLOSE;
    case NP_ACT_L: return NP_CMD_TAB_PREV;
    case NP_ACT_R: return NP_CMD_TAB_NEXT;
    case NP_ACT_X: return NP_CMD_X;
    case NP_ACT_Y: return NP_CMD_Y;
    default: return NP_CMD_NONE;
    }
}

np_menu_cmd np_input_menu_cmd(const np_app *app, const SDL_Event *e, int pad)
{
    if (e->type == SDL_EVENT_KEY_DOWN) {
        /* Fixed keys first so menus stay usable whatever the bindings. */
        switch (e->key.scancode) {
        case SDL_SCANCODE_UP: return NP_CMD_UP;
        case SDL_SCANCODE_DOWN: return NP_CMD_DOWN;
        case SDL_SCANCODE_LEFT: return NP_CMD_LEFT;
        case SDL_SCANCODE_RIGHT: return NP_CMD_RIGHT;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: return NP_CMD_CONFIRM;
        case SDL_SCANCODE_ESCAPE: return NP_CMD_BACK;
        case SDL_SCANCODE_PAGEUP: return NP_CMD_TAB_PREV;
        case SDL_SCANCODE_PAGEDOWN: return NP_CMD_TAB_NEXT;
        default: break;
        }
        if (e->key.repeat) {
            int act = np_input_action_for(app, e->key.scancode, NP_PAD_NONE);
            return act >= NP_ACT_UP && act <= NP_ACT_RIGHT ? action_cmd(act) : NP_CMD_NONE;
        }
        return action_cmd(np_input_action_for(app, e->key.scancode, NP_PAD_NONE));
    }
    if (pad == NP_PAD_NONE)
        return NP_CMD_NONE;
    switch (pad) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return NP_CMD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return NP_CMD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return NP_CMD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return NP_CMD_RIGHT;
    case SDL_GAMEPAD_BUTTON_GUIDE: return NP_CMD_CLOSE;
    default: return action_cmd(np_input_action_for(app, 0, pad));
    }
}

static np_finger *finger_find(np_app *app, SDL_FingerID id)
{
    for (int i = 0; i < NP_MAX_FINGERS; i++)
        if (app->fingers[i].kind != NP_FINGER_FREE && app->fingers[i].id == id)
            return &app->fingers[i];
    return NULL;
}

/* A short, light pulse on every gamepad: the desktop stand-in for the
 * haptic tick a phone gives when an on-screen button is pressed. */
void np_input_rumble(np_app *app)
{
    if (!app->opt.rumble)
        return;
    for (int p = 0; p < NP_MAX_PADS; p++)
        if (app->pads[p])
            SDL_RumbleGamepad(app->pads[p], 0x3000, 0x6000, 30);
}

static void recompute_controls(np_app *app)
{
    uint16_t keys = 0;
    for (int i = 0; i < NP_MAX_FINGERS; i++) {
        const np_finger *f = &app->fingers[i];
        if (f->kind == NP_FINGER_CONTROL) {
            int ff, menu, any;
            keys |= np_touchpad_hit(app, f->x, f->y, &ff, &menu, &any);
        }
    }
    if (keys & ~app->control_keys)
        np_input_rumble(app); /* a button newly under a finger */
    app->control_keys = keys;
}

static int ui_active(const np_app *app) { return app->view == NP_VIEW_LAUNCHER || app->page != NP_PAGE_NONE; }

static void pointer_down(np_app *app, SDL_FingerID id, float x, float y, int button)
{
    np_finger *f = finger_find(app, id);
    if (!f) {
        for (int i = 0; i < NP_MAX_FINGERS && !f; i++)
            if (app->fingers[i].kind == NP_FINGER_FREE)
                f = &app->fingers[i];
        if (!f)
            return;
    }
    f->id = id;
    f->x = x;
    f->y = y;
    f->kind = NP_FINGER_IGNORED;
    if (ui_active(app)) {
        f->kind = NP_FINGER_UI;
        np_ui_pointer(app, x, y, 1, 0, button);
        return;
    }
    if (button != 1)
        return;
    int ff = 0, menu = 0, any = 0;
    if (np_touchpad_visible(app))
        np_touchpad_hit(app, x, y, &ff, &menu, &any);
    if (any) {
        f->kind = NP_FINGER_CONTROL;
        if (ff) {
            app->ff_toggle = !app->ff_toggle;
            np_app_toast(app, app->ff_toggle ? "Fast-forward on" : "Fast-forward off");
        }
        if (menu) {
            np_app_open_page(app, NP_PAGE_OPTIONS);
            return; /* open_page released every finger */
        }
    } else {
        int tx, ty, stylus_taken = 0;
        for (int i = 0; i < NP_MAX_FINGERS; i++)
            stylus_taken |= app->fingers[i].kind == NP_FINGER_STYLUS;
        if (!stylus_taken && np_layout_touch(&app->layout, x, y, 0, &tx, &ty))
            f->kind = NP_FINGER_STYLUS;
    }
    recompute_controls(app);
}

static void pointer_move(np_app *app, SDL_FingerID id, float x, float y)
{
    np_finger *f = finger_find(app, id);
    if (!f) {
        if (id == NP_MOUSE_FINGER && ui_active(app))
            np_ui_pointer(app, x, y, 0, 0, 0); /* hover */
        return;
    }
    f->x = x;
    f->y = y;
    if (f->kind == NP_FINGER_UI)
        np_ui_pointer(app, x, y, 0, 0, 0);
    else if (f->kind == NP_FINGER_CONTROL)
        recompute_controls(app);
}

static void pointer_up(np_app *app, SDL_FingerID id, float x, float y, int button)
{
    np_finger *f = finger_find(app, id);
    if (!f)
        return;
    np_finger_kind kind = f->kind;
    f->kind = NP_FINGER_FREE;
    if (kind == NP_FINGER_UI && ui_active(app))
        np_ui_pointer(app, x, y, 0, 1, button);
    recompute_controls(app);
}

static int mouse_button(Uint8 b) { return b == SDL_BUTTON_LEFT ? 1 : b == SDL_BUTTON_RIGHT ? 3 : 0; }

int np_input_pointer_event(np_app *app, const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_FINGER_DOWN:
        app->touch_seen = 1;
        pointer_down(app, e->tfinger.fingerID, e->tfinger.x, e->tfinger.y, 1);
        return 1;
    case SDL_EVENT_FINGER_MOTION:
        pointer_move(app, e->tfinger.fingerID, e->tfinger.x, e->tfinger.y);
        return 1;
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        pointer_up(app, e->tfinger.fingerID, e->tfinger.x, e->tfinger.y, 1);
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.which == SDL_TOUCH_MOUSEID || !mouse_button(e->button.button))
            return 1; /* synthesized from touch: the finger events handle it */
        if (finger_find(app, NP_MOUSE_FINGER))
            return 1; /* one button at a time */
        pointer_down(app, NP_MOUSE_FINGER, e->button.x, e->button.y, mouse_button(e->button.button));
        return 1;
    case SDL_EVENT_MOUSE_MOTION:
        if (e->motion.which != SDL_TOUCH_MOUSEID)
            pointer_move(app, NP_MOUSE_FINGER, e->motion.x, e->motion.y);
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.which != SDL_TOUCH_MOUSEID && mouse_button(e->button.button))
            pointer_up(app, NP_MOUSE_FINGER, e->button.x, e->button.y, mouse_button(e->button.button));
        return 1;
    case SDL_EVENT_MOUSE_WHEEL:
        if (ui_active(app) && e->wheel.y != 0.0f)
            np_ui_command(app, e->wheel.y > 0 ? NP_CMD_UP : NP_CMD_DOWN);
        return 1;
    default:
        return 0;
    }
}

void np_input_stylus(const np_app *app, np_input *in)
{
    in->touch = 0;
    for (int i = 0; i < NP_MAX_FINGERS; i++) {
        const np_finger *f = &app->fingers[i];
        int tx, ty;
        if (f->kind == NP_FINGER_STYLUS && np_layout_touch(&app->layout, f->x, f->y, 1, &tx, &ty)) {
            in->touch = 1;
            in->touch_x = (uint16_t)tx;
            in->touch_y = (uint16_t)ty;
            return;
        }
    }
}

void np_input_release_all(np_app *app)
{
    for (int i = 0; i < NP_MAX_FINGERS; i++)
        app->fingers[i].kind = NP_FINGER_FREE;
    app->control_keys = 0;
    app->ui_press_hit = -1;
}
