/*
 * pcview, the native SDL2 viewer for the pokeplatinum PC port.
 *
 * A separate 64-bit process on purpose: the port is -m32 and the host's
 * SDL2 is 64-bit ELF, so the two meet in a shared-memory page instead of a
 * link (pc/include/pc_view.h has the protocol and the reasoning). This is
 * pokediamond's pcview brought to feature parity over platinum's simpler
 * fixed-size channel: layouts, enhancement scalers, aspect/integer scaling,
 * a gamepad, --shot for headless verification and --selftest for the
 * properties none of that may quietly lose.
 *
 *   $ PC_VIEW=pokeplatinum ./build/pc/pokeplatinum &
 *   $ ./build/pc/pcview pokeplatinum
 *
 * or just ./pc/play.sh, which does both and cleans up.
 *
 * Keys: arrows = D-pad, X = a, z = b, s = x, a = y, q = l, w = R,
 * Return = Start, Backspace = Select, Esc quits. Tab held fast-forwards
 * (the port skips its pacer while it is down). F11 or Alt+Enter toggles
 * fullscreen; F12 writes a screenshot beside the current directory. The
 * mouse on the lower screen is the pen; leaving the lower screen lifts it.
 * A gamepad maps by POSITION, not by letter: SDL's A, the bottom face
 * button, is the DS's B, because a DS has X at the top and the Xbox layout
 * has Y there, and matching letters would put confirm where cancel is.
 *
 * Build: make -f pc/Makefile pcview (native cc + sdl2-config, no -m32).
 */

#include <SDL.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>            /* the viewer has no SDK headers to clash with */
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "../include/pc_view.h"
#include "../src/pc_png.h"
#include "pc_window_icon.h"

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

enum view_filter {
    VIEW_FILTER_NEAREST,
    VIEW_FILTER_LINEAR,
    VIEW_FILTER_SCALE2X
};

/*
 * The smart layout's two sizes, and the ramp between them. At rest the touch
 * screen is half the main screen's height: big enough to read the Poketch,
 * small enough that the game is the thing on the screen. While the game is
 * asking for the pen; which the port knows, because the game starts the
 * SDK's touch sampling when a screen wants it; the two become equal, so a
 * battle's move menu is as readable as the battle.
 *
 * They do not SWAP. In a Pokemon battle the top screen is the fight and the
 * bottom is the menu; making the menu large and the fight small would be the
 * wrong way round. Equal is what "both are wanted" looks like.
 *
 * The ramp is per presented frame, so the change reads as the window making
 * room rather than as a glitch.
 */
#define VIEW_SEC_MIN  50
#define VIEW_SEC_MAX 100
#define VIEW_SEC_STEP  4

enum view_layout {
    VIEW_LAYOUT_SMART,          /* main screen large, touch screen beside it,
                                 * and the touch screen grows to match when
                                 * the game asks for the pen */
    VIEW_LAYOUT_STACKED,        /* top screen above the touch screen */
    VIEW_LAYOUT_WIDE            /* side by side, top screen on the left */
};

enum view_aspect {
    VIEW_ASPECT_NATIVE,         /* 4:3 screens, the DS's own */
    VIEW_ASPECT_STRETCH         /* fill the window, proportions be damned */
};

struct view_opts {
    const char *name;           /* the shm channel, argv[1] or --shm */
    int scale;                  /* initial window size, in DS pixels */
    int layout;
    int rs;                     /* render scale, 1..4 */
    int filter;
    int integer;                /* scale by whole source pixels only */
    int aspect;
    int fullscreen;
    int no_audio;
    const char *audio_device;
    const char *shot;           /* write one composed frame here and exit */
    const char *shots;          /* where F12 writes; NULL means here */
    int wait_ms;                /* how long --shot waits for a frame */
};

/* ------------------------------------------------------------------ */
/* Layout: where the picture lands in the window                       */
/* ------------------------------------------------------------------ */

/*
 * Fit src into win preserving proportions, centred; integer mode snaps to
 * whole multiples when the window is at least source-sized. This is
 * pokediamond's pc_view_layout_wh, carried as code rather than reinvented,
 * because --selftest pins its properties and a second spelling is a second
 * program to disagree with.
 */
struct view_rect { int x, y, w, h; };

static void view_sleep_ms(int ms);      /* defined with the attach helpers */

static struct view_rect view_layout(int src_w, int src_h,
                                    int win_w, int win_h,
                                    int integer, int aspect) {
    struct view_rect r;
    long sw = src_w, sh = src_h;
    int w, h;

    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    if (win_w < 1) win_w = 1;
    if (win_h < 1) win_h = 1;

    if (aspect == VIEW_ASPECT_STRETCH) {
        r.x = 0; r.y = 0; r.w = win_w; r.h = win_h;
        return r;
    }

    if ((long)win_w * sh <= (long)win_h * sw) {      /* width-limited  */
        w = win_w;
        h = (int)(((long)win_w * sh) / sw);
    } else {                                          /* height-limited */
        h = win_h;
        w = (int)(((long)win_h * sw) / sh);
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    if (integer && win_w >= sw && win_h >= sh) {
        int s = (int)(win_w / sw);
        int t = (int)(win_h / sh);

        if (t < s) s = t;
        w = (int)sw * s;
        h = (int)sh * s;
    }

    r.w = w;
    r.h = h;
    r.x = (win_w - w) / 2;
    r.y = (win_h - h) / 2;
    return r;
}

/*
 * The composed picture's size in source pixels, by layout. `src_w` and
 * `src_h` are what the port published this frame, 256x192 natively, wider
 * when it is rendering a wider field of view, and both multiplied when it is
 * rendering at a higher internal resolution, so the window's proportions
 * follow the picture rather than a constant.
 */
/*
 * `sec` is the smart layout's secondary screen, as a percentage of the main
 * one: VIEW_SEC_MIN at rest and VIEW_SEC_MAX while the game is asking for
 * the pen. The other two layouts ignore it.
 */
static void view_composed_wh(int layout, int src_w, int src_h, int sec,
                             int *cw, int *ch) {
    if (src_w < PC_VIEW_W) src_w = PC_VIEW_W;
    if (src_h < PC_VIEW_H) src_h = PC_VIEW_H;
    if (layout == VIEW_LAYOUT_WIDE)        { *cw = src_w * 2; *ch = src_h; }
    else if (layout == VIEW_LAYOUT_SMART)  { *cw = src_w + src_w * sec / 100;
                                             *ch = src_h; }
    else                                   { *cw = src_w;     *ch = src_h * 2; }
}

/*
 * Both screens' destination rectangles for a window of win_w x win_h.
 * dst[0] is the top screen, dst[1] the touch screen; splitting the fitted
 * rectangle rather than fitting each screen keeps the pair glued at every
 * window size, including odd ones; the remainder pixel goes to the second
 * screen, never to a gap between them.
 */
static struct view_rect view_screen_rects(const struct view_opts *o,
                                          int src_w, int src_h, int sec,
                                          int win_w, int win_h,
                                          SDL_Rect dst[2]) {
    int cw, ch;
    struct view_rect r;

    view_composed_wh(o->layout, src_w, src_h, sec, &cw, &ch);
    r = view_layout(cw, ch, win_w, win_h, o->integer, o->aspect);
    if (o->layout == VIEW_LAYOUT_SMART) {
        /* The main screen takes the height; the touch screen keeps the same
         * proportions at `sec` per cent of it and sits centred beside it, so
         * neither is ever stretched. */
        int w0 = (int)(((long)r.w * 100) / (100 + sec));
        int w1 = r.w - w0;
        /* The height comes from the width the touch screen actually got,
         * not from `sec` again: the two roundings disagree by a pixel and
         * the visible cost of that is a touch screen a little out of
         * proportion, where the cost of taking the remainder is nothing. */
        int h1 = w0 > 0 ? (int)(((long)w1 * r.h) / w0) : 0;

        dst[0] = (SDL_Rect){ r.x, r.y, w0, r.h };
        dst[1] = (SDL_Rect){ r.x + w0, r.y + (r.h - h1) / 2, w1, h1 };
    } else if (o->layout == VIEW_LAYOUT_WIDE) {
        int w0 = r.w / 2;

        dst[0] = (SDL_Rect){ r.x,      r.y, w0,       r.h };
        dst[1] = (SDL_Rect){ r.x + w0, r.y, r.w - w0, r.h };
    } else {
        int h0 = r.h / 2;

        dst[0] = (SDL_Rect){ r.x, r.y,      r.w, h0       };
        dst[1] = (SDL_Rect){ r.x, r.y + h0, r.w, r.h - h0 };
    }
    return r;
}

/*
 * The mouse, inverted through the same layout: window coordinates to a
 * touch-screen pixel, or -1 when the pointer is not on the touch screen.
 * The pen LIFTS off the edge of the lower screen rather than dragging along
 * it; the upper screen has no panel on it and neither does the letterbox.
 *
 * A wide frame carries the same 256-column panel with margins beside it,
 * and the panel is the only part a stylus can reach: the hardware's touch
 * coordinates are 8 bits over a 256-pixel screen, so a margin is off the
 * digitizer in the same way the letterbox is. The pen lifts there too.
 */
static int view_window_to_touch(const struct view_opts *o,
                                int src_w, int src_h, int sec,
                                int win_w, int win_h,
                                int mx, int my, int *tx, int *ty) {
    SDL_Rect dst[2];
    int x, y, margin, hds;

    if (src_w < PC_VIEW_W) src_w = PC_VIEW_W;
    if (src_h < PC_VIEW_H) src_h = PC_VIEW_H;
    /* The internal resolution multiplied both axes, so the published width
     * is in rendered pixels and the panel arithmetic below is in the DS's.
     * The height is what says by how much. */
    hds = src_h / PC_VIEW_H;
    if (hds < 1) hds = 1;
    src_w /= hds;
    margin = (src_w - PC_VIEW_W) / 2;
    view_screen_rects(o, src_w * hds, src_h, sec, win_w, win_h, dst);
    if (dst[1].w < 1 || dst[1].h < 1) return -1;
    if (mx < dst[1].x || mx >= dst[1].x + dst[1].w ||
        my < dst[1].y || my >= dst[1].y + dst[1].h)
        return -1;
    x = (int)(((long)(mx - dst[1].x) * src_w) / dst[1].w) - margin;
    y = (int)(((long)(my - dst[1].y) * PC_VIEW_H) / dst[1].h);
    if (x < 0 || x >= PC_VIEW_W) return -1;   /* on a margin: no panel */
    if (x < 0) x = 0;
    if (x >= PC_VIEW_W) x = PC_VIEW_W - 1;
    if (y < 0) y = 0;
    if (y >= PC_VIEW_H) y = PC_VIEW_H - 1;
    *tx = x;
    *ty = y;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Render resolution: the enhancement scalers                          */
/* ------------------------------------------------------------------ */

/*
 * --render-scale N draws each screen from an N-times-larger texture, and
 * --filter says how those pixels are made: nearest replicates (crisp),
 * linear over a replicated texture is sharp-bilinear (square pixel edges at
 * any window size), scale2x is EPX edge smoothing, never inventing a
 * colour the source does not hold. These are enhancements with no console
 * pixel to compare against, so --selftest pins the properties the
 * algorithms themselves state instead: sizes, block equality, identity on
 * flat colour, no invented colours. (pokediamond's scalers, carried whole.)
 */
static void view_replicate(const uint32_t *s, int w, int h,
                           uint32_t *d, int f) {
    int x, y, i, j;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t p = s[(size_t)y * w + x];

            for (j = 0; j < f; j++) {
                uint32_t *row = d + ((size_t)y * f + j) * ((size_t)w * f);

                for (i = 0; i < f; i++) row[(size_t)x * f + i] = p;
            }
        }
    }
}

