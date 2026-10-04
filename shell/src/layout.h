/*
 * Screen layout: where the two DS screens go in the window, and the inverse
 * mapping from a window point back to a bottom-screen (stylus) pixel. Kept
 * free of SDL so the touch mapping can be unit-tested for every layout,
 * rotation, swap and scale mode.
 *
 * Model: the screens are first arranged in "content space" (DS pixel units,
 * unrotated), the content box is rotated by quarter turns clockwise, then
 * scaled and centred in the window. Drawing uses each screen's centre,
 * unrotated size and the rotation angle, which is exactly what
 * SDL_RenderTextureRotated takes.
 */
#ifndef NP_LAYOUT_H
#define NP_LAYOUT_H

typedef enum np_layout_mode {
    NP_LAYOUT_VERTICAL,    /* top above bottom */
    NP_LAYOUT_HORIZONTAL,  /* side by side */
    NP_LAYOUT_HYBRID,      /* large primary screen, small secondary at its lower right */
    NP_LAYOUT_TOP_ONLY,
    NP_LAYOUT_BOTTOM_ONLY,
    NP_LAYOUT_COUNT
} np_layout_mode;

typedef enum np_scale_mode {
    NP_SCALE_FIT,     /* largest scale that fits, fractional allowed */
    NP_SCALE_INTEGER, /* largest whole-number scale; falls back to fit below 1x */
    NP_SCALE_COUNT
} np_scale_mode;

typedef struct np_layout_params {
    np_layout_mode mode;
    int swap;     /* exchange the screens' places (ignored by single-screen modes) */
    int rotation; /* quarter turns clockwise, 0..3 */
    np_scale_mode scale;
    /* Width of each screen in DS pixel units: 256, or more when the core
     * renders widescreen with the DS picture centred (0 = 256). */
    int screen_w;
    /* With a controller skin: window rectangles {x, y, w, h} for the top
     * ([0..3]) and bottom ([4..7]) screen, each fitted inside its own
     * rectangle at the screens' aspect; a width of 0 hides that screen.
     * mode, swap, rotation and scale are then ignored. NULL otherwise. */
    const float *frames;
} np_layout_params;

typedef struct np_screen_place {
    int visible;
    float cx, cy;         /* centre in window pixels */
    float w, h;           /* drawn size before rotation */
    float bx, by, bw, bh; /* window-space bounding box after rotation */
    /* Placement in content space, for the inverse mapping. */
    float content_x, content_y, content_scale;
} np_screen_place;

typedef struct np_layout {
    np_screen_place screen[2]; /* [0] top, [1] bottom */
    int rotation;              /* quarter turns clockwise */
    float scale;               /* window pixels per content unit */
    float origin_x, origin_y;  /* top-left of the rotated content box */
    float content_w, content_h;
    float screen_w;            /* per screen, DS pixel units */
} np_layout;

void np_layout_compute(np_layout *l, const np_layout_params *p, float win_w, float win_h);

/*
 * Maps window point (wx, wy) to a bottom-screen pixel. Returns 1 and writes
 * (tx, ty) in 0..255 x 0..191 when the point is on the DS picture of the
 * bottom screen (wide screens' side bars are not). With `clamp` set, points
 * off it are clamped to its nearest edge pixel (a stylus dragged past the
 * edge stays down), and 1 is returned whenever the bottom screen is
 * visible. Returns 0 otherwise.
 */
int np_layout_touch(const np_layout *l, float wx, float wy, int clamp, int *tx, int *ty);

#endif
