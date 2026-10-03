/*
 * The port's half of the window, pc/include/pc_view.h has the model.
 *
 * PC_VIEW=<name> names a POSIX shm object; this creates it, sizes it and
 * publishes every frame into it. The viewer (pc/sdl/pcview.c, a native
 * 64-bit SDL2 process) attaches to the same name. No PC_VIEW, no cost:
 * pc_video.c's pc_view_publish reference is weak and this file's strong
 * definition simply finds view_map NULL and returns.
 *
 * Live input arrives through the same page: the viewer owns the in_*
 * words, and this samples them once per published frame and hands them to
 * pc_input's live path. A viewer that never wrote (in_seq == 0) never
 * perturbs a scripted run.
 */

#include "pc_view.h"
#include "pc_bench.h"
#include "pc_prof.h"
#include "pc_video.h"
#include "pc_gpu2d.h"
#include "pc_gpu3d.h"
#include "pc_gpu3d_soft.h"
#include "pc_workers.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include "pc_win_ipc.h"
#elif defined(__wasm__)
#include <np_core.h>    /* NP_KEY_*, the runtime's key bits */
#include <pc_wasm.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/*
 * This file's one way to sleep. On Windows the high-resolution wait lives
 * in pc_win_clock.c, and reaching it by OVERRIDING nanosleep is a link
 * race the port lost on 2026-08-27: a newer mingw's winpthreads carried
 * its own strong nanosleep, the shipped exe's pacer slept on the 15.6 ms
 * tick, and the live build skipped a third of its pictures (`slip 23 ms`
 * in every trace row) while a locally linked exe of the same source was
 * clean. The pacer calls the named entry now; a name cannot lose a race.
 */
#if defined(_WIN32)
void pc_sleep_ns(long long ns);
static void view_sleep(const struct timespec *t)
{
    pc_sleep_ns((long long)t->tv_sec * 1000000000LL + t->tv_nsec);
}
#elif !defined(__wasm__)
static void view_sleep(const struct timespec *t)
{
    nanosleep(t, NULL);
}
#endif

extern void pc_input_live(unsigned keys, int touch_on,
                          unsigned x, unsigned y);
extern unsigned pc_audio_read(int16_t *dst, unsigned frames);
extern void pc_audio_set_sink(void (*sink)(void));

#if defined(__wasm__)
/*
 * wasm32 (pc/Makefile.wasm): the runtime is the window.
 *
 * There is no second process here: core/include/np_guest_abi.h's frame
 * descriptor (pc_wasm_frame, pc/wasm/include/pc_wasm.h) replaces the shared
 * page, and np_host_vblank() replaces both the seqlock and the pacer. The
 * guest is parked inside that call while the runtime reads the frame, so the
 * descriptor needs no lock and the pixels need no copy; and the runtime
 * decides when the next frame starts, so this side never sleeps.
 *
 * What crosses, and how it maps onto what the shm channel carries:
 *
 *   screen   The 2D engine's own surfaces (pc_video.h: 0x00RRGGBB words,
 *            row-major, 256x192), which is the descriptor's format already,
 *            so they are handed over by address, top screen first by
 *            POWCNT1's DSEL the way pc_video.c orders a frame dump.
 *   audio    The ring the viewer is fed, PC_VIEW_AUDIO_FRAMES (2^15) u32
 *            stereo frames with left in the low half, filled from the same
 *            pc_audio sink the moment the mixer produces, at
 *            PC_VIEW_AUDIO_RATE. Here it is a static rather than part of a
 *            page; the descriptor's audio_head is the page's.
 *   input    NP_KEY_* are PAD_Read's bits, which is what pc_input_live()
 *            takes. Applied only when the runtime's input CHANGES: the
 *            page's in_seq rule ("live input wins over the script only
 *            once a viewer has actually spoken"), so a runtime that never
 *            touches the pad leaves a PC_INPUT script in charge.
 *   quit     in_quit ends the run where a closed viewer window does, here
 *            at the frame boundary, with exit(0): the atexit handlers
 *            (pc_card_backup_sync stores the save, the dumps close) run.
 *
 * Not carried: wide and HD frames (PC_ASPECT / PC_HD3D are refused to
 * native, below), turbo (the runtime's to decide), the viewer pid (there is
 * no viewer process), and the lid (see view_wasm_input).
 */
_Static_assert(NP_KEY_A == PC_VIEW_KEY_A && NP_KEY_B == PC_VIEW_KEY_B
               && NP_KEY_SELECT == PC_VIEW_KEY_SELECT
               && NP_KEY_START == PC_VIEW_KEY_START
               && NP_KEY_RIGHT == PC_VIEW_KEY_RIGHT
               && NP_KEY_LEFT == PC_VIEW_KEY_LEFT
               && NP_KEY_UP == PC_VIEW_KEY_UP
               && NP_KEY_DOWN == PC_VIEW_KEY_DOWN
               && NP_KEY_R == PC_VIEW_KEY_R && NP_KEY_L == PC_VIEW_KEY_L
               && NP_KEY_X == PC_VIEW_KEY_X && NP_KEY_Y == PC_VIEW_KEY_Y,
               "NP_KEY_* must be PAD_Read's bits, which pc_input_live takes");
#define VIEW_WASM_KEYS 0x0FFFu      /* A..Y; DEBUG is not a player's key */

_Static_assert((PC_VIEW_AUDIO_FRAMES & (PC_VIEW_AUDIO_FRAMES - 1)) == 0,
               "the descriptor's audio ring must be a power of two");

static uint32_t view_audio[PC_VIEW_AUDIO_FRAMES];
static uint32_t view_audio_head;    /* stereo frames ever written */

/* The sink: view_publish_audio's loop below, aimed at the static ring. */
static void view_publish_audio(void)
{
    int16_t buf[256 * 2];
    unsigned got;

    while ((got = pc_audio_read(buf, sizeof buf / (2 * sizeof buf[0]))) != 0) {
        unsigned i;

        for (i = 0; i < got; i++) {
            view_audio[(view_audio_head + i) % PC_VIEW_AUDIO_FRAMES] =
                (uint32_t)(uint16_t)buf[i * 2 + 0] |
                ((uint32_t)(uint16_t)buf[i * 2 + 1] << 16);
        }
        view_audio_head += got;
    }
}

int pc_view_init(void)
{
    const char *name = getenv("PC_VIEW");

    if (name != NULL && name[0] != '\0') {
        fprintf(stderr, "pc-view: PC_VIEW=%s ignored: on wasm every frame"
                        " goes to the runtime\n", name);
    }
    pc_audio_set_sink(view_publish_audio);
    return 0;
}

/* Native only: the surfaces handed over are the 256x192 ones. Refused to
 * native rather than half-applied, because widening the frustum without
 * publishing the margins would be a picture that disagrees with itself. */
void pc_view_set_hd(int scale)
{
    if (scale > 1) {
        fprintf(stderr, "pc-view: PC_HD3D=%d is not supported on wasm;"
                        " rendering at native resolution\n", scale);
    }
    pc_gpu3d_set_hd(1);
    pc_gpu3d_soft_set_scale(1);
    pc_gpu2d_set_hd(1);
}

void pc_view_set_aspect(int width)
{
    if (width == PC_VIEW_ASPECT_AUTO || width > PC_VIEW_W) {
        fprintf(stderr, "pc-view: PC_ASPECT is not supported on wasm;"
                        " rendering at native width\n");
    }
    pc_gpu3d_set_wide(0);
    pc_gpu3d_soft_set_width(PC_VIEW_W);
}

/*
 * The runtime's half, read once the guest is resumed. The lid is read and
 * not applied: closing it would set the hinge bit pc_input.c keeps open on
 * every host, and the game answers that with PM_GoSleepMode (src/main.c),
 * which masks every IRQ but FIFO/timer and then spins on the VBlank count
 * (NitroSDK libraries/spi/src/pm.c, PM_GoSleepMode); the port delivers
 * VBlank only from OS_Halt, so that spin would never end and neither would
 * the frame the runtime is waiting for.
 */