/* EPX / Scale2x: a corner leans toward a neighbour only where the two
 * adjacent neighbours agree and the two opposite ones do not. */
static void view_scale2x(const uint32_t *s, int w, int h, uint32_t *d) {
    int x, y;

    for (y = 0; y < h; y++) {
        const uint32_t *row = s + (size_t)y * w;
        const uint32_t *up  = y > 0     ? row - w : row;
        const uint32_t *dn  = y + 1 < h ? row + w : row;
        uint32_t *o0 = d + (size_t)(2 * y) * (2 * w);
        uint32_t *o1 = o0 + (size_t)(2 * w);

        for (x = 0; x < w; x++) {
            uint32_t P = row[x];
            uint32_t A = up[x];                      /* above */
            uint32_t B = x + 1 < w ? row[x + 1] : P; /* right */
            uint32_t C = x > 0     ? row[x - 1] : P; /* left  */
            uint32_t D = dn[x];                      /* below */

            o0[2 * x]     = (C == A && C != D && A != B) ? A : P;
            o0[2 * x + 1] = (A == B && A != C && B != D) ? B : P;
            o1[2 * x]     = (D == C && D != B && C != A) ? C : P;
            o1[2 * x + 1] = (B == D && B != A && D != C) ? D : P;
        }
    }
}

/* AdvMAME3x, the 3x member of the same family. */
static void view_scale3x(const uint32_t *s, int w, int h, uint32_t *d) {
    int x, y;

    for (y = 0; y < h; y++) {
        const uint32_t *row = s + (size_t)y * w;
        const uint32_t *up  = y > 0     ? row - w : row;
        const uint32_t *dn  = y + 1 < h ? row + w : row;
        uint32_t *o0 = d + (size_t)(3 * y) * (3 * w);
        uint32_t *o1 = o0 + (size_t)(3 * w);
        uint32_t *o2 = o1 + (size_t)(3 * w);

        for (x = 0; x < w; x++) {
            int xl = x > 0 ? x - 1 : x, xr = x + 1 < w ? x + 1 : x;
            uint32_t A = up[xl],  B = up[x],  C = up[xr];
            uint32_t D = row[xl], E = row[x], F = row[xr];
            uint32_t G = dn[xl],  H = dn[x],  I = dn[xr];

            o0[3 * x]     = (D == B && B != F && D != H) ? D : E;
            o0[3 * x + 1] = ((D == B && B != F && D != H && E != C) ||
                             (B == F && B != D && F != H && E != A)) ? B : E;
            o0[3 * x + 2] = (B == F && B != D && F != H) ? F : E;
            o1[3 * x]     = ((D == B && B != F && D != H && E != G) ||
                             (D == H && D != B && H != F && E != A)) ? D : E;
            o1[3 * x + 1] = E;
            o1[3 * x + 2] = ((B == F && B != D && F != H && E != I) ||
                             (H == F && D != H && B != F && E != C)) ? F : E;
            o2[3 * x]     = (D == H && D != B && H != F) ? D : E;
            o2[3 * x + 1] = ((D == H && D != B && H != F && E != I) ||
                             (H == F && D != H && B != F && E != G)) ? H : E;
            o2[3 * x + 2] = (H == F && D != H && B != F) ? F : E;
        }
    }
}

/* src (w x h) into dst (w*rs x h*rs); mid is scratch for rs==4 with
 * scale2x. rs==1 is a copy so every caller can texture from dst. */
static void view_upscale(const uint32_t *src, int w, int h,
                         uint32_t *dst, uint32_t *mid, int rs, int filter) {
    if (rs <= 1) {
        memcpy(dst, src, (size_t)w * h * sizeof *src);
        return;
    }
    if (filter == VIEW_FILTER_SCALE2X) {
        if (rs == 2) { view_scale2x(src, w, h, dst); return; }
        if (rs == 3) { view_scale3x(src, w, h, dst); return; }
        if (rs == 4 && mid != NULL) {
            view_scale2x(src, w, h, mid);
            view_scale2x(mid, 2 * w, 2 * h, dst);
            return;
        }
    }
    view_replicate(src, w, h, dst, rs);
}

/* ------------------------------------------------------------------ */
/* Input: the keyboard and the gamepad                                 */
/* ------------------------------------------------------------------ */

/*
 * By scancode, so the physical key survives a layout that moves Z and Y;
 * and read as a LEVEL out of SDL_GetKeyboardState() rather than tracked
 * through events, because a level is what the channel carries and a missed
 * release in an event-tracked mask is a button stuck down. (Diamond's
 * lesson; the first platinum viewer tracked events.)
 */
struct view_keymap { SDL_Scancode sc; uint32_t pad; };

/*
 * The keyboard map is data, not a constant, so a player who wants A on a
 * different key does not edit C. `--bind PAD=KEY,...` replaces individual
 * rows; everything not named keeps the value below, so the defaults are
 * exactly what they always were and a viewer started with no --bind is the
 * viewer this port shipped.
 *
 * BY SCANCODE, like the defaults: a scancode is a physical key, so a binding
 * survives the player switching keyboard layout; which is the whole reason
 * the defaults were scancodes in the first place. The names accepted are
 * SDL's own (SDL_GetScancodeFromName), so "Left Shift", "Keypad 5" and "["
 * all work and nothing here has to carry a table of them.
 */
static struct view_keymap KEYMAP[] = {
    { SDL_SCANCODE_UP,        PC_VIEW_KEY_UP     },
    { SDL_SCANCODE_DOWN,      PC_VIEW_KEY_DOWN   },
    { SDL_SCANCODE_LEFT,      PC_VIEW_KEY_LEFT   },
    { SDL_SCANCODE_RIGHT,     PC_VIEW_KEY_RIGHT  },
    { SDL_SCANCODE_X,         PC_VIEW_KEY_A      },
    { SDL_SCANCODE_Z,         PC_VIEW_KEY_B      },
    { SDL_SCANCODE_S,         PC_VIEW_KEY_X      },
    { SDL_SCANCODE_A,         PC_VIEW_KEY_Y      },
    { SDL_SCANCODE_Q,         PC_VIEW_KEY_L      },
    { SDL_SCANCODE_W,         PC_VIEW_KEY_R      },
    { SDL_SCANCODE_RETURN,    PC_VIEW_KEY_START  },
    { SDL_SCANCODE_BACKSPACE, PC_VIEW_KEY_SELECT },
};

/*
 * The gamepad, positionally. A DS has X at the top and the Xbox layout SDL
 * names buttons for has Y there; matching letters would put confirm where
 * cancel is, so the mapping matches thumb positions instead.
 */
struct view_padmap { SDL_GameControllerButton button; uint32_t pad; };

static const struct view_padmap PADMAP[] = {
    { SDL_CONTROLLER_BUTTON_A,             PC_VIEW_KEY_B      },
    { SDL_CONTROLLER_BUTTON_B,             PC_VIEW_KEY_A      },
    { SDL_CONTROLLER_BUTTON_X,             PC_VIEW_KEY_Y      },
    { SDL_CONTROLLER_BUTTON_Y,             PC_VIEW_KEY_X      },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  PC_VIEW_KEY_L      },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PC_VIEW_KEY_R      },
    { SDL_CONTROLLER_BUTTON_START,         PC_VIEW_KEY_START  },
    { SDL_CONTROLLER_BUTTON_BACK,          PC_VIEW_KEY_SELECT },
    { SDL_CONTROLLER_BUTTON_DPAD_UP,       PC_VIEW_KEY_UP     },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN,     PC_VIEW_KEY_DOWN   },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT,     PC_VIEW_KEY_LEFT   },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    PC_VIEW_KEY_RIGHT  },
};

#define VIEW_PADMAP_N ((int)(sizeof PADMAP / sizeof PADMAP[0]))

/* A left stick reported as the d-pad past a deadzone. No oracle exists , 
 * a DS has no stick, so the number is a preference, written once here. */
#define VIEW_STICK_DEADZONE 12000

/* The cushion playback starts on: 188 ms. */
/* PCVIEW_AUDIO_DEBUG's ring of per-callback records. */
#define VIEW_AUDIO_LOG 8192u
struct view_audio_rec { uint32_t ms, want, got, have; };

#define VIEW_AUDIO_CUSHION 6144u

#define VIEW_KEYMAP_N ((int)(sizeof KEYMAP / sizeof KEYMAP[0]))

/* The pad bits by name, for --bind. The order is the DS's own button order
 * rather than alphabetical, because that is how a player thinks about them. */
static const struct { const char *name; uint32_t pad; } PADNAMES[] = {
    { "a", PC_VIEW_KEY_A }, { "b", PC_VIEW_KEY_B },
    { "x", PC_VIEW_KEY_X }, { "y", PC_VIEW_KEY_Y },
    { "l", PC_VIEW_KEY_L }, { "r", PC_VIEW_KEY_R },
    { "start", PC_VIEW_KEY_START }, { "select", PC_VIEW_KEY_SELECT },
    { "up", PC_VIEW_KEY_UP }, { "down", PC_VIEW_KEY_DOWN },
    { "left", PC_VIEW_KEY_LEFT }, { "right", PC_VIEW_KEY_RIGHT },
};

/*
 * `--bind a=k,start=return`. Returns 0, or -1 having said what it did not
 * understand: a binding silently ignored is a key that does nothing and a
 * player with no way to find out why.
 */
