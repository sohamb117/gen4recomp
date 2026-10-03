/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tables emitted into the build dir by features/tools/gen_charmap.py. */
#ifndef NDSDATA_GEN4_CHARMAP_H
#define NDSDATA_GEN4_CHARMAP_H

#include <stddef.h>
#include <stdint.h>

struct g4_char_entry {
    uint16_t code;
    const char *text; /* UTF-8 */
};

extern const struct g4_char_entry g4_charmap[];     /* sorted by code */
extern const size_t g4_charmap_count;
extern const struct g4_char_entry g4_charmap_enc[]; /* sorted by text bytes */
extern const size_t g4_charmap_enc_count;
extern const struct g4_char_entry g4_cmdmap[];      /* sorted by code */
extern const size_t g4_cmdmap_count;
extern const uint16_t g4_strvar_hi[];
extern const size_t g4_strvar_hi_count;

#endif
