/*
 * 3ds/src/3ds_gpu.h: the picture, on the PICA.
 *
 * The GPU present. The CPU blit in 3ds_view.c rotated and converted two 256x192 DS
 * surfaces into the two framebuffers a pixel at a time: 24.7 ms a frame on
 * hardware, measured, and nearly constant because it is a fixed-size
 * memory-to-memory copy. This file does the same picture with two textured
 * quads and leaves the CPU with a straight copy into linear memory.
 *
 * The picture must not change. That is the whole of the GPU present: the same pixels the
 * blit produced, produced by the GPU, so the tasks after it can change how the
 * picture is *built* against a path already known to present it unaltered.
 * PRESENT_VERIFY is how that claim is checked rather than argued; see
 * gpu_verified() below.
 *
 * What stays on the CPU. 3ds_view.c keeps its blit and its font, and the two
 * paths that draw without a game under them keep using it: the fault handler
 * and the hang detector. Both run when the port is already broken:
 * One from a signal handler, one from libctru's VBlank callback on another
 * thread, and neither may touch a GPU frame the main thread might be inside.
 * A CPU write into the framebuffer still reaches the LCD after citro3d has
 * been drawing, because no frame is in flight by the time either of them runs.
 */

#ifndef POKEPLATINUM_3DS_GPU_H
#define POKEPLATINUM_3DS_GPU_H

#include <stddef.h>
#include <stdint.h>

/*
 * Which of the two present paths runs, read once at start-up from
 * sdmc:/3ds/pokeplatinum/present.txt; the port's own convention for a knob
 * a tester can turn on a console with no command line (REPLAY_PATH, and the
 * reports beside it).
 *
 *   gpu      the ship path: the PICA draws, the CPU blit is not called
 *   cpu      the old path, kept so a console can be asked both questions
 *   verify   the GPU path, and every frame compared with what the CPU blit
 *            would have written, byte for byte, into present-report.txt
 *
 * An absent or unreadable file is `gpu`. A file naming something else is a
 * refusal with a line on stderr rather than a silent fallback: a knob that
 * quietly does the other thing is how a measured run gets measured wrong.
 */
enum {
    PRESENT_GPU = 0,
    PRESENT_CPU,
    PRESENT_VERIFY
};

#define PRESENT_PATH        "sdmc:/3ds/pokeplatinum/present.txt"
#define PRESENT_REPORT_PATH "sdmc:/3ds/pokeplatinum/present-report.txt"
/* The first frame the tile path got wrong, both ways, as two PPMs. */
#define PRESENT_WANT_PATH   "sdmc:/3ds/pokeplatinum/bg-want.ppm"
#define PRESENT_GOT_PATH    "sdmc:/3ds/pokeplatinum/bg-got.ppm"

/*
 * Bring up citro3d: two render targets, the shader, the two surface textures
 * and the font atlas. Returns 0 if the GPU path is live and -1 if it is not,
 * in which case every present falls back to the CPU blit and says so once.
 *
 * Call it after gfxInitDefault() AND BEFORE watchdog_arm(). C3D_Init installs
 * its own GSPGPU_EVENT_VBlank0 callback and libctru keeps exactly one callback
 * per event, so whichever of the two runs last owns that slot. This port wants
 * the hang detector there: citro3d's callback feeds C3D_FrameSync and
 * C3D_FrameRate, and this file uses neither; the frame is paced by
 * gspWaitForVBlank in 3ds_frame.c, the way it was before the GPU arrived.
 */
int gpu_init(void);

/* Whether gpu_init() succeeded and the mode asked for the GPU at all. */
int gpu_ready(void);

/* What present.txt asked for, and its name for a report or a screen. */
int gpu_mode(void);
const char *gpu_mode_name(void);

/*
 * One frame, from the same two surfaces and two letterbox lines view_present()
 * takes. Either surface may be NULL, which draws that screen's letterbox and
 * text alone.
 *
 * Does not swap and does not wait: C3D_FrameEnd hands the frame to the GPU and
 * returns, the swap happens in citro3d's queue callback when the transfer
 * lands, and 3ds_frame.c still owns the pacing.
 */
void gpu_present(const uint32_t *top, const uint32_t *bottom,
                 const char *line_top, const char *line_bottom, int upper);

