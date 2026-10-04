/*
 * On-screen control layouts: where each touch control sits, how big it is
 * and how opaque, for one window orientation. Positions are fractions of
 * the window's width and height and sizes fractions of its short side, so
 * a layout survives resizing and scales with the device; landscape and
 * portrait each have their own. The defaults reproduce the built-in
 * arrangement for any window size.
 *
 * SDL-free so hit-testing, defaults and the file format are unit-tested;
 * touchpad.c draws and edits layouts.
 */
#ifndef NP_TOUCHLAYOUT_H
#define NP_TOUCHLAYOUT_H

#include <stddef.h>
#include <stdint.h>

typedef enum np_tc_id {
    NP_TC_DPAD,
    NP_TC_A,
    NP_TC_B,
    NP_TC_X,
    NP_TC_Y,
    NP_TC_L,
    NP_TC_R,
    NP_TC_SELECT,
    NP_TC_START,
    NP_TC_FF,
    NP_TC_MENU,
    NP_TC_COUNT
} np_tc_id;

typedef struct np_tc_item {
    float cx, cy;   /* centre: fractions of window width / height */
    float w, h;     /* size: fractions of the window's short side */
    float opacity;  /* 0.1 .. 1 */
} np_tc_item;

typedef struct np_tc_layout {
    np_tc_item item[NP_TC_COUNT];
} np_tc_layout;

extern const char *const np_tc_ids[NP_TC_COUNT];   /* "dpad", "a", ... (file keys) */
extern const char *const np_tc_names[NP_TC_COUNT]; /* "D-pad", "A", ... */

/* D-pad and face buttons are round (hit and drawn as the inscribed circle). */
int np_tc_round(int id);

/* The built-in arrangement for a W x H window. */
void np_tc_default(np_tc_layout *l, float W, float H);

/* Item rectangle in window pixels: x, y, w, h. */
void np_tc_rect(const np_tc_item *it, float W, float H, float r[4]);

/* Keys held by a touch at (x, y); *ff / *menu for those buttons. Returns 1
 * when the touch is on any control. */
int np_tc_hit(const np_tc_layout *l, float W, float H, float x, float y, uint16_t *keys, int *ff, int *menu);

/* The control under (x, y) for the editor (smallest first), or -1. */
int np_tc_pick(const np_tc_layout *l, float W, float H, float x, float y);

/* Keeps an item usable: size 0.05..1 of the short side, centre on screen,
 * opacity 0.1..1. */
void np_tc_clamp(np_tc_item *it);

/* "dpad = cx cy w h opacity" lines. Returns the length (as snprintf). */
int np_tc_format(const np_tc_layout *l, char *out, size_t n);
/* Applies one `key = value` line; returns 0, or -1 for an unknown key or a
 * malformed value (the layout is unchanged). */
int np_tc_parse(np_tc_layout *l, const char *key, const char *value);

#endif
