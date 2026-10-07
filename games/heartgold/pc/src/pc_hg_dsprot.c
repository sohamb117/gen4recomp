/*
 * The anti-piracy overlay's entry points (lib/dsprot, overlay ds_protect).
 *
 * DSProt is copy protection: its tests read the card's secure area and CRCs
 * and the console's MAC address and owner data, and a positive answer makes
 * the game sabotage itself (fieldmap.c, overlay_124.c, touch_save_app.c,
 * pokedex, overlay_27.s). The port does not bypass it (no forced "genuine"
 * answer) and does not emulate the hardware it probes. Reaching any entry
 * point stops the core with a message naming it, the same rule as Black/White
 * ov230 (docs/BW_PLAN.md).
 */
#include <nitro/types.h>

/* stdio/stdlib clash with the game headers here (wchar_t); wasi-libc's write. */
extern long write(int fd, const void *buf, unsigned long len);

static void dsprot_stop(const char *name) __attribute__((noreturn));
static void dsprot_stop(const char *name)
{
    static const char head[] = "pc_hg_dsprot: ";
    static const char tail[] = ": anti-piracy check reached; not bypassed or emulated, stopping\n";
    unsigned long n = 0;
    while (name[n] != '\0') n++;
    write(2, head, sizeof head - 1);
    write(2, name, n);
    write(2, tail, sizeof tail - 1);
    __builtin_trap();
}

u32 DSProt_DetectFlashcart(void *callback)    { (void)callback; dsprot_stop("DSProt_DetectFlashcart"); }
u32 DSProt_DetectNotFlashcart(void *callback) { (void)callback; dsprot_stop("DSProt_DetectNotFlashcart"); }
u32 DSProt_DetectEmulator(void *callback)     { (void)callback; dsprot_stop("DSProt_DetectEmulator"); }
u32 DSProt_DetectNotEmulator(void *callback)  { (void)callback; dsprot_stop("DSProt_DetectNotEmulator"); }
u32 DSProt_DetectDummy(void *callback)        { (void)callback; dsprot_stop("DSProt_DetectDummy"); }
u32 DSProt_DetectNotDummy(void *callback)     { (void)callback; dsprot_stop("DSProt_DetectNotDummy"); }
