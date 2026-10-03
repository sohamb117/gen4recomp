/*
 * 3ds/src/3ds_view.h: the publish seam between a DS-sized picture and the
 * 3DS LCDs.
 *
 * Everything below `view_present` is pure C over caller-owned buffers: no
 * libctru, no globals, no allocation. That is deliberate, so a later producer
 * swap does not have to touch the blit, and so the host harness in 3ds/tests
 * links these same functions natively and dumps the picture as a PNG on a
 * machine with no 3DS and no emulator.
 *
 * Screen geometry, and why every function takes `sw, sh` in that order.
 * gfxGetFramebuffer reports width 240 and height 400 or 320: those are the
 * framebuffer's axes, not the picture's. The panel is physically rotated, so
 * the displayed image is 400x240 or 320x240 and a pixel at display coordinate
 * (x right, y down from the top-left) lives at
 *
 *     (x * 240 + (239 - y)) * 3 bytes
 *
 * libctru's own console proves it: consoleDrawChar walks eight increasing
 * addresses to paint one glyph column bottom-to-top, then adds 240 - 8 to step
 * one pixel right.
 *
 * So `sw` and `sh` here are always the display width and height, and callers
 * on the 3DS pass the framebuffer's reported height as `sw` and its reported
 * width as `sh`. Getting that pair backwards is the one mistake this API can
 * still make, which is why the swap happens once, inside `view_present`.
 *
 * Pixel format: surfaces are 0x00RRGGBB uint32_t, the same word order pc_video
 * already produces, so a producer swap is a pointer change and not a colour
 * conversion. The framebuffer is GSP_BGR8_OES, three bytes, blue first.
 */

#ifndef POKEPLATINUM_3DS_VIEW_H
#define POKEPLATINUM_3DS_VIEW_H

#include <stdint.h>

/* The picture a DS produces, and the two panels it has to land on. */
#define VIEW_DS_WIDTH      256
#define VIEW_DS_HEIGHT     192
#define VIEW_TOP_WIDTH     400
#define VIEW_BOTTOM_WIDTH  320
#define VIEW_SCREEN_HEIGHT 240

/*
 * The test pattern, shared with the host harness so the dumped PNG and
 * the hardware show the same picture. A flat fill would not prove the blit
 * landed the right way up, so each surface also gets a one-pixel edge and an
 * 8x8 corner block at its own (0,0): if the marker is not at the top left of
 * the letterboxed area, the rotation is wrong, and no amount of "the colour
 * is right" would have caught it.
 */
#define VIEW_MARK_SIZE       8
#define VIEW_COLOUR_TOP      0x00204080u /* top screen body: blue      */
#define VIEW_COLOUR_BOTTOM   0x00802040u /* bottom screen body: maroon */
#define VIEW_COLOUR_EDGE     0x00FFFFFFu /* one-pixel frame            */
#define VIEW_COLOUR_MARK     0x00FFC000u /* (0,0) corner block: amber  */
/*
 * The same block in red. It is the self-test verdict in a form a screenshot
 * can be asked about: the SELF line beside it is 5x7 text and reading that
 * back would mean decoding the font, while one 8x8 block of a known colour at
 * a known corner is a pixel lookup. 3ds/tests/shot_verdict.py is the reader.
 * Amber is a pass, red is a failure, and the two are far enough apart in
 * every channel that an emulator's scaling cannot confuse them.
 */
#define VIEW_COLOUR_MARK_BAD 0x00FF0000u /* the same block, on a failure */
#define VIEW_COLOUR_LETTERBOX 0x00000000u
#define VIEW_COLOUR_TEXT     0x00E0E0E0u

/* Text is 5x7 in a 6x8 cell; the letterbox band above the picture is 24 rows
 * on both screens, so one line fits with room to spare. */
#define VIEW_GLYPH_WIDTH   5
#define VIEW_GLYPH_HEIGHT  7
#define VIEW_GLYPH_ADVANCE 6

/*
 * Where the picture lands on a screen of this width and height: `view_blit`
 * centres at 1x, so this is the same expression it computes. It is a macro
 * rather than a comment because the touch screen has to undo it,
 * 3ds_input.c maps a press back through these, and a pen that lands
 * somewhere other than where the player is pointing is the whole failure
 * mode. 3ds/tests/view_dump.c checks that a blitted pixel (0,0) really is
 * here, which is what binds the two together.
 */
#define VIEW_ORIGIN_X(sw) (((sw) - VIEW_DS_WIDTH) / 2)
#define VIEW_ORIGIN_Y(sh) (((sh) - VIEW_DS_HEIGHT) / 2)

/* Where `view_present` puts its two debug lines, in display coordinates. */
#define VIEW_TEXT_TOP_Y    8
#define VIEW_TEXT_BOTTOM_Y 225

/* Paint the pattern into a w*h surface of 0x00RRGGBB words. */
void view_test_pattern(uint32_t *px, int w, int h,
                       uint32_t body, uint32_t edge, uint32_t mark);

/* One display pixel. Out-of-range coordinates are dropped, not clamped. */
void view_put(uint8_t *fb, int sw, int sh, int x, int y, uint32_t rgb);

/* Fill the whole display with one colour. */
void view_clear(uint8_t *fb, int sw, int sh, uint32_t rgb);

/*
 * Blit a w*h surface into the display at 1x, centred. Nothing scales and
 * nothing is filtered: 256x192 inside 400x240 is 72 columns of letterbox
 * either side and 24 rows above and below, and inside 320x240 it is 32 and
 * 24. Whether that stays 1x is the decision, not this function's.
 */
void view_blit(uint8_t *fb, int sw, int sh,
               const uint32_t *px, int w, int h);

/* view_put's inverse: the 0x00RRGGBB word at a display coordinate, and 0 for
 * a coordinate off the screen. 3ds_gpu.c reads its font sheet back with it. */
uint32_t view_get(const uint8_t *fb, int sw, int sh, int x, int y);

/*
 * Draw a string at (x, y), top-left of the first cell. Lowercase is folded to
 * uppercase and anything with no glyph draws a hollow box, so a missing
 * character is visible on a photograph rather than silently absent.
 */
void view_text(uint8_t *fb, int sw, int sh, int x, int y,
               const char *s, uint32_t rgb);

/*
 * One whole screen: letterbox, picture centred, and the two lines in the
 * bands. `px` may be NULL, which leaves the picture out. This is what
 * view_present() does to each panel, and since the GPU present it is also what
 * PRESENT_VERIFY composes with to check the GPU path pixel for pixel.
 */
void view_compose(uint8_t *fb, int sw, int sh, const uint32_t *px,
                  const char *line_top, const char *line_bottom);

#ifdef __3DS__
/*
 * Clear, blit and label both screens for one frame. Does not swap, scan or
 * pace: 3ds_frame.c owns that sequence, for the game and the self-test loop
 * alike. `line_top` and `line_bottom` go in the letterbox
 * bands of *both* screens, whichever screen a photograph catches, the input
 * state is on it.
 */
void view_present(const uint32_t *top, const uint32_t *bottom,
                  const char *line_top, const char *line_bottom);
#endif

#endif /* POKEPLATINUM_3DS_VIEW_H */