static void view_wasm_input(const np_frame_desc *d)
{
    static uint32_t last_keys, last_touch, last_x, last_y;
    static int said_lid;
    uint32_t keys = d->in_keys & VIEW_WASM_KEYS;
    uint32_t touch = d->in_touch != 0;
    uint32_t x = 0, y = 0;

    if (touch) {
        x = d->in_touch_x < PC_VIEW_W ? d->in_touch_x : PC_VIEW_W - 1;
        y = d->in_touch_y < PC_VIEW_H ? d->in_touch_y : PC_VIEW_H - 1;
    }
    /* Zero is "never spoken", the page's in_seq == 0. */
    if (keys != last_keys || touch != last_touch
        || x != last_x || y != last_y) {
        last_keys = keys;
        last_touch = touch;
        last_x = x;
        last_y = y;
        pc_input_live(keys, (int)touch, x, y);
    }

    if (d->in_lid != 0 && !said_lid) {
        said_lid = 1;
        fprintf(stderr, "pc-view: lid close ignored, sleep mode is not"
                        " emulated\n");
    }

    if (d->in_quit != 0) {
        fprintf(stderr, "pc-view: the runtime asked to quit, ending the"
                        " run.\n");
        exit(0);
    }
}

void pc_view_publish(uint64_t frame)
{
    np_frame_desc *d = &pc_wasm_frame;
    const int upper = pc_video_upper_engine();

    d->screen[0] = (uint32_t)(uintptr_t)pc_video_surface(upper);
    d->screen[1] = (uint32_t)(uintptr_t)pc_video_surface(
        upper == PC_VIDEO_MAIN ? PC_VIDEO_SUB : PC_VIDEO_MAIN);
    d->width = PC_VIEW_W;
    d->height = PC_VIEW_H;
    d->stride = PC_VIEW_W;
    d->frame_lo = (uint32_t)frame;
    d->frame_hi = (uint32_t)(frame >> 32);

    d->audio_ring = (uint32_t)(uintptr_t)view_audio;
    d->audio_ring_frames = PC_VIEW_AUDIO_FRAMES;
    d->audio_head = view_audio_head;
    d->audio_rate = PC_VIEW_AUDIO_RATE;

    np_host_vblank(d);

    view_wasm_input(d);
}

#else /* !__wasm__: the shared page */


static struct pc_view_shm *view_map;

/* Is that process still running? The two spellings of one question. */
static int pc_view_pid_alive(unsigned pid)
{
#if defined(_WIN32)
    return pcw_pid_alive(pid);
#else
    /* Signal 0 checks for existence without delivering anything; EPERM means
     * a process that exists and is not ours, which is still alive. */
    if (pid == 0) return 0;
    if (kill((pid_t)pid, 0) == 0) return 1;
    return errno == EPERM;
#endif
}

/*
 * Widescreen, and what it is not. The port owns the projection, so a wider
 * picture is a wider field of view rather than stretched pixels: the clip
 * matrix's X column is scaled by 256/W and the viewport is scaled the other
 * way, which leaves the centre 256 columns exactly the native picture and
 * gives the margins world that was always there and never had pixels. All
 * of that lives in pc/hw; this file is where the extra columns reach the
 * window.
 *
 * The margins are the rasterizer's, not the 2D ENGINE'S. Only the 3D layer
 * can draw outside 256 columns; a background is a tile map that ends, and
 * a sprite's X is nine bits of hardware. So the margins carry 3D where the
 * screen is showing 3D and black everywhere else, which is what the
 * letterbox would have been.
 *
 * What it costs, stated plainly: the box test culls against the frustum
 * actually being drawn, so a game that asks "is this cube visible" gets the
 * wider answer and may keep an object a native run would have dropped. That
 * is the point of the feature and it is also the one way it can change a
 * run, which is why the instruments that compare runs refuse to start
 * beside it and why frame dumps stay native whatever this is set to.
 */
#define VIEW_ASPECT_DEADBAND 4      /* columns; a drag should not flicker */

static int view_aspect_mode;        /* 0 native, a width, or _AUTO       */
static int view_wide_debug;         /* PC_WIDE_DEBUG: report the gate     */
static int view_wide_w;             /* the width in force: 0 = native    */
static int view_hd_s = 1;           /* internal resolution: 1..HD_MAX    */

static void view_apply_width(int w)
{
    if (w <= PC_VIEW_W) {
        w = PC_VIEW_W;
    } else if (w > (int)PC_VIEW_WIDE_MAX) {
        w = (int)PC_VIEW_WIDE_MAX;
    }
    w &= ~1;    /* even, so the two margins are the same size */
    /* Both halves, always together: the geometry engine widens the frustum
     * and the rasterizer widens the surface it draws into. One without the
     * other is a picture that disagrees with itself. */
    pc_gpu3d_set_wide(w > PC_VIEW_W ? w : 0);
    pc_gpu3d_soft_set_width(w);
    view_wide_w = w > PC_VIEW_W ? w : 0;
}

/*
 * Internal resolution. Set once, before any guest code runs: the rasterizer's
 * buffers are sized from it and the published frame's shape follows, so this
 * is not something a run changes halfway through.
 */
void pc_view_set_hd(int scale)
{
    view_hd_s = (scale >= 2 && scale <= (int)PC_VIEW_HD_MAX) ? scale : 1;
    pc_gpu3d_set_hd(view_hd_s);
    pc_gpu3d_soft_set_scale(view_hd_s);
    /* The 2D engine records what the compose below needs, which native
     * pixels the 3D layer is showing at, and what it is over, and only
     * while this is on. */
    pc_gpu2d_set_hd(view_hd_s);
}

void pc_view_set_aspect(int width)
{
    view_aspect_mode = width;
    {
        const char *d = getenv("PC_WIDE_DEBUG");

        /* 1 reports every two seconds, which is enough to see what a scene
         * is doing; "all" reports every frame, which is what a flicker
         * needs, a gate that opens and closes is invisible at any
         * cadence coarser than the thing it is gating. */
        view_wide_debug = d == NULL ? 0 : (strcmp(d, "all") == 0 ? 2 : 1);
    }
    if (width == PC_VIEW_ASPECT_AUTO) {
        /* Native until a viewer says otherwise, so a run started adaptive
         * with no window is a native run rather than a guess. */
        view_apply_width(PC_VIEW_W);
    } else {
        view_apply_width(width);
    }
}

/*
 * The adaptive half: the viewer publishes the shape of the area one screen
 * is drawn into, and this turns it into the width the next frame renders
 * at. Sampled at the frame boundary, after the frame that was rendered at
 * the old width has been published, so `width` in the page always describes
 * the pixels beside it.
 */
static void view_retune_aspect(void)
{
    int want, cur;

    if (view_aspect_mode != PC_VIEW_ASPECT_AUTO || view_map == NULL) {
        return;
    }
    want = pc_view_aspect_width(view_map->in_aspect_n, view_map->in_aspect_d);
    cur = view_wide_w != 0 ? view_wide_w : PC_VIEW_W;
    if (want == cur) {
        return;
    }
    /* A window being dragged crosses a width every few pixels; re-rendering
     * at each one is free, but a picture whose field of view breathes while
     * the frame stands still is not. Move on a real change or not at all. */
    if (want > cur - VIEW_ASPECT_DEADBAND && want < cur + VIEW_ASPECT_DEADBAND
        && want != PC_VIEW_W && want != (int)PC_VIEW_WIDE_MAX) {
        return;
    }
    view_apply_width(want);
}

/*
 * The window is the session. The port has no window of its own, the viewer
 * holds it, so closing that window is a person ending the run, and a port
 * that kept going would be a process nobody can see, still holding its
 * channel, still writing its save. They pile up: the next run finds the name
 * taken and refuses, which is how this was noticed at all.
 *
 * Two ways a window goes away and both are handled. The viewer says so on
 * its way out, which is the ordinary case; and if it dies without saying
 * anything (killed, crashed, machine shutting down) the pid it published
 * stops being a live process, which is checked about once a second because
 * asking more often costs a syscall a frame to learn nothing.
 *
 * Ending the run means exit(0) and not a kill: the atexit handlers close the
 * frame dump and the WAV header, and the state report runs. The save is
 * already durable, pc_card_rom.c pwrites it when the game saves, not at
 * exit, so nothing is lost either way, but a run that ends tidily is one
 * whose instruments still say what happened.
 *
 * PC_KEEP_ALIVE=1 turns this off, for attaching a viewer to a long headless
 * run and detaching again without ending it.
 */
