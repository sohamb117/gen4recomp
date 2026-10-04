/*
 * Shell application state, shared by the modules that make up the app:
 *   main.c     SDL main callbacks, game session, frame pacing, autotest
 *   input.c    keyboard/gamepad bindings, fingers, stylus, menu commands
 *   touchpad.c on-screen touch controls
 *   ui.c       bitmap-font drawing, launcher, options/controls/about pages,
 *              save-slot pages (slot list, slot actions, confirm, name entry)
 *   audio.c    SDL audio stream fed from the core
 * Everything runs on the main thread except file dialog callbacks, which
 * only hand a path over through `pending_lock`.
 */
#ifndef NP_APP_H
#define NP_APP_H

#include <SDL3/SDL.h>

#include "launch.h"
#include "layout.h"
#include "lowpass.h"
#include "modpkg.h"
#include "np_core.h"
#include "np_guest_abi.h"
#include "options.h"
#include "storage.h"

#define NP_MAX_PADS 8
#define NP_MAX_FINGERS 10
#define NP_MAX_HITS 64
#define NP_MOUSE_FINGER ((SDL_FingerID)-2) /* the mouse acts as one more finger */

typedef enum np_view { NP_VIEW_LAUNCHER, NP_VIEW_GAME } np_view;
typedef enum np_page {
    NP_PAGE_NONE,
    NP_PAGE_OPTIONS,
    NP_PAGE_CONTROLS,
    NP_PAGE_ABOUT,
    NP_PAGE_SLOTS,     /* one game's save slots */
    NP_PAGE_SLOT_MENU, /* actions on the selected slot */
    NP_PAGE_CONFIRM,   /* delete confirmation */
    NP_PAGE_TEXT,      /* name entry (slots, trainer, nicknames) */
    NP_PAGE_EDITOR,    /* save editor (editor.c) */
    NP_PAGE_SYNC,      /* folder sync conflict chooser (sync.c) */
    NP_PAGE_MODS,      /* mod manager (mods.c) */
    NP_PAGE_UPDATES,   /* updater (update.c) */
    NP_PAGE_TOUCH_EDIT, /* touch controls layout editor (touchpad.c) */
} np_page;

/* Work handed from dialogs, drops and URLs to the main loop. */
typedef enum np_pending_kind {
    NP_PENDING_NONE,
    NP_PENDING_ROM,        /* import a cartridge */
    NP_PENDING_SAV_IMPORT, /* add a .sav as a slot of pending_game */
    NP_PENDING_SAV_EXPORT, /* write slot pending_slot of pending_game to path */
    NP_PENDING_MESSAGE,    /* a dialog failed; path holds the message */
    NP_PENDING_GIFT_IMPORT, /* a .pgt/.pcd for the open save editor */
    NP_PENDING_CARD_EXPORT, /* PNG of the open save; pending_card is the np_card_kind */
    NP_PENDING_SYNC_FOLDER, /* path is the folder picked for sync */
    NP_PENDING_MOD_INSTALL, /* a package .zip for mods.c */
    NP_PENDING_GBA_ROM,     /* a .gba for the GBA slot */
    NP_PENDING_GBA_SAVE,    /* the .sav that goes with it */
    NP_PENDING_SKIN,        /* a .deltaskin to install */
} np_pending_kind;

typedef enum np_text_purpose {
    NP_TEXT_NEW_SLOT,
    NP_TEXT_RENAME_SLOT,
    NP_TEXT_TRAINER_NAME, /* the editor's; commit goes to np_editor_text_done */
    NP_TEXT_NICKNAME,
    NP_TEXT_LAN_PEER, /* Options: "host:port" to join */
    NP_TEXT_LAN_RELAY, /* Options: internet relay "host:port" */
    NP_TEXT_LAN_PIN,   /* Options: relay room PIN */
    NP_TEXT_CART_NAME, /* Mods: name for a new sealed cart */
} np_text_purpose;

