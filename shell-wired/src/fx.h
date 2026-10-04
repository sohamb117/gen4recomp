/*
 * Display effects and presentation presets.
 *
 * Effects are drawn with plain SDL_Renderer calls, no shaders, so they run
 * identically on Metal (macOS/iOS), Direct3D/Vulkan (Windows) and the
 * software renderer the autotests use:
 *   - each screen is a textured mesh (SDL_RenderGeometry): one quad, or a
 *     16x12 grid bent into a barrel when CRT curvature is on;
 *   - LCD GRID, SCANLINES and the CRT aperture mask are tiny pattern
 *     textures repeated once per DS pixel/row over the same mesh
 *     (SDL_TEXTURE_ADDRESS_WRAP), so they follow scaling, rotation and
 *     curvature exactly; the CRT vignette is a radial texture multiplied in;
 *   - SMOOTH runs Scale2x on the CPU (scale2x.c) and samples linearly.
 * Two effect slots are applied in order with their own intensity.
 *
 * Performance presets only change presentation: High = effects as set,
 * VSync on, no cap; Balanced = no curvature, VSync on, 60 FPS cap; Low = no
 * effects, VSync off, 30 FPS cap; Auto = High until frames take too long to
 * produce, then Low (and back when there is headroom again).
 */
#ifndef NP_FX_H
#define NP_FX_H

struct np_app;

typedef struct np_present {
    int fx[2], intensity[2]; /* effective effects after the preset */
    int curvature;
    int vsync;
    int fps_cap;
} np_present;

int np_fx_init(struct np_app *app);
void np_fx_destroy(struct np_app *app);
/* What the options and the performance preset resolve to right now. */
void np_fx_resolve(const struct np_app *app, np_present *out);
/* New frame from the core: uploads (and scales, for SMOOTH) both screens. */
void np_fx_upload(struct np_app *app);
/* Draws screen `which` at its place in app->layout with the effects. */
void np_fx_draw_screen(struct np_app *app, int which);
/* Feeds the Auto preset: CPU time spent producing the last presented frame.
 * Returns 1 when the preset changed its mind (re-apply video options). */
int np_fx_auto_sample(struct np_app *app, double work_ms);

#endif