static void view_check_viewer_gone(void)
{
    static int keep_alive = -1;
    static unsigned watched_pid;
    static uint64_t n;
    const char *why = NULL;

    if (keep_alive < 0) {
        const char *env = getenv("PC_KEEP_ALIVE");

        keep_alive = env != NULL && env[0] != '\0' && env[0] != '0';
    }
    if (keep_alive || view_map == NULL) {
        return;
    }

    if (view_map->in_quit != 0) {
        why = "the window closed";
    } else {
        /* A pid is only worth watching once one has been published, and only
         * worth re-checking now and then. */
        if (view_map->in_viewer_pid != 0) {
            watched_pid = view_map->in_viewer_pid;
        }
        if (watched_pid != 0 && (n++ % 60) == 0
            && !pc_view_pid_alive(watched_pid)) {
            why = "the viewer is gone";
        }
    }

    if (why != NULL) {
        fprintf(stderr, "pc-view: %s, ending the run.\n", why);
        exit(0);
    }
}

/*
 * One wide frame: each screen's rows packed at `pw` columns with the native
 * surface in the centre and the rasterizer's own wide line in the margins,
 * finished through the 2D engine's own last two steps so a fade dims the
 * full width and the seam stays invisible.
 *
 * The margins are drawn only while the screen is on, engine A is showing
 * what it composed, BG0 is enabled and BG0 is the 3D layer, and only while
 * that layer is unscrolled, because the scroll is applied to the centre 256
 * alone and a scrolled margin would sit a few pixels out. Every other case
 * is black.
 */
/* Composed here first, copied into the page inside the seqlock. See
 * pc_view_publish() for why the difference matters. */
static uint32_t view_wide_buf[2][PC_VIEW_WIDE_MAX * PC_VIEW_H];

static void view_compose_wide(uint32_t pw)
{
    const uint32_t dispcnt = *(volatile uint32_t *)0x04000000u;
    const uint16_t powcnt  = *(volatile uint16_t *)0x04000304u;
    const uint16_t mb      = *(volatile uint16_t *)0x0400006Cu;
    const uint16_t bg0hofs = *(volatile uint16_t *)0x04000010u;
    const uint32_t margin  = (pw - PC_VIEW_W) / 2u;
    const uint32_t *a = pc_video_surface(0);
    const uint32_t *b = pc_video_surface(1);
    /* The scroll used to be a fourth condition here: a scrolled 3D layer
     * blacked the margins, because the shift was applied to the centre 256
     * columns alone and a scrolled margin would have sat a few pixels out.
     * The rasterizer's wide row carries the shift now, so the margins follow
     * the picture instead of blinking off; which they did for 212 of the
     * recorded replay's 25,000 frames, all of them a screen shake. */
    int margins_live = (powcnt & 1u) != 0
                       && ((dispcnt >> 16) & 3u) == 1u
                       && (dispcnt & (1u << 8)) != 0
                       && (dispcnt & (1u << 3)) != 0;
    uint32_t y, x;

    /*
     * The margins are black in two completely different situations: the
     * screen is not showing 3D at all, and the screen is showing 3D that
     * happens to draw nothing out there. They look identical in a
     * screenshot, so this reports which one it is, the gate's inputs, and
     * how many pixels of the middle row the rasterizer actually drew inside
     * the margins.
     */
    if (view_wide_debug) {
        static uint64_t n;
        int ww = 0;
        const uint32_t *l = pc_gpu3d_soft_line_wide((int)(PC_VIEW_H / 2), &ww);
        unsigned in_margin = 0, in_panel = 0;

        for (x = 0; x < (uint32_t)ww; x++) {
            if ((l[x] >> 24) == 0) continue;
            if (x < margin || x >= margin + PC_VIEW_W) in_margin++;
            else in_panel++;
        }
        if (view_wide_debug == 1 ? (n++ % 120) == 0 : (n++, 1)) {
            fprintf(stderr, "pc-wide: width=%d live=%d dispcnt=%08x"
                            " powcnt=%04x bg0hofs=%03x  mid row drawn:"
                            " %u of %u in the margins, %u of %u in the panel\n",
                    ww, margins_live, (unsigned)dispcnt, (unsigned)powcnt,
                    (unsigned)(bg0hofs & 0x1FFu), in_margin,
                    (unsigned)(pw - PC_VIEW_W), in_panel,
                    (unsigned)PC_VIEW_W);
        }
    }

    for (y = 0; y < PC_VIEW_H; y++) {
        uint32_t *d0 = &view_wide_buf[0][y * pw];
        uint32_t *d1 = &view_wide_buf[1][y * pw];
        const uint32_t *s0 = &a[y * PC_VIEW_W];
        const uint32_t *s1 = &b[y * PC_VIEW_W];
        const uint32_t *wide = NULL;
        int ww = 0;

        if (margins_live) {
            wide = pc_gpu3d_soft_line_wide((int)y, &ww);
            if ((uint32_t)ww != pw) {
                wide = NULL;    /* a width change mid-flight: black, once */
            }
        }
        for (x = 0; x < margin; x++) {
            uint32_t l = 0, r = 0;

            if (wide != NULL) {
                /* Alpha zero is "the rasterizer drew nothing here", which
                 * is the clear colour's business on the panel and black
                 * out here; there is no 2D layer behind a margin. */
                if ((wide[x] >> 24) != 0) l = pc_gpu2d_present_px(wide[x], mb);
                if ((wide[pw - 1 - x] >> 24) != 0)
                    r = pc_gpu2d_present_px(wide[pw - 1 - x], mb);
            }
            d0[x] = l;
            d0[pw - 1 - x] = r;
            d1[x] = 0;
            d1[pw - 1 - x] = 0;
        }
        for (x = 0; x < PC_VIEW_W; x++) {
            d0[margin + x] = s0[x];
            d1[margin + x] = s1[x];
        }
    }
}

/*
 * The internal resolution, and what it does and does not multiply.
 *
 * The 3D layer is rasterized at S times the DS's pixel count on each axis;
 * everything else is exactly the DS's own picture and is replicated. That
 * asymmetry is the feature rather than a shortcut: the overworld is 2D art
 * drawn for 256x192, and a filter guessing at sub-pixels would be inventing
 * detail the artists did not draw, while the 3D layer's sub-pixels are real.
 *
 * So each native pixel becomes an SxS block, and a sub-pixel takes the
 * high-resolution 3D value where the native pixel it came from is the 3D layer
 * showing through. The engine is asked rather than guessed at:
 * pc_gpu2d_hd3d_covers() answers from the layer stack the 2D engine composed,
 * and pc_gpu2d_hd3d_px() runs the sub-pixel back through the same blend unit,
 * so a translucent surface keeps its detail and a sub-pixel the polygon does
 * not reach shows the 2D that was under the layer.
 *
 * Outside the 256-column panel there is no 2D at all, so the margins are pure
 * 3D and substitute unconditionally.
 */
static uint32_t view_hd_buf[2][PC_VIEW_FRAME_WORDS];

/*
 * The HD compose rides the worker pool. It is a per-pixel pass over the
 * whole HD frame, replicate the native 2D, present the rasterizer's
 * margins, merge the panel row by row, and serial it was the largest
 * single piece of a published frame on the handheld (2 to 5 ms at scale 2).
 * Rows are independent: each writes its own span of view_hd_buf and reads
 * shared state that holds still for the frame, and the 3D rows are fetched
 * through the re-entrant spelling into a per-thread scratch. The one order
 * dependency is the render itself, which the first fetch triggers, so one
 * row is fetched BEFORE the fan-out, and the workers find it rendered.
 */
