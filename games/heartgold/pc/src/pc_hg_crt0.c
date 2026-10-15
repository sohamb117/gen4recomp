/*
 * The one function of HG/SS's crt0 (lib/asm/crt0.s, not recompiled: the
 * host is the reset entry) that game code calls: NitroSDK 4.2's
 * OSi_ReferSymbol, which only keeps a symbol referenced for the linker
 * (wifi.s and libVCT.s call it). Its ROM body is `bx lr`.
 */
void OSi_ReferSymbol(void *symbol)
{
    (void)symbol;
}
