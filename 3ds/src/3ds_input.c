/*
 * 3ds/src/3ds_input.c: the two words a reset console starts with.
 *
 * What the game reads before anything has driven it. KEYINPUT at 0x04000130 is
 * ten bits, active low, so nothing held is 0x03FF. The ARM7 publishes the X and
 * Y buttons and the pen in a halfword at 0x027FFFA8 in the shared work area,
 * where nothing held and the pen up is 0x2C00. The game reads both before the
 * first frame, so guest memory has to have them already.
 *
 * The function is pc_input_init(), deliberately: this file is the 3DS answer
 * to pc/src/pc_input.c the way 3ds_window.c is the answer to
 * pc_guest_window.c. That file cannot be compiled here, because it reaches the
 * two words through a cast that is a wild pointer on a host where a guest
 * address is not a host address, and it carries a script parser, a recorder
 * and getenv() besides.
 *
 * The masks are PC's, not re-derived: `~held & 0x03FF` and `~held & 0x2C00`
 * are pc_input.c's publish_keys() exactly, so the two ports idle identically.
 * 3ds/tests/run.sh greps that file and fails if either constant moves.
 *
 * The console's own buttons, and the coincidence that makes it short:
 * libctru's KEY_A through KEY_Y are bits 0 to 11 in exactly the DS's PAD_Read
 * order, because the 3DS inherited the layout and added its new buttons above
 * bit 13. So the mapping is a mask, not a table. What is left is the Circle
 * Pad, which a DS does not have.
 *
 * The circle pad threshold is ours, not libctru's. libctru derives
 * KEY_CPAD_LEFT and friends with a threshold of its own that is not in the
 * header this port compiles against, so using those bits would put an
 * undocumented constant in the middle of the control feel. hidCircleRead() and
 * an explicit +/-40 puts the number where it can be read and changed.
 *
 * Scanning is not done here. libctru latches down, held and up at
 * hidScanInput, so a second scan in the same frame eats every edge the first
 * one saw: 3ds_frame.c scans once per frame and this file reads.
 *
 * Still not here: the touch screen and its calibration. An invented
 * calibration would be a wrong one, and nothing reads it yet.
 *
 * Everything except pc_input_frame() is libctru-free, so
 * 3ds/tests/input_reset.c runs all of it on a build machine.
 */

#include <stddef.h>

#include "3ds_input.h"
#include "3ds_guest.h"
#include "3ds_replay.h"
#include "3ds_view.h"

/*
 * pc/src/pc_tp.c's two entry points. Weak because the self-test .3dsx does
 * not link pc/src at all and pc_input_init() runs there; the game link always
 * has them, and 3ds/tests/pc_src_class.py is what says pc_tp.c is a file this
 * port takes.
 */
extern void pc_tp_boot_userinfo(void) __attribute__((weak));
extern void pc_tp_set(unsigned short x, unsigned short y, int touching)
    __attribute__((weak));

#ifdef __3DS__
#include <3ds.h>
#endif

int pc_input_init(void)
{
    /*
     * The touch calibration the ARM7 boot leaves in the shared work area on
     * hardware, and the pen up. pc/src/pc_input.c's init does both and this
     * is the same two calls: without the calibration TP_SetCalibrateParam
     * computes inverse dot sizes of zero and every sample lands on pixel
     * (0,0), which the PC port measured as a touch that never presses a
     * button. Nothing here invents a calibration, pc_tp.c's is the PC
     * port's own, derived from two points at 16 raw counts per pixel.
     */
    if (pc_tp_boot_userinfo != NULL) {
        pc_tp_boot_userinfo();
    }
    if (pc_tp_set != NULL) {
        pc_tp_set(0, 0, 0);
    }

    /*
     * And a scripted session if the card has one. Read here rather than at
     * host init because this is where the PC port reads its own: the two
     * words have to be idle before the first line can replace them.
     */
    (void)replay_init(REPLAY_PATH);

    return input_publish_keys(0);
}

int input_publish_keys(uint16_t held)
{
    uint16_t *keys = (uint16_t *)armrec_host_ptr(INPUT_KEYS_ADDR);
    uint16_t *xy = (uint16_t *)armrec_host_ptr(INPUT_XY_ADDR);

    if (keys == NULL || xy == NULL) {
        return 0;
    }
    *keys = (uint16_t)(~held & INPUT_KEYS_MASK);
    *xy = (uint16_t)(~held & INPUT_XY_MASK);
    return 1;
}