static int view_bind(const char *spec) {
    char buf[512], *p;

    snprintf(buf, sizeof buf, "%s", spec);
    for (p = strtok(buf, ","); p != NULL; p = strtok(NULL, ",")) {
        char *eq = strchr(p, '=');
        SDL_Scancode sc;
        int i, found = -1;

        if (eq == NULL) {
            fprintf(stderr, "pcview: --bind wants PAD=KEY, got '%s'\n", p);
            return -1;
        }
        *eq = '\0';
        for (i = 0; i < (int)(sizeof PADNAMES / sizeof PADNAMES[0]); i++) {
            if (SDL_strcasecmp(p, PADNAMES[i].name) == 0) found = i;
        }
        if (found < 0) {
            fprintf(stderr, "pcview: --bind: no button named '%s'\n", p);
            return -1;
        }
        sc = SDL_GetScancodeFromName(eq + 1);
        if (sc == SDL_SCANCODE_UNKNOWN) {
            fprintf(stderr, "pcview: --bind: no key named '%s' (SDL's names:"
                            " a..z, Return, Space, Left Shift, Keypad 5)\n",
                    eq + 1);
            return -1;
        }
        for (i = 0; i < VIEW_KEYMAP_N; i++) {
            if (KEYMAP[i].pad == PADNAMES[found].pad) KEYMAP[i].sc = sc;
        }
    }
    return 0;
}

static uint32_t view_keys_to_pad(const Uint8 *state, int nkeys, int alt_held) {
    uint32_t m = 0;
    int i;

    if (state == NULL) return 0;
    for (i = 0; i < VIEW_KEYMAP_N; i++) {
        if ((int)KEYMAP[i].sc >= nkeys || !state[KEYMAP[i].sc]) continue;
        /* Alt+Enter is the fullscreen chord; it must not leak Start. */
        if (alt_held && KEYMAP[i].sc == SDL_SCANCODE_RETURN) continue;
        m |= KEYMAP[i].pad;
    }
    return m;
}

static uint32_t view_axes_to_pad(int lx, int ly) {
    uint32_t m = 0;

    if (lx <= -VIEW_STICK_DEADZONE) m |= PC_VIEW_KEY_LEFT;
    if (lx >=  VIEW_STICK_DEADZONE) m |= PC_VIEW_KEY_RIGHT;
    if (ly <= -VIEW_STICK_DEADZONE) m |= PC_VIEW_KEY_UP;
    if (ly >=  VIEW_STICK_DEADZONE) m |= PC_VIEW_KEY_DOWN;
    return m;
}

static uint32_t view_controller_to_pad(SDL_GameController *gc) {
    uint32_t m = 0;
    int i;

    if (gc == NULL) return 0;
    for (i = 0; i < VIEW_PADMAP_N; i++) {
        if (SDL_GameControllerGetButton(gc, PADMAP[i].button)) m |= PADMAP[i].pad;
    }
    return m | view_axes_to_pad(
        SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX),
        SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
}

/* ------------------------------------------------------------------ */
/* Sound                                                               */
/* ------------------------------------------------------------------ */

/*
 * The port mixes into the shared ring; this end hands it to SDL. The
 * callback must ALWAYS fill its buffer: a run momentarily behind gets
 * silence, not a fragment of the last second. The tail is ours; the port
 * never reads it, so a viewer stopped in a debugger costs the port nothing.
 */
struct view_audio {
    const struct pc_view_shm *v;
    uint32_t tail;
    uint64_t dropped;
    uint64_t starved;
    uint64_t calls;
    struct view_audio_rec *log;     /* PCVIEW_AUDIO_DEBUG, else NULL */
    uint64_t nlog;
};

static void view_audio_cb(void *arg, Uint8 *stream, int len)
{
    struct view_audio *a = (struct view_audio *)arg;
    unsigned want = (unsigned)len / (2 * sizeof(int16_t));
    uint32_t lost = 0;
    unsigned got;

    got = pc_view_audio_read(a->v, &a->tail, (int16_t *)stream, want, &lost);
    a->dropped += lost;
    a->calls++;
    /*
     * PCVIEW_AUDIO_DEBUG records every callback's shortfall and the ring
     * level it saw, so a complaint about the sound cutting out can be read
     * as a SHAPE rather than as a total. A total cannot tell one long gap
     * from a hundred short ones, and those have different causes. Written
     * here and printed from the main loop, because a callback that does
     * I/O causes the very fault it is measuring.
     */
    if (a->log != NULL) {
        struct view_audio_rec *r = &a->log[a->nlog % VIEW_AUDIO_LOG];

        r->ms   = SDL_GetTicks();
        r->want = want;
        r->got  = got;
        r->have = a->v->audio_head - a->tail;
        a->nlog++;
    }
    if (got < want) {
        a->starved += want - got;
        memset(stream + got * 2 * sizeof(int16_t), 0,
               (size_t)(want - got) * 2 * sizeof(int16_t));
    }
}

/*
 * Opened at the guest's own rate with no SDL resampling allowed; a device
 * that cannot take 32,728 Hz fails and we retry letting SDL pick, the one
 * case where it resamples. Failure is never fatal: a window with no sound
 * beats no window. Returned paused, started once the ring holds a cushion.
 */
static SDL_AudioDeviceID view_audio_open(struct view_audio *a,
                                         const char *device)
{
    SDL_AudioSpec want, have;
    SDL_AudioDeviceID dev;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "pcview: no audio here (%s); the picture still "
                        "works.\n", SDL_GetError());
        return 0;
    }

    SDL_memset(&want, 0, sizeof want);
    want.freq     = (int)(a->v->audio_rate ? a->v->audio_rate
                                           : PC_VIEW_AUDIO_RATE);
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    /* 1024 frames is 31 ms at this rate: short enough to stay in step
     * with the picture, long enough to ride a scheduling hiccup. */
    want.samples  = 1024;
    want.callback = view_audio_cb;
    want.userdata = a;

    dev = SDL_OpenAudioDevice(device, 0, &want, &have, 0);
    if (dev == 0) {
        dev = SDL_OpenAudioDevice(device, 0, &want, &have,
                                  SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    }
    if (dev == 0) {
        fprintf(stderr, "pcview: cannot open an audio device (%s); the "
                        "picture still works.\n", SDL_GetError());
        return 0;
    }
    fprintf(stderr, "pcview: audio at %d Hz%s%s\n", have.freq,
            device ? " on " : "", device ? device : "");
    return dev;
}

static void view_list_audio_devices(void) {
    int i, n;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "pcview: SDL audio: %s\n", SDL_GetError());
        return;
    }
    n = SDL_GetNumAudioDevices(0);
    for (i = 0; i < n; i++)
        printf("%s\n", SDL_GetAudioDeviceName(i, 0));
}

/* ------------------------------------------------------------------ */
/* Screenshots                                                         */
/* ------------------------------------------------------------------ */

/*
 * The presented frame, at the renderer's real output size, NOT the logical
 * one; the first draft sized the surface from logical values and segfaulted
 * 4x past the end. That size is the point: a screenshot at the window's
 * resolution is the picture the player is looking at, upscaler and all,
 * where a 256x384 dump would be the picture before the viewer touched it.
 *
 * The format follows the extension. PNG is what a player wants and what F12
 * writes; PPM stays for --shot, where the test that reads it wants three
 * lines and the bytes rather than a decoder.
 */
