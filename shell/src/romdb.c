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
    {"26ad0b9967aa279c4a266ee69f52b9b2332399a5", NP_GAME_BLACK, NP_ROM_ACCEPTED,
     "Pokemon - Black Version (USA, Europe) (NDSi Enhanced)"},
    {"bc696a0dfb448c7b3a8a206f0f8214411a039208", NP_GAME_WHITE, NP_ROM_ACCEPTED,
     "Pokemon - White Version (USA, Europe) (NDSi Enhanced)"},
    /* GBA: the US 1.0 releases pret's pokeruby and pokeemerald match */
    {"f28b6ffc97847e94a6c21a63cacf633ee5c8df1e", NP_GAME_RUBY, NP_ROM_ACCEPTED, "Pokemon - Ruby Version (USA)"},
    {"610b96a9c9a7d03d2bafb655e7560ccff1a6d894", NP_GAME_RUBY, NP_ROM_UNSUPPORTED,
     "Pokemon - Ruby Version (USA, Europe) (Rev 1)"},
    {"5b64eacf892920518db4ec664e62a086dd5f5bc8", NP_GAME_RUBY, NP_ROM_UNSUPPORTED,
     "Pokemon - Ruby Version (USA, Europe) (Rev 2)"},
    {"3ccbbd45f8553c36463f13b938e833f652b793e4", NP_GAME_SAPPHIRE, NP_ROM_ACCEPTED,
     "Pokemon - Sapphire Version (USA)"},
    {"4722efb8cd45772ca32555b98fd3b9719f8e60a9", NP_GAME_SAPPHIRE, NP_ROM_UNSUPPORTED,
     "Pokemon - Sapphire Version (USA, Europe) (Rev 1)"},
    {"89b45fb172e6b55d51fc0e61989775187f6fe63c", NP_GAME_SAPPHIRE, NP_ROM_UNSUPPORTED,
     "Pokemon - Sapphire Version (USA, Europe) (Rev 2)"},
    {"f3ae088181bf583e55daf962a92bb46f4f1d07b7", NP_GAME_EMERALD, NP_ROM_ACCEPTED,
     "Pokemon - Emerald Version (USA, Europe)"},
};

static const char *const k_titles[NP_GAME_COUNT] = {
    [NP_GAME_DIAMOND] = "Diamond", [NP_GAME_PEARL] = "Pearl",       [NP_GAME_PLATINUM] = "Platinum",
    [NP_GAME_BLACK] = "Black",     [NP_GAME_WHITE] = "White",       [NP_GAME_RUBY] = "Ruby",
    [NP_GAME_SAPPHIRE] = "Sapphire", [NP_GAME_EMERALD] = "Emerald"};
static const char *const k_ids[NP_GAME_COUNT] = {
    [NP_GAME_DIAMOND] = "diamond", [NP_GAME_PEARL] = "pearl",       [NP_GAME_PLATINUM] = "platinum",
    [NP_GAME_BLACK] = "black",     [NP_GAME_WHITE] = "white",       [NP_GAME_RUBY] = "ruby",
    [NP_GAME_SAPPHIRE] = "sapphire", [NP_GAME_EMERALD] = "emerald"};

const np_game np_launcher_games[] = {NP_GAME_DIAMOND, NP_GAME_PEARL, NP_GAME_PLATINUM, NP_GAME_BLACK,
                                     NP_GAME_WHITE,   NP_GAME_RUBY,  NP_GAME_SAPPHIRE, NP_GAME_EMERALD};
const int np_launcher_game_count = (int)(sizeof np_launcher_games / sizeof np_launcher_games[0]);

int np_launcher_card(np_game game)
{
    for (int i = 0; i < np_launcher_game_count; i++)
        if (np_launcher_games[i] == game)
            return i;
    return 0;
}

int np_game_known(np_game game) { return (unsigned)game < NP_GAME_COUNT && k_ids[game] != NULL; }

int np_game_is_gba(np_game game)
{
    return game == NP_GAME_RUBY || game == NP_GAME_SAPPHIRE || game == NP_GAME_EMERALD;
}

int np_game_from_id(const char *id)
{
    for (int g = 0; g < NP_GAME_COUNT; g++) {
        if (!k_ids[g])
            continue;
        size_t i = 0;
        while (k_ids[g][i] && id[i] && (id[i] | 0x20) == k_ids[g][i])
            i++;
        if (!k_ids[g][i] && !id[i])
            return g;
    }
    return -1;
}

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
    return np_game_known(game) ? k_titles[game] : "?";
}

const char *np_game_id(np_game game)
{
    return np_game_known(game) ? k_ids[game] : "unknown";
}