typedef enum np_menu_cmd {
    NP_CMD_NONE,
    NP_CMD_UP,
    NP_CMD_DOWN,
    NP_CMD_LEFT,
    NP_CMD_RIGHT,
    NP_CMD_CONFIRM,
    NP_CMD_BACK,
    NP_CMD_CLOSE, /* leave the overlay entirely */
    NP_CMD_TAB_PREV, /* L, Page Up */
    NP_CMD_TAB_NEXT, /* R, Page Down */
    NP_CMD_X,        /* X: the page's secondary action (editor: undo) */
    NP_CMD_Y,        /* Y (editor: redo) */
} np_menu_cmd;

/* A clickable region drawn this frame; `id` meaning depends on the page. */
typedef struct np_hit {
    SDL_FRect r;
    int id;
} np_hit;

typedef enum np_finger_kind {
    NP_FINGER_FREE,
    NP_FINGER_STYLUS,  /* drives the DS touch screen */
    NP_FINGER_CONTROL, /* on an on-screen button or the d-pad */
    NP_FINGER_UI,      /* tapping a menu item */
    NP_FINGER_IGNORED, /* landed on nothing; ignored until lifted */
} np_finger_kind;

typedef struct np_finger {
    SDL_FingerID id;
    np_finger_kind kind;
    float x, y;
} np_finger;

/* NP_AUTOTEST (see main.c): boot a core on a synthetic cartridge header, a
 * real cartridge (rom=), or start the app normally (boot=app); run N
 * iterations through the real render path with scripted input, write a PNG
 * of the window and exit. */
typedef enum np_autotest_boot { NP_AT_SYNTHETIC, NP_AT_ROM, NP_AT_APP } np_autotest_boot;

#define NP_AUTOTEST_MAX_PRESS 1024

/* One step of the press= schedule (see main.c). */
typedef struct np_press {
    int frame, n;      /* first frame, frames held */
    int every, count;  /* repeat period (0: once) and repetitions */
    uint16_t keys;
    uint8_t tap;       /* touch instead of keys */
    uint16_t x, y;
} np_press;

typedef struct np_autotest {
    int active;
    np_autotest_boot boot;
    int frames; /* iterations to run before the capture */
    int ran;
    char png[1024];
    char rom[1024];     /* NP_AT_ROM: cartridge path */
    np_input input;     /* fixed input fed to every frame */
    uint8_t header[0x200]; /* NP_AT_SYNTHETIC: cartridge header */
    uint8_t *save;      /* in-memory backup chip (unless storage) */
    uint32_t save_len;
    int saves, loads; /* successful save_store / save_load calls */
    uint64_t audio_frames;
    int audio_peak;
    double audio_treble; /* sum of squared steps, for the music filter check */
    int page; /* captured view: 0 game, -1 launcher, or an np_page over the game */
    int storage; /* use the real (portable) user-data root */
    char imports[4096]; /* '\n'-separated ROMs to run through the importer first */
    char script[6144];  /* "frame:kind:args;..." synthetic events */
    np_press presses[NP_AUTOTEST_MAX_PRESS]; /* press= schedule */
    int npress;
    int shot_every; /* shots=N: also write <png>-<iteration>.png every N iterations */
    char drop[1024];    /* storage for a scripted drop event's text */
    char slot[NP_SLOT_NAME_MAX + 1]; /* save slot for rom=/synthetic boots */
    int rewind_from, rewind_frames;  /* rewind=F+N: hold rewind for N iterations from F */
    char sync_folder[1024];          /* sync=<folder>: folder sync target */
    char gba_rom[1024], gba_save[1024]; /* gba=, gbasave=: the GBA slot */
    int realtime;      /* realtime=1: pace iterations at the logic clock, like the app */
    uint64_t next_ns;  /* realtime: when the next iteration is due */
} np_autotest;

