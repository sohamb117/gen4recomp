/*
 * 3ds/src/3ds_input.h: the keypad words, on this console.
 *
 * pc_input_init() is pc/src/pc_input.c's name and pc_main.c's call; this file
 * is what answers it here. See 3ds_input.c for why that file cannot be
 * compiled for this console and for what is deliberately absent.
 */

#ifndef POKEPLATINUM_3DS_INPUT_H
#define POKEPLATINUM_3DS_INPUT_H

#include <stdint.h>

/*
 * KEYINPUT, and the ARM7's X/Y/pen halfword in the shared work area. The
 * masks are the bits each port owns; both are active low, so the mask itself
 * is the idle value. pc/src/pc_input.c writes the same four numbers and
 * 3ds/tests/run.sh fails if the two ever disagree.
 */
#define INPUT_KEYS_ADDR 0x04000130u
#define INPUT_KEYS_MASK 0x03FFu
#define INPUT_XY_ADDR   0x027FFFA8u
#define INPUT_XY_MASK   0x2C00u

/*
 * Idle both words: nothing held, pen up, lid open. Returns 1, or 0 if guest
 * memory is not there. Called from the crt after armrec_mem_init().
 */
int pc_input_init(void);

/*
 * Publish a held-key mask, PAD_Read-positive, the way the game reads it
 * back. The input path feeds this from libctru's HID; the crt also calls it once,
 * through pc_input_init(), with nothing held.
 */
int input_publish_keys(uint16_t held);

/*
 * The DS's PAD_Read bits, which are also libctru's KEY_* bits for
 * everything a DS has; see 3ds_input.c. Only the four this file has to
 * name are here; the rest come across by having the same number.
 */
#define INPUT_KEY_RIGHT 0x0010u
#define INPUT_KEY_LEFT  0x0020u
#define INPUT_KEY_UP    0x0040u
#define INPUT_KEY_DOWN  0x0080u
#define INPUT_KEYS_HID  0x0FFFu   /* A B SELECT START D-pad R L X Y */

/*
 * How far the Circle Pad has to leave centre on an axis before it presses
 * that D-pad bit. The pad reads roughly +/-150 at the rim.
 */
#define INPUT_CPAD_ON 40

/*
 * libctru's held mask and one Circle Pad reading -> a DS held mask. Pure,
 * so the console and the build machine check the same function.
 */
uint16_t input_map_keys(uint32_t held, int cpadX, int cpadY);

/*
 * A press on the lower LCD, in that screen's 320x240 display coordinates,
 * turned into the DS screen coordinate under it. Returns 0 for a press in
 * the letterbox, which is not a place a DS pen can be. Pure.
 */
int input_map_touch(int px, int py, uint16_t *dsX, uint16_t *dsY);

/*
 * The per-frame publish pc/src/pc_os_lite.c calls at the top of OS_Halt,
 * before the VBlank handler runs. Keypad and pen both. Reads libctru's HID;
 * does not scan it, 3ds_frame.c scans once per frame and a second scan
 * eats the edges.
 */
void pc_input_frame(unsigned long long frame);

/*
 * The reset state, at the addresses the game reads it from, both directions
 * through the translator, plus a key in each port. Leaves the two words idle.
 * Returns failures, fills `*ran`. Requires a bound slab.
 */
int input_selftest(int *ran);

#endif /* POKEPLATINUM_3DS_INPUT_H */
