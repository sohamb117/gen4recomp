/*
 * 3ds/src/3ds_gpu3d_wrap.c: how long the DS's 3D engine takes.
 *
 * Why this exists. 3ds_perf.c timed the blit, the buffer flush, the wait for
 * a refresh and the software 2D engine, and called everything left over "the
 * game". On a screen with no polygons on it that remainder was 4.5 ms and the
 * label was true. On a screen with 450 polygons on it the remainder was 83 ms
 * and the label was a lie: nearly all of it is pc_gpu3d_soft rasterizing, and
 * the one number the whole frame budget rests on; what the game's own code
 * costs, was hidden inside a number that moves with the scene.
 *
 * Why the wrapper is on pc_gpu3d_vblank and not on the rasterizer.
 * --wrap redirects UNDEFINED references only, and pc_gpu3d.c carries a weak
 * definition of pc_gpu3d_soft_render_frame() of its own, for builds that link
 * the geometry half without the rasterizer. A reference from an object that
 * also defines the name is not an undefined reference, so a wrapper there
 * would link clean and never be reached, the same shape that reported 0.00
 * ms a frame for the entire 2D engine before 3ds_render_wrap.c moved to the
 * registration call. pc_gpu3d_vblank() has exactly one caller,
 * pc_video_frame_end() in another object, so this one really is redirected.
 *
 * What it therefore measures is the whole per-frame 3D event and not just the
 * rasterizer: the polygon sort, the render-register latch and the render. That
 * is the right unit for a budget; all of it is work the DS did in silicon,
 * and it is also the unit a PICA backend would replace.
 */

#include <3ds.h>

#include "3ds_gpu3d.h"
#include "3ds_perf.h"
#include "3ds_watchdog.h"

extern void __real_pc_gpu3d_vblank(void);

void __wrap_pc_gpu3d_vblank(void)
{
    uint64_t mark = svcGetSystemTick();

    watchdog_where("3d rasterizer");
    __real_pc_gpu3d_vblank();
    gpu3d_survey();
    perf_phase(PERF_GPU3D, svcGetSystemTick() - mark);
}