typedef struct np_app {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *screen_tex[2];
    SDL_Texture *font_tex;
    float out_w, out_h; /* render output, pixels */

    np_options opt;
    char options_path[1100];
    int options_dirty;

    np_view view;
    np_page page;
    np_page page_parent; /* where Back goes from the current page */
    int sel, col, scroll;
    int capture;              /* controls page: waiting for a key/button */
    uint64_t capture_deadline; /* ns */
    np_hit hits[NP_MAX_HITS];
    int nhits;
    int ui_press_hit; /* hit id under the pointer when it went down, or -1 */

    int launcher_sel;
    char status[320]; /* launcher message line */
    char toast[160];
    uint64_t toast_until;

    SDL_Mutex *pending_lock;
    np_pending_kind pending;
    int pending_shown; /* the "working" message has been drawn */
    char pending_path[1024];
    np_game pending_game;
    char pending_slot[NP_SLOT_NAME_MAX + 1];
    int pending_card; /* np_card_kind for NP_PENDING_CARD_EXPORT */
    np_pending_kind dialog_kind; /* what the open file dialog is for */

    /* Save-slot pages */
    np_game slots_game;
    np_slot_list slots;
    int slot_sel; /* index into slots.slot of the slot the actions apply to */
    np_text_purpose text_purpose;
    char text[96]; /* at most text_max characters */
    char text_error[128];
    int osk_sel; /* on-screen keyboard key */
    int text_max; /* characters allowed on the text page */

    struct np_editor *editor; /* open save editor, or NULL */
    struct np_fx_state *fx;   /* display effects (fx.c) */
    struct np_net *net;       /* local wireless transport while enabled */
    char net_error[128];
    uint32_t net_realm; /* the running game's mod-set realm (carts.c); 0 = vanilla */
    char sync_status[96]; /* last folder sync result */
    char mods_root[1100]; /* np_host.content_root while a core runs */
    struct np_session *session; /* session.c, while a game runs */
    int rewind_hold;            /* the rewind action is held */

    np_core *core;
    np_game game;
    char slot[NP_SLOT_NAME_MAX + 1]; /* save slot of the running game */
    SDL_IOStream *rom_io;
    SDL_IOStream *gba_io; /* the cartridge in the GBA slot while a core runs */
    np_host host;
    np_frame frame;
    int have_frame;
    double accum_ns;
    uint64_t last_ns;
    int ff_toggle, ff_hold;
    int backgrounded, minimized, focused;
    np_layout layout;

    SDL_Gamepad *pads[NP_MAX_PADS];
    np_finger fingers[NP_MAX_FINGERS];
    int touch_seen;
    uint16_t control_keys; /* from the on-screen controls */
    int control_ff_hold;   /* a skin's fast-forward (hold) item is held */
    uint8_t trigger_down[NP_MAX_PADS][2];

    SDL_AudioStream *audio;
    int audio_running;
    np_lowpass lowpass; /* Options > Music filter */

    np_autotest autotest;
} np_app;

