/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic D/P/Pt/HG/SS save images for tests (layout per the decomps,
 * built independently of save4's own offset table). */
#ifndef NP_TESTS_SYNTH_SAVE_H
#define NP_TESTS_SYNTH_SAVE_H

#include <stdint.h>

#include "save4/save4.h"

/* Values the synthetic image contains (newest copy = backup). */
#define SYNTH_TRAINER_NAME "Lucas"
#define SYNTH_TID 12345
#define SYNTH_SID 54321
#define SYNTH_MONEY_NEW 3000u
#define SYNTH_MONEY_OLD 1000u
#define SYNTH_COUNTER_OLD 4u
#define SYNTH_COUNTER_NEW 5u
#define SYNTH_BADGES 0x07
#define SYNTH_KANTO_BADGES 0x03 /* HG/SS only */
#define SYNTH_BOX_SPECIES 396 /* Starly in box 1 slot 1 */
#define SYNTH_HGSS_BULBY_BALL 18 /* HG/SS: Bulby sits in a Level Ball (BALL_LEVEL, item 493) */

/* Build a hand-made Pokémon (decrypted). Party data is filled when `party`. */
void synth_make_mon(pkm4 *p, uint16_t species, uint8_t level, uint32_t pid, const char *nickname,
                    uint16_t move0, int party);

/* Fill `img` (SAVE4_IMAGE_SIZE bytes) with a complete two-copy save:
 * primary copy is older (SYNTH_COUNTER_OLD, money SYNTH_MONEY_OLD), backup
 * copy is newer. Unknown regions are filled with deterministic noise so tests
 * can check that edits preserve bytes save4 does not interpret.
 * Party: Turtwig Lv5 "Turtwig", Bulbasaur Lv10 "Bulby". HG/SS images carry
 * PlayerProfile.version 7 / 8 and mons from that game. */
void synth_save_build(uint8_t *img, save4_game game);

/* Offsets used by the builder (exposed so tests can corrupt / inspect). */
uint32_t synth_general_size(save4_game game);
uint32_t synth_storage_offset(save4_game game); /* within a copy */
uint32_t synth_storage_size(save4_game game);
uint32_t synth_footer_size(save4_game game);

#endif
