/*
 * 3ds/tests/view_dump.c: run the 3DS blit on this machine and look at it.
 *
 * There is no 3DS and no emulator here yet, so the gate cannot be a
 * photograph. It is this instead: `3ds/src/3ds_view.c` is pure C over caller
 * buffers, so the host can link the *same* object the 3dsx runs, hand it the
 * same test pattern, and dump what came out as a PNG.
 *
 *   $ gcc -O2 -Wall -Wextra -I3ds/src -o build/3ds/view_dump \
 *         3ds/tests/view_dump.c 3ds/src/3ds_view.c
 *   $ build/3ds/view_dump [outdir]        (default build/3ds/shots)
 *
 * (`__3DS__` is not defined, so `view_present` and the libctru include
 * compile out and nothing here needs devkitARM. The run.sh wraps both
 * commands.)
 *
 * Two pngs per screen, and the second one is the point:
 *
 *   <screen>.png        the display, 400x240 / 320x240, read back through
 *                       the same rotation the blit wrote through.
 *   <screen>-fbmem.png  the framebuffer as it sits in memory, 240 wide and
 *                       400 / 320 tall, one row per display *column*.
 *
 * The first proves centring, colour, the letterbox and the font. It cannot
 * prove the rotation, because writing and reading with one wrong formula
 * looks exactly like writing and reading with the right one. The second is
 * the honest picture of the bytes: the image in it is on its side, and if
 * the amber corner marker is not where a 90-degree turn would put it, the
 * mapping is wrong. Neither is a substitute for the LCD.
 *
 * The checks below are geometry, not pixels-vs-a-golden-file: nothing is
 * pinned to a hash, because the pattern is meant to change when the producer
 * does.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "3ds_view.h"

/* The PNG encoder the PC port already carries; a header of static functions. */
#include "../../pc/src/pc_png.h"

#define SURFACE_PIXELS (VIEW_DS_WIDTH * VIEW_DS_HEIGHT)

/* The strings a real frame would carry, with the widest plausible values. */
static const char kLineTop[]    = "HID:00000FFF  START EXITS";
static const char kLineBottom[] = "TP:319,239  FRAME:000000";

static int gFailures;

static void check(int ok, const char *what)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        gFailures++;
    }
}