/* main.c */
void np_app_toast(np_app *app, const char *fmt, ...);
/* Boots `game` with save slot `slot` (must exist). */
int np_app_start_game(np_app *app, np_game game, const char *slot);
/* Boots the last used slot, or a new "Slot 1" when the game has none. */
int np_app_continue(np_app *app, np_game game);
/* Reboots the running game from its slot's last save. */
int np_app_reload_game(np_app *app);
void np_app_stop_game(np_app *app);
/* Queues work for the main loop; safe from any thread. */
void np_app_request(np_app *app, np_pending_kind kind, const char *path);
void np_app_open_rom_dialog(np_app *app);
void np_app_open_sav_import_dialog(np_app *app, np_game game);
void np_app_open_sav_export_dialog(np_app *app, np_game game, const char *slot);
void np_app_open_gift_import_dialog(np_app *app);
void np_app_open_card_export_dialog(np_app *app, int kind);
void np_app_open_sync_folder_dialog(np_app *app);
void np_app_open_mod_install_dialog(np_app *app);
/* Options > GBA cartridge: pick a .gba, or (save) its .sav file. */
void np_app_open_gba_dialog(np_app *app, int save);
void np_app_open_skin_dialog(np_app *app);
/* Whether the running core booted with a cartridge in the GBA slot. */
int np_app_gba_inserted(const np_app *app);
void np_app_apply_video_options(np_app *app);
void np_app_open_page(np_app *app, np_page page);
int np_app_speed(const np_app *app); /* effective multiplier, 0 = uncapped */
/* Rereads app->slots for app->slots_game; keeps slot_sel on `select` if given. */
void np_app_refresh_slots(np_app *app, const char *select);
/* Opens the save-slot page for `game`. */
void np_app_open_slots(np_app *app, np_game game);
/* Acts on a launch request (command line or URL). */
void np_app_launch(np_app *app, const np_launch *req);
/* (Re)opens or closes local wireless to match the options. */
void np_app_net_apply(np_app *app);
/* Stations in range, or -1 with wireless off. */
int np_app_net_peers(const np_app *app);

/* session.c: contract v2 features of the running game */
typedef struct np_session np_session;
void np_session_begin(np_app *app);
void np_session_end(np_app *app);
/* Pushes the game options to the core (after any change). */
void np_session_apply_options(np_app *app);
/* After each guest frame played forward. */
void np_session_frame_done(np_app *app);
/* Hold-to-rewind, once per presented frame: 0 loaded an older snapshot (run
 * one frame to show it), 1 keep showing the current one, -1 nothing older. */
int np_session_rewind_step(np_app *app);
int np_session_rewind_depth(const np_app *app);
size_t np_session_rewind_bytes(const np_app *app);
/* F1/F2 quick save/load, F5/F6/F7 snapshots, camera keys; 1 if handled. */
int np_session_hotkey(np_app *app, int scancode);

/* sync.c: folder sync of save slots (no-ops while no folder is set) */
/* Every slot of every game, e.g. at launch and quit; quiet: toast only news. */
void np_sync_all(np_app *app, int quiet);
/* One slot, after it was written. */
void np_sync_slot(np_app *app, np_game game, const char *slot);
/* A slot deleted or renamed here: drops its synced copy if nobody changed it. */
void np_sync_forget(np_app *app, np_game game, const char *slot);
int np_sync_conflicts(const np_app *app);
void np_sync_draw(np_app *app);
void np_sync_command(np_app *app, np_menu_cmd cmd);
void np_sync_hit(np_app *app, int id);

/* mods.c: runtime content packages (Platinum) */
/* The content root for `game`'s core into out; 0 if it exists, else -1. */
int np_mods_content_root(const np_app *app, np_game game, char *out, size_t n);
void np_mods_install(np_app *app, const char *zip_path);
/* Opens the page; `banner` explains a boot error, or NULL. */
void np_mods_open(np_app *app, const char *banner);
/* After a core failed: opens the page if the error is a package's. */
void np_mods_boot_failed(np_app *app, const char *error);
void np_mods_draw(np_app *app);
void np_mods_command(np_app *app, np_menu_cmd cmd);
void np_mods_hit(np_app *app, int id);
/* The enabled packages in load order (directory names); -1 if one has a
 * problem. */
int np_mods_enabled(char (*names)[NP_MOD_ID_MAX], int max);
/* Seals the enabled packages as cart `name` and returns to the Mods page. */
void np_mods_seal(np_app *app, const char *name);

/* carts.c: sealed mod sets */
#define NP_CART_MAX_PKGS 32
int np_carts_list(char (*names)[NP_SLOT_NAME_MAX + 1], int max);
int np_cart_seal(np_app *app, const char *name);
int np_cart_delete(const char *name);
int np_cart_describe(const char *name, char *out, size_t n); /* "N packages, <hash>" */
/* For booting `slot`: "PC_MODS=..." for a bound cart ("" otherwise) and the
 * link realm of the active set. -1 (with app->status) if the slot's cart is
 * missing or changed since sealing. */
