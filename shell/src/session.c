/*
 * Features of a running game built on the core's contract v2: game options
 * pushed to the guest (volumes, render scale, widescreen, camera, instant
 * text, bug fixes), quick save (the game's own save, requested through
 * NP_OPT_QUICKSAVE_SEQ), quick load (reboot from the slot's last save),
 * in-memory snapshot slots and hold-to-rewind (np_core_state_*).
 *
 * Snapshots hold native stacks, so they live only in memory for this core;
 * the cartridge save stays the one persistent state. Everything here is
 * refused while a wireless session runs: the partner cannot follow a
 * rewound or reloaded machine.
 */
#include "app.h"
#include "rewind.h"

#define SNAP_SLOTS 4
/* Rewind captures every sixth guest frame (10 per second): a snapshot costs
 * the core ~12 ms, so this keeps the average under 2 ms per frame. Playback
 * steps back one capture every second presented frame, i.e. 3x. */
#define REWIND_INTERVAL 6
#define REWIND_STEP_FRAMES 2
/* Coded history per second of rewind; see README for the measured sizes. */
#define REWIND_BYTES_PER_SECOND (8u << 20)

struct np_session {
    np_rewind *rewind;
    int rewind_seconds; /* the history's budget was sized for this */
    int capture_in;     /* guest frames until the next capture */
    int rewind_end_shown;
    int step_wait; /* presented frames to hold the current rewind step */

    uint8_t *state; /* np_core_state_save scratch */
    size_t state_cap;

    uint8_t *snap[SNAP_SLOTS];
    size_t snap_len[SNAP_SLOTS], snap_cap[SNAP_SLOTS];
    int snap_slot;

    uint32_t qs_seq; /* quick save request awaiting its result */
    int qs_pending;
    uint32_t resets; /* NP_STAT_RESETS when the history was last valid */
    uint64_t reload_armed_until; /* ns: a second F2 before this reloads */
};

static int linked(const np_app *app) { return app->core && np_core_status(app->core, NP_STAT_LINK_ACTIVE); }

void np_session_apply_options(np_app *app)
{
    if (!app->core)
        return;
    const np_options *o = &app->opt;
    np_core_set_option(app->core, NP_OPT_BGM_VOLUME, (uint32_t)(o->bgm_volume * 256 / 100));
    np_core_set_option(app->core, NP_OPT_SE_VOLUME, (uint32_t)(o->se_volume * 256 / 100));
    np_core_set_option(app->core, NP_OPT_RENDER_SCALE, (uint32_t)o->render_scale);
    np_core_set_option(app->core, NP_OPT_WIDESCREEN, (uint32_t)o->widescreen);
    np_core_set_option(app->core, NP_OPT_CAMERA_ZOOM, (uint32_t)o->camera_zoom);
    np_core_set_option(app->core, NP_OPT_CAMERA_TILT, (uint32_t)(int32_t)o->camera_tilt);
    np_core_set_option(app->core, NP_OPT_TEXT_INSTANT, (uint32_t)o->text_instant);
    np_core_set_option(app->core, NP_OPT_RULES, o->fix_bugs ? NP_RULE_FIX_BUGS : 0);
    np_session *s = app->session;
    if (s && s->rewind_seconds != o->rewind_seconds) {
        np_rewind_destroy(s->rewind); /* resized on the next capture */
        s->rewind = NULL;
    }
}

void np_session_begin(np_app *app)
{
    np_session_end(app);
    app->session = SDL_calloc(1, sizeof *app->session);
    np_session_apply_options(app);
}

void np_session_end(np_app *app)
{
    np_session *s = app->session;
    if (!s)
        return;
    np_rewind_destroy(s->rewind);
    SDL_free(s->state);
    for (int i = 0; i < SNAP_SLOTS; i++)
        SDL_free(s->snap[i]);
    SDL_free(s);
    app->session = NULL;
}