uint16_t input_map_keys(uint32_t held, int cpadX, int cpadY)
{
    uint16_t keys = (uint16_t)(held & INPUT_KEYS_HID);

    if (cpadX >= INPUT_CPAD_ON) {
        keys |= INPUT_KEY_RIGHT;
    } else if (cpadX <= -INPUT_CPAD_ON) {
        keys |= INPUT_KEY_LEFT;
    }

    /* dy is positive upwards, which is the direction the DS bit means. */
    if (cpadY >= INPUT_CPAD_ON) {
        keys |= INPUT_KEY_UP;
    } else if (cpadY <= -INPUT_CPAD_ON) {
        keys |= INPUT_KEY_DOWN;
    }

    return keys;
}

/*
 * The lower LCD is not scaled, so neither is the pen. The task text said
 * scale 320x240 into 256x192; view_blit puts the picture up at 1x, centred,
 * so the map is an offset and a rejection instead. Scaling here would land
 * the pen 32 columns and 24 rows away from where the player is pointing at
 * the far edges; the arithmetic has to match the blit, not the panel, and
 * VIEW_ORIGIN_X/Y is the blit's own expression. If anything ever scales the
 * picture, this is the other half that has to move with it.
 *
 * A press in the letterbox is not a press. A DS has no letterbox, so there
 * is no DS coordinate for it; the pen comes up, which is what happens on a
 * DS when it leaves the panel. Clamping to the edge instead would keep a
 * drag alive past the border, which is a different console's feel.
 */
int input_map_touch(int px, int py, uint16_t *dsX, uint16_t *dsY)
{
    int x = px - VIEW_ORIGIN_X(VIEW_BOTTOM_WIDTH);
    int y = py - VIEW_ORIGIN_Y(VIEW_SCREEN_HEIGHT);

    if (x < 0 || y < 0 || x >= VIEW_DS_WIDTH || y >= VIEW_DS_HEIGHT) {
        return 0;
    }
    if (dsX != NULL) {
        *dsX = (uint16_t)x;
    }
    if (dsY != NULL) {
        *dsY = (uint16_t)y;
    }
    return 1;
}

