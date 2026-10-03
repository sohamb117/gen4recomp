/*
 * The other way into the driver's registers.
 *
 * pc/arm7snd/include/nitro/consts_shared.h includes this header by name
 * rather than through registers.h, and a translation unit that arrives that
 * way would otherwise get the DS literals: the shadow next to this one only
 * fires when registers.h is asked for. So this file is a forwarder. It pulls
 * in the shadowed registers.h, which pulls in the driver's own pair and then
 * moves every macro in both.
 *
 * Not a copy of the redefinitions. Two files defining the same twenty macros
 * is two files to keep right, and the second one would be the one nobody
 * remembers.
 */

#ifndef POKEPLATINUM_3DS_ARM7SND_REGISTERS_SHARED_H
#define POKEPLATINUM_3DS_ARM7SND_REGISTERS_SHARED_H

#include <registers.h>

#endif /* POKEPLATINUM_3DS_ARM7SND_REGISTERS_SHARED_H */
