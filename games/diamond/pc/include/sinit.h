/*
 * wasm override for include/sinit.h.
 *
 * On the DS this header puts &NitroStaticInit into the overlay's .sinit
 * section, and FS_StartOverlay calls every entry of that section after the
 * overlay is loaded (Poketch-style overlays hand their callbacks to the
 * system there). clang ignores mwcc's section pragmas, so without this the
 * function would compile and never run.
 *
 * Here a constructor RECORDS the function with the overlay it belongs to;
 * nothing runs at process start. The host's FS_StartOverlay (DPHost,
 * pc/src) calls the recorded functions of the overlay it has just loaded.
 * PC_DP_OVERLAY is the overlay number, from the TU's arm9/overlays/NN path
 * (pc/mk/game.mk); a TU outside an overlay that includes this fails to
 * compile, loudly.
 *
 * Found ahead of include/sinit.h because every includer is a .c file
 * (`#include "sinit.h"`), whose quote search reaches the -I list, where
 * pc/include comes first.
 */
#ifndef GUARD_SINIT_H
#define GUARD_SINIT_H

void pc_dp_sinit_record(unsigned overlay_id, void (*fn)(void));

static void NitroStaticInit(void);

__attribute__((constructor)) static void pc_dp_sinit_ctor(void)
{
    pc_dp_sinit_record(PC_DP_OVERLAY, NitroStaticInit);
}

#endif // GUARD_SINIT_H
