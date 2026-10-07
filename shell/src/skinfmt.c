/* Delta DS skin info.json (see skinfmt.h). */
#include "skinfmt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "np_core.h"

#define MAX_TOKENS 16384

typedef struct ctx {
    const char *s;
    const np_json_tok *t;
} ctx;

static float num(const ctx *c, int obj, const char *key, float fallback)
{
    double v = np_json_number(c->s, c->t, np_json_get(c->s, c->t, obj, key), fallback);
    return isfinite(v) ? (float)v : fallback;
}

static int rect(const ctx *c, int obj, np_skin_rect *r)
{
    if (obj < 0 || c->t[obj].type != NP_JSON_OBJECT)
        return -1;
    *r = (np_skin_rect){num(c, obj, "x", 0), num(c, obj, "y", 0), num(c, obj, "width", -1), num(c, obj, "height", -1)};
    return r->w > 0 && r->h > 0 ? 0 : -1;
}

typedef struct edges {
    float top, bottom, left, right;
} edges;

static edges read_edges(const ctx *c, int obj, edges fallback)
{
    if (obj < 0 || c->t[obj].type != NP_JSON_OBJECT)
        return fallback;
    return (edges){num(c, obj, "top", fallback.top), num(c, obj, "bottom", fallback.bottom),
                   num(c, obj, "left", fallback.left), num(c, obj, "right", fallback.right)};
}

static const struct {
    const char *name;
    uint16_t key;
} key_names[] = {
    {"a", NP_KEY_A},           {"b", NP_KEY_B},         {"x", NP_KEY_X},       {"y", NP_KEY_Y},
    {"l", NP_KEY_L},           {"r", NP_KEY_R},         {"start", NP_KEY_START}, {"select", NP_KEY_SELECT},
    {"up", NP_KEY_UP},         {"down", NP_KEY_DOWN},   {"left", NP_KEY_LEFT}, {"right", NP_KEY_RIGHT},
};

/* One "inputs" string: a key, or a special action. */
static int input_named(const ctx *c, int tok, np_skin_item *it)
{
    for (size_t k = 0; k < sizeof key_names / sizeof key_names[0]; k++)
        if (np_json_eq(c->s, c->t, tok, key_names[k].name)) {
            it->keys |= key_names[k].key;
            return 0;
        }
    static const struct {
        const char *name;
        np_skin_action action;
    } special[] = {{"menu", NP_SKIN_MENU},
                   {"fastForward", NP_SKIN_FF_HOLD},
                   {"toggleFastForward", NP_SKIN_FF_TOGGLE},
                   {"quickSave", NP_SKIN_QUICK_SAVE},
                   {"quickLoad", NP_SKIN_QUICK_LOAD}};
    for (size_t k = 0; k < sizeof special / sizeof special[0]; k++)
        if (np_json_eq(c->s, c->t, tok, special[k].name)) {
            it->action = special[k].action;
            return 0;
        }
    return -1; /* an input this port has no use for */
}

static int parse_item(const ctx *c, int obj, edges defaults, np_skin_item *it)
{
    memset(it, 0, sizeof *it);
    if (rect(c, np_json_get(c->s, c->t, obj, "frame"), &it->frame))
        return -1;
    int inputs = np_json_get(c->s, c->t, obj, "inputs");
    if (inputs < 0)
        return -1;
    it->action = NP_SKIN_KEYS;
    if (c->t[inputs].type == NP_JSON_ARRAY) {
        for (int i = 0; i < c->t[inputs].size; i++)
            input_named(c, np_json_at(c->t, inputs, i), it);
        if (it->action == NP_SKIN_KEYS && !it->keys)
            return -1;
    } else if (c->t[inputs].type == NP_JSON_OBJECT) {
        if (np_json_get(c->s, c->t, inputs, "up") >= 0 && np_json_get(c->s, c->t, inputs, "left") >= 0)
            it->action = NP_SKIN_DPAD;
        else if (np_json_get(c->s, c->t, inputs, "x") >= 0 && np_json_get(c->s, c->t, inputs, "y") >= 0)
            it->action = NP_SKIN_TOUCH_SCREEN;
        else
            return -1; /* analog sticks and the like */
    } else {
        return -1;
    }
    edges e = read_edges(c, np_json_get(c->s, c->t, obj, "extendedEdges"), defaults);
    it->hit = (np_skin_rect){it->frame.x - e.left, it->frame.y - e.top, it->frame.w + e.left + e.right,
                             it->frame.h + e.top + e.bottom};
    return 0;
}

static int plain_file_name(const char *n)
{
    return n[0] && n[0] != '.' && !strchr(n, '/') && !strchr(n, '\\') && !strchr(n, ':');
}