/* ------------------------------------------------------------------ */
/* What the background path draws with                                 */
/* ------------------------------------------------------------------ */
/*
 * The vertex buffer is this file's, and the 2D renderer writes into it: one
 * buffer, one attribute layout and one shader for every quad in the frame,
 * which is what lets a screen's picture and its letterbox text be two draws
 * out of one submission.
 */
struct gpu_vertex {
    float x, y, z;
    float u, v;
};

/* `count` vertices, or NULL when the frame has asked for more than the buffer
 * holds; which the caller must treat as "this engine is not drawable this
 * frame" rather than as a short picture. */
struct gpu_vertex *gpu_vertex_alloc(int count);
int gpu_vertex_count(void);

/* Push a CPU-written range out of the data cache, for memory the GPU is about
 * to read. Wraps the libctru call so 3ds_gpu2d.c does not include <3ds.h>. */
void gpu_cache_flush(const void *p, size_t bytes);

/*
 * PRESENT_VERIFY's counters: frames compared, frames byte-identical, and the
 * first disagreement's screen, coordinate and colours. `frames` is zero in the
 * other two modes, which is what tells a report reader that nothing was
 * checked rather than that nothing was wrong.
 */
struct gpu_verdict {
    unsigned long frames;
    unsigned long equal;
    /* ...and the same two counted over the screens the background path composed out of tiles
     * alone, because a run where the GPU 2D path never came up would otherwise
     * pass this check by not being tested. */
    unsigned long tile_frames;
    unsigned long tile_equal;
    unsigned long pixels;      /* differing bytes over every compared frame */
    int first_screen;          /* 0 upper, 1 lower, -1 if none             */
    int first_tiles;           /* ...and whether that screen was tile-drawn */
    int first_x, first_y;
    uint32_t first_want, first_got;
    /* ...and the first one on a screen the background path composed, which is a different
     * question from the first one anywhere: a coordinate and two colours out
     * of the tile path is what says whether a wrong picture there is a wrong
     * tile, a wrong palette or a wrong corner of the atlas. */
    int tile_first;
    int tile_x, tile_y;
    uint32_t tile_want, tile_got;
    /*
     * The effect pass and why this check stopped being one number. Everything the PICA
     * composed up to the 3D layer was the software renderer's own arithmetic moved to
     * the GPU (a texel copied, a colour replaced) so the claim was that
     * the two framebuffers are equal and any differing byte is a bug. BLDCNT's
     * alpha blend cannot be that: the DS blends in sixteenths of six bits and
     * the PICA's blend unit works in 255ths of eight, and no arrangement of
     * its factors is exact. So a frame that ran the blend unit is counted
     * apart and the claim over it is a BOUND, no channel further than one
     * six-bit step, which is four of 255. A frame that did not is held to the
     * old claim, unchanged.
     */
    unsigned long exact_frames;
    unsigned long exact_equal;
    unsigned long approx_frames;
    unsigned long approx_pixels;
    int approx_worst;          /* the widest channel gap on those frames */
    /*
     * The 3D rasterizer and a third kind of frame. The effect pass's two classes are both about the
     * PICA doing the software renderer's arithmetic, exactly, or one
     * six-bit step out. A frame whose 3D layer the PICA RASTERIZED is neither:
     * The DS truncates its depth gradient before interpolating and
     * approximates the perspective curve with eight bits, and this renderer
     * does not mark edges at all, so whole pixels differ rather than shades of
     * them. Counted apart because averaging it into either of the other two
     * would hide both, and reported without a bound, because what the bound
     * should be is exactly what the 3D rasterizer's pixel diff is for.
     */
    unsigned long l3d_frames;
    unsigned long l3d_equal;
    unsigned long l3d_pixels;
    int l3d_worst;
};

/* One six-bit step, which is what a DS colour moves by: a PICA blend that
 * lands inside this is on a neighbouring colour of the same ramp, and one
 * that does not is a bug rather than a rounding. */
#define PRESENT_SLACK 4

void gpu_verdict_get(struct gpu_verdict *out);

/* The report, written at exit and every PRESENT_REPORT_EVERY verified frames:
 * The same shape as the sound and hang reports, and read back by the gate
 * the same way. */
#define PRESENT_REPORT_EVERY 60u
int gpu_write_report(const char *path);

/* C3D_Fini, before gfxExit. 3ds_apt.c's teardown calls it. */
void gpu_exit(void);

#endif /* POKEPLATINUM_3DS_GPU_H */