#ifdef __3DS__
void pc_input_frame(unsigned long long frame)
{
    circlePosition cpad;
    touchPosition pen;
    ReplayState scripted;
    uint32_t held;

    /*
     * A loaded script drives instead of the console, and drives the pen too.
     * Merging the two would make a measured run depend on whether anything
     * was resting on the buttons, which is the property a script exists to
     * remove; there is no script on an ordinary boot.
     */
    if (replay_frame(frame, &scripted)) {
        input_publish_keys(scripted.keys);
        if (pc_tp_set != NULL) {
            pc_tp_set(scripted.x, scripted.y, scripted.touching);
        }
        return;
    }

    held = hidKeysHeld();

    hidCircleRead(&cpad);
    input_publish_keys(input_map_keys(held, cpad.dx, cpad.dy));

    /*
     * KEY_TOUCH is libctru's own bit for "the panel is being pressed"; the
     * position it reports when nothing is holds the last press, so it is only
     * read when the bit is set. touch_wanted, the game asking for the pen,
     * is deliberately ignored: it can highlight the lower screen one day, and
     * gating the pen on it would make the panel dead wherever the game forgot
     * to ask.
     */
    if (pc_tp_set != NULL) {
        uint16_t dsX = 0;
        uint16_t dsY = 0;

        hidTouchRead(&pen);
        if ((held & KEY_TOUCH) != 0
            && input_map_touch(pen.px, pen.py, &dsX, &dsY)) {
            pc_tp_set(dsX, dsY, 1);
        } else {
            pc_tp_set(0, 0, 0);
        }
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#ifdef INPUT_SELFTEST_VERBOSE
#include <stdio.h>
#define FAILNOTE() fprintf(stderr, "  3ds_input.c:%d failed\n", __LINE__)
#else
#define FAILNOTE() ((void)0)
#endif

#define CHECK(cond)                                                           \
    do {                                                                      \
        ran++;                                                                \
        if (!(cond)) {                                                        \
            failed++;                                                         \
            FAILNOTE();                                                       \
        }                                                                     \
    } while (0)

int input_selftest(int *ranOut)
{
    uint16_t *keys = (uint16_t *)armrec_host_ptr(INPUT_KEYS_ADDR);
    uint16_t *xy = (uint16_t *)armrec_host_ptr(INPUT_XY_ADDR);
    uint16_t tx = 0;
    uint16_t ty = 0;
    int ran = 0;
    int failed = 0;

    CHECK(keys != NULL);
    CHECK(xy != NULL);
    if (keys == NULL || xy == NULL) {
        if (ranOut != NULL) {
            *ranOut = ran;
        }
        return failed;
    }

    /* The gate this phase ends on: after init, the two words are what a reset
     * console has, at the addresses the game reads them from. */
    CHECK(pc_input_init() == 1);
    CHECK(*keys == INPUT_KEYS_MASK);
    CHECK(*xy == INPUT_XY_MASK);

    /* Both directions through the translator, so a right value at a wrong
     * address cannot pass. */
    CHECK(armrec_guest_addr(keys) == INPUT_KEYS_ADDR);
    CHECK(armrec_guest_addr(xy) == INPUT_XY_ADDR);

    /* Active low, and only the bits each port owns. A is bit 0; X and Y live
     * in the ARM7's word and not in KEYINPUT at all. */
    CHECK(input_publish_keys(0x0001u) == 1);      /* A */
    CHECK(*keys == (uint16_t)(INPUT_KEYS_MASK & ~0x0001u));
    CHECK(*xy == INPUT_XY_MASK);

    CHECK(input_publish_keys(0x0400u) == 1);      /* X */
    CHECK(*keys == INPUT_KEYS_MASK);
    CHECK(*xy == (uint16_t)(INPUT_XY_MASK & ~0x0400u));

    CHECK(input_publish_keys(0xFFFFu) == 1);      /* everything at once */
    CHECK(*keys == 0);
    CHECK(*xy == 0);

    /*
     * The mapping, 7.5. libctru's bits 0 to 11 are the DS's, so the checks
     * that matter are the mask (nothing above bit 11 may cross: zl, zr,
     * KEY_TOUCH and the C-stick are not DS buttons) and the Circle Pad,
     * which has no DS twin at all.
     */
    CHECK(input_map_keys(0x0001u, 0, 0) == 0x0001u);            /* A     */
    CHECK(input_map_keys(0x0800u, 0, 0) == 0x0800u);            /* Y     */
    CHECK(input_map_keys(0x0009u, 0, 0) == 0x0009u);            /* A+ST  */
    CHECK(input_map_keys(0xFFFFF000u, 0, 0) == 0);              /* not ours */
    CHECK(input_map_keys(0x00008000u, 0, 0) == 0);              /* ZR    */

    CHECK(input_map_keys(0, INPUT_CPAD_ON, 0) == INPUT_KEY_RIGHT);
    CHECK(input_map_keys(0, -INPUT_CPAD_ON, 0) == INPUT_KEY_LEFT);
    CHECK(input_map_keys(0, 0, INPUT_CPAD_ON) == INPUT_KEY_UP);
    CHECK(input_map_keys(0, 0, -INPUT_CPAD_ON) == INPUT_KEY_DOWN);
    /* One short of the threshold is not a press, in all four directions. */
    CHECK(input_map_keys(0, INPUT_CPAD_ON - 1, INPUT_CPAD_ON - 1) == 0);
    CHECK(input_map_keys(0, -(INPUT_CPAD_ON - 1), -(INPUT_CPAD_ON - 1)) == 0);
    /* Diagonals are both axes, and the rim is no different from the edge of
     * the deadzone. */
    CHECK(input_map_keys(0, 150, 150) == (INPUT_KEY_RIGHT | INPUT_KEY_UP));
    CHECK(input_map_keys(0, -150, -150) == (INPUT_KEY_LEFT | INPUT_KEY_DOWN));
    /* And the pad ORs with the D-pad rather than replacing it. */
    CHECK(input_map_keys(INPUT_KEY_DOWN, -60, 0)
          == (INPUT_KEY_DOWN | INPUT_KEY_LEFT));

    /* Through the words, so the mapping and the publish agree: X is the
     * ARM7's word and LEFT is KEYINPUT. */
    CHECK(input_publish_keys(input_map_keys(0x0400u, -60, 0)) == 1);
    CHECK(*keys == (uint16_t)(INPUT_KEYS_MASK & ~INPUT_KEY_LEFT));
    CHECK(*xy == (uint16_t)(INPUT_XY_MASK & ~0x0400u));

    /*
     * The pen, 7.6. The picture is blitted at 1x and centred, so the map is
     * the blit's own offset: 32 columns and 24 rows into a 320x240 panel.
     */
    CHECK(input_map_touch(32, 24, &tx, &ty) == 1 && tx == 0 && ty == 0);
    CHECK(input_map_touch(32 + 255, 24 + 191, &tx, &ty) == 1
          && tx == 255 && ty == 191);
    CHECK(input_map_touch(32 + 128, 24 + 96, &tx, &ty) == 1
          && tx == 128 && ty == 96);
    /* And the letterbox is not a DS coordinate, on all four sides. */
    CHECK(input_map_touch(31, 24, NULL, NULL) == 0);
    CHECK(input_map_touch(32, 23, NULL, NULL) == 0);
    CHECK(input_map_touch(32 + 256, 24, NULL, NULL) == 0);
    CHECK(input_map_touch(32, 24 + 192, NULL, NULL) == 0);
    CHECK(input_map_touch(0, 0, NULL, NULL) == 0);

    /* And back to idle, because this runs on the way into the game. */
    CHECK(pc_input_init() == 1);
    CHECK(*keys == INPUT_KEYS_MASK);
    CHECK(*xy == INPUT_XY_MASK);

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
