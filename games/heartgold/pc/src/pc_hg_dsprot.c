/*
 * The anti-piracy overlay's entry points (lib/dsprot, overlay ds_protect).
 *
 * On the cartridge DSProt_Detect* are stubs that decrypt
 * DSProtInternal_Detect* (lib/dsprot/src/dsprot_main.c), run it, and encrypt
 * it again; the ROM build makes them (lib/dsprot/build/*_decrypter.s) from
 * the C. The tests behind them read the card's secure area and CRCs and the
 * console's MAC address and owner data, which the port does not model as
 * the hardware has them, and a failed test makes the game sabotage itself
 * (fieldmap.c moves a heap by 0x3A1 bytes per positive DSProt_DetectFlashcart).
 *
 * So each answers as on a genuine cartridge in a genuine console: every
 * *_IsBad test FALSE, every *_IsGood test TRUE, and the dummy queue empty.
 * dsprotMain's arithmetic then gives Detect{Flashcart,Emulator,Dummy} FALSE
 * and DetectNot{Flashcart,Emulator,Dummy} TRUE, and the callback runs when
 * the answer is TRUE. A callback can be a recompiled function's address
 * (overlay_27.s), so this file goes through the IR bridge like game C.
 */
#include <nitro/types.h>

typedef void (*DSProtCallback)(void);

static u32 dsprot_answer(u32 ret, void *callback)
{
    if (callback != NULL && ret) {
        ((DSProtCallback)callback)();
    }
    return ret;
}

u32 DSProt_DetectFlashcart(void *callback)    { return dsprot_answer(FALSE, callback); }
u32 DSProt_DetectNotFlashcart(void *callback) { return dsprot_answer(TRUE, callback); }
u32 DSProt_DetectEmulator(void *callback)     { return dsprot_answer(FALSE, callback); }
u32 DSProt_DetectNotEmulator(void *callback)  { return dsprot_answer(TRUE, callback); }
u32 DSProt_DetectDummy(void *callback)        { return dsprot_answer(FALSE, callback); }
u32 DSProt_DetectNotDummy(void *callback)     { return dsprot_answer(TRUE, callback); }