int np_carts_for_boot(np_app *app, np_game game, const char *slot, char *pc_mods, size_t n, uint32_t *realm);

/* update.c: consent-based updater (hidden without a repository or HTTP) */
int np_update_enabled(const np_app *app);
void np_update_open(np_app *app);
void np_update_draw(np_app *app);
void np_update_command(np_app *app, np_menu_cmd cmd);
void np_update_hit(np_app *app, int id);
/* Cancels a transfer and joins the worker (at quit). */
void np_update_shutdown(void);

/* input.c */
void np_input_gamepad_added(np_app *app, SDL_JoystickID id);
void np_input_gamepad_removed(np_app *app, SDL_JoystickID id);
void np_input_close_gamepads(np_app *app);
/* DS keys and fast-forward-hold from keyboard, gamepads and touch controls. */
uint16_t np_input_poll_keys(np_app *app, int *ff_hold);
/* The gamepad binding value newly pressed by `e` (button down, or a trigger
 * crossing its threshold), else NP_PAD_NONE. Call once per event. */
int np_input_pad_press(np_app *app, const SDL_Event *e);
/* The action bound to `scancode` (if nonzero) or `pad`, or -1. */
int np_input_action_for(const np_app *app, int scancode, int pad);
/* Maps a key/gamepad event to a menu command, or NP_CMD_NONE. */
np_menu_cmd np_input_menu_cmd(const np_app *app, const SDL_Event *e, int pad);
/* Finger/mouse tracking; returns 1 if the event was consumed. */
int np_input_pointer_event(np_app *app, const SDL_Event *e);
/* Stylus state for the next frame. */
void np_input_stylus(const np_app *app, np_input *in);
void np_input_release_all(np_app *app);
/* A short gamepad rumble if Options > Rumble on press is on. */
void np_input_rumble(np_app *app);

/* touchpad.c */
int np_touchpad_visible(const np_app *app);
/* What a touch at one point presses: keys, plus one-shot actions. */
typedef struct np_touch_hit {
    uint16_t keys;
    int any; /* on a control at all (else the stylus may have it) */
    int ff_toggle, ff_hold, menu, quick_save, quick_load;
} np_touch_hit;
void np_touchpad_hit(const np_app *app, float x, float y, np_touch_hit *h);
void np_touchpad_draw(np_app *app);
/* Reads touch-controls.ini (after the storage root is known). */
void np_touchpad_load(np_app *app);
/* The layout editor page. */
void np_touchedit_open(np_app *app);
void np_touchedit_close(np_app *app); /* saves edits; on leaving the page */
void np_touchedit_draw(np_app *app);
void np_touchedit_pointer(np_app *app, float x, float y, int pressed, int released);
int np_touchedit_key(np_app *app, const SDL_KeyboardEvent *k);
void np_touchedit_command(np_app *app, np_menu_cmd cmd);

/* skin.c: Delta controller skins */
void np_skin_apply(np_app *app);    /* loads opt.skin (none if "") */
void np_skin_install(np_app *app, const char *path);
void np_skin_cycle(np_app *app, int dir); /* next/previous installed, or none */
const char *np_skin_name(void);          /* the loaded skin, or NULL */
/* Whether the skin covers the window's current orientation. */
int np_skin_active(const np_app *app);
/* Screen rectangles for np_layout_params.frames; 0 without a skin. */
int np_skin_frames(const np_app *app, float frames[8]);
void np_skin_draw_art(np_app *app);
void np_skin_draw_pressed(np_app *app, uint16_t keys);
/* Adds what a touch presses on the skin to h; 0 without a skin. */
int np_skin_hit(const np_app *app, float x, float y, np_touch_hit *h);
void np_skin_shutdown(void);