struct hd_job {
    uint32_t pw, s, hw, margin;
    uint16_t mb;
    int live;
};

static volatile int32_t hd_cursor;
static uint32_t HdScratch[PC_WORKERS_MAX]
                         [PC_VIEW_WIDE_MAX * PC_VIEW_HD_MAX];

static void hd_band(void *ctx, int slice, int nslices)
{
    const struct hd_job *J = (const struct hd_job *)ctx;
    const uint32_t pw = J->pw, s = J->s, hw = J->hw, margin = J->margin;
    uint32_t y, x, sy, sx;
    int32_t y0, yend;

    (void)nslices;
    for (;;) {
        y0 = __atomic_fetch_add(&hd_cursor, 24, __ATOMIC_ACQ_REL);
        if (y0 >= (int32_t)PC_VIEW_H) break;
        yend = y0 + 24;
        if (yend > (int32_t)PC_VIEW_H) yend = (int32_t)PC_VIEW_H;

    for (y = (uint32_t)y0; y < (uint32_t)yend; y++) {
        const uint32_t *n0 = &view_wide_buf[0][y * pw];
        const uint32_t *n1 = &view_wide_buf[1][y * pw];

        for (sy = 0; sy < s; sy++) {
            uint32_t *d0 = &view_hd_buf[0][(y * s + sy) * hw];
            uint32_t *d1 = &view_hd_buf[1][(y * s + sy) * hw];
            const uint32_t *hd = NULL;
            int hdw = 0, hds = 1;

            if (J->live) {
                hd = pc_gpu3d_soft_line_hd_r((int)(y * s + sy), &hdw, &hds,
                                             HdScratch[slice]);
                /* A width or scale change mid-flight: replicate, once. */
                if ((uint32_t)hdw != hw || (uint32_t)hds != s) hd = NULL;
            }
            /* Replicate first, both screens; that is the whole answer
             * for the sub screen, which has no 3D layer, and it is the
             * answer for every panel pixel the 3D layer is not showing at. */
            for (x = 0; x < pw; x++) {
                for (sx = 0; sx < s; sx++) {
                    d0[x * s + sx] = n0[x];
                    d1[x * s + sx] = n1[x];
                }
            }
            if (hd == NULL) {
                continue;
            }
            /* The margins are the rasterizer's alone: no 2D was ever composed
             * out here, so alpha 0 is black rather than something showing
             * through. */
            for (x = 0; x < pw; x++) {
                if (x >= margin && x < margin + PC_VIEW_W) continue;
                for (sx = 0; sx < s; sx++) {
                    uint32_t raw = hd[x * s + sx];

                    d0[x * s + sx] = (raw >> 24) != 0
                                     ? pc_gpu2d_present_px(raw, J->mb) : 0;
                }
            }
            /* ...and the panel is the 2D engine's answer, a row at a time. */
            pc_gpu2d_hd3d_row((int)y, (int)s, &hd[margin * s],
                              &d0[margin * s], J->mb);
        }
    }
    }
}

/* PC_HD_SERIAL: run the HD compose on this thread alone. A bisection knob,
 * not a mode, when a frame is wrong only with the pool live, this is the
 * flag that says whether the compose is the half that needs the pool to be
 * wrong. */
static int view_hd_serial(void)
{
    static int on = -1;

    if (on < 0) on = getenv("PC_HD_SERIAL") != NULL;
    return on;
}

static void view_compose_hd(uint32_t pw, uint32_t s)
{
    const uint32_t dispcnt = *(volatile uint32_t *)0x04000000u;
    const uint16_t powcnt  = *(volatile uint16_t *)0x04000304u;
    struct hd_job J;

    J.pw = pw;
    J.s = s;
    J.hw = pw * s;
    J.margin = (pw - PC_VIEW_W) / 2u;
    J.mb = *(volatile uint16_t *)0x0400006Cu;
    J.live = (powcnt & 1u) != 0
             && ((dispcnt >> 16) & 3u) == 1u
             && (dispcnt & (1u << 8)) != 0
             && (dispcnt & (1u << 3)) != 0;
    if (J.live) {
        int hdw = 0, hds = 1;

        /* The row fetch is what renders a due frame; do it once, on this
         * thread, so the fan-out below reads a finished picture. */
        (void)pc_gpu3d_soft_line_hd(0, &hdw, &hds);
    }
    hd_cursor = 0;
    if (view_hd_serial()) hd_band(&J, 0, 1);
    else                  pc_workers_run(hd_band, &J);
}

/*
 * PC_VIEW_DIGEST: per-band digests of the HD frame where it is composed and
 * again after the locked copy into the page, compared band by band and
 * reported WITH WHERE on a mismatch. The rasterizer has already been proved
 * equal to its serial self on the machine that smears at hd3d 4; this is
 * the instrument that arbitrates the remaining suspects: a mismatch here is
 * the publish tearing; digests that agree while the screen still smears
 * move the question to the viewer's read.
 */
#define VIEW_DIGEST_BAND 24
static uint64_t view_band_digest[2][(PC_VIEW_H + VIEW_DIGEST_BAND - 1)
                                    / VIEW_DIGEST_BAND];

static int view_digest_on(void)
{
    static int on = -1;

    if (on < 0) on = getenv("PC_VIEW_DIGEST") != NULL;
    return on;
}

static uint64_t view_fnv64(const uint32_t *w, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i;

    for (i = 0; i < n; i++) {
        h = (h ^ w[i]) * 1099511628211ull;
    }
    return h;
}

static void view_digest_compose(uint32_t hw, uint32_t s)
{
    uint32_t b, y0, y1;
    int p;

    for (p = 0; p < 2; p++) {
        for (b = 0; b * VIEW_DIGEST_BAND < PC_VIEW_H; b++) {
            y0 = b * VIEW_DIGEST_BAND;
            y1 = y0 + VIEW_DIGEST_BAND;
            if (y1 > PC_VIEW_H) y1 = PC_VIEW_H;
            view_band_digest[p][b] =
                view_fnv64(&view_hd_buf[p][(size_t)y0 * s * hw],
                           (size_t)(y1 - y0) * s * hw);
        }
    }
}

static void view_digest_page(uint64_t frame, uint32_t hw, uint32_t s)
{
    uint32_t b, y0, y1;
    uint64_t d;
    int p;

    for (p = 0; p < 2; p++) {
        for (b = 0; b * VIEW_DIGEST_BAND < PC_VIEW_H; b++) {
            y0 = b * VIEW_DIGEST_BAND;
            y1 = y0 + VIEW_DIGEST_BAND;
            if (y1 > PC_VIEW_H) y1 = PC_VIEW_H;
            d = view_fnv64((const uint32_t *)
                           &view_map->pix[p][(size_t)y0 * s * hw],
                           (size_t)(y1 - y0) * s * hw);
            if (d != view_band_digest[p][b]) {
                fprintf(stderr, "pc-view-digest: frame %llu plane %d band %u"
                        " rows %u-%u: composed %016llx, page %016llx\n",
                        (unsigned long long)frame, p, b, y0, y1 - 1,
                        (unsigned long long)view_band_digest[p][b],
                        (unsigned long long)d);
            }
        }
    }
}

/*
 * The audio half, registered with pc_audio.c as its sink, so the mixer
 * publishes samples the moment they exist (several times per frame).
 * Publishing continuously rather than at the frame boundary means a frame
 * that costs more wall time than a frame lasts still streams its audio as
 * it is mixed, the difference between a viewer riding out a slow frame
 * on its cushion and one starving through it (diamond's lesson, kept).
 */
static void view_publish_audio(void)
{
    int16_t buf[256 * 2];
    unsigned got;

    if (view_map == NULL) {
        return;
    }
    view_map->audio_rate = PC_VIEW_AUDIO_RATE;

    for (;;) {
        unsigned i;
        uint32_t head;

        got = pc_audio_read(buf, sizeof buf / (2 * sizeof buf[0]));
        if (got == 0) {
            break;
        }
        head = view_map->audio_head;
        for (i = 0; i < got; i++) {
            view_map->audio[(head + i) % PC_VIEW_AUDIO_FRAMES] =
                (uint32_t)(uint16_t)buf[i * 2 + 0] |
                ((uint32_t)(uint16_t)buf[i * 2 + 1] << 16);
        }
        __atomic_store_n(&view_map->audio_head, head + got,
                         __ATOMIC_RELEASE);
    }
}

