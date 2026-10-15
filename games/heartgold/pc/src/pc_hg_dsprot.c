/*
 * Experimental fixed-result substitution; not a verified playable HGSS port.
 * The original detector tests are not executed. Positive answers retain
 * dsprotMain's callback-on-true convention; negative answers do not call it.
 * Shared by the HEARTGOLD and SOULSILVER build configurations.
 */
#include <nitro/types.h>
#include <nitro/fs/overlay.h>

static u32 dsprot_answer(u32 answer, void *callback)
{
    if (answer && callback != NULL) {
        ((void (*)(void))callback)();
    }
    return answer;
}

u32 DSProt_DetectFlashcart(void *callback)    { return dsprot_answer(FALSE, callback); }
u32 DSProt_DetectNotFlashcart(void *callback) { return dsprot_answer(TRUE, callback); }
u32 DSProt_DetectEmulator(void *callback)     { return dsprot_answer(FALSE, callback); }
u32 DSProt_DetectNotEmulator(void *callback)  { return dsprot_answer(TRUE, callback); }
u32 DSProt_DetectDummy(void *callback)        { return dsprot_answer(FALSE, callback); }
u32 DSProt_DetectNotDummy(void *callback)     { return dsprot_answer(TRUE, callback); }

/*
 * ds_protect's static initialisers. The ROM's overlay table lists five, one
 * NitroStaticInit per lib/dsprot/build/*_decoder.s (dsprot_main_decrypter,
 * integrity_decrypter, encryptor, coretests_decrypter, rc4). Each hands its
 * table of encoded function bodies to Encryptor_DecodeFunctionTable, which
 * rewrites those instruction words in place in the overlay's RAM image.
 * Nothing else: no state outside the encoded bodies is written.
 *
 * Here those instruction words are never executed. The overlay's code is
 * lib/dsprot/src's C, compiled in the clear (pc/tools/hg_lsf.py), and its
 * entry points are the detectors above, so the decoders have nothing to do.
 * Each is recorded with the overlay loader (games/diamond/pc/src/
 * pc_dp_overlay.c) as having run, which keeps that loader's count of the
 * ROM table's entries exact for ds_protect: a sixth entry, or a decoder that
 * became recompiled code, still stops the run there.
 */
FS_EXTERN_OVERLAY(ds_protect);

void pc_dp_sinit_record(unsigned overlay_id, void (*fn)(void));

static void dsprot_decoder_superseded(void)
{
}

__attribute__((constructor)) static void dsprot_record_decoders(void)
{
    int i;

    for (i = 0; i < 5; i++) {
        pc_dp_sinit_record(FS_OVERLAY_ID(ds_protect), dsprot_decoder_superseded);
    }
}