static int view_write_shot(SDL_Renderer *ren, const char *path) {
    int ow = 0, oh = 0, y, x;
    size_t n = strlen(path);
    int png = n > 4 && strcmp(path + n - 4, ".png") == 0;
    SDL_Surface *sf;
    unsigned char *raw = NULL, *z = NULL;
    size_t rawn = 0, zcap = 0, o = 0;
    FILE *f = NULL;

    SDL_GetRendererOutputSize(ren, &ow, &oh);
    sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32,
                                        SDL_PIXELFORMAT_XRGB8888);
    if (sf == NULL ||
        SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_XRGB8888,
                             sf->pixels, sf->pitch) != 0) {
        fprintf(stderr, "pcview: readback failed: %s\n", SDL_GetError());
        if (sf != NULL) SDL_FreeSurface(sf);
        return -1;
    }

    if (png) {
        rawn = (size_t)oh * (1 + (size_t)ow * 3);
        zcap = rawn + rawn / 2 + 64;
        raw = (unsigned char *)malloc(rawn);
        z = (unsigned char *)malloc(zcap);
        if (raw == NULL || z == NULL) {
            fprintf(stderr, "pcview: no memory for a %dx%d screenshot\n",
                    ow, oh);
            free(raw); free(z); SDL_FreeSurface(sf);
            return -1;
        }
    } else {
        f = fopen(path, "wb");
        if (f == NULL) {
            fprintf(stderr, "pcview: %s: %s\n", path, strerror(errno));
            SDL_FreeSurface(sf);
            return -1;
        }
        fprintf(f, "P6\n%d %d\n255\n", ow, oh);
    }

    for (y = 0; y < oh; y++) {
        const uint32_t *row = (const uint32_t *)((char *)sf->pixels
                                                 + (size_t)y * sf->pitch);

        if (png) raw[o++] = 0;          /* filter type 0 */
        for (x = 0; x < ow; x++) {
            unsigned char rgb[3] = { (row[x] >> 16) & 0xFF,
                                     (row[x] >> 8) & 0xFF,
                                     row[x] & 0xFF };
            if (png) {
                raw[o++] = rgb[0]; raw[o++] = rgb[1]; raw[o++] = rgb[2];
            } else {
                fwrite(rgb, 1, 3, f);
            }
        }
    }

    if (png) {
        int ok = pc_png_write_raw(path, (unsigned)ow, (unsigned)oh,
                                  raw, rawn, z, zcap, "pcview");
        free(raw); free(z);
        SDL_FreeSurface(sf);
        if (!ok) return -1;
    } else {
        fclose(f);
        SDL_FreeSurface(sf);
    }
    fprintf(stderr, "pcview: wrote %s\n", path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* The viewer                                                          */
/* ------------------------------------------------------------------ */

struct viewer {
    struct pc_view_shm *shm;
    int shm_fd;                     /* POSIX: kept open, fstat detects the unlink */
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex[2];            /* [0] top screen, [1] touch screen */
    int vsync;
    int src_w;                      /* the last frame's published width  */
    int src_h;                      /* and its height: internal resolution
                                     * multiplies both                    */
    int sec;                        /* the smart layout's touch screen, as a
                                     * percentage of the main one          */
    uint64_t torn;                  /* frames that could not be read whole */
    uint64_t presented;             /* frames that could */
    uint32_t *scaled[2];            /* per-screen upscale buffers */
    uint32_t *mid;                  /* scale2x rs==4 scratch */
};

static int viewer_open_video(struct viewer *vw, const struct view_opts *o) {
    int cw, ch, i;

    /* The channel already carries a width, whether or not a frame has been
     * published yet, so a port that starts wide opens a window shaped for
     * the picture instead of resizing itself in front of the user. */
    /* The smart layout starts at rest, so the window opens shaped for the
     * layout it is about to draw rather than for one with no touch screen
     * in it. */
    vw->sec = VIEW_SEC_MIN;
    vw->src_w = vw->shm != NULL ? (int)vw->shm->width : PC_VIEW_W;
    vw->src_h = vw->shm != NULL ? (int)vw->shm->height : PC_VIEW_H;
    if (vw->src_w < PC_VIEW_W) vw->src_w = PC_VIEW_W;
    if (vw->src_w > (int)(PC_VIEW_WIDE_MAX * PC_VIEW_HD_MAX))
        vw->src_w = (int)(PC_VIEW_WIDE_MAX * PC_VIEW_HD_MAX);
    if (vw->src_h < PC_VIEW_H) vw->src_h = PC_VIEW_H;
    if (vw->src_h > (int)(PC_VIEW_H * PC_VIEW_HD_MAX))
        vw->src_h = (int)(PC_VIEW_H * PC_VIEW_HD_MAX);
    view_composed_wh(o->layout, vw->src_w, vw->src_h, vw->sec,
                     &cw, &ch);
    vw->win = SDL_CreateWindow("Pok\xc3\xa9mon Platinum",
                               SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                               cw * o->scale, ch * o->scale,
                               SDL_WINDOW_RESIZABLE);
    if (vw->win == NULL) {
        fprintf(stderr, "pcview: cannot open a window: %s\n", SDL_GetError());
        return -1;
    }
    pc_set_window_icon(vw->win);
    if (o->fullscreen)
        SDL_SetWindowFullscreen(vw->win, SDL_WINDOW_FULLSCREEN_DESKTOP);

    /*
     * Ask for accelerated+vsync, fall back to software, then CHECK what was
     * granted. `SDL_CreateRenderer(win, -1, PRESENTVSYNC)` names a property
     * without naming a driver, and under WSLg SDL is free to hand back a
     * renderer that ignores it, the first platinum viewer never asked, so
     * its loop ran unpaced at 53% of a core with X doing the throttling.
     */
    vw->ren = SDL_CreateRenderer(vw->win, -1, SDL_RENDERER_ACCELERATED |
                                              SDL_RENDERER_PRESENTVSYNC);
    if (vw->ren == NULL)
        vw->ren = SDL_CreateRenderer(vw->win, -1, SDL_RENDERER_SOFTWARE);
    if (vw->ren == NULL) {
        fprintf(stderr, "pcview: cannot create a renderer: %s\n",
                SDL_GetError());
        return -1;
    }
    {
        SDL_RendererInfo ri;

        if (SDL_GetRendererInfo(vw->ren, &ri) == 0) {
            vw->vsync = (ri.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
            /*
             * The software renderer REPORTS the vsync flag and blocks on
             * nothing, measured at 77% of a core with the flag set. A
             * claim of vsync from a renderer with no display link behind
             * it is not pacing, so the clock paces whenever the renderer
             * is the software one, whatever the flag says.
             */
            if (strcmp(ri.name, "software") == 0) vw->vsync = 0;
            fprintf(stderr, "pcview: video %s, renderer %s%s\n",
                    SDL_GetCurrentVideoDriver(), ri.name,
                    vw->vsync ? ", vsync" : ", paced by the clock");
        }
    }

    /* The hint is read at texture creation. Nearest keeps a DS pixel a
     * square; linear over a replicated texture is sharp-bilinear. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
                o->filter == VIEW_FILTER_LINEAR ? "linear" : "nearest");
    /* Sized for the widest frame the protocol allows and used a frame at a
     * time: the width can change between frames when the port is following
     * the window, and reallocating a texture mid-drag would make a resize
     * flicker. */
    for (i = 0; i < 2; i++) {
        vw->tex[i] = SDL_CreateTexture(vw->ren, SDL_PIXELFORMAT_XRGB8888,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       (int)PC_VIEW_WIDE_MAX
                                         * (int)PC_VIEW_HD_MAX * o->rs,
                                       PC_VIEW_H * (int)PC_VIEW_HD_MAX
                                         * o->rs);
        vw->scaled[i] = malloc((size_t)PC_VIEW_WIDE_MAX * PC_VIEW_HD_MAX
                               * o->rs * PC_VIEW_H * PC_VIEW_HD_MAX
                               * o->rs * 4);
        if (vw->tex[i] == NULL || vw->scaled[i] == NULL) {
            fprintf(stderr, "pcview: SDL setup: %s\n", SDL_GetError());
            return -1;
        }
    }
    vw->mid = NULL;
    if (o->rs == 4 && o->filter == VIEW_FILTER_SCALE2X) {
        vw->mid = malloc((size_t)PC_VIEW_WIDE_MAX * PC_VIEW_HD_MAX * 2
                         * PC_VIEW_H * PC_VIEW_HD_MAX * 2 * 4);
        if (vw->mid == NULL) return -1;
    }
    return 0;
}

/*
 * The shape of the area one screen gets, reported back so a port asked for
 * an adaptive aspect can render into it. Written every frame rather than on
 * a resize event, because it is two words and that makes it correct after a
 * fullscreen toggle, a layout change and a viewer that attached late,
 * without any of them having to remember to say so.
 */
static void viewer_report_aspect(struct viewer *vw, const struct view_opts *o,
                                 int win_w, int win_h) {
    unsigned n, d;

    if (o->layout == VIEW_LAYOUT_WIDE) { n = (unsigned)win_w / 2u;
                                         d = (unsigned)win_h; }
    else if (o->layout == VIEW_LAYOUT_SMART) {
        /* The main screen's own area, which is what the port renders into;
         * the touch screen beside it is a passenger. */
        n = (unsigned)((long)win_w * 100 / (100 + vw->sec));
        d = (unsigned)win_h;
    }
    else                               { n = (unsigned)win_w;
                                         d = (unsigned)win_h / 2u; }
    if (n == 0 || d == 0) return;
    if (vw->shm->in_aspect_n == n && vw->shm->in_aspect_d == d) return;
    vw->shm->in_aspect_n = n;
    vw->shm->in_aspect_d = d;
}

/* One frame: seqlock-copy, upscale, place, present. `frame` is the caller's
 * 2 x WIDE_MAX*H scratch so --shot and the loop share the exact path. */
static void viewer_present(struct viewer *vw, const struct view_opts *o,
                           uint32_t frame[2][PC_VIEW_FRAME_WORDS]) {
    SDL_Rect dst[2], src;
    int win_w = 0, win_h = 0, upper = 0, tries, got = 0;
    int sw = PC_VIEW_W, sh = PC_VIEW_H;
    uint32_t s0, s1;

    /*
     * Retries with a pause, and none of them was worth anything without it:
     * sixteen attempts with no delay all land inside the same write, because
     * the odd-seq check costs nanoseconds and burns a try. The pause is what
     * lets the writer finish; one millisecond is sixty times shorter than a
     * frame and longer than the whole publish.
     *
     * The width is read inside the lock with the pixels it describes. Reading
     * it outside would eventually pair one frame's width with another's rows,
     * which is not a stale picture but a sheared one.
     */
    for (tries = 0; tries < 16; tries++) {
        s0 = vw->shm->seq;
        if (s0 & 1) { if (tries >= 2) view_sleep_ms(1); continue; }
        __sync_synchronize();
        sw = (int)vw->shm->width;
        sh = (int)vw->shm->height;
        if (sw < PC_VIEW_W) sw = PC_VIEW_W;
        if (sw > (int)(PC_VIEW_WIDE_MAX * PC_VIEW_HD_MAX))
            sw = (int)(PC_VIEW_WIDE_MAX * PC_VIEW_HD_MAX);
        if (sh < PC_VIEW_H) sh = PC_VIEW_H;
        if (sh > (int)(PC_VIEW_H * PC_VIEW_HD_MAX))
            sh = (int)(PC_VIEW_H * PC_VIEW_HD_MAX);
        memcpy(frame[0], (const void *)vw->shm->pix[0],
               (size_t)sw * sh * 4);
        memcpy(frame[1], (const void *)vw->shm->pix[1],
               (size_t)sw * sh * 4);
        upper = (int)vw->shm->upper_engine;
        __sync_synchronize();
        s1 = vw->shm->seq;
        if (s0 == s1) { got = 1; break; }
        if (tries >= 2) view_sleep_ms(1);
    }

    /*
     * A frame that could not be read is not drawn. What was in the buffer
     * after sixteen failures was half of one frame and half of another, and
     * putting it on the screen is the flash a person sees; keeping the last
     * good picture for one more frame is invisible. The textures already
     * hold it, so the window is still presented, skipping the present
     * entirely leaves it to the backend whether anything is on screen.
     */
    if (!got) {
        vw->torn++;
        SDL_GetRendererOutputSize(vw->ren, &win_w, &win_h);
        viewer_report_aspect(vw, o, win_w, win_h);
        src = (SDL_Rect){ 0, 0, vw->src_w * o->rs, vw->src_h * o->rs };
        view_screen_rects(o, vw->src_w, vw->src_h, vw->sec,
                          win_w, win_h, dst);
        SDL_SetRenderDrawColor(vw->ren, 0, 0, 0, 255);
        SDL_RenderClear(vw->ren);
        SDL_RenderCopy(vw->ren, vw->tex[0], &src, &dst[0]);
        SDL_RenderCopy(vw->ren, vw->tex[1], &src, &dst[1]);
        SDL_RenderPresent(vw->ren);
        return;
    }
    vw->presented++;

    view_upscale(frame[upper ? 1 : 0], sw, sh,
                 vw->scaled[0], vw->mid, o->rs, o->filter);
    view_upscale(frame[upper ? 0 : 1], sw, sh,
                 vw->scaled[1], vw->mid, o->rs, o->filter);
    /* Into the corner of a texture sized for the widest frame; the rest of
     * it is stale and never drawn, because every copy names this rect. */
    src = (SDL_Rect){ 0, 0, sw * o->rs, sh * o->rs };
    SDL_UpdateTexture(vw->tex[0], &src, vw->scaled[0], sw * o->rs * 4);
    SDL_UpdateTexture(vw->tex[1], &src, vw->scaled[1], sw * o->rs * 4);

    /* What the pen is inverted through until the next frame lands: the
     * width that is actually on the screen, not the one being asked for. */
    vw->src_w = sw;
    vw->src_h = sh;

    /*
     * The smart layout makes room rather than taking it. Growing the touch
     * screen inside a fixed window would shrink the game by a quarter at
     * exactly the moment a battle starts, which is the wrong way round,
     * so the window widens instead, keeping its height and keeping the main
     * screen exactly the size it already was. It narrows again when the pen
     * stops being wanted.
     *
     * Not while fullscreen or maximized (the player has said what size they
     * want), and not past the display's usable width; there the picture
     * simply refits and letterboxes, which is the old behaviour and is
     * merely less good rather than wrong.
     */
    if (o->layout == VIEW_LAYOUT_SMART) {
        int want = vw->shm->touch_wanted ? VIEW_SEC_MAX : VIEW_SEC_MIN;
        int was = vw->sec;

        if (vw->sec < want) vw->sec += VIEW_SEC_STEP;
        else if (vw->sec > want) vw->sec -= VIEW_SEC_STEP;
        if (vw->sec < VIEW_SEC_MIN) vw->sec = VIEW_SEC_MIN;
        if (vw->sec > VIEW_SEC_MAX) vw->sec = VIEW_SEC_MAX;

        if (vw->sec != was) {
            Uint32 fl = SDL_GetWindowFlags(vw->win);

            if (!(fl & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP
                        | SDL_WINDOW_MAXIMIZED))) {
                SDL_Rect usable;
                int ww = 0, wh = 0, cw = 0, ch = 0, target;

                SDL_GetWindowSize(vw->win, &ww, &wh);
                view_composed_wh(o->layout, vw->src_w, vw->src_h, vw->sec,
                                 &cw, &ch);
                target = ch > 0 ? (int)(((long)wh * cw) / ch) : ww;
                if (SDL_GetDisplayUsableBounds(
                        SDL_GetWindowDisplayIndex(vw->win), &usable) != 0) {
                    usable.w = target;      /* unknown: do not second-guess */
                }
                if (target > usable.w) target = usable.w;
                if (target > 0 && target != ww) {
                    SDL_SetWindowSize(vw->win, target, wh);
                }
            }
        }
    }

    SDL_GetRendererOutputSize(vw->ren, &win_w, &win_h);
    viewer_report_aspect(vw, o, win_w, win_h);
    view_screen_rects(o, sw, sh, vw->sec, win_w, win_h, dst);
    SDL_SetRenderDrawColor(vw->ren, 0, 0, 0, 255);
    SDL_RenderClear(vw->ren);
    SDL_RenderCopy(vw->ren, vw->tex[0], &src, &dst[0]);
    SDL_RenderCopy(vw->ren, vw->tex[1], &src, &dst[1]);
    SDL_RenderPresent(vw->ren);
}

static int viewer_run(struct viewer *vw, const struct view_opts *o) {
    static uint32_t frame[2][PC_VIEW_FRAME_WORDS];
    SDL_GameController *gc = NULL;
    struct view_audio audio = { 0 };
    SDL_AudioDeviceID adev = 0;
    int audio_started = 0;
    uint64_t starved_seen = 0;
    uint32_t sent_keys = 0, last_seq = 0;
    int sent_touch = 0, sent_tx = 0, sent_ty = 0, sent_turbo = 0;
    int mouse_down = 0, running = 1, stalled = 0;
    unsigned tick = 0, presented = 0;
    Uint64 next_tick = 0;
    const char *env_shot = getenv("PCVIEW_SHOT");

    if (!o->no_audio) {
        audio.v = vw->shm;
        /* Play from now, not from a ring of stale samples published
         * before this viewer attached. */
        audio.tail = vw->shm->audio_head;
        if (getenv("PCVIEW_AUDIO_DEBUG") != NULL) {
            audio.log = (struct view_audio_rec *)
                        calloc(VIEW_AUDIO_LOG, sizeof *audio.log);
        }
        adev = view_audio_open(&audio, o->audio_device);
    }

    /* A controller that is already plugged in raises no ADDED event. */
    {
        int i;

        for (i = 0; i < SDL_NumJoysticks(); i++) {
            if (!SDL_IsGameController(i)) continue;
            gc = SDL_GameControllerOpen(i);
            if (gc != NULL) {
                fprintf(stderr, "pcview: gamepad: %s\n",
                        SDL_GameControllerName(gc));
                break;
            }
        }
    }

    while (running) {
        SDL_Event ev;
        uint32_t keys;
        int turbo;
        int touch_on, tx, ty;

        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_ESCAPE) running = 0;
                if (ev.key.keysym.sym == SDLK_F11 ||
                    (ev.key.keysym.sym == SDLK_RETURN &&
                     (ev.key.keysym.mod & KMOD_ALT) != 0)) {
                    Uint32 fs = SDL_GetWindowFlags(vw->win) &
                                SDL_WINDOW_FULLSCREEN_DESKTOP;

                    SDL_SetWindowFullscreen(vw->win, fs ? 0 :
                                            SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                if (ev.key.keysym.sym == SDLK_F12) {
                    char stamp[64], path[1024];
                    time_t t = time(NULL);
                    struct tm *tm = localtime(&t);   /* one thread asks */

                    strftime(stamp, sizeof stamp,
                             "pokeplatinum-%Y%m%d-%H%M%S.png", tm);
                    snprintf(path, sizeof path, "%s%s%s",
                             o->shots ? o->shots : "",
                             o->shots ? "/" : "", stamp);
                    view_write_shot(vw->ren, path);
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) mouse_down = 1;
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) mouse_down = 0;
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (gc == NULL) {
                    gc = SDL_GameControllerOpen(ev.cdevice.which);
                    if (gc != NULL)
                        fprintf(stderr, "pcview: gamepad: %s\n",
                                SDL_GameControllerName(gc));
                }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (gc != NULL &&
                    ev.cdevice.which == SDL_JoystickInstanceID(
                        SDL_GameControllerGetJoystick(gc))) {
                    SDL_GameControllerClose(gc);
                    gc = NULL;
                    fprintf(stderr, "pcview: gamepad removed\n");
                }
                break;
            }
        }

        /* Input as LEVELS, keyboard and gamepad merged, published only on
         * change so an idle viewer writes nothing. */
        {
            int nkeys = 0;
            const Uint8 *state = SDL_GetKeyboardState(&nkeys);
            int alt = (SDL_GetModState() & KMOD_ALT) != 0;

            keys = view_keys_to_pad(state, nkeys, alt)
                 | view_controller_to_pad(gc);

            /* Fast-forward, held: Tab, or either shoulder trigger on a
             * gamepad. Level-read like every other key here, an
             * event-tracked hold is a key stuck down the moment the window
             * loses focus mid-press. It is published on its own word rather
             * than folded into in_seq, because it is not a game input and
             * must not decide whether a script has been overridden. */
            turbo = (SDL_SCANCODE_TAB < nkeys && state[SDL_SCANCODE_TAB])
                 || (gc != NULL
                     && (SDL_GameControllerGetAxis(
                             gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16384
                      || SDL_GameControllerGetAxis(
                             gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384));
            if (turbo != sent_turbo) {
                vw->shm->in_turbo = (uint32_t)turbo;
                sent_turbo = turbo;
            }
        }
        touch_on = 0; tx = sent_tx; ty = sent_ty;
        if (mouse_down) {
            int mx = 0, my = 0, win_w = 0, win_h = 0;

            SDL_GetMouseState(&mx, &my);
            SDL_GetRendererOutputSize(vw->ren, &win_w, &win_h);
            if (view_window_to_touch(o, vw->src_w, vw->src_h, vw->sec,
                                     win_w, win_h, mx, my,
                                     &tx, &ty) == 0)
                touch_on = 1;
        }
        if (keys != sent_keys || touch_on != sent_touch ||
            (touch_on && (tx != sent_tx || ty != sent_ty))) {
            vw->shm->in_keys = keys;
            vw->shm->in_touch = (uint32_t)touch_on;
            vw->shm->in_touch_x = (uint32_t)tx;
            vw->shm->in_touch_y = (uint32_t)ty;
            __sync_synchronize();
            vw->shm->in_seq++;
            sent_keys = keys; sent_touch = touch_on;
            sent_tx = tx; sent_ty = ty;
        }

        /*
         * Start the sound only once there is a cushion, and re-establish it
         * after a starve. Producer and device run at the same rate by
         * construction, so the ring stays at whatever level playback starts
         * at; starting at zero makes every slightly-early callback pad
         * silence, a permanent crackle. 6,144 frames is 188 ms, deeper
         * than the worst measured stall, inaudible against the game's own
         * response time. (Diamond's numbers, carried.)
         *
         * A starve is worth stopping for at all; see the check below.
         */
        if (adev != 0 && audio_started) {
            uint64_t starved_now;

            SDL_LockAudioDevice(adev);
            starved_now = audio.starved;
            SDL_UnlockAudioDevice(adev);
            if (starved_now != starved_seen) {
                starved_seen = starved_now;
                SDL_PauseAudioDevice(adev, 1);
                audio_started = 0;
            }
        }
        if (adev != 0 && !audio_started) {
            uint32_t head = __atomic_load_n(&vw->shm->audio_head,
                                            __ATOMIC_ACQUIRE);

            if (head - audio.tail >= VIEW_AUDIO_CUSHION) {
                SDL_PauseAudioDevice(adev, 0);
                audio_started = 1;
            }
        }

        viewer_present(vw, o, frame);

        if (env_shot != NULL && ++presented == 120)
            view_write_shot(vw->ren, env_shot);

        /*
         * The window closes on its own when the port ends. The port side
         * never unlinks the channel (pc/play.sh does) so st_nlink going
         * to zero is "the session is over"; a seq that stops moving with
         * the file still linked is a port that died hard, said in the
         * title rather than guessed at.
         */
        if (++tick % 120 == 0) {
#if !defined(_WIN32)
            struct stat st;

            if (vw->shm_fd >= 0 &&
                fstat(vw->shm_fd, &st) == 0 && st.st_nlink == 0) {
                fprintf(stderr, "pcview: the port ended; closing\n");
                running = 0;
            }
#endif
            if (vw->shm->seq == last_seq && !stalled) {
                stalled = 1;
                SDL_SetWindowTitle(vw->win,
                                   "Pok\xc3\xa9mon Platinum (stalled)");
            } else if (vw->shm->seq != last_seq && stalled) {
                stalled = 0;
                SDL_SetWindowTitle(vw->win, "Pok\xc3\xa9mon Platinum");
            }
            last_seq = vw->shm->seq;
        }

        /*
         * Nothing blocks when the renderer is not vsynced, and a fixed
         * SDL_Delay(1) is not pacing either; it is a 400 KB copy and a
         * full present several hundred times a second. Pace to the DS's
         * own 59.83 Hz by the clock: delay whatever remains of this
         * frame's 16.7 ms, and resynchronise rather than sprint after a
         * stall.
         */
        if (!vw->vsync) {
            Uint64 now = SDL_GetTicks64();

            if (next_tick == 0 || now > next_tick + 100) next_tick = now;
            if (next_tick > now) SDL_Delay((Uint32)(next_tick - now));
            next_tick += 16;
            /* 16 ms x 60 undershoots 1 s by 40 ms; every third frame
             * carries the extra millisecond so a minute stays a minute. */
            if (tick % 3 == 0) next_tick += 1;
        }
    }

    if (adev != 0) {
        SDL_CloseAudioDevice(adev);
        if (audio.log != NULL) {
            uint64_t i2, first = audio.nlog > VIEW_AUDIO_LOG
                                 ? audio.nlog - VIEW_AUDIO_LOG : 0;
            uint32_t prev = 0;

            fprintf(stderr, "pcview: %llu audio callbacks; the short ones:\n",
                    (unsigned long long)audio.calls);
            for (i2 = first; i2 < audio.nlog; i2++) {
                const struct view_audio_rec *r =
                    &audio.log[i2 % VIEW_AUDIO_LOG];

                if (r->got >= r->want) { prev = r->ms; continue; }
                fprintf(stderr, "  t=%8u ms  %6u since last  short %5u of %5u (%.1f ms)  ring %u\n",
                        r->ms, r->ms - prev, r->want - r->got, r->want,
                        (r->want - r->got) * 1000.0 / 32728.0, r->have);
                prev = r->ms;
            }
        }
        if (audio.dropped || audio.starved)
            fprintf(stderr, "pcview: audio dropped %llu, starved %llu "
                            "frames over the session\n",
                    (unsigned long long)audio.dropped,
                    (unsigned long long)audio.starved);
    }
    if (gc != NULL) SDL_GameControllerClose(gc);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Attaching                                                           */
/* ------------------------------------------------------------------ */

static void view_sleep_ms(int ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

static int viewer_attach(struct viewer *vw, const char *name, int wait_ms) {
    int waited = 0;
    struct pc_view_shm *shm;
#if defined(_WIN32)
    HANDLE h;
    char local[160];

    snprintf(local, sizeof local, "Local\\%s", name);
    for (;;) {
        h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, local);
        if (h != NULL) break;
        if (waited >= wait_ms) {
            fprintf(stderr,
                    "pcview: cannot open %s (error %lu)\n"
                    "  Start the port first: pokeplatinum.exe PC_VIEW=%s\n",
                    local, (unsigned long)GetLastError(), name);
            return -1;
        }
        view_sleep_ms(100);
        waited += 100;
    }
    shm = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof *shm);
    if (shm == NULL) {
        fprintf(stderr, "pcview: MapViewOfFile failed, error %lu\n",
                (unsigned long)GetLastError());
        CloseHandle(h);
        return -1;
    }
    vw->shm_fd = -1;
#else
    int fd;

    for (;;) {
        fd = shm_open(name, O_RDWR, 0600);
        if (fd >= 0) break;
        if (waited >= wait_ms) {
            fprintf(stderr,
                    "pcview: shm_open(%s): %s\n"
                    "  Start the port with PC_VIEW=%s first; it creates the "
                    "channel.\n",
                    name, strerror(errno), name);
            return -1;
        }
        view_sleep_ms(100);
        waited += 100;
    }
    shm = mmap(NULL, sizeof *shm, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        fprintf(stderr, "pcview: mmap: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    vw->shm_fd = fd;
#endif
    while (shm->magic != PC_VIEW_MAGIC && waited < wait_ms) {
        view_sleep_ms(100);
        waited += 100;
    }
    if (shm->magic != PC_VIEW_MAGIC) {
        fprintf(stderr, "pcview: %s has no channel magic (yet?), is the "
                        "port running with PC_VIEW=%s?\n", name, name);
        return -1;
    }
    if (shm->version != PC_VIEW_VERSION) {
        fprintf(stderr, "pcview: protocol version %u, expected %u, "
                        "rebuild whichever side is stale\n",
                (unsigned)shm->version, PC_VIEW_VERSION);
        return -1;
    }
    vw->shm = shm;
    /* Who is watching. The port has no window; this process holds it, so the
     * port ends its run when this one goes away. Published at attach rather
     * than at first present, so a viewer that dies while still opening its
     * window is noticed too. */
#if defined(_WIN32)
    shm->in_viewer_pid = (uint32_t)GetCurrentProcessId();
#else
    shm->in_viewer_pid = (uint32_t)getpid();
#endif
    shm->in_quit = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Selftest                                                            */
/* ------------------------------------------------------------------ */

static int st_failures;

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "selftest: FAIL: %s\n", what);
        st_failures++;
    }
}

static uint32_t xs32(uint32_t *s) {          /* xorshift; determinism, not art */
    uint32_t x = *s;

    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

static void selftest_layout(void) {
    static const int wins[][2] = {
        { 512, 768 }, { 511, 767 }, { 1920, 1080 }, { 100, 900 },
        { 900, 100 }, { 256, 384 }, { 3, 5 }, { 4096, 4096 },
    };
    static const int widths[] = { PC_VIEW_W, 300, (int)PC_VIEW_WIDE_MAX };
    int layout, integer, i, wi;

    for (layout = 0; layout < 2; layout++)
    for (wi = 0; wi < (int)(sizeof widths / sizeof *widths); wi++) {
        int cw, ch;

        view_composed_wh(layout, widths[wi], PC_VIEW_H, VIEW_SEC_MIN,
                         &cw, &ch);
        for (integer = 0; integer < 2; integer++) {
            for (i = 0; i < (int)(sizeof wins / sizeof *wins); i++) {
                int ww = wins[i][0], wh = wins[i][1];
                struct view_rect r = view_layout(cw, ch, ww, wh, integer,
                                                 VIEW_ASPECT_NATIVE);

                check(r.x >= 0 && r.y >= 0 &&
                      r.x + r.w <= ww && r.y + r.h <= wh,
                      "the picture stays inside the window");
                check(abs((ww - r.w) - 2 * r.x) <= 1 &&
                      abs((wh - r.h) - 2 * r.y) <= 1,
                      "the picture is centred");
                /* Proportions: r.w/r.h == cw/ch within a pixel's rounding. */
                check(labs((long)r.w * ch - (long)r.h * cw) <= cw ||
                      labs((long)r.w * ch - (long)r.h * cw) <= ch,
                      "proportions survive the fit");
                if (integer && ww >= cw && wh >= ch)
                    check(r.w % cw == 0 && r.h % ch == 0,
                          "integer mode scales by whole source pixels");
            }
        }
    }
    /* Stretch fills the window, whatever the window is. */
    {
        struct view_rect r = view_layout(256, 384, 777, 333, 0,
                                         VIEW_ASPECT_STRETCH);

        check(r.x == 0 && r.y == 0 && r.w == 777 && r.h == 333,
              "stretch fills the window");
    }
}

static void selftest_screen_rects(void) {
    struct view_opts o = { 0 };
    SDL_Rect d[2];
    int layout, sw;

    o.scale = 2; o.rs = 1;
    for (layout = 0; layout < 3; layout++)
    for (sw = PC_VIEW_W; sw <= (int)PC_VIEW_WIDE_MAX; sw += 43) {
        o.layout = layout;
        view_screen_rects(&o, sw, PC_VIEW_H, VIEW_SEC_MIN, 1000, 700, d);
        /* The two screens tile the fitted rectangle exactly: no gap, no
         * overlap, whichever way the remainder pixel fell. Smart is the
         * exception; its touch screen is deliberately smaller and
         * centred, so what it owes is proportions rather than tiling. */
        if (layout == VIEW_LAYOUT_SMART) {
            check(d[0].x + d[0].w == d[1].x, "smart: screens abut");
            check(d[1].w * 100 <= d[0].w * VIEW_SEC_MIN + 100
                  && d[1].w * 100 >= d[0].w * VIEW_SEC_MIN - 100,
                  "smart: the touch screen is the asked-for fraction");
            check(d[1].y + d[1].h / 2 >= d[0].y + d[0].h / 2 - 1
                  && d[1].y + d[1].h / 2 <= d[0].y + d[0].h / 2 + 1,
                  "smart: the touch screen is centred beside the main one");
            check(d[0].w * d[1].h <= d[1].w * d[0].h + d[0].w
                  && d[0].w * d[1].h >= d[1].w * d[0].h - d[0].w,
                  "smart: both screens keep the same proportions");
        } else if (layout == VIEW_LAYOUT_WIDE) {
            check(d[0].y == d[1].y && d[0].h == d[1].h,
                  "wide: screens share a row");
            check(d[0].x + d[0].w == d[1].x, "wide: screens abut");
        } else {
            check(d[0].x == d[1].x && d[0].w == d[1].w,
                  "stacked: screens share a column");
            check(d[0].y + d[0].h == d[1].y, "stacked: screens abut");
        }
    }
}

static void selftest_mouse(void) {
    struct view_opts o = { 0 };
    int layout, integer, tx, ty;

    o.scale = 2; o.rs = 1;
    for (layout = 0; layout < 3; layout++) {
        for (integer = 0; integer < 2; integer++) {
            SDL_Rect d[2];
            int px, py;

            o.layout = layout;
            o.integer = integer;
            view_screen_rects(&o, PC_VIEW_W, PC_VIEW_H, VIEW_SEC_MIN,
                              1024, 768, d);

            /* The touch screen's centre maps to its own middle pixel. */
            px = d[1].x + d[1].w / 2;
            py = d[1].y + d[1].h / 2;
            check(view_window_to_touch(&o, PC_VIEW_W, PC_VIEW_H, VIEW_SEC_MIN,
                                     1024, 768, px, py,
                                       &tx, &ty) == 0,
                  "a click on the touch screen lands");
            check(abs(tx - PC_VIEW_W / 2) <= 1 && abs(ty - PC_VIEW_H / 2) <= 1,
                  "the touch screen's centre is its centre");

            /* A click on the TOP screen is not a touch. */
            px = d[0].x + d[0].w / 2;
            py = d[0].y + d[0].h / 2;
            check(view_window_to_touch(&o, PC_VIEW_W, PC_VIEW_H, VIEW_SEC_MIN,
                                     1024, 768, px, py,
                                       &tx, &ty) != 0,
                  "the top screen has no panel on it");

            /* Nor is the letterbox. */
            check(view_window_to_touch(&o, PC_VIEW_W, PC_VIEW_H, VIEW_SEC_MIN,
                                     1024, 768, 0, 0,
                                       &tx, &ty) != 0,
                  "the letterbox has no panel on it");

            /* Every corner of the touch screen inverts inside range. */
            check(view_window_to_touch(&o, PC_VIEW_W, PC_VIEW_H, VIEW_SEC_MIN,
                                     1024, 768,
                                       d[1].x, d[1].y, &tx, &ty) == 0 &&
                  tx == 0 && ty == 0,
                  "the touch screen's top-left corner is (0,0)");
            check(view_window_to_touch(&o, PC_VIEW_W, PC_VIEW_H, VIEW_SEC_MIN,
                                     1024, 768,
                                       d[1].x + d[1].w - 1,
                                       d[1].y + d[1].h - 1, &tx, &ty) == 0 &&
                  tx == PC_VIEW_W - 1 && ty == PC_VIEW_H - 1,
                  "the touch screen's bottom-right corner is (255,191)");
        }
    }
}

/*
 * The pen against a wide frame. The panel is still 256 columns and still
 * where the game thinks it is; what changed is that the rectangle on screen
 * is wider than the panel, and a stylus cannot reach past it. Everything
 * here is the invariant a wide picture must not break: the centre is still
 * the centre, the margins take no touch, and the panel still spans its
 * whole range.
 */
static void selftest_mouse_wide(void) {
    struct view_opts o = { 0 };
    int layout, sw, tx, ty;

    o.scale = 2; o.rs = 1;
    for (layout = 0; layout < 2; layout++) {
        for (sw = 258; sw <= (int)PC_VIEW_WIDE_MAX; sw += 42) {
            SDL_Rect d[2];
            int margin = (sw - PC_VIEW_W) / 2;
            int px, py, seen_lo = 0, seen_hi = 0, i;

            o.layout = layout;
            view_screen_rects(&o, sw, PC_VIEW_H, VIEW_SEC_MIN, 1024, 768, d);

            px = d[1].x + d[1].w / 2;
            py = d[1].y + d[1].h / 2;
            check(view_window_to_touch(&o, sw, PC_VIEW_H, VIEW_SEC_MIN,
                                     1024, 768, px, py,
                                       &tx, &ty) == 0,
                  "wide: a click in the middle of the panel lands");
            check(abs(tx - PC_VIEW_W / 2) <= 1 && abs(ty - PC_VIEW_H / 2) <= 1,
                  "wide: the panel's centre is still (128,96)");

            /* The margins: on the picture, off the digitizer. */
            check(view_window_to_touch(&o, sw, PC_VIEW_H, VIEW_SEC_MIN,
                                     1024, 768,
                                       d[1].x, py, &tx, &ty) != 0,
                  "wide: the left margin has no panel on it");
            check(view_window_to_touch(&o, sw, PC_VIEW_H, VIEW_SEC_MIN,
                                     1024, 768,
                                       d[1].x + d[1].w - 1, py,
                                       &tx, &ty) != 0,
                  "wide: the right margin has no panel on it");

            /* And the panel still reaches both ends: walking the row finds
             * column 0 and column 255 somewhere inside it. */
            for (i = 0; i < d[1].w; i++) {
                if (view_window_to_touch(&o, sw, PC_VIEW_H, VIEW_SEC_MIN,
                                     1024, 768,
                                         d[1].x + i, py, &tx, &ty) != 0)
                    continue;
                if (tx == 0) seen_lo = 1;
                if (tx == PC_VIEW_W - 1) seen_hi = 1;
                check(tx >= 0 && tx < PC_VIEW_W,
                      "wide: every touch is inside the panel");
            }
            check(seen_lo && seen_hi,
                  "wide: the panel still spans column 0 to column 255");
            check(margin > 0, "wide: a wide frame has margins");
        }
    }
}

/*
 * The width policy itself, which is the port's and is pinned here because
 * this is the side that can be run in a loop: a window shape in, the
 * columns the port will render out.
 */
static void selftest_aspect(void) {
    unsigned n;

    check(pc_view_aspect_width(0, 0) == PC_VIEW_W,
          "no window shape is a native picture");
    check(pc_view_aspect_width(4, 3) == PC_VIEW_W,
          "4:3 is the console's own width");
    check(pc_view_aspect_width(1, 4) == PC_VIEW_W,
          "a tall window never renders narrower than the DS");
    check(pc_view_aspect_width(1000, 1) == (int)PC_VIEW_WIDE_MAX,
          "an absurdly wide window stops at the cap");
    check(pc_view_aspect_width(16, 9) > PC_VIEW_W &&
          pc_view_aspect_width(16, 9) <= (int)PC_VIEW_WIDE_MAX,
          "16:9 is wider than native and inside the cap");
    check(pc_view_aspect_width(16, 10) < pc_view_aspect_width(16, 9),
          "16:10 is narrower than 16:9");

    /* Monotonic, even, and in range for every shape a window can have,
     * the properties the deadband and the margin split rely on. */
    {
        int prev = PC_VIEW_W;

        for (n = 256; n <= 4000; n += 7) {
            int w = pc_view_aspect_width(n, 1080);

            check(w >= PC_VIEW_W && w <= (int)PC_VIEW_WIDE_MAX,
                  "the width stays inside the protocol's range");
            check((w & 1) == 0, "the width is even, so the margins match");
            check(w >= prev, "a wider window never narrows the picture");
            prev = w;
        }
    }
}

static void selftest_scalers(void) {
    enum { W = 16, H = 12 };
    static uint32_t src[W * H], d2[W * 2 * H * 2], d3[W * 3 * H * 3];
    static uint32_t rep[W * 3 * H * 3];
    uint32_t seed = 0x12345u;
    int i, x, y;

    /* Flat colour: every scaler is the identity, sized up. */
    for (i = 0; i < W * H; i++) src[i] = 0x336699;
    view_scale2x(src, W, H, d2);
    for (i = 0; i < W * 2 * H * 2; i++)
        if (d2[i] != 0x336699) break;
    check(i == W * 2 * H * 2, "scale2x is the identity on flat colour");
    view_scale3x(src, W, H, d3);
    for (i = 0; i < W * 3 * H * 3; i++)
        if (d3[i] != 0x336699) break;
    check(i == W * 3 * H * 3, "scale3x is the identity on flat colour");

    /* Random input: no invented colours, and replicate's blocks replicate. */
    for (i = 0; i < W * H; i++) src[i] = xs32(&seed) & 0xFFFFFF;
    view_scale2x(src, W, H, d2);
    for (i = 0; i < W * 2 * H * 2; i++) {
        int sx = (i % (W * 2)) / 2, sy = (i / (W * 2)) / 2, ok = 0, dx, dy;

        for (dy = -1; dy <= 1 && !ok; dy++)
            for (dx = -1; dx <= 1 && !ok; dx++) {
                int nx = sx + dx, ny = sy + dy;

                if (nx >= 0 && nx < W && ny >= 0 && ny < H &&
                    src[ny * W + nx] == d2[i]) ok = 1;
            }
        if (!ok) break;
    }
    check(i == W * 2 * H * 2, "scale2x never invents a colour");

    view_replicate(src, W, H, rep, 3);
    for (y = 0; y < H * 3 && st_failures == 0; y++)
        for (x = 0; x < W * 3; x++)
            if (rep[y * (W * 3) + x] != src[(y / 3) * W + (x / 3)]) {
                check(0, "replicate makes NxN blocks");
                y = H * 3;
                break;
            }
}

/*
 * The SDL path, headless: a synthetic channel, engine A solid red, engine
 * B solid blue, through the REAL viewer_present(), read back and checked
 * by position. The expectation comes from view_screen_rects() called here,
 * not from what was handed to SDL, so a placement defect cannot vouch for
 * itself. Runs under whatever video driver is present; the caller sets
 * SDL_VIDEODRIVER=dummy to make it displayless.
 */
static void selftest_sdl(void) {
    static struct pc_view_shm shm;      /* static: it is ~4.5 MB */
    static uint32_t frame[2][PC_VIEW_FRAME_WORDS];
    struct view_opts o = { 0 };
    struct viewer vw;
    int layout, i;

    o.scale = 2; o.rs = 1; o.wait_ms = 0;
    shm.magic = PC_VIEW_MAGIC;
    shm.version = PC_VIEW_VERSION;
    shm.seq = 2;

    /* Both widths, because the packing is the thing most likely to be got
     * wrong: a wide frame's rows are `width` apart, and a reader that keeps
     * stepping by 256 gets a picture that shears a little more each row. A
     * margin painted its own colour makes that visible to a readback. */
    for (layout = 0; layout < 4; layout++) {
        SDL_Rect d[2];
        int ow = 0, oh = 0, sw, margin, x, y;
        SDL_Surface *sf;

        sw = (layout & 2) ? (int)PC_VIEW_WIDE_MAX : PC_VIEW_W;
        margin = (sw - PC_VIEW_W) / 2;
        shm.width = (uint32_t)sw;
        for (y = 0; y < PC_VIEW_H; y++) {
            for (x = 0; x < sw; x++) {
                int panel = x >= margin && x < margin + PC_VIEW_W;

                shm.pix[0][y * sw + x] = panel ? 0xFF0000    /* A: red   */
                                               : 0x00FF00;   /* margin   */
                shm.pix[1][y * sw + x] = panel ? 0x0000FF    /* B: blue  */
                                               : 0x000000;
            }
        }

        memset(&vw, 0, sizeof vw);
        vw.shm = &shm;
        vw.shm_fd = -1;
        o.layout = layout % 3;
        if (viewer_open_video(&vw, &o) != 0) {
            check(0, "the SDL path opens");
            return;
        }
        viewer_present(&vw, &o, frame);

        SDL_GetRendererOutputSize(vw.ren, &ow, &oh);
        view_screen_rects(&o, sw, PC_VIEW_H, vw.sec, ow, oh, d);
        sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32,
                                            SDL_PIXELFORMAT_XRGB8888);
        if (sf != NULL &&
            SDL_RenderReadPixels(vw.ren, NULL, SDL_PIXELFORMAT_XRGB8888,
                                 sf->pixels, sf->pitch) == 0) {
            const uint32_t *px = sf->pixels;
            int pitch = sf->pitch / 4;
            uint32_t top, bot;

            top = px[(d[0].y + d[0].h / 2) * pitch + d[0].x + d[0].w / 2];
            bot = px[(d[1].y + d[1].h / 2) * pitch + d[1].x + d[1].w / 2];
            check((top & 0xFFFFFF) == 0xFF0000,
                  "the top screen shows engine A");
            check((bot & 0xFFFFFF) == 0x0000FF,
                  "the touch screen shows engine B");
            if (sw > PC_VIEW_W) {
                /* Well inside the margin: 5% across a picture whose margin
                 * is 12.5% of it, so no rounding puts this on the panel. */
                uint32_t ml = px[(d[0].y + d[0].h / 2) * pitch
                                 + d[0].x + d[0].w / 20];
                uint32_t mr = px[(d[0].y + d[0].h / 2) * pitch
                                 + d[0].x + d[0].w - 1 - d[0].w / 20];

                check((ml & 0xFFFFFF) == 0x00FF00 &&
                      (mr & 0xFFFFFF) == 0x00FF00,
                      "wide: both margins are drawn, and drawn as published");
            }
        } else {
            check(0, "readback works");
        }
        if (sf != NULL) SDL_FreeSurface(sf);

        /* upper_engine flips the pair without anything else moving. */
        shm.upper_engine = 1;
        viewer_present(&vw, &o, frame);
        sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32,
                                            SDL_PIXELFORMAT_XRGB8888);
        if (sf != NULL &&
            SDL_RenderReadPixels(vw.ren, NULL, SDL_PIXELFORMAT_XRGB8888,
                                 sf->pixels, sf->pitch) == 0) {
            const uint32_t *px = sf->pixels;
            int pitch = sf->pitch / 4;
            uint32_t top;

            top = px[(d[0].y + d[0].h / 2) * pitch + d[0].x + d[0].w / 2];
            check((top & 0xFFFFFF) == 0x0000FF,
                  "upper_engine=1 puts engine B on top");
        }
        if (sf != NULL) SDL_FreeSurface(sf);
        shm.upper_engine = 0;

        for (i = 0; i < 2; i++) {
            if (vw.tex[i] != NULL) SDL_DestroyTexture(vw.tex[i]);
            free(vw.scaled[i]);
        }
        free(vw.mid);
        if (vw.ren != NULL) SDL_DestroyRenderer(vw.ren);
        if (vw.win != NULL) SDL_DestroyWindow(vw.win);
    }
}

