/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Internal helpers shared by the Gen 4 and Gen 5 sources of ndsdata. */
#ifndef NDSDATA_GEN5_H
#define NDSDATA_GEN5_H

#include "ndsdata/ndsdata.h"

/* Load a NARC by path and parse it; *data owns the bytes (gen4_data.c). */
nd_status nd_load_narc(const nd_rom *rom, const char *path, uint8_t **data, size_t *len, nd_narc *narc);

/* Every string of a Gen 5 message bank as malloc'd UTF-8 (gen5_text.c). */
nd_status g5_msgbank_strings(const uint8_t *data, size_t size, char ***list, uint32_t *count);

/* Black/White species, experience, move tables and type chart (gen5_data.c). */
nd_status g5_gamedata_load(nd_gamedata *gd, const nd_rom *rom);

/* Zone id -> location-name index table from the zone headers (gen5_data.c). */
nd_status g5_zone_locations_load(const nd_rom *rom, uint16_t **zone_location, uint32_t *zone_count);

#endif