static int parse_rep(const ctx *c, int obj, int gba, np_skin_rep *r)
{
    memset(r, 0, sizeof *r);
    int map = np_json_get(c->s, c->t, obj, "mappingSize");
    r->map_w = num(c, map, "width", 0);
    r->map_h = num(c, map, "height", 0);
    if (map < 0 || !(r->map_w > 0) || !(r->map_h > 0))
        return -1;
    int assets = np_json_get(c->s, c->t, obj, "assets");
    static const char *const png_keys[] = {"large", "medium", "small"};
    for (int i = 0; i < 3 && !r->asset[0]; i++)
        np_json_string(c->s, c->t, np_json_get(c->s, c->t, assets, png_keys[i]), r->asset, sizeof r->asset);
    if (!r->asset[0] && !np_json_string(c->s, c->t, np_json_get(c->s, c->t, assets, "resizable"), r->asset,
                                        sizeof r->asset))
        r->asset_is_pdf = 1;
    if (!r->asset[0] || !plain_file_name(r->asset))
        return -1;
    r->translucent = np_json_bool(c->s, c->t, np_json_get(c->s, c->t, obj, "translucent"), 0);

    edges defaults = read_edges(c, np_json_get(c->s, c->t, obj, "extendedEdges"), (edges){0, 0, 0, 0});
    int items = np_json_get(c->s, c->t, obj, "items");
    for (int i = 0; items >= 0 && i < c->t[items].size && r->nitems < NP_SKIN_MAX_ITEMS; i++)
        if (!parse_item(c, np_json_at(c->t, items, i), defaults, &r->item[r->nitems]))
            r->nitems++;

    int screens = np_json_get(c->s, c->t, obj, "screens");
    for (int i = 0; screens >= 0 && i < c->t[screens].size && r->nscreens < 2; i++) {
        int sc = np_json_at(c->t, screens, i);
        np_skin_screen *dst = &r->screen[r->nscreens];
        if (rect(c, np_json_get(c->s, c->t, sc, "outputFrame"), &dst->output))
            continue;
        /* Without an inputFrame a DS screen shows the top, then the
         * bottom; a GBA screen the whole picture. */
        if (rect(c, np_json_get(c->s, c->t, sc, "inputFrame"), &dst->input))
            dst->input = gba ? (np_skin_rect){0, 0, 240, 160} : (np_skin_rect){0, (float)(192 * r->nscreens), 256, 192};
        r->nscreens++;
        if (gba)
            break; /* one screen */
    }
    np_skin_rect game;
    if (!r->nscreens && !rect(c, np_json_get(c->s, c->t, obj, "gameScreenFrame"), &game)) {
        if (gba) {
            r->screen[0] = (np_skin_screen){{0, 0, 240, 160}, game};
            r->nscreens = 1;
        } else {
            /* Older DS skins give one frame for both screens, top above bottom. */
            r->screen[0] = (np_skin_screen){{0, 0, 256, 192}, {game.x, game.y, game.w, game.h * 0.5f}};
            r->screen[1] = (np_skin_screen){{0, 192, 256, 192}, {game.x, game.y + game.h * 0.5f, game.w, game.h * 0.5f}};
            r->nscreens = 2;
        }
    }
    if (!r->nscreens)
        return -1;
    r->present = 1;
    return 0;
}

int np_skin_parse(const char *json, size_t len, np_skin_def *d, const char **why)
{
    memset(d, 0, sizeof *d);
    np_json_tok *t = malloc(sizeof *t * MAX_TOKENS);
    if (!t) {
        *why = "out of memory";
        return -1;
    }
    int rc = -1;
    ctx c = {json, t};
    if (np_json_parse(json, len, t, MAX_TOKENS) <= 0 || t[0].type != NP_JSON_OBJECT) {
        *why = "info.json is not valid JSON";
        goto done;
    }
    int type = np_json_get(json, t, 0, "gameTypeIdentifier");
    d->gba = np_json_eq(json, t, type, NP_SKIN_GAME_TYPE_GBA);
    if (!d->gba && !np_json_eq(json, t, type, NP_SKIN_GAME_TYPE)) {
        *why = "not a Nintendo DS or Game Boy Advance skin";
        goto done;
    }
    np_json_string(json, t, np_json_get(json, t, 0, "name"), d->name, sizeof d->name);
    np_json_string(json, t, np_json_get(json, t, 0, "identifier"), d->identifier, sizeof d->identifier);
    int reps = np_json_get(json, t, 0, "representations");
    static const char *const order[][2] = {
        {"iphone", "edgeToEdge"}, {"iphone", "standard"}, {"ipad", "standard"}, {"ipad", "splitView"}};
    static const char *const orient[2] = {"landscape", "portrait"};
    for (int o = 0; o < 2; o++)
        for (size_t k = 0; k < sizeof order / sizeof order[0] && !d->rep[o].present; k++) {
            int dev = np_json_get(json, t, reps, order[k][0]);
            int disp = np_json_get(json, t, dev, order[k][1]);
            int rep = np_json_get(json, t, disp, orient[o]);
            if (rep >= 0)
                parse_rep(&c, rep, d->gba, &d->rep[o]);
        }
    if (!d->rep[0].present && !d->rep[1].present) {
        *why = "the skin has no usable portrait or landscape layout";
        goto done;
    }
    if (!d->name[0])
        strcpy(d->name, "Unnamed skin");
    rc = 0;
done:
    free(t);
    return rc;
}

int np_skin_item_at(const np_skin_rep *r, float x, float y)
{
    int best = -1;
    float area = 0;
    for (int i = 0; i < r->nitems; i++) {
        const np_skin_rect *h = &r->item[i].hit;
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h && (best < 0 || h->w * h->h < area)) {
            best = i;
            area = h->w * h->h;
        }
    }
    return best;
}

uint16_t np_skin_dpad_keys(const np_skin_item *it, float x, float y)
{
    float dx = (x - (it->frame.x + it->frame.w * 0.5f)) / it->frame.w;
    float dy = (y - (it->frame.y + it->frame.h * 0.5f)) / it->frame.h;
    float d = sqrtf(dx * dx + dy * dy);
    uint16_t keys = 0;
    if (d < 0.1f)
        return 0; /* the centre fifth is neutral */
    if (dx < -0.4f * d)
        keys |= NP_KEY_LEFT;
    if (dx > 0.4f * d)
        keys |= NP_KEY_RIGHT;
    if (dy < -0.4f * d)
        keys |= NP_KEY_UP;
    if (dy > 0.4f * d)
        keys |= NP_KEY_DOWN;
    return keys;
}
