/*
 * Experimental fixed-result substitution; not a verified playable HGSS port.
 * The original detector tests are not executed. Positive answers retain
 * dsprotMain's callback-on-true convention; negative answers do not call it.
 * Shared by the HEARTGOLD and SOULSILVER build configurations.
 */
#include <nitro/types.h>

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
