/*
 * wasm shadow of lib/gds/include/gds_internal.h.
 *
 * That header DEFINES a variable at file scope (`UnkStruct_ov61_0222E764
 * Unk_ov61_0222E764;`, a tentative definition) and five lib/gds TUs include
 * it. mwcc and gcc -fcommon merge the five into one common symbol; wasm has
 * no common symbols (pc/Makefile.wasm builds -fno-common), so the link sees
 * five strong definitions. Declaring the name weak before the header defines
 * it gives the same result as the common merge: five identical zero-filled
 * weak definitions, one survives, every TU's references bind to it.
 */
#ifndef POKEPLATINUM_WASM_GDS_INTERNAL_H
#define POKEPLATINUM_WASM_GDS_INTERNAL_H

#pragma weak Unk_ov61_0222E764

#include_next "gds_internal.h"

#endif /* POKEPLATINUM_WASM_GDS_INTERNAL_H */