/* Saves the machine into *buf (grown to fit). Returns the length, 0 if the
 * core cannot take snapshots. */
static size_t take_state(np_app *app, uint8_t **buf, size_t *cap)
{
    size_t need = np_core_state_size(app->core);
    if (!need)
        return 0;
    if (need > *cap) {
        uint8_t *p = SDL_realloc(*buf, need);
        if (!p)
            return 0;
        *buf = p;
        *cap = need;
    }
    size_t written = 0;
    if (np_core_state_save(app->core, *buf, *cap, &written))
        return 0;
    return written;
}

void np_session_frame_done(np_app *app)
{
    np_session *s = app->session;
    if (!s || !app->core)
        return;
    if (s->qs_pending && np_core_status(app->core, NP_STAT_QUICKSAVE_SEQ) == s->qs_seq) {
        s->qs_pending = 0;
        switch (np_core_status(app->core, NP_STAT_QUICKSAVE_RESULT)) {
        case NP_QS_SAVED: np_app_toast(app, "Saved"); break;
        case NP_QS_REFUSED: np_app_toast(app, "Can't save right now"); break;
        default: np_app_toast(app, "The save failed"); break;
        }
        SDL_Log("quick save %u: %s", s->qs_seq, app->toast);
    }
    /* A soft reset rebooted the guest: states of the instance before it are
     * refused by np_core_state_load, so their history goes. */
    uint32_t resets = np_core_status(app->core, NP_STAT_RESETS);
    if (resets != s->resets) {
        s->resets = resets;
        if (s->rewind)
            np_rewind_clear(s->rewind);
        SDL_Log("soft reset %u: the game rebooted", resets);
    }
    if (!app->opt.rewind_seconds || linked(app) || --s->capture_in > 0)
        return;
    s->capture_in = REWIND_INTERVAL;
    size_t len = take_state(app, &s->state, &s->state_cap);
    if (!len)
        return;
    if (!s->rewind) {
        s->rewind = np_rewind_create((size_t)app->opt.rewind_seconds * REWIND_BYTES_PER_SECOND, len);
        s->rewind_seconds = app->opt.rewind_seconds;
        if (!s->rewind)
            return;
    }
    np_rewind_push(s->rewind, s->state, len);
    s->rewind_end_shown = 0;
}

int np_session_rewind_step(np_app *app)
{
    np_session *s = app->session;
    if (!s || !app->core)
        return -1;
    if (linked(app)) {
        np_app_toast(app, "No rewind during a wireless session");
        return -1;
    }
    if (s->step_wait > 0) {
        s->step_wait--;
        return 1;
    }
    s->step_wait = REWIND_STEP_FRAMES - 1;
    const uint8_t *state;
    size_t len;
    if (!s->rewind || np_rewind_step_back(s->rewind, &state, &len)) {
        if (!s->rewind_end_shown)
            np_app_toast(app, app->opt.rewind_seconds ? "Start of the rewind history" : "Rewind is off (Options)");
        s->rewind_end_shown = 1;
        return -1;
    }
    if (np_core_state_load(app->core, state, len)) {
        SDL_Log("rewind: state load failed: %s", np_core_last_error(app->core));
        np_rewind_clear(s->rewind);
        return -1;
    }
    s->capture_in = REWIND_INTERVAL;
    return 0;
}

int np_session_rewind_depth(const np_app *app)
{
    const np_session *s = app->session;
    return s && s->rewind ? np_rewind_depth(s->rewind) : 0;
}

size_t np_session_rewind_bytes(const np_app *app)
{
    const np_session *s = app->session;
    return s && s->rewind ? np_rewind_used(s->rewind) : 0;
}

static void quick_save(np_app *app, np_session *s)
{
    if (linked(app)) {
        np_app_toast(app, "Save from the game's menu during a wireless session");
        return;
    }
    s->qs_seq = np_core_get_option(app->core, NP_OPT_QUICKSAVE_SEQ) + 1;
    np_core_set_option(app->core, NP_OPT_QUICKSAVE_SEQ, s->qs_seq);
    s->qs_pending = 1;
    np_app_toast(app, "Saving...");
}

