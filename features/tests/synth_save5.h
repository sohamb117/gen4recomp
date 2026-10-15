/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic Black/White save images for tests (layout per save5.c's sources). */
#ifndef NP_TESTS_SYNTH_SAVE5_H
#define NP_TESTS_SYNTH_SAVE5_H

#include <stdint.h>

#include "save5/save5.h"

/* Values the synthetic image contains. The backup copy is newer
 * (SYNTH5_COUNTER_NEW) and differs from the primary only in the money
 * block; every other block is identical in both copies. */
#define SYNTH5_TRAINER_NAME "Hilbert"
#define SYNTH5_TID 24680
#define SYNTH5_SID 13579
#define SYNTH5_MONEY_NEW 3000u
#define SYNTH5_MONEY_OLD 1000u
#define SYNTH5_COUNTER_OLD 4u
#define SYNTH5_COUNTER_NEW 5u
#define SYNTH5_BADGES 0x07
#define SYNTH5_MAP 391
#define SYNTH5_BOX_SPECIES 506      /* Lillipup in box 1 slot 1 */
#define SYNTH5_CARD_ID 77           /* Wonder Card in slot 1 */
#define SYNTH5_MG_SEED 0x13572468u

/* A hand-made Pokémon (decrypted). Party data is filled when `party`. */
void synth5_make_mon(pkm5 *p, uint16_t species, uint8_t level, uint32_t pid, uint8_t nature, const char *nickname,
                     uint16_t move0, int party);

/* A Wonder Card (.pgf) of `type` (1 Pokémon: Victini Lv15, 2 item: Master
 * Ball) with card id `id` and title "Test Card". */
void synth5_make_pgf(uint8_t card[SAVE5_PGF_SIZE], uint8_t type, uint16_t id);

/* Fill `img` (SAVE5_IMAGE_SIZE bytes) with a complete two-copy save of
 * `game`. Blocks save5 does not interpret, and the bytes past the two
 * copies, hold deterministic noise so tests can check edits preserve them.
 * Party: Snivy Lv5 "SNIVY", Patrat Lv3 "Pat". Bag: 5 Potions (medicine),
 * 10 Poke Balls (items). Flags 10 and 2000 set; var 0x4010 = 0x1234. Dex:
 * Snivy caught, Patrat seen. */
void synth5_build(uint8_t *img, save5_game game);

#endif
