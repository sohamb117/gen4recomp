/*
 * Hashes are the No-Intro SHA-1s of the full (untrimmed) cartridge dumps.
 */
#include "romdb.h"

#include <string.h>

static const np_rom_entry entries[] = {
    {"a46233d8b79a69ea87aa295a0efad5237d02841e", NP_GAME_DIAMOND, NP_ROM_ACCEPTED, "Pokemon Diamond (USA)"},
    {"99083bf15ec7c6b81b4ba241ee10abd9e80999ac", NP_GAME_PEARL, NP_ROM_ACCEPTED, "Pokemon Pearl (USA)"},
    {"0862ec35b24de5c7e2dcb88c9eea0873110d755c", NP_GAME_PLATINUM, NP_ROM_ACCEPTED,
     "Pokemon Platinum (USA) (Rev 1)"},
    {"ce81046eda7d232513069519cb2085349896dec7", NP_GAME_PLATINUM, NP_ROM_UNSUPPORTED,
     "Pokemon Platinum (USA) (Rev 0)"},
};

const np_rom_entry *np_romdb_lookup(const char *sha1_hex)
{
    for (size_t i = 0; i < sizeof entries / sizeof entries[0]; i++)
        if (!strcmp(entries[i].sha1, sha1_hex))
            return &entries[i];
    return NULL;
}

const np_rom_entry *np_romdb_accepted(np_game game)
{
    for (size_t i = 0; i < sizeof entries / sizeof entries[0]; i++)
        if (entries[i].game == game && entries[i].status == NP_ROM_ACCEPTED)
            return &entries[i];
    return NULL;
}

const char *np_game_title(np_game game)
{
    static const char *const titles[NP_GAME_COUNT] = {"Diamond", "Pearl", "Platinum"};
    return (unsigned)game < NP_GAME_COUNT ? titles[game] : "?";
}

const char *np_game_id(np_game game)
{
    static const char *const ids[NP_GAME_COUNT] = {"diamond", "pearl", "platinum"};
    return (unsigned)game < NP_GAME_COUNT ? ids[game] : "unknown";
}