static uint32_t fb_get(const uint8_t *fb, int sw, int sh, int x, int y)
{
    const uint8_t *p;

    if (x < 0 || y < 0 || x >= sw || y >= sh) {
        return 0xFFFFFFFFu;
    }
    p = fb + ((size_t)x * (size_t)sh + (size_t)(sh - 1 - y)) * 3;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/*
 * Pack an image into the row-major, one-filter-byte-per-row form
 * pc_png_write_raw wants, then write it. `rotated` picks which of the two
 * views of the framebuffer to take: 0 is memory order, 1 is the display.
 */
static int dump(const char *path, const uint8_t *fb, int sw, int sh,
                int rotated)
{
    unsigned w = (unsigned)(rotated ? sw : sh);
    unsigned h = (unsigned)(rotated ? sh : sw);
    size_t rawn = (size_t)h * (1 + 3 * (size_t)w);
    size_t zcap = rawn + rawn / 2 + 4096;
    unsigned char *raw = malloc(rawn);
    unsigned char *z = malloc(zcap);
    unsigned x, y;
    int ok;

    if (raw == NULL || z == NULL) {
        fprintf(stderr, "view_dump: out of memory for %s\n", path);
        free(raw);
        free(z);
        return 0;
    }

    for (y = 0; y < h; y++) {
        unsigned char *row = raw + (size_t)y * (1 + 3 * (size_t)w);

        row[0] = 0; /* filter type 0 */
        for (x = 0; x < w; x++) {
            uint32_t c;

            if (rotated) {
                c = fb_get(fb, sw, sh, (int)x, (int)y);
            } else {
                const uint8_t *p = fb + ((size_t)y * (size_t)w + x) * 3;

                c = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                    | ((uint32_t)p[2] << 16);
            }
            row[1 + 3 * x + 0] = (unsigned char)((c >> 16) & 0xFFu);
            row[1 + 3 * x + 1] = (unsigned char)((c >> 8) & 0xFFu);
            row[1 + 3 * x + 2] = (unsigned char)(c & 0xFFu);
        }
    }

    ok = pc_png_write_raw(path, w, h, raw, rawn, z, zcap, "view_dump");
    free(raw);
    free(z);
    if (ok) {
        printf("wrote %s (%ux%u)\n", path, w, h);
    }
    return ok;
}

static void render(uint8_t *fb, int sw, int sh, const uint32_t *px)
{
    int tx = (sw - VIEW_DS_WIDTH) / 2;

    view_clear(fb, sw, sh, VIEW_COLOUR_LETTERBOX);
    view_blit(fb, sw, sh, px, VIEW_DS_WIDTH, VIEW_DS_HEIGHT);
    view_text(fb, sw, sh, tx, VIEW_TEXT_TOP_Y, kLineTop, VIEW_COLOUR_TEXT);
    view_text(fb, sw, sh, tx, VIEW_TEXT_BOTTOM_Y, kLineBottom,
              VIEW_COLOUR_TEXT);
}

static void inspect(const char *name, const uint8_t *fb, int sw, int sh,
                    uint32_t body)
{
    /* The macros, not a second copy of the arithmetic: the check below that
     * the picture starts here is what lets 3ds_input.c trust them to undo a
     * touch coordinate. */
    int ox = VIEW_ORIGIN_X(sw);
    int oy = VIEW_ORIGIN_Y(sh);
    char what[96];
    int lit = 0;
    int x;

    snprintf(what, sizeof what, "%s: letterbox at (0,0) is black", name);
    check(fb_get(fb, sw, sh, 0, 0) == VIEW_COLOUR_LETTERBOX, what);

    snprintf(what, sizeof what, "%s: column %d, left of the picture, is black",
             name, ox - 1);
    check(fb_get(fb, sw, sh, ox - 1, oy) == VIEW_COLOUR_LETTERBOX, what);

    snprintf(what, sizeof what, "%s: picture starts at (%d,%d)", name, ox, oy);
    check(fb_get(fb, sw, sh, ox, oy) == VIEW_COLOUR_MARK, what);

    snprintf(what, sizeof what, "%s: corner marker is %d wide",
             name, VIEW_MARK_SIZE);
    check(fb_get(fb, sw, sh, ox + VIEW_MARK_SIZE - 1, oy) == VIEW_COLOUR_MARK
              && fb_get(fb, sw, sh, ox + VIEW_MARK_SIZE, oy)
                     == VIEW_COLOUR_EDGE,
          what);

    snprintf(what, sizeof what, "%s: one-pixel frame along the top edge", name);
    check(fb_get(fb, sw, sh, ox + VIEW_DS_WIDTH / 2, oy) == VIEW_COLOUR_EDGE
              && fb_get(fb, sw, sh, ox + VIEW_DS_WIDTH / 2, oy + 1) == body,
          what);

    snprintf(what, sizeof what, "%s: bottom right pixel of the picture", name);
    check(fb_get(fb, sw, sh, ox + VIEW_DS_WIDTH - 1, oy + VIEW_DS_HEIGHT - 1)
              == VIEW_COLOUR_EDGE,
          what);

    snprintf(what, sizeof what, "%s: nothing spills past the picture", name);
    check(fb_get(fb, sw, sh, ox + VIEW_DS_WIDTH, oy) == VIEW_COLOUR_LETTERBOX
              && fb_get(fb, sw, sh, ox, oy + VIEW_DS_HEIGHT)
                     == VIEW_COLOUR_LETTERBOX,
          what);

    for (x = 0; x < sw; x++) {
        if (fb_get(fb, sw, sh, x, VIEW_TEXT_TOP_Y + 3) == VIEW_COLOUR_TEXT) {
            lit++;
        }
    }
    snprintf(what, sizeof what, "%s: text in the upper band (%d lit pixels)",
             name, lit);
    check(lit > 0, what);

    lit = 0;
    for (x = 0; x < sw; x++) {
        if (fb_get(fb, sw, sh, x, VIEW_TEXT_BOTTOM_Y + 3) == VIEW_COLOUR_TEXT) {
            lit++;
        }
    }
    snprintf(what, sizeof what, "%s: text in the lower band (%d lit pixels)",
             name, lit);
    check(lit > 0, what);

    /* The band the text sits in must not be inside the picture. */
    snprintf(what, sizeof what, "%s: both text bands are in the letterbox",
             name);
    check(VIEW_TEXT_TOP_Y + VIEW_GLYPH_HEIGHT <= oy
              && VIEW_TEXT_BOTTOM_Y >= oy + VIEW_DS_HEIGHT
              && VIEW_TEXT_BOTTOM_Y + VIEW_GLYPH_HEIGHT <= sh,
          what);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "build/3ds/shots";
    static uint32_t top[SURFACE_PIXELS];
    static uint32_t bottom[SURFACE_PIXELS];
    static uint8_t fbTop[VIEW_TOP_WIDTH * VIEW_SCREEN_HEIGHT * 3];
    static uint8_t fbBottom[VIEW_BOTTOM_WIDTH * VIEW_SCREEN_HEIGHT * 3];
    char path[512];

    /* The default is under build/, which a clean checkout does not have. */
    mkdir(dir, 0777);

    view_test_pattern(top, VIEW_DS_WIDTH, VIEW_DS_HEIGHT,
                      VIEW_COLOUR_TOP, VIEW_COLOUR_EDGE, VIEW_COLOUR_MARK);
    view_test_pattern(bottom, VIEW_DS_WIDTH, VIEW_DS_HEIGHT,
                      VIEW_COLOUR_BOTTOM, VIEW_COLOUR_EDGE, VIEW_COLOUR_MARK);

    render(fbTop, VIEW_TOP_WIDTH, VIEW_SCREEN_HEIGHT, top);
    render(fbBottom, VIEW_BOTTOM_WIDTH, VIEW_SCREEN_HEIGHT, bottom);

    inspect("top", fbTop, VIEW_TOP_WIDTH, VIEW_SCREEN_HEIGHT,
            VIEW_COLOUR_TOP);
    inspect("bottom", fbBottom, VIEW_BOTTOM_WIDTH, VIEW_SCREEN_HEIGHT,
            VIEW_COLOUR_BOTTOM);

    snprintf(path, sizeof path, "%s/top.png", dir);
    if (!dump(path, fbTop, VIEW_TOP_WIDTH, VIEW_SCREEN_HEIGHT, 1)) {
        gFailures++;
    }
    snprintf(path, sizeof path, "%s/top-fbmem.png", dir);
    if (!dump(path, fbTop, VIEW_TOP_WIDTH, VIEW_SCREEN_HEIGHT, 0)) {
        gFailures++;
    }
    snprintf(path, sizeof path, "%s/bottom.png", dir);
    if (!dump(path, fbBottom, VIEW_BOTTOM_WIDTH, VIEW_SCREEN_HEIGHT, 1)) {
        gFailures++;
    }
    snprintf(path, sizeof path, "%s/bottom-fbmem.png", dir);
    if (!dump(path, fbBottom, VIEW_BOTTOM_WIDTH, VIEW_SCREEN_HEIGHT, 0)) {
        gFailures++;
    }

    if (gFailures != 0) {
        printf("\nview_dump: %d check(s) failed.\n", gFailures);
        return 1;
    }
    printf("\nview_dump: every check passed.\n");
    return 0;
}