static int selftest(void) {
    st_failures = 0;
    selftest_layout();
    selftest_screen_rects();
    selftest_mouse();
    selftest_mouse_wide();
    selftest_aspect();
    selftest_scalers();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "selftest: SDL_Init: %s (video checks skipped)\n",
                SDL_GetError());
    } else {
        selftest_sdl();
        SDL_Quit();
    }
    if (st_failures == 0) {
        printf("pcview selftest: all checks pass\n");
        return 0;
    }
    printf("pcview selftest: %d FAILURES\n", st_failures);
    return 1;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0) {
    printf("usage: %s [NAME] [options]\n"
           "\n"
           "  NAME           the channel the port's PC_VIEW=NAME names\n"
           "                 (default: pokeplatinum)\n"
           "  --scale N      initial window scale, default 2 (512x768)\n"
           "  --layout MODE  smart (the default: the game large, the touch\n"
           "                 screen beside it and growing to match when the\n"
           "                 game asks for the pen), stacked (the DS's own),\n"
           "                 or wide, the two\n"
           "                 screens side by side\n"
           "  --render-scale N  render from an N-times-larger texture, 1..4\n"
           "  --filter NAME  nearest (crisp, the default), linear (with\n"
           "                 --render-scale 2+ it is sharp-bilinear), or\n"
           "                 scale2x (EPX edge smoothing, render scale 2-4)\n"
           "  --integer      scale by whole source pixels only\n"
           "  --stretch      fill the window, ignoring proportions\n"
           "  --fullscreen   start fullscreen (F11 / Alt+Enter toggles)\n"
           "  --audio-device NAME  play the sound on this device\n"
           "  --list-audio-devices  print this machine's devices, exit\n"
           "  --no-audio     open no audio device (also PCVIEW_NO_AUDIO=1)\n"
           "  --bind PAD=KEY[,...]  rebind the keyboard; PAD is a, b, x, y,\n"
           "                 l, r, start, select or a direction, KEY is one of\n"
           "                 SDL's key names. Unnamed buttons keep the\n"
           "                 defaults below.\n"
           "  --shots DIR    where F12 screenshots go (default: here)\n"
           "  --shot PATH    write one composed frame and exit; PNG if PATH\n"
           "                 ends .png, binary PPM otherwise.\n"
           "                 works under SDL_VIDEODRIVER=dummy, which is\n"
           "                 how a test reads this program's output\n"
           "  --wait MS      how long --shot waits for the channel, and how\n"
           "                 long attach waits for the port, default 5000\n"
           "  --selftest     check layout, mouse inverse, scalers and the\n"
           "                 SDL path; no port needed\n"
           "  --help         this message\n"
           "\n"
           "Keys: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R,\n"
           "Return = Start, Backspace = Select, Esc quits. F11/Alt+Enter\n"
           "fullscreen, F12 screenshot. HOLD TAB to fast-forward (either\n"
           "gamepad trigger does the same). The left mouse button is the stylus,\n"
           "on the touch screen only; leaving it lifts the pen. A gamepad\n"
           "maps by position (SDL's A; the bottom button, is the DS's B),\n"
           "and the left stick is the d-pad past a deadzone.\n"
           "\n"
           "The window closes on its own when the play session ends. F12\n"
           "screenshots are PNGs at the window's own resolution, timestamped,\n"
           "and land wherever --shots says, the launcher points that at the\n"
           "folder your save is in.\n",
           argv0);
}

