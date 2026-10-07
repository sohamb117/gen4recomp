/*
 * Delta emulator controller skins (".deltaskin": a zip with info.json and
 * the art) for the DS and the GBA. The format, as Delta documents it: per
 * device ("iphone", "ipad") and display type ("edgeToEdge", "standard",
 * "splitView") there are "portrait" and "landscape" representations, each
 * with art (`assets`: a "resizable" PDF or "small"/"medium"/"large" PNGs),
 * `items` (an input or d-pad over a frame, optionally with extendedEdges),
 * `screens` (which part of the game's output, `inputFrame` in 256x384 DS
 * or 240x160 GBA space, goes where, `outputFrame`), all in `mappingSize`
 * points.
 *
 * A desktop window has no device class, so the first representation found
 * in the order iphone/edgeToEdge, iphone/standard, ipad/standard, ipad/
 * splitView is used for each orientation. SDL-free so parsing is
 * unit-tested; skin.c loads the art and draws.
 */
#ifndef NP_SKINFMT_H
#define NP_SKINFMT_H

#include <stddef.h>
#include <stdint.h>

#define NP_SKIN_GAME_TYPE "com.rileytestut.delta.game.ds"
#define NP_SKIN_GAME_TYPE_GBA "com.rileytestut.delta.game.gba"
#define NP_SKIN_MAX_ITEMS 40

typedef enum np_skin_action {
    NP_SKIN_KEYS,         /* `keys` (NP_KEY_*) while held */
    NP_SKIN_DPAD,         /* direction from the touch's position */
    NP_SKIN_MENU,         /* opens Options */
    NP_SKIN_FF_HOLD,      /* fastForward */
    NP_SKIN_FF_TOGGLE,    /* toggleFastForward */
    NP_SKIN_QUICK_SAVE,   /* quickSave */
    NP_SKIN_QUICK_LOAD,   /* quickLoad */
    NP_SKIN_TOUCH_SCREEN, /* the DS touch screen: left to the stylus */
} np_skin_action;

typedef struct np_skin_rect {
    float x, y, w, h;
} np_skin_rect;

typedef struct np_skin_item {
    np_skin_action action;
    uint16_t keys;
    np_skin_rect frame; /* the art's frame */
    np_skin_rect hit;   /* frame grown by extendedEdges */
} np_skin_item;

typedef struct np_skin_screen {
    np_skin_rect input;  /* part of the output: 256x384 DS (top above bottom) or 240x160 GBA */
    np_skin_rect output; /* where it goes */
} np_skin_screen;

typedef struct np_skin_rep {
    int present;
    char asset[128];  /* file in the skin: preferred PNG, else the PDF */
    int asset_is_pdf;
    float map_w, map_h; /* mappingSize */
    int translucent;
    int nitems, nscreens;
    np_skin_item item[NP_SKIN_MAX_ITEMS];
    np_skin_screen screen[2];
} np_skin_rep;

typedef struct np_skin_def {
    char name[96];
    char identifier[128];
    int gba;            /* a GBA skin (NP_SKIN_GAME_TYPE_GBA), else a DS one */
    np_skin_rep rep[2]; /* [0] landscape, [1] portrait */
} np_skin_def;

/* Parses info.json. Returns 0, or -1 with *why (not JSON, not a DS skin, no
 * usable representation, an asset name that is not a plain file name). */
int np_skin_parse(const char *json, size_t len, np_skin_def *d, const char **why);

/* The item whose hit area contains (x, y) in mapping points, preferring the
 * smallest; -1 if none. */
int np_skin_item_at(const np_skin_rep *r, float x, float y);

/* Keys for a touch at (x, y) on d-pad item `it` (8-way from its centre). */
uint16_t np_skin_dpad_keys(const np_skin_item *it, float x, float y);

#endif