/* ui.c */
int np_ui_init(np_app *app);
void np_ui_destroy(np_app *app);
float np_ui_scale(const np_app *app);
void np_ui_text(np_app *app, float x, float y, float scale, const char *s, SDL_Color c);
void np_ui_fill(np_app *app, SDL_FRect r, SDL_Color c);
void np_ui_frame(np_app *app, SDL_FRect r, float t, SDL_Color c);
void np_ui_text_clip(np_app *app, float x, float y, float s, const char *str, int max_cols, SDL_Color c);
/* Word-wrapped text (drawn only with `draw`); returns the lines used. */
int np_ui_text_wrap(np_app *app, float x, float y, float s, float line_h, int cols, const char *str, SDL_Color c,
                    int draw);
/* Registers a clickable rectangle for this frame. */
void np_ui_hit(np_app *app, SDL_FRect r, int id);
void np_ui_button(np_app *app, SDL_FRect r, const char *label, int selected, int id, float s);

/* A modal panel: title, list area, footer with a hint, Back and scroll. */
typedef struct np_page_frame {
    float s, cw, lh; /* text scale, character width, line height */
    SDL_FRect panel;
    float list_y; /* first row */
    int rows;     /* visible rows */
    int cols;     /* text columns inside the panel */
} np_page_frame;
void np_ui_begin_page(np_app *app, np_page_frame *f, const char *title);
void np_ui_end_page(np_app *app, const np_page_frame *f, const char *hint, int total);
void np_ui_keep_visible(np_app *app, int sel, int rows, int total);
/* Opens the name entry page; `max` characters, prefilled with `initial`. */
void np_ui_open_text(np_app *app, np_text_purpose purpose, const char *initial, int max);
void np_ui_draw(np_app *app); /* launcher or the open page, plus toast */
void np_ui_command(np_app *app, np_menu_cmd cmd);
/* Leaves the open page the way Back does. */
void np_ui_back(np_app *app);
/* Pointer press/move/release at render coordinates; button 3 = secondary. */
void np_ui_pointer(np_app *app, float x, float y, int pressed, int released, int button);
int np_ui_capture_event(np_app *app, const SDL_Event *e, int pad); /* rebinding, name entry */

/* editor.c: the save editor page (NP_PAGE_EDITOR). */
/* Opens slot `slot` of `game`; on failure leaves the page and explains. */
void np_editor_open(np_app *app, np_game game, const char *slot);
/* Standalone: edits any save file (game -1: from the save, D/P by ROM). */
void np_editor_open_file(np_app *app, int game, const char *path);
void np_editor_close(np_app *app);
void np_editor_draw(np_app *app);
void np_editor_command(np_app *app, np_menu_cmd cmd);
/* Hit ids drawn by the editor are >= NP_EDITOR_HIT_BASE. */
#define NP_EDITOR_HIT_BASE 10000
void np_editor_hit(np_app *app, int id, int activate, int dir);
/* Keys and typed text the editor handles itself (shortcuts, filters, digits). */
int np_editor_event(np_app *app, const SDL_Event *e);
/* Result of the text page opened for NP_TEXT_TRAINER_NAME / NP_TEXT_NICKNAME:
 * returns NULL on success or an error to show on the text page. */
const char *np_editor_text_done(np_app *app, const char *text);
void np_editor_text_cancel(np_app *app);
/* Adds a .pgt/.pcd file to the open save as a Mystery Gift. */
void np_editor_import_gift(np_app *app, const char *path);
/* Writes a Trainer Card or Pokedex diploma PNG (np_card_kind) of the open save. */
void np_editor_export_card(np_app *app, int kind, const char *path);

/* audio.c */
int np_audio_open(np_app *app);
void np_audio_close(np_app *app);
void np_audio_update_gain(np_app *app);
void np_audio_set_paused(np_app *app, int paused);
/* Drains the core after a batch of frames; `speed` as np_app_speed. */
void np_audio_pump(np_app *app, int speed);

#endif