int main(int argc, char **argv)
{
    struct view_opts o = { 0 };
    struct viewer vw;
    int i, want_selftest = 0, want_list = 0;

    o.name = "pokeplatinum";
    o.scale = 2;
    o.rs = 1;
    o.wait_ms = 5000;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (a[0] != '-') { o.name = a; continue; }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(a, "--selftest") == 0) {
            want_selftest = 1;
        } else if (strcmp(a, "--list-audio-devices") == 0) {
            want_list = 1;
        } else if (strcmp(a, "--no-audio") == 0) {
            o.no_audio = 1;
        } else if (strcmp(a, "--integer") == 0) {
            o.integer = 1;
        } else if (strcmp(a, "--stretch") == 0) {
            o.aspect = VIEW_ASPECT_STRETCH;
        } else if (strcmp(a, "--fullscreen") == 0) {
            o.fullscreen = 1;
        } else if (strcmp(a, "--scale") == 0 && i + 1 < argc) {
            o.scale = atoi(argv[++i]);
            if (o.scale < 1 || o.scale > 8) {
                fprintf(stderr, "pcview: --scale wants 1..8\n");
                return 2;
            }
        } else if (strcmp(a, "--render-scale") == 0 && i + 1 < argc) {
            o.rs = atoi(argv[++i]);
            if (o.rs < 1 || o.rs > 4) {
                fprintf(stderr, "pcview: --render-scale wants 1..4\n");
                return 2;
            }
        } else if (strcmp(a, "--layout") == 0 && i + 1 < argc) {
            const char *m = argv[++i];

            if (strcmp(m, "smart") == 0)     o.layout = VIEW_LAYOUT_SMART;
            else if (strcmp(m, "stacked") == 0) o.layout = VIEW_LAYOUT_STACKED;
            else if (strcmp(m, "wide") == 0) o.layout = VIEW_LAYOUT_WIDE;
            else {
                fprintf(stderr,
                        "pcview: --layout wants smart, stacked or wide\n");
                return 2;
            }
        } else if (strcmp(a, "--filter") == 0 && i + 1 < argc) {
            const char *m = argv[++i];

            if (strcmp(m, "nearest") == 0)      o.filter = VIEW_FILTER_NEAREST;
            else if (strcmp(m, "linear") == 0)  o.filter = VIEW_FILTER_LINEAR;
            else if (strcmp(m, "scale2x") == 0) o.filter = VIEW_FILTER_SCALE2X;
            else {
                fprintf(stderr,
                        "pcview: --filter wants nearest, linear or scale2x\n");
                return 2;
            }
        } else if (strcmp(a, "--audio-device") == 0 && i + 1 < argc) {
            o.audio_device = argv[++i];
        } else if (strcmp(a, "--bind") == 0 && i + 1 < argc) {
            if (view_bind(argv[++i]) != 0) return 2;
        } else if (strcmp(a, "--shots") == 0 && i + 1 < argc) {
            o.shots = argv[++i];
        } else if (strcmp(a, "--shot") == 0 && i + 1 < argc) {
            o.shot = argv[++i];
        } else if (strcmp(a, "--wait") == 0 && i + 1 < argc) {
            o.wait_ms = atoi(argv[++i]);
        } else {
            fprintf(stderr, "pcview: unknown option %s (try --help)\n", a);
            return 2;
        }
    }
    if (o.filter == VIEW_FILTER_SCALE2X && o.rs < 2) {
        fprintf(stderr, "pcview: scale2x wants --render-scale 2, 3 or 4\n");
        return 2;
    }
    if (getenv("PCVIEW_NO_AUDIO") != NULL) o.no_audio = 1;

    if (want_selftest) return selftest();
    if (want_list) { view_list_audio_devices(); return 0; }

    memset(&vw, 0, sizeof vw);
    if (viewer_attach(&vw, o.name, o.wait_ms) != 0) return 1;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        /* The controller subsystem can be absent; the picture must not be. */
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "pcview: SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
    }

    if (o.shot != NULL) {
        static uint32_t frame[2][PC_VIEW_FRAME_WORDS];
        int waited = 0, rc;

        /* Wait for a frame, not just for the channel: seq 0 is a port
         * that has not published yet, and a shot of it is a shot of
         * nothing with a success exit code. */
        while (vw.shm->seq < 2 && waited < o.wait_ms) {
            view_sleep_ms(50);
            waited += 50;
        }
        if (viewer_open_video(&vw, &o) != 0) return 1;
        /* Present until the smart layout has finished making room. A still
         * picture of a layout halfway through its ramp is a picture of
         * neither layout, and this is the path a test reads. */
        for (waited = 0; waited < 64; waited++) {
            int want = vw.shm->touch_wanted ? VIEW_SEC_MAX : VIEW_SEC_MIN;

            viewer_present(&vw, &o, frame);
            if (o.layout != VIEW_LAYOUT_SMART || vw.sec == want) break;
        }
        rc = view_write_shot(vw.ren, o.shot);
        SDL_Quit();
        return rc == 0 ? 0 : 1;
    }

    if (viewer_open_video(&vw, &o) != 0) return 1;
    viewer_run(&vw, &o);
    /* Closing the window is a person ending the session, and the port cannot
     * see the window. Saying so lets it finish tidily rather than be noticed
     * missing a second later by the pid check. */
    if (vw.torn != 0) {
        /* Worth saying: a frame the viewer could not read whole is a frame
         * the port published while this process was copying it, and the
         * number is how often that happened over a session. */
        fprintf(stderr, "pcview: %llu of %llu frames could not be read whole"
                        " and were skipped\n",
                (unsigned long long)vw.torn,
                (unsigned long long)(vw.torn + vw.presented));
    }
    if (vw.shm != NULL) vw.shm->in_quit = 1;
    SDL_Quit();
    return 0;
}