#if defined(__BIONIC__)
/*
 * The page, inside one process.
 *
 * bionic has no shm_open and Android has no /dev/shm, and that is not an
 * oversight to route around: an app is ONE process, so there is no second
 * process to share a page with. The frame protocol does not change, the
 * same struct, the same magic, the same sequence counters, only where the
 * memory comes from and how a reader finds it.
 *
 * A name still maps to a page, because everything above this line and the
 * whole frontend address the channel by name. The table is what a filesystem
 * did on the other two hosts. It is unlocked and says why: it is written at
 * session boundaries, once, before any frame is published, and the per-frame
 * protocol lives in the page rather than here.
 *
 * pc_view_local_page() is the attach side, and it is exported because the
 * reader is in this process now: the Android frontend calls it instead of
 * opening a name. It answers NULL for a name nobody published, which is the
 * same "wait, do not fail" a POSIX attach-before-create gives that caller.
 */
#define PC_VIEW_LOCAL_MAX 4
#define PC_VIEW_LOCAL_NAME 64

static struct pc_view_local {
    char name[PC_VIEW_LOCAL_NAME];
    void *map;
    size_t size;
} sViewLocal[PC_VIEW_LOCAL_MAX];

void *pc_view_local_page(const char *name, size_t *size)
{
    int i;

    if (name == NULL) {
        return NULL;
    }
    /* A POSIX name may lead with '/' and the desktop's callers vary; match
     * the way shm_open does, on the name past it. */
    if (name[0] == '/') {
        name++;
    }
    for (i = 0; i < PC_VIEW_LOCAL_MAX; i++) {
        if (sViewLocal[i].map != NULL
            && strcmp(sViewLocal[i].name, name) == 0) {
            if (size != NULL) {
                *size = sViewLocal[i].size;
            }
            return sViewLocal[i].map;
        }
    }
    return NULL;
}

static struct pc_view_shm *pc_view_local_create(const char *name)
{
    const size_t size = sizeof(struct pc_view_shm);
    const char *key = (name[0] == '/') ? name + 1 : name;
    void *map;
    int i;

    map = pc_view_local_page(name, NULL);
    if (map != NULL) {
        return map;                     /* published twice: same page */
    }
    for (i = 0; i < PC_VIEW_LOCAL_MAX; i++) {
        if (sViewLocal[i].map == NULL) {
            break;
        }
    }
    if (i == PC_VIEW_LOCAL_MAX || strlen(key) >= PC_VIEW_LOCAL_NAME) {
        fprintf(stderr, "pc-view: no room for a channel named '%s'\n", key);
        return NULL;
    }
    /* MAP_SHARED so a fork keeps seeing the frames, which is what the page
     * meant on the other hosts and costs nothing here. */
    map = mmap(NULL, size, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "pc-view: mmap: %s\n", strerror(errno));
        return NULL;
    }
    strcpy(sViewLocal[i].name, key);
    sViewLocal[i].map = map;
    sViewLocal[i].size = size;
    return map;
}
#endif /* __BIONIC__ */