static void quick_load(np_app *app, np_session *s)
{
    if (linked(app)) {
        np_app_toast(app, "Not during a wireless session");
        return;
    }
    uint64_t now = SDL_GetTicksNS();
    if (now >= s->reload_armed_until) {
        s->reload_armed_until = now + 3 * SDL_NS_PER_SECOND;
        np_app_toast(app, "Press F2 again to reload the last save (unsaved progress is lost)");
        return;
    }
    s->reload_armed_until = 0;
    np_app_reload_game(app); /* ends this session */
}

static void snapshot(np_app *app, np_session *s, int load)
{
    int i = s->snap_slot;
    if (linked(app)) {
        np_app_toast(app, "Not during a wireless session");
        return;
    }
    if (!load) {
        size_t len = take_state(app, &s->snap[i], &s->snap_cap[i]);
        if (!len) {
            np_app_toast(app, "Snapshots are not available in this build");
            return;
        }
        s->snap_len[i] = len;
        np_app_toast(app, "Snapshot %d taken (kept until the game closes)", i + 1);
        return;
    }
    if (!s->snap_len[i]) {
        np_app_toast(app, "Snapshot %d is empty (F5 takes one)", i + 1);
        return;
    }
    if (np_core_state_load(app->core, s->snap[i], s->snap_len[i])) {
        np_app_toast(app, "Snapshot %d could not be loaded", i + 1);
        return;
    }
    if (s->rewind)
        np_rewind_clear(s->rewind); /* history from another timeline */
    np_app_toast(app, "Snapshot %d loaded", i + 1);
}

static void camera(np_app *app, int dzoom, int dtilt, int reset)
{
    np_options *o = &app->opt;
    if (reset) {
        o->camera_zoom = 256;
        o->camera_tilt = 0;
    } else {
        o->camera_zoom = SDL_clamp(o->camera_zoom + dzoom, 64, 1024);
        o->camera_tilt = SDL_clamp(o->camera_tilt + dtilt, -720, 720);
    }
    app->options_dirty = 1;
    np_session_apply_options(app);
    np_app_toast(app, "Camera zoom %d%%, tilt %+d deg (0 resets)", o->camera_zoom * 100 / 256, o->camera_tilt / 16);
}

int np_session_hotkey(np_app *app, int scancode)
{
    np_session *s = app->session;
    if (!s || !app->core || app->view != NP_VIEW_GAME || app->page != NP_PAGE_NONE)
        return 0;
    switch (scancode) {
    case SDL_SCANCODE_F1: quick_save(app, s); return 1;
    case SDL_SCANCODE_F2: quick_load(app, s); return 1;
    case SDL_SCANCODE_F5: snapshot(app, s, 0); return 1;
    case SDL_SCANCODE_F7: snapshot(app, s, 1); return 1;
    case SDL_SCANCODE_F6:
        s->snap_slot = (s->snap_slot + 1) % SNAP_SLOTS;
        np_app_toast(app, "Snapshot slot %d%s", s->snap_slot + 1, s->snap_len[s->snap_slot] ? "" : " (empty)");
        return 1;
    case SDL_SCANCODE_MINUS: camera(app, 32, 0, 0); return 1;  /* farther */
    case SDL_SCANCODE_EQUALS: camera(app, -32, 0, 0); return 1; /* closer */
    case SDL_SCANCODE_3: camera(app, 0, -80, 0); return 1;     /* 5 degrees down */
    case SDL_SCANCODE_4: camera(app, 0, 80, 0); return 1;      /* 5 degrees toward the horizon */
    case SDL_SCANCODE_0: camera(app, 0, 0, 1); return 1;
    default: return 0;
    }
}
