/*
 * 3ds/src/3ds_render_wrap.c: how long the software 2D engine takes.
 *
 * Why a link wrapper and not a timer at the call site. pc_gpu2d_render() is
 * reached through a function pointer that pc/src/pc_video.c calls once a
 * frame, and that file is compiled by both ports; a timer there would put
 * console-only code in shared source. `--wrap` is how this port already
 * interposes on names it must not edit, the division helpers and the bulk
 * memory primitives, so the same mechanism answers this.
 *
 * But not on the renderer itself, and this cost a measurement. `--wrap`
 * redirects UNDEFINED references only. pc_gpu2d.c names its own
 * pc_gpu2d_render when it registers it, that reference is resolved inside
 * pc_gpu2d.o before the linker ever considers wrapping, and the wrapper is
 * simply never reached. It reported 0.00 ms a frame for the DS's entire 2D
 * engine, which is a believable-looking number and completely false. The same
 * footnote is already in the Makefile about the Async DMA forms; it applies to
 * anything a file registers about itself.
 *
 * So the interposition is on the REGISTRATION, which really is a cross-object
 * call (pc_gpu2d.o into pc_video.o) and the shim this file substitutes is
 * what the frame loop then calls.
 *
 * What it is for. The frame breakdown in 3ds_perf.c measures the blit, the
 * buffer flush and the wait for a refresh, and calls the remainder "game".
 * That remainder was 180 ms of a 192 ms frame with no polygons on screen,
 * which is a number that decides an architecture: if the DS's 2D engine in
 * software is most of it, no amount of compiler flags reaches 60 fps and the
 * console's own GPU has to draw the picture. This is what tells those apart.
 */

#include <3ds.h>
#include <stdint.h>

#include "3ds_gpu2d.h"
#include "3ds_perf.h"
#include "3ds_watchdog.h"

typedef void (*pc_video_renderer_fn)(uint64_t frame);
typedef int (*pc_gpu2d_external_fn)(int engine);

extern void __real_pc_video_set_renderer(pc_video_renderer_fn fn,
                                         const char *name);
/*
 * Declared rather than included: pc/include is not on this chain's include
 * path, which is the same reason the file already declares the renderer
 * type itself.
 */
extern void pc_gpu2d_set_external(pc_gpu2d_external_fn fn);

static pc_video_renderer_fn sRenderer;

static void timed_render(uint64_t frame)
{
    uint64_t mark = svcGetSystemTick();
    struct bg_frame bg[2];

    /*
     * The background path's survey, and here for the reason this shim exists at all: it has
     * to see the registers the software renderer is about to read, on the same
     * frame, before the renderer reads them, and this is the only place in
     * the port between the game's last store and that read. It touches no
     * guest state and its cost is inside PERF_GPU2D with the renderer's own,
     * which is where a reader would look for it.
     */
    (void)gpu2d_survey(bg);

    watchdog_where("2d engine");
    if (sRenderer != NULL) {
        sRenderer(frame);
    }
    /* The display-mode work: a claimed engine skipped the software capture unit. */
    gpu2d_capture();
    perf_phase(PERF_GPU2D, svcGetSystemTick() - mark);
}

/*
 * The background path. The software renderer asks this once per engine per frame, from inside
 * the call the shim below is already wrapped around, so the survey has
 * always run for this frame by the time it is asked.
 */
static int external_engine(int engine)
{
    return gpu2d_claims(engine);
}

void __wrap_pc_video_set_renderer(pc_video_renderer_fn fn, const char *name)
{
    sRenderer = fn;
    pc_gpu2d_set_external(fn != NULL ? external_engine : NULL);
    /* A NULL registration is the reset path and must stay a reset, not a shim
     * wrapped around nothing. */
    __real_pc_video_set_renderer(fn != NULL ? timed_render : NULL, name);
}