int pc_view_init(void)
{
    const char *name = getenv("PC_VIEW");
#if !defined(_WIN32) && !defined(__BIONIC__)
    int fd;
#endif

    if (name == NULL || name[0] == '\0') {
        return 0;
    }
#if defined(_WIN32)
    {
        /* A named Windows file mapping (Local\<name>); the viewer opens
         * the same name. The handle is deliberately never closed, the
         * mapping lives exactly as long as the port. */
        void *handle = NULL;

        view_map = pcw_shm_create(name, sizeof(struct pc_view_shm), &handle);
        if (view_map == NULL) {
            fprintf(stderr, "pc-view: %s\n", pcw_ipc_error());
            return -1;
        }
    }
#elif defined(__BIONIC__)
    view_map = pc_view_local_create(name);
    if (view_map == NULL) {
        return -1;
    }
#else
    fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        fprintf(stderr, "pc-view: shm_open(%s): %s\n", name, strerror(errno));
        return -1;
    }
    if (ftruncate(fd, (off_t)sizeof(struct pc_view_shm)) != 0) {
        fprintf(stderr, "pc-view: ftruncate: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    view_map = mmap(NULL, sizeof(struct pc_view_shm),
                    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (view_map == MAP_FAILED) {
        view_map = NULL;
        fprintf(stderr, "pc-view: mmap: %s\n", strerror(errno));
        return -1;
    }
#endif

    /*
     * Is somebody already publishing here? A page left behind by a port that
     * has exited is fine to reuse and is the normal case on Linux, where the
     * shm object outlives the process. A port that is still RUNNING is not:
     * Both would write this page every frame, and the reader would assemble
     * one picture out of two games. Across a protocol version it is worse
     * than that; an older port does not maintain the fields this one
     * publishes, so the reader takes a row width out of what the other port
     * considers pixel data and reads the whole frame at the wrong stride.
     *
     * The two cases differ in exactly one observable way: a live publisher
     * advances `seq`. So look, wait a couple of frames, and look again.
     */
    if (view_map->magic == PC_VIEW_MAGIC) {
        uint32_t before = view_map->seq;
        struct timespec pause = { 0, 40 * 1000 * 1000 };  /* ~2.5 frames */

        view_sleep(&pause);
        if (view_map->seq != before) {
            fprintf(stderr,
                    "pc-view: another port (pid %u) is already publishing on"
                    " '%s'. Two ports on one channel produce a torn picture"
                    " assembled from both. Close that one, or give this run a"
                    " channel of its own with PC_VIEW=<other-name>.\n",
                    (unsigned)view_map->publisher, name);
            view_map = NULL;
            return -1;
        }
    }

    /* Zero the input channel before stamping the magic: a viewer that
     * attaches the instant the magic appears must find in_seq == 0. */
    view_map->in_seq = 0;
    view_map->in_keys = 0;
    view_map->in_touch = 0;
    view_map->in_aspect_n = 0;
    view_map->in_aspect_d = 0;
    view_map->seq = 0;
    /* Before the first frame, so a viewer that attaches on the magic reads
     * a width rather than a zero. */
    view_map->width = (uint32_t)(view_wide_w != 0 ? view_wide_w : PC_VIEW_W)
                      * (uint32_t)view_hd_s;
    view_map->height = PC_VIEW_H * (uint32_t)view_hd_s;
    view_map->version = PC_VIEW_VERSION;
#if defined(_WIN32)
    view_map->publisher = (uint32_t)pcw_self_pid();
#else
    view_map->publisher = (uint32_t)getpid();
#endif
    __sync_synchronize();
    view_map->magic = PC_VIEW_MAGIC;

    pc_audio_set_sink(view_publish_audio);

#if defined(_WIN32)
    fprintf(stderr, "pc-view: publishing frames at Local\\%s"
                    " (run build\\pc-win32\\pcview.exe %s to watch)\n",
            name, name);
#else
    fprintf(stderr, "pc-view: publishing frames at /dev/shm/%s%s\n",
            name[0] == '/' ? name + 1 : name,
            " (run build/pc/pcview to watch)");
#endif
    return 0;
}

/*
 * The pacer's frame skip, and why it exists: the mixer emits one frame's
 * audio per simulated frame, so a run below 60 fps produces audio slower
 * than the viewer plays it and the ring runs dry, heard as a dropout every
 * few seconds, and misdiagnosed as an audio bug four separate times before
 * the rate was measured. At --hd3d 2 and above the rasterizer IS the frame
 * (new-game replay at scale 4: 15.4 of 19.3 ms, 79.8%), so when the pacer
 * misses a deadline there, the next frame keeps the previous 3D picture
 * (pc_gpu3d_soft_skip_next) and skips the compose and publish below, the
 * simulation, the mixer and the input sampling still run at full rate, and
 * only the picture repeats. Never at native resolution, where the suite
 * pins exact behaviour and a machine this slow has a different problem; and
 * never more than three in a row, so the picture keeps moving (15 fps at
 * worst) even when the machine cannot hold 60 whatever this drops.
 */
static int view_skip_next;      /* decided by the pacer, consumed below */

/*
 * Compose first, then take the lock, and this is not a micro-optimisation.
 * `seq` is odd while the port writes, and a reader that finds it odd for
 * its whole attempt gives up. The native path was two memcpys, so that
 * window was tens of microseconds and a collision was rare and invisible.
 * Building a wide frame in place, a per-pixel loop over both screens,
 * held the lock an order of magnitude longer, and a reader that lost the
 * race now had a WIDTH and PIXELS from different frames: rows read at the
 * wrong stride, which is a picture sheared diagonally rather than a
 * slightly stale one. That is the flash every few seconds.
 *
 * So the wide frame is composed into a buffer of its own, and the lock
 * covers exactly what it covered before: two memcpys.
 */
#if defined(__SSE2__)
#include <emmintrin.h>
/*
 * The page is written once a frame and read by another process; this side
 * never loads it back. A cached copy therefore evicts a page's worth of
 * useful lines per frame for data with no second use here, and the -m32
 * glibc memcpy stays temporal at these sizes, measured on the 4x ladder,
 * the locked copies were 0.68 ms a frame plain and 1.0 ms wide (a tenth of
 * the frame); streaming stores take 13-20% off the copy itself and the
 * frame gains more than the copy saved (fps +2.5% at 4x, +1.2% at 3x wide,
 * interleaved pairs), which is the eviction not happening. The page's pixel arrays
 * sit 8 bytes off a 16-byte boundary (ten header words, and the layout is
 * the protocol, so it cannot be padded): a scalar head aligns the
 * DESTINATION, which is the side movntdq cares about, and the loads stay
 * unaligned. The seqlock's closing __sync_synchronize() is an mfence, so
 * the reader's ordering does not rest on the sfence here; that is only
 * the helper being correct on its own.
 */
static void view_copy_stream(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    size_t head = (size_t)(-(uintptr_t)d & 15);

    if (head > n) head = n;
    if (head != 0) { memcpy(d, s, head); d += head; s += head; n -= head; }

    while (n >= 64) {
        __m128i a = _mm_loadu_si128((const __m128i *)(s +  0));
        __m128i b = _mm_loadu_si128((const __m128i *)(s + 16));
        __m128i c = _mm_loadu_si128((const __m128i *)(s + 32));
        __m128i e = _mm_loadu_si128((const __m128i *)(s + 48));

        _mm_stream_si128((__m128i *)(d +  0), a);
        _mm_stream_si128((__m128i *)(d + 16), b);
        _mm_stream_si128((__m128i *)(d + 32), c);
        _mm_stream_si128((__m128i *)(d + 48), e);
        d += 64; s += 64; n -= 64;
    }
    while (n >= 16) {
        _mm_stream_si128((__m128i *)d, _mm_loadu_si128((const __m128i *)s));
        d += 16; s += 16; n -= 16;
    }
    if (n != 0) memcpy(d, s, n);
    _mm_sfence();
}
#else
/* The other hosts keep libc's answer: NEON has no write-around store this
 * shape, and the 3DS never publishes a page at all. */
#define view_copy_stream(dst, src, n) memcpy((dst), (src), (n))
#endif

static void view_publish_frame(uint64_t frame)
{
    {
        const uint32_t pw = (uint32_t)(view_wide_w != 0 ? view_wide_w
                                                        : PC_VIEW_W);

        /* HD composes from the native wide frame, so it needs one even at
         * 256 columns, where it is two memcpys' worth of work and only
         * happens when HD is on at all. */
        if (view_wide_w != 0 || view_hd_s > 1) {
            view_compose_wide(pw);
        }
        if (view_hd_s > 1) {
            view_compose_hd(pw, (uint32_t)view_hd_s);
            if (view_digest_on()) {
                view_digest_compose(pw * (uint32_t)view_hd_s,
                                    (uint32_t)view_hd_s);
            }
        }
    }

    PC_BENCH_BEGIN(bench_t);
    view_map->seq++;                    /* odd: writer busy */
    __sync_synchronize();

    if (view_hd_s > 1) {
        const uint32_t pw = (uint32_t)(view_wide_w != 0 ? view_wide_w
                                                        : PC_VIEW_W);
        const size_t n = (size_t)pw * view_hd_s * PC_VIEW_H * view_hd_s;

        view_copy_stream(view_map->pix[0], view_hd_buf[0], n * sizeof view_hd_buf[0][0]);
        view_copy_stream(view_map->pix[1], view_hd_buf[1], n * sizeof view_hd_buf[1][0]);
        view_map->width = pw * (uint32_t)view_hd_s;
        view_map->height = PC_VIEW_H * (uint32_t)view_hd_s;
        if (view_digest_on()) {
            view_digest_page(frame, pw * (uint32_t)view_hd_s,
                             (uint32_t)view_hd_s);
        }
    } else if (view_wide_w != 0) {
        view_copy_stream(view_map->pix[0], view_wide_buf[0],
               (size_t)view_wide_w * PC_VIEW_H * sizeof view_wide_buf[0][0]);
        view_copy_stream(view_map->pix[1], view_wide_buf[1],
               (size_t)view_wide_w * PC_VIEW_H * sizeof view_wide_buf[1][0]);
        view_map->width = (uint32_t)view_wide_w;
        view_map->height = PC_VIEW_H;
    } else {
        /* Rows pack at 256 here, which is the first 256*192 words of an
         * array sized for the widest frame; a native frame is byte for
         * byte the frame this channel has always carried. */
        view_copy_stream(view_map->pix[0], pc_video_surface(0),
               (size_t)PC_VIEW_W * PC_VIEW_H * sizeof view_map->pix[0][0]);
        view_copy_stream(view_map->pix[1], pc_video_surface(1),
               (size_t)PC_VIEW_W * PC_VIEW_H * sizeof view_map->pix[1][0]);
        view_map->width = PC_VIEW_W;
        view_map->height = PC_VIEW_H;
    }
    {
        extern int pc_tp_sampling(void);

        view_map->touch_wanted = (uint32_t)(pc_tp_sampling() != 0);
    }
    view_map->frame_lo = (uint32_t)frame;
    view_map->frame_hi = (uint32_t)(frame >> 32);
    view_map->upper_engine = (uint32_t)pc_video_upper_engine();

    __sync_synchronize();
    view_map->seq++;                    /* even: stable */
    PC_BENCH_END(PC_BENCH_PUBLISH, bench_t);
}

/*
 * The late-frame autopsy. `--trace-pace` could say that three frames in a
 * hundred still missed the 60 Hz deadline once the governor stopped sleeping
 * in system ticks. It could not say what they spent the time on, and a count
 * with no attribution leaves a guess standing where a measurement belongs.
 *
 * So the trace splits the frame period into the four things it can be, and
 * they sum to it exactly:
 *
 *   work    the guest resuming to the picture being handed over
 *   publish the wide and HD compose and the two memcpys into the shared page
 *   pace    the pacer's own arithmetic, which should be noise
 *   slack   whatever was left of the 16.67 ms to sleep
 *
 * and it carries a fifth, `slip`, belonging to the previous period: how much
 * later the wait returned than the deadline it was given.
 *
 * The algebra is exact, which is why the split is worth taking:
 *
 *   ahead = 16.67ms - (slip[previous] + work + publish + pace)
 *
 * so a frame is late when those four exceed the budget and nothing else can
 * make it late. The mean of each over the late frames alone is the point;
 * whole-run means are dominated by the frames that were fine.
 */
static struct {
    unsigned long long work, pub, pace, slack, slip;
    unsigned long long max_work, max_pub, max_slip;
    unsigned long long late_work, late_pub, late_slip;
    unsigned long long worst, worst_work, worst_pub, worst_slip;
    unsigned long long worst_3d, worst_2d, worst_audio, worst_io;
    unsigned late_n;
} view_pace;

static long long view_ns(const struct timespec *a, const struct timespec *b)
{
    return (long long)(b->tv_sec - a->tv_sec) * 1000000000LL
           + (long long)(b->tv_nsec - a->tv_nsec);
}

/* PC_TRACE_PACE, read once. Only the accounting above is gated on it, the
 * publish marks themselves are taken every frame, because the skip decides
 * on them. */
static int view_pace_traced(void)
{
    static int on = -1;

    if (on < 0) on = getenv("PC_TRACE_PACE") != NULL;
    return on;
}

void pc_view_publish(uint64_t frame)
{
    static uint32_t last_in_seq;
    int skip = view_skip_next;
    int traced = view_pace_traced();
    struct timespec pub0 = { 0, 0 }, pub1 = { 0, 0 };

    view_skip_next = 0;
    if (view_map == NULL) {
        return;
    }

    /* Taken every frame, not only when traced: the pacer's skip decides on
     * the publish cost below, and a decision may not depend on whether a
     * diagnostic was asked for. */
    clock_gettime(CLOCK_MONOTONIC, &pub0);
    if (!skip) {
        view_publish_frame(frame);
    }
    clock_gettime(CLOCK_MONOTONIC, &pub1);

    /* The input channel: viewer-owned words, sampled here. Applying at the
     * publish point is one frame boundary after the script's application
     * point (top of OS_Halt), which is the same place a console's ARM7
     * refresh could land relative to a frame. Live input wins over the
     * script only once a viewer has actually spoken. */
    if (view_map->in_seq != 0 && view_map->in_seq != last_in_seq) {
        last_in_seq = view_map->in_seq;
        pc_input_live(view_map->in_keys,
                      (int)view_map->in_touch,
                      view_map->in_touch_x,
                      view_map->in_touch_y);
    }

    /* The window's shape, sampled here for the same reason and applied to
     * the frame after this one. */
    view_retune_aspect();

    /* And whether there is still a window. */
    view_check_viewer_gone();

    /* Pacing, and ONLY here: the port renders ~4x realtime unthrottled,
     * which headless instruments want and a human at a window does not.
     * A windowed session is already nondeterministic (a human is typing
     * into it), so holding 60 fps against the host clock costs nothing
     * the channel had. PC_PACE=0 turns it off. */
    {
        static int pace = -1;
        static int turbo_held = 0;
        static struct timespec next;
        /* The late-frame counter; see the note where it is reported. */
        static unsigned paced, late, skipped;
        static int said_slow;
        /* The autopsy's carry: when the guest last resumed, how late that
         * resume was, and whether either holds a real measurement yet. */
        static struct timespec wake;
        static long long slip;
        static int marked;
        static unsigned long long bench_mark[PC_BENCH_N];
        /* What dropping a picture would save, from the last frame that drew
         * one; see where the skip is decided. */
        static unsigned long long picture_ns, render_mark;

        if (pace < 0) {
            const char *env = getenv("PC_PACE");
            pace = (env == NULL || env[0] != '0');
            if (pace) clock_gettime(CLOCK_MONOTONIC, &next);
        }
        /* Held fast-forward. The pacer is the only thing standing between
         * this port and its unthrottled rate, so the whole feature is
         * declining to sleep; nothing about the frame changes. The clock
         * is re-anchored on release, or the first paced frame after a long
         * hold would think it was minutes behind and sprint. */
        if (view_map != NULL && view_map->in_turbo) {
            turbo_held = 1;
        } else if (turbo_held) {
            turbo_held = 0;
            /* The held frames are not a period, and the rasterize they ran
             * is not one frame's worth: both carries are dropped rather than
             * charged to the frame that happens to come next. */
            marked = 0;
            picture_ns = 0;
            render_mark = pc_gpu3d_render_ns();
            if (pace) clock_gettime(CLOCK_MONOTONIC, &next);
        }

        if (pace && !turbo_held) {
            struct timespec now;

            next.tv_nsec += 16666667L;      /* 1/60 s */
            if (next.tv_nsec >= 1000000000L) {
                next.tv_nsec -= 1000000000L;
                next.tv_sec += 1;
            }
            /* Fell more than ~6 frames behind (a slow scene, a paused
             * debugger): re-anchor instead of sprinting to catch up. */
            clock_gettime(CLOCK_MONOTONIC, &now);
            {
                long long ahead = (next.tv_sec - now.tv_sec) * 1000000000LL
                                  + (next.tv_nsec - now.tv_nsec);

                /* The period that just ended, split. Taken here because
                 * everything but the sleep has happened by now, and the
                 * sleep is the one part the budget is not spent on. */
                if (traced && marked) {
                    long long w = view_ns(&wake, &pub0);
                    long long p = view_ns(&pub0, &pub1);
                    long long c = view_ns(&pub1, &now);

                    if (w < 0) w = 0;
                    if (p < 0) p = 0;
                    if (c < 0) c = 0;
                    /* The frame boundary into the profiler's stream, with
                     * this period's work; what lets prof_fold.py keep
                     * only the spike frames' samples, from the
                     * 2026-08-27 stutter finding. A no-op without
                     * PC_PROF. */
                    pc_prof_frame((uint32_t)(w / 1000));
                    view_pace.work += (unsigned long long)w;
                    view_pace.pub += (unsigned long long)p;
                    view_pace.pace += (unsigned long long)c;
                    view_pace.slip += slip > 0 ? (unsigned long long)slip : 0ull;
                    if ((unsigned long long)w > view_pace.max_work) {
                        view_pace.max_work = (unsigned long long)w;
                    }
                    if ((unsigned long long)p > view_pace.max_pub) {
                        view_pace.max_pub = (unsigned long long)p;
                    }
                    if (slip > 0
                        && (unsigned long long)slip > view_pace.max_slip) {
                        view_pace.max_slip = (unsigned long long)slip;
                    }
                    if (ahead > 0) {
                        view_pace.slack += (unsigned long long)ahead;
                    } else {
                        /* The one frame that explains the window: the
                         * biggest overrun, kept with its own breakdown
                         * rather than with the window's averages. */
                        unsigned long long over =
                            (unsigned long long)(-ahead);

                        view_pace.late_n++;
                        view_pace.late_work += (unsigned long long)w;
                        view_pace.late_pub += (unsigned long long)p;
                        view_pace.late_slip +=
                            slip > 0 ? (unsigned long long)slip : 0ull;
                        if (over > view_pace.worst) {
                            view_pace.worst = over;
                            view_pace.worst_work = (unsigned long long)w;
                            view_pace.worst_pub = (unsigned long long)p;
                            view_pace.worst_slip =
                                slip > 0 ? (unsigned long long)slip : 0ull;
                            view_pace.worst_3d =
                                pc_bench_span_ns(PC_BENCH_3D)
                                - bench_mark[PC_BENCH_3D];
                            view_pace.worst_2d =
                                pc_bench_span_ns(PC_BENCH_2D)
                                - bench_mark[PC_BENCH_2D];
                            view_pace.worst_audio =
                                pc_bench_span_ns(PC_BENCH_AUDIO)
                                - bench_mark[PC_BENCH_AUDIO];
                            view_pace.worst_io =
                                pc_bench_span_ns(PC_BENCH_IO)
                                - bench_mark[PC_BENCH_IO];
                        }
                    }
                }

                if (ahead < -100000000LL) {
                    next = now;         /* fell behind: re-anchor, no sprint */
                } else if (ahead > 0) {
                    /* Relative sleep rather than TIMER_ABSTIME: mingw's
                     * winpthreads has nanosleep but not clock_nanosleep.
                     *
                     * How coarsely this can sleep was measured twice and the
                     * first answer was wrong. Comparing total elapsed time
                     * over 1800 frames said the Windows tick cost nothing;
                     * a sum cannot see per-frame jitter, and back then a
                     * frame took most of the budget anyway. Once the
                     * renderers got fast the same tick was three-quarters of
                     * every miss; pc/src/pc_win_clock.c has the distribution
                     * and the waitable timer that answers it. The `slip`
                     * column below is what that measurement looks like from
                     * in here, so it cannot go unnoticed a third time. */
                    struct timespec rem = { (time_t)(ahead / 1000000000LL),
                                            (long)(ahead % 1000000000LL) };
                    view_sleep(&rem);
                }

                /* The carry for the next period: when the guest resumes and
                 * how late that is against the deadline it waited for. A
                 * frame that did not sleep still slips; the overrun is
                 * debt against the next frame's budget, not free. */
                if (traced) {
                    int i;

                    clock_gettime(CLOCK_MONOTONIC, &wake);
                    slip = view_ns(&next, &wake);
                    for (i = 0; i < PC_BENCH_N; i++) {
                        bench_mark[i] = pc_bench_span_ns(i);
                    }
                    marked = 1;
                }

                /*
                 * The rate counters, and they count at EVERY resolution.
                 * Only the skip below is native-exempt; whether this machine
                 * is holding 60 fps is worth answering at native too, since
                 * the mixer's rate rides on it either way, and while these
                 * lived inside the skip's own guard, `--trace-pace` at
                 * --hd3d 1 reported nought of nought frames late whatever
                 * the truth was.
                 */
                paced++;
                if (ahead <= 0) late++;

                /*
                 * What dropping the next picture would save, and it is
                 * exactly two things: the rasterize this frame paid for and
                 * the publish below it. The 2D compose is not one of them;
                 * it runs whether or not a window is watching, so that a
                 * frame dump cannot change the run.
                 *
                 * Measured on a frame that DREW a picture. A skipped frame's
                 * own figures are the saving already taken; reading those
                 * back would say a skip saves nothing and refuse the next
                 * one, which is precisely the case the three-in-a-row cap
                 * exists to allow.
                 */
                {
                    unsigned long long r = pc_gpu3d_render_ns();

                    if (!skip) {
                        long long p = view_ns(&pub0, &pub1);

                        picture_ns = (r - render_mark)
                                     + (p > 0 ? (unsigned long long)p : 0ull);
                    }
                    render_mark = r;
                }

                /*
                 * The skip decision; see view_skip_next above. Made here
                 * because `ahead` is this frame's verdict: at or past the
                 * deadline means the machine did not afford this frame's
                 * picture, so the next frame drops its picture rather than
                 * its pace.
                 *
                 * And only when dropping it would have helped. `ahead <= 0`
                 * alone cannot tell a frame the rasterizer made expensive
                 * from a frame the game made expensive, and the two want
                 * opposite answers. Measured: the frames that missed the
                 * deadline spent 17 ms on average and 102 ms at worst, of
                 * which the rasterize was about 1 ms. That is a map load, not
                 * a picture, and dropping three pictures there saves four
                 * milliseconds while the player watches the game stay stopped
                 * for three frames longer.
                 *
                 * So the picture has to be a real part of why the frame went
                 * over, a quarter of it or more.
                 */
                if (view_hd_s > 1) {
                    static unsigned run;

                    if (ahead <= 0 && run < 3u
                        && picture_ns * 4ull >= (unsigned long long)(-ahead)) {
                        run++;
                        skipped++;
                        view_skip_next = 1;
                        pc_gpu3d_soft_skip_next();
                    } else {
                        run = 0;
                    }
                }

                /*
                 * A higher internal resolution than the machine can hold is
                 * The one way this setting fails silently, so it says so.
                 *
                 * --hd3d N does N^2 the rasterizer's pixel work, and a
                 * machine that cannot keep up does not fall over; it just
                 * runs the game slowly, with nothing on screen to say why.
                 * That silent failure is the reason the default is 1 and it
                 * is the reason a ceiling of 4 is safe to offer: five
                 * seconds of missed deadlines and the port names the setting
                 * that is costing them.
                 *
                 * Only above 1, because a machine that cannot hold 60 fps at
                 * the DS's own resolution has a different problem and this
                 * would be blaming the wrong thing. Once, because it is a
                 * hint and not a monitor; a scene that is briefly heavy is
                 * absorbed by the window rather than reported.
                 */
                {
                    /*
                     * PC_TRACE_PACE: the rate this run is actually keeping,
                     * every 300 paced frames. The vblank heartbeat says a run
                     * is alive; only this says whether it is keeping time,
                     * and that is the question every audio complaint has
                     * turned out to be, the mixer emits one frame's audio
                     * per frame, so a run below 60 makes audio slow and the
                     * window's ring runs dry.
                     */
                    static struct timespec mark;
                    static unsigned since;

                    if (traced && ++since >= 300u) {
                        double dt = (double)(now.tv_sec - mark.tv_sec)
                            + (double)(now.tv_nsec - mark.tv_nsec) / 1e9;
                        double n = (double)since;

                        if (mark.tv_sec != 0) {
                            fprintf(stderr, "pc-pace: %.1f fps over %.2fs"
                                            " (%u of %u frames late,"
                                            " %u pictures skipped)\n",
                                    dt > 0.0 ? since / dt : 0.0, dt,
                                    late, paced ? paced : since, skipped);
                            fprintf(stderr, "pc-pace:   every frame  work"
                                            " %.2f (max %.2f)  publish %.2f"
                                            " (max %.2f)  slip %.2f (max"
                                            " %.2f)  slack %.2f ms\n",
                                    view_pace.work / 1e6 / n,
                                    view_pace.max_work / 1e6,
                                    view_pace.pub / 1e6 / n,
                                    view_pace.max_pub / 1e6,
                                    view_pace.slip / 1e6 / n,
                                    view_pace.max_slip / 1e6,
                                    view_pace.slack / 1e6 / n);
                            if (view_pace.late_n != 0) {
                                double ln = (double)view_pace.late_n;

                                fprintf(stderr, "pc-pace:   the %u late "
                                                " work %.2f  publish %.2f "
                                                " slip %.2f ms\n",
                                        view_pace.late_n,
                                        view_pace.late_work / 1e6 / ln,
                                        view_pace.late_pub / 1e6 / ln,
                                        view_pace.late_slip / 1e6 / ln);
                                fprintf(stderr, "pc-pace:   worst miss %.2f"
                                                " ms over: work %.2f +"
                                                " publish %.2f + slip %.2f",
                                        view_pace.worst / 1e6,
                                        view_pace.worst_work / 1e6,
                                        view_pace.worst_pub / 1e6,
                                        view_pace.worst_slip / 1e6);
                                /* PC_BENCH as well: which part of `work`,
                                 * for the one frame that explains the
                                 * window. Without it the two are silent
                                 * rather than zero, which is honest. */
                                if (pc_bench_on) {
                                    fprintf(stderr, "  [3D %.2f  2D %.2f "
                                                    " audio %.2f  card %.2f]",
                                            view_pace.worst_3d / 1e6,
                                            view_pace.worst_2d / 1e6,
                                            view_pace.worst_audio / 1e6,
                                            view_pace.worst_io / 1e6);
                                }
                                fputc('\n', stderr);
                            }
                            fflush(stderr);
                        }
                        memset(&view_pace, 0, sizeof view_pace);
                        mark = now;
                        since = 0;
                    }
                }
                /*
                 * With the skip carrying the load, chronic lateness means
                 * the machine cannot hold 60 even dropping three pictures
                 * in four; heavy skipping means it is holding 60 by showing
                 * a visibly reduced frame rate. Either way the honest
                 * advice is the same and it is said once.
                 */
                if (paced >= 300u) {
                    if (view_hd_s > 1 && !said_slow
                        && (late * 2u > paced || skipped * 3u > paced)) {
                        said_slow = 1;
                        fprintf(stderr,
                                "pokeplatinum-pc: this machine cannot draw"
                                " every frame at --hd3d %d (last %u frames:"
                                " %u late, %u pictures skipped to keep the"
                                " game and its sound at full speed). Step"
                                " down to --hd3d %d; the 2D art is the same"
                                " picture at every setting, so only the 3D"
                                " layer's sharpness changes.\n",
                                view_hd_s, paced, late, skipped,
                                view_hd_s - 1);
                    }
                    paced = late = skipped = 0;
                }
            }
        }
    }
}

#endif /* !__wasm__ */
