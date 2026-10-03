/*
 * The 2D engines' background and sprite layers.
 *
 * This is the renderer pc/include/pc_video.h reserves a slot for. It reads
 * guest state only, the two register blocks at 0x04000000 and 0x04001000,
 * palette RAM, and VRAM through the windows the model puts it in, and writes
 * the two surfaces pc_video_surface() hands out. Nothing here is reachable
 * from the game: the game writes registers, and this reads them.
 *
 * Backgrounds: text, affine, extended and the large bitmap, at every size,
 * with the scroll registers, the character and screen bases, the 4bpp and 8bpp
 * split, the extended palettes and priority. Sprites: 128 read out of OAM per
 * scanline, both character mappings and all four 1D boundaries, all sixteen
 * shape and size pairs, both colour depths with standard and extended
 * palettes, the three bitmap mappings and the reserved fourth, rotation and
 * scaling with the double-size box, both flips, and priority against both the
 * other sprites and the backgrounds. The OBJ layer is interleaved between the
 * backgrounds at each priority rather than composed on top of them.
 *
 * The window, blend and mosaic units come later, so BLDCNT and MOSAIC are
 * ignored and DISPCNT's three window-enable bits do nothing. Two outputs are
 * computed for those and unread until then: the OBJ-window mask, and the
 * semi-transparent and bitmap sprites' alpha in the pixel's top byte. The
 * composite also keeps the second-from-top layer blending will need.
 *
 * One frame at a time, which is the contract and also a limit. The renderer
 * runs once per frame, at VBlank, and sees one register state, so a game that
 * changes a scroll register on an HBlank interrupt gets the value it ended the
 * frame with. The scanline structure is still here: the affine reference
 * points advance by BGxPB and BGxPD per line exactly as the hardware's do, so
 * what is missing is only the mid-frame write.
 *
 * What traps rather than guessing: DISPCNT's main-memory display mode has no
 * display FIFO behind it here, so it aborts with a message naming the register
 * rather than drawing something plausible. A wrong picture that looks right is
 * the expensive kind of defect.
 */

#ifndef POKEDIAMOND_PC_GPU2D_H
#define POKEDIAMOND_PC_GPU2D_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Install the renderer with pc_video_set_renderer(). Called from pc_main.c
 * after armrec_mem_init(), because reading a register before guest memory
 * exists is a fault rather than a zero.
 */
void pc_gpu2d_install(void);

/*
 * Render one frame into the two surfaces. This is what gets installed; it is
 * exposed so a test can drive it without a frame loop, which is how
 * pc/tests/test_bg.c replays melonDS's sweep.
 */
void pc_gpu2d_render(uint64_t frame);

/*
 * The last two steps this engine applies to every pixel it shows: the
 * MASTER_BRIGHT fade for the engine whose register is passed, then the
 * six-to-eight-bit expansion. Exposed for the wide margins, which are drawn
 * by the rasterizer rather than composed here and must end up on the same
 * curve; a fade has to dim the whole width or the seam appears the moment
 * the screen fades. `bgr6` is a rasterizer pixel, six bits a channel with
 * blue high; the result is 0x00RRGGBB.
 */
uint32_t pc_gpu2d_present_px(uint32_t bgr6, uint16_t master_bright_reg);

/*
 * The high-resolution 3D path's way in, pc/src/pc_view.c's compose.
 *
 * pc_gpu2d_set_hd() arms the recording: at a scale above 1 this engine writes
 * down, per native pixel of engine A, whether the 3D layer is what shows
 * there and the two-deep stack the blend unit had under it. Off, nothing is
 * recorded and nothing is paid.
 *
 * pc_gpu2d_hd3d_covers() answers the first question for a native pixel.
 * pc_gpu2d_hd3d_row() answers the second for a whole rendered row of the
 * panel: `src` is the rasterizer's row at S samples per native pixel, `dst`
 * already holds the replicated native pixels, and every sub-pixel the 3D
 * layer is showing at is overwritten with what the DS's blend unit and
 * master brightness would have made of it, including the ones no polygon
 * reaches, which come out as the 2D stack composed with BG0 off. Everything
 * else is left as the caller replicated it. Valid until the next frame is
 * composed.
 */
void pc_gpu2d_set_hd(int scale);
int pc_gpu2d_hd3d_covers(int x, int y);
void pc_gpu2d_hd3d_row(int y, int s, const uint32_t *src, uint32_t *dst,
                       uint16_t mb);

/* The five ways a 3D pixel can win or lose its native pixel, built by hand
 * because a played game reaches three of them almost never. Runs from
 * --selftest; 1 is pass. */
int pc_gpu2d_hd3d_selftest(void);

/*
 * An engine whose picture is produced somewhere other than here. A port with
 * its own compositor, the 3DS one draws the DS backgrounds with the console's
 * GPU, answers 1 for an engine it has taken over for the coming frame, and
 * this file then neither composes that engine nor writes its surface. The
 * surface is left exactly as it was, because the only caller that can answer 1
 * is one that is not going to read it.
 *
 * A registration rather than a weak symbol on purpose: a weak undefined
 * function is not resolvable in a PE image, so the Windows build of this port
 * would not link one.
 */
typedef int (*pc_gpu2d_external_fn)(int engine);
void pc_gpu2d_set_external(pc_gpu2d_external_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* POKEDIAMOND_PC_GPU2D_H */
