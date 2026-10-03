/* Host stand-in for the SDK's SINIT section.

   On the DS, including this header puts NitroStaticInit in a section the
   overlay loader walks. gcc ignores those pragmas, so without this the
   function is compiled and never called; which is why a Poketch app
   overlay comes up with a garbage currAppInit.

   The constructor only RECORDS the pointer. Running it at process start
   would be wrong: PoketchSystem_SetAppFunctions needs a live system, and
   battle_main's NitroStaticInit loads another overlay. FS_StartOverlay
   calls the ones that belong to the overlay that just loaded. */

#ifndef POKEPLATINUM_PC_NITRO_SINIT_H
#define POKEPLATINUM_PC_NITRO_SINIT_H

#ifdef PLATFORM_PC

void pc_sinit_record(const char *file, void (*fn)(void));

static void NitroStaticInit(void);

static void pc_sinit_ctor(void) __attribute__((constructor));
static void pc_sinit_ctor(void)
{
    /* __BASE_FILE__ is the TU on the command line, not this header. */
    pc_sinit_record(__BASE_FILE__, NitroStaticInit);
}

#elif defined(SDK_CW) || defined(__MWERKS__)
    static void NitroStaticInit(void);

    #pragma define_section SINIT ".sinit" abs32 RWX
    #pragma section        SINIT begin

    SDK_FORCE_EXPORT static void * NitroStaticInit_[] = { NitroStaticInit };
    #pragma section        SINIT end
#elif defined(SDK_ADS)
    TO BE DEFINED
#elif defined(SDK_GCC)
    TO BE DEFINED
#endif

#endif
