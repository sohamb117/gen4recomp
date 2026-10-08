/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Gen 4 save image (Sinnoh and Johto): copy selection, checksums and typed
 * views.
 *
 * Block layout (offsets within one 0x40000 copy):
 *   Each save table entry occupies SaveTableEntry_BodySize() bytes: Platinum
 *   rounds sizeof() up past the next multiple of 4 and adds 4
 *   (pokeplatinum src/savedata.c); D/P only round up past the next multiple of
 *   4 (pokediamond arm9/src/save.c SaveArray_sizeof). Entries are laid out in
 *   gSaveTable order (pokeplatinum src/savedata/save_table.c, pokediamond
 *   arm9/src/save_arrays.c) and every block ends with a 0x14-byte footer.
 *
 * Offsets below come from PKHeX (GPLv3; PKHeX.Core/Saves/SAV4Pt.cs,
 * SAV4DP.cs, SAV4.cs: GeneralSize, StorageSize, Trainer1, Party, EventWork,
 * EventFlag, PokeDex) and were cross-checked against the decomp structs with
 * the size rules above:
 *   Pt: SystemData 0x00 (body 0x64) -> PlayerSave 0x64 (sizeof 0x2C, body
 *       0x34) -> Party 0x98 (sizeof 0x590, body 0x598) -> Bag 0x630 (sizeof
 *       0x774, body 0x77C) -> VarsFlags 0xDAC (vars u16[288], flags at 0xFEC)
 *       ... Pokedex 0x1328; general block 0xCF2C. Storage block = PCBoxes
 *       (sizeof 0x121C8, body 0x121D0) + footer = 0x121E4.
 *   D/P: SystemData 0x00 (body 0x60) -> PlayerData 0x60 (body 0x30) -> Party
 *       0x90 (body 0x594) -> Bag 0x624 (body 0x778) -> VarsFlags 0xD9C (flags
 *       at 0xFDC) ... Pokedex 0x12DC; general block 0xC100. Storage block =
 *       PCStorage (pokediamond include/pokemon_storage_system.h, sizeof
 *       0x121C8, body 0x121CC)
 *       + footer = 0x121E0.
 *
 * HG/SS (pokeheartgold): every entry occupies GetSaveChunkSizePlusCRC() =
 *   ((sizeof + 3) & ~3) + 4 bytes in gSaveChunkHeaders order (src/save.c
 *   SaveData_InitSubstructs, src/save_arrays.c); the general block (entries
 *   0-40) ends in a 0x10-byte SaveChunkFooter, and the storage block (entry
 *   41, PCStorage) starts at the next 0x100 boundary (SaveData_InitSlotSpecs).
 *   The sizeof()s, taken from the decomp's structs (and the asm *_sizeof
 *   constants for the undecompiled entries), give:
 *     SysInfo 0x00 (0x5C) -> PLAYERDATA 0x60 (0x2C) -> Party 0x90 (0x5B0:
 *     PartyCore 0x590 + PartyExtra 0x20) -> Bag 0x644 (0x79C) -> VarsFlags
 *     0xDE4 (0x44C: vars u16[368], flags at 0x10C4) -> LocalFieldData 0x1234
 *     (0x80) -> Pokedex 0x12B8 (0x340) -> ... MysteryGift 0x9D3C (entry 27,
 *     0x1680) ... TrainerHouse 0xE714 (0xF00); general block 0xF628 =
 *     0xE714 + 0xF04 + 0x10. Storage block at 0xF700: PCStorage (sizeof
 *     0x122FC, body 0x12300) + footer = 0x12310. These match PKHeX's
 *     SAV4HGSS GeneralSize 0xF628, StorageSize 0x12310 and storage start
 *     0xF700.
 */
#include "save4/save4.h"

#include <stdlib.h>
#include <string.h>

#include "ndsdata/ndsdata.h"

struct save4_layout {
    save4_game game;
    bool hgss;               /* HG/SS footer, copy selection and PCStorage */
    uint32_t general_size;
    uint32_t storage_offset; /* within a copy */
    uint32_t storage_size;
    uint32_t player; /* PlayerSave / PlayerData */
    uint32_t party;  /* Party: capacity, count, Pokemon[6] */
    uint32_t bag;
    const uint8_t *pocket_cap; /* slots per pocket, in Bag order */
    uint32_t vars;
    uint32_t flags;
    uint16_t num_vars;
    uint32_t dex;
    uint32_t mg_unlocked;  /* SystemData.isMysteryGiftUnlocked / SaveSysInfo / SysInfo.mysteryGiftActive */
    uint32_t dex_obtained; /* Pokedex.pokedexObtained / unlockedSinnohDex / dexEnabled, relative to dex */
    uint32_t dex_national; /* Pokedex.nationalDexObtained / unlockedNationalDex / nationalDex, relative to dex */
    uint32_t mystery;      /* MysteryGift save table entry */
    /* Inside the MysteryGift entry (see the Mystery Gift section below). */
    uint16_t mg_pgt_used;  /* u32 pgtUsed[8] slot markers; 0: a slot is used when its type is valid */
    uint16_t mg_card_used; /* u32 cardUsed[3] slot markers; 0: likewise */
    uint16_t mg_pgts;      /* PGT pgts[8] */
    uint16_t mg_cards;     /* WonderCard wonderCards[3] */
    uint16_t mg_special;   /* HG/SS specialWonderCard; 0: none */
    uint16_t mg_crc;       /* entry CRC-16 offset (= bytes covered); 0: the game keeps none */
    uint8_t mg_link_card0; /* PGT.wondercardSlot written for Wonder Card slot 0 (slot i: +i) */
    uint8_t mg_link_none;  /* PGT.wondercardSlot written for a gift received without a card */
    uint8_t mg_tag_end;    /* a slot's type is valid while 0 < type < mg_tag_end */
    uint32_t mg_types;     /* bit t: MysteryGiftType t can be delivered */
    uint32_t location;     /* FieldOverworldState / LocalFieldData: Location player, entrance */
    uint32_t poketch;      /* Poketch, the entry right after VarsFlags; 0: none */
    /* PC storage, relative to the storage block. */
    uint32_t box_current;  /* u32 current box */
    uint32_t box_mons;     /* box 0 slot 0 */
    uint32_t box_stride;   /* bytes per box */
    uint32_t box_names;    /* u16 names[18][20] */
    uint32_t box_modified; /* HG/SS u32 boxModifiedFlag; 0: none */
};

#define MG_TYPES(lo, hi) ((uint32_t)(((1ull << ((hi) + 1)) - 1) & ~((1ull << (lo)) - 1)))
#define MG_TYPE(t) (1u << (t))

static const uint8_t kPocketCapSinnoh[SAVE4_POCKET_COUNT] = {165, 50, 100, 12, 40, 64, 15, 30};
/* NUM_BAG_ITEMS .. NUM_BAG_BATTLE_ITEMS, pokeheartgold include/constants/items.h. */
static const uint8_t kPocketCapJohto[SAVE4_POCKET_COUNT] = {165, 50, 101, 12, 40, 64, 24, 30};

/*
 * Pt: SystemData.isMysteryGiftUnlocked 0x48 (body 0x64, first entry);
 *     Pokedex.pokedexObtained 0x31A, nationalDexObtained 0x31B;
 *     MysteryGift 0xB4C0, sizeof 0x1328 (0x100 + 8 * 0x104 + 3 * 0x358),
 *     CRC-16 at 0x132C over 0x132C bytes (SaveData_SetChecksum).
 * D/P (pokediamond):
 *   mg_unlocked: SaveSysInfo (include/save_system_info.h, entry 0 at 0x00)
 *     mysteryGiftActive 0x48 = rtcOffset 8 + macAddr 6 + birth 2 + RTC 0x38.
 *   dex: Pokedex (include/pokedex.h) unlockedSinnohDex 0x138 (set by
 *     ScrCmd_GiveSinnohDex), unlockedNationalDex 0x139 (ScrCmd_NationalDex).
 *   mystery: save table entry 31 (arm9/src/save_arrays.c, sub_0202AC20 ->
 *     0x1354), counted back from the end of the general block with
 *     SaveArray_sizeof = n + 4 - n % 4:
 *       0xC100 - 0x14 footer                             = 0xC0EC
 *       - entry 34 SaveEasyChat (8 -> 0xC)               = 0xC0E0
 *       - entry 33 UnkSaveStruct0202C0E4 (0x28 -> 0x2C)  = 0xC0B4
 *       - entry 32 sub_0202BE98 (0x688 -> 0x68C)         = 0xBA28
 *       - entry 31 MysteryGift (0x1354 -> 0x1358)        = 0xA6D0
 *     Its shape (arm9/asm/unk_0202AC20.s) differs from Pt: u8 received[256]
 *     0x000, u32 pgtUsed[8] 0x100, u32 cardUsed[3] 0x120, PGT[8] 0x12C,
 *     WonderCard[3] 0x94C (0x12C + 8 * 0x104), end 0x1354 (0x94C + 3 * 0x358);
 *     no entry CRC (save.c has no SaveData_SetChecksum).
 *   location: LocalFieldData (save table entry 6, sub_02034D7C -> 0xA0;
 *     arm9/asm/unk_02034D7C.s: the current Location at +0, the entrance
 *     at +0x14) directly before the Pokedex: 0x12DC - 0xA4 = 0x1238. Pt's
 *     FieldOverworldState (sizeof 0xA0, body 0xA8) likewise ends at the
 *     Pokedex: 0x1328 - 0xA8 = 0x1280, as a pc_lab-minted save shows.
 *   poketch: the save table entry after VarsFlags (both tables), so
 *     VarsFlags + its body: sizeof 0x3AC (vars u16[288] + flags u8[364]),
 *     Pt body 0x3B4 -> 0xDAC + 0x3B4 = 0x1160; D/P body 0x3B0 -> 0xD9C +
 *     0x3B0 = 0x114C. Cross-check: Pt Poketch body 0x120 ends at 0x1280,
 *     D/P Poketch (include/poketch.h, sizeof 0xE8, body 0xEC) ends at
 *     0x1238, each the next entry's offset above.
 * HG/SS (pokeheartgold):
 *   mg_unlocked: SysInfo.mysteryGiftActive (include/sav_system_info.h) 0x48
 *     = rtc_offset 8 + mac 6 + birth 2 + SysInfo_RTC 0x38 (the compiler
 *     aligns s64 to 4, as the D/P layout above shows).
 *   dex: Pokedex (include/pokedex.h, size 0x340) dexEnabled 0x336
 *     (Pokedex_Enable), nationalDex 0x337 (Pokedex_SetNatDexFlag).
 *   mystery: MysteryGiftSave (include/mystery_gift.h): receivedFlags 0x000,
 *     MysteryGift gifts[8] 0x100, WonderCard cards[3] 0x920,
 *     specialWonderCard 0x1328, size 0x1680; CRC-16 at 0x1680 over 0x1680
 *     bytes (SaveSubstruct_UpdateCRC / SaveSubstruct_AssertCRC, src/save.c).
 *     A gift without card is linked to 3 (ov74 TryInsertGift(mg, gift, 3)),
 *     a card's gift to its slot (SaveMysteryGift_TryInsertCard).
 *   location: LocalFieldData.currentPosition 0x1234.
 *   PCStorage (include/pokemon_storage_system.h): PC_BOX boxes[18] (30
 *     BoxPokemon + 16 bytes = 0x1000 each), curBox 0x12000,
 *     boxModifiedFlag 0x12004, box_names 0x12008.
 */
static const save4_layout kLayouts[] = {
    {.game = SAVE4_GAME_PT, .general_size = 0xCF2C, .storage_offset = 0xCF2C, .storage_size = 0x121E4,
     .player = 0x64, .party = 0x98, .bag = 0x630, .pocket_cap = kPocketCapSinnoh, .vars = 0xDAC, .flags = 0xFEC,
     .num_vars = SAVE4_NUM_VARS, .dex = 0x1328, .mg_unlocked = 0x48, .dex_obtained = 0x31A, .dex_national = 0x31B,
     .mystery = 0xB4C0, .mg_pgts = 0x100, .mg_cards = 0x920, .mg_crc = 0x132C, .mg_link_card0 = 0, .mg_link_none = 3,
     .mg_tag_end = SAVE4_MG_UNKNOWN + 1, .mg_types = MG_TYPES(SAVE4_MG_POKEMON, SAVE4_MG_UNKNOWN),
     .location = 0x1280, .poketch = 0x1160, .box_current = 0, .box_mons = 4,
     .box_stride = SAVE4_BOX_SLOTS * PKM4_BOX_SIZE, .box_names = 0x11EE4},
    {.game = SAVE4_GAME_DP, .general_size = 0xC100, .storage_offset = 0xC100, .storage_size = 0x121E0,
     .player = 0x60, .party = 0x90, .bag = 0x624, .pocket_cap = kPocketCapSinnoh, .vars = 0xD9C, .flags = 0xFDC,
     .num_vars = SAVE4_NUM_VARS, .dex = 0x12DC, .mg_unlocked = 0x48, .dex_obtained = 0x138, .dex_national = 0x139,
     .mystery = 0xA6D0, .mg_pgt_used = 0x100, .mg_card_used = 0x120, .mg_pgts = 0x12C, .mg_cards = 0x94C,
     .mg_link_card0 = 1, .mg_link_none = 0, .mg_tag_end = SAVE4_MG_SECRET_KEY,
     .mg_types = MG_TYPES(SAVE4_MG_POKEMON, SAVE4_MG_POKETCH_APP), .location = 0x1238, .poketch = 0x114C,
     .box_current = 0, .box_mons = 4, .box_stride = SAVE4_BOX_SLOTS * PKM4_BOX_SIZE, .box_names = 0x11EE4},
    {.game = SAVE4_GAME_HG, .hgss = true, .general_size = 0xF628, .storage_offset = 0xF700,
     .storage_size = 0x12310, .player = 0x60, .party = 0x90, .bag = 0x644, .pocket_cap = kPocketCapJohto,
     .vars = 0xDE4, .flags = 0x10C4, .num_vars = SAVE4_HGSS_NUM_VARS, .dex = 0x12B8, .mg_unlocked = 0x48,
     .dex_obtained = 0x336, .dex_national = 0x337, .mystery = 0x9D3C, .mg_pgts = 0x100, .mg_cards = 0x920,
     .mg_special = 0x1328, .mg_crc = 0x1680, .mg_link_card0 = 0, .mg_link_none = 3,
     .mg_tag_end = SAVE4_MG_MEMORIAL_PHOTO + 1, /* MG_TAG_MAX */
     .mg_types = MG_TYPES(SAVE4_MG_POKEMON, SAVE4_MG_BATTLE_REG) | MG_TYPE(SAVE4_MG_COSMETICS) |
                 MG_TYPE(SAVE4_MG_MANAPHY_EGG) | MG_TYPES(SAVE4_MG_UNKNOWN, SAVE4_MG_MEMORIAL_PHOTO),
     .location = 0x1234, .poketch = 0, .box_current = 0x12000, .box_mons = 0, .box_stride = 0x1000,
     .box_names = 0x12008, .box_modified = 0x12004},
};

/* PlayerSave (include/save_player.h) = Options(2) + pad(2) + TrainerInfo
 * (include/trainer_info.h) + coins + PlayTime. HG/SS PLAYERDATA
 * (include/player_data.h) has the same shape: Options, PlayerProfile
 * (version 0x20, kantoBadges 0x23), coins, IGT. */
enum {
    PL_NAME = 0x04, /* charcode_t name[8] */
    PL_ID = 0x14,   /* u32: TID low, SID high */
    PL_MONEY = 0x18,
    PL_GENDER = 0x1C,
    PL_LANGUAGE = 0x1D,
    PL_BADGES = 0x1E,
    PL_GAMECODE = 0x20,   /* version */
    PL_STORYFLAGS = 0x21, /* bit0 isMainStoryCleared, bit1 hasNationalDex */
    PL_KANTO_BADGES = 0x23, /* HG/SS only */
    PL_COINS = 0x24,
    PL_HOURS = 0x26,
    PL_MINUTES = 0x28,
    PL_SECONDS = 0x29
};

/* Box names: PC_BOX_NAME_BUFFER_LEN / HG/SS BOX_NAME_LENGTH. */
enum {
    BOX_NAME_CODES = 20,
    BOX_NAME_MAX = 8      /* BOX_NAME_LEN, include/constants/string.h */
};

#define VERSION_HEARTGOLD 7  /* pokeheartgold include/config.h */
#define VERSION_SOULSILVER 8
#define ITEM_LOCK_CAPSULE 533 /* pokeheartgold include/constants/items.h */

#define DEX_MAGIC 0xBEEFCAFEu /* MAGIC_NUMBER, include/pokedex.h */
#define DEX_CAUGHT 0x04
#define DEX_SEEN 0x44

static const char *const kPocketNames[SAVE4_POCKET_COUNT] = {
    "items", "key_items", "tms_hms", "mail", "medicine", "berries", "balls", "battle_items"};

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t g32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void s16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void s32(uint8_t *p, uint32_t v)
{
    s16(p, (uint16_t)v);
    s16(p + 2, (uint16_t)(v >> 16));
}

const char *save4_status_str(save4_status st)
{
    switch (st) {
    case SAVE4_OK: return "ok";
    case SAVE4_ERR_ARG: return "invalid argument";
    case SAVE4_ERR_SIZE: return "save image smaller than 512 KiB";
    case SAVE4_ERR_EMPTY: return "save image is blank (no save data)";
    case SAVE4_ERR_UNKNOWN_GAME: return "not a Diamond/Pearl/Platinum/HeartGold/SoulSilver save";
    case SAVE4_ERR_CHECKSUM: return "save checksum failure: no valid copy of a save block";
    case SAVE4_ERR_RANGE: return "value or index out of range";
    case SAVE4_ERR_NOMEM: return "out of memory";
    case SAVE4_ERR_ENCODE: return "text contains characters the game cannot display";
    case SAVE4_ERR_PKM_CHECKSUM: return "Pokemon data checksum mismatch";
    case SAVE4_ERR_LAYOUT: return "unexpected save structure";
    case SAVE4_ERR_NOSPACE: return "no free slot";
    case SAVE4_ERR_UNSUPPORTED: return "not supported by this game";
    }
    return "unknown error";
}

const char *save4_game_name(save4_game g)
{
    switch (g) {
    case SAVE4_GAME_DP: return "DP";
    case SAVE4_GAME_PT: return "Pt";
    case SAVE4_GAME_HG: return "heartgold";
    case SAVE4_GAME_SS: return "soulsilver";
    default: return "unknown";
    }
}

bool save4_game_is_hgss(save4_game g) { return g == SAVE4_GAME_HG || g == SAVE4_GAME_SS; }

/* MATH_CalcCRC16CCITT (NitroSDK libraries/math/src/crc.c): table-driven,
 * r = (r << 8) ^ t[((r >> 8) ^ byte) & 0xFF], init 0xFFFF. */
uint16_t save4_crc16(const void *data, size_t len)
{
    static uint16_t table[256];
    static int init;
    if (!init) {
        for (int i = 0; i < 256; i++) {
            uint16_t r = (uint16_t)(i << 8);
            for (int b = 0; b < 8; b++)
                r = (uint16_t)((r & 0x8000) ? ((r << 1) ^ 0x1021) : (r << 1));
            table[i] = r;
        }
        init = 1;
    }
    const uint8_t *p = data;
    uint32_t r = 0xFFFF;
    for (size_t i = 0; i < len; i++)
        r = ((r << 8) ^ table[((r >> 8) ^ p[i]) & 0xFF]) & 0xFFFF;
    return (uint16_t)r;
}

/* ---------------------------------------------------- footers / copies */

static uint32_t block_offset(const save4_layout *L, int b)
{
    return b == SAVE4_BLOCK_GENERAL ? 0 : L->storage_offset;
}
static uint32_t block_size(const save4_layout *L, int b)
{
    return b == SAVE4_BLOCK_GENERAL ? L->general_size : L->storage_size;
}
static uint32_t footer_size(const save4_layout *L) { return L->hgss ? SAVE4_HGSS_FOOTER_SIZE : SAVE4_FOOTER_SIZE; }

/* SaveBlockFooter_Validate (HG/SS ValidateSaveSectorFooter) minus the
 * checksum. Both footers end in the u16 CRC. */
static bool footer_shape_ok(const uint8_t *img, const save4_layout *L, int copy, int b)
{
    const uint8_t *f = img + copy * SAVE4_COPY_SIZE + block_offset(L, b) + block_size(L, b) - footer_size(L);
    if (L->hgss)
        return g32(f + 4) == block_size(L, b) && g32(f + 8) == SAVE4_SIGNATURE && g16(f + 12) == b;
    return g32(f + 8) == block_size(L, b) && g32(f + 12) == SAVE4_SIGNATURE && f[16] == b;
}

static void check_block(save4 *s, int b)
{
    const save4_layout *L = s->layout;
    save4_block_state *st = &s->blocks[b];
    const uint32_t fs = footer_size(L);
    st->offset = block_offset(L, b);
    st->size = block_size(L, b);
    for (int c = 0; c < 2; c++) {
        const uint8_t *base = s->img + c * SAVE4_COPY_SIZE + st->offset;
        const uint8_t *f = base + st->size - fs;
        st->save_counter[c] = g32(f);
        st->block_counter[c] = L->hgss ? 0 : g32(f + 4);
        st->stored_crc[c] = g16(base + st->size - 2);
        st->valid[c] = footer_shape_ok(s->img, L, c, b) && st->stored_crc[c] == save4_crc16(base, st->size - fs);
    }
}

/* SaveCheckInfo_CompareCounters (HG/SS SaveCounterCompare: the same) */
static int cmp_counters(uint32_t a, uint32_t b)
{
    if (a == 0xFFFFFFFFu && b == 0)
        return -1;
    if (a == 0 && b == 0xFFFFFFFFu)
        return 1;
    return a > b ? 1 : (a < b ? -1 : 0);
}

enum { SECTOR_VALID, SECTOR_PARTIAL, SECTOR_INVALID };

/* SaveCheckInfo_CompareSectors */
static int cmp_sectors(const save4_block_state *b, int *cur, int *stale)
{
    if (b->valid[0] && b->valid[1]) {
        int g = cmp_counters(b->save_counter[0], b->save_counter[1]);
        int k = cmp_counters(b->block_counter[0], b->block_counter[1]);
        int primary = g > 0 || (g == 0 && k >= 0);
        *cur = primary ? 0 : 1;
        *stale = !*cur;
        return SECTOR_VALID;
    }
    if (b->valid[0] || b->valid[1]) {
        *cur = b->valid[0] ? 0 : 1;
        *stale = -1;
        return SECTOR_PARTIAL;
    }
    *cur = *stale = -1;
    return SECTOR_INVALID;
}

/* SaveData_LoadCheck */
static save4_status select_copies(save4 *s)
{
    save4_block_state *n = &s->blocks[SAVE4_BLOCK_GENERAL], *x = &s->blocks[SAVE4_BLOCK_STORAGE];
    int cn, sn, cb, sb;
    int rn = cmp_sectors(n, &cn, &sn);
    int rb = cmp_sectors(x, &cb, &sb);
    if (rn == SECTOR_INVALID || rb == SECTOR_INVALID)
        return SAVE4_ERR_CHECKSUM;

    int use_n = cn, use_b = cb;
    save4_load_result res = SAVE4_LOAD_OK;
    if (rn == SECTOR_VALID && rb == SECTOR_VALID) {
        if (n->save_counter[cn] != x->save_counter[cb]) {
            use_n = sn;
            res = SAVE4_LOAD_RECOVERED;
        }
    } else if (rn == SECTOR_PARTIAL && rb == SECTOR_VALID) {
        res = SAVE4_LOAD_RECOVERED;
        if (n->save_counter[cn] == x->save_counter[cb])
            use_b = cb;
        else if (n->save_counter[cn] == x->save_counter[sb])
            use_b = sb;
        else
            return SAVE4_ERR_CHECKSUM; /* LOAD_RESULT_ERROR */
    } else if (rn == SECTOR_VALID && rb == SECTOR_PARTIAL) {
        if (n->save_counter[cn] != x->save_counter[cb]) {
            use_n = sn;
            res = SAVE4_LOAD_RECOVERED;
        }
    } else {
        res = cn == cb ? SAVE4_LOAD_OK : SAVE4_LOAD_RECOVERED;
    }
    n->active = use_n;
    x->active = use_b;
    s->load_result = res;
    return SAVE4_OK;
}

/* HG/SS SaveSlotCheckCompare: 2, 1 or 0 valid copies; *newer / *older the
 * copy index (2: none). An invalid copy counts as 0
 * (SaveSlotCheck_InitFromSavedat). */
static int hgss_compare(const save4_block_state *b, int *newer, int *older)
{
    int r = cmp_counters(b->valid[0] ? b->save_counter[0] : 0, b->valid[1] ? b->save_counter[1] : 0);
    if (b->valid[0] && b->valid[1]) {
        *newer = r < 0 ? 1 : 0;
        *older = !*newer;
        return 2;
    }
    *older = 2;
    if (b->valid[0] || b->valid[1]) {
        *newer = b->valid[0] ? 0 : 1;
        return 1;
    }
    *newer = 2;
    return 0;
}

static uint32_t hgss_count(const save4_block_state *b, int copy) { return b->valid[copy] ? b->save_counter[copy] : 0; }

/* HG/SS Save_GetSaveFilesStatus (pokeheartgold src/save.c): both blocks come
 * from one copy, the one whose general and storage counts agree.
 * LOAD_STATUS_IS_GOOD -> OK, LOAD_STATUS_SLOT_FAIL -> RECOVERED,
 * LOAD_STATUS_TOTAL_FAIL / NOT_EXIST -> checksum failure. */
static save4_status select_copies_hgss(save4 *s)
{
    save4_block_state *n = &s->blocks[SAVE4_BLOCK_GENERAL], *x = &s->blocks[SAVE4_BLOCK_STORAGE];
    int nn, on, nb, ob;
    int gn = hgss_compare(n, &nn, &on), gb = hgss_compare(x, &nb, &ob);
    if (gn == 0 || gb == 0)
        return SAVE4_ERR_CHECKSUM;
    int slot = -1;
    save4_load_result res = SAVE4_LOAD_OK;
    if (gn == 2 && gb == 2) {
        if (hgss_count(n, nn) == hgss_count(x, nn)) {
            slot = nn;
        } else if (hgss_count(n, on) == hgss_count(x, on)) {
            slot = on;
            res = SAVE4_LOAD_RECOVERED;
        }
    } else if (gn == 1 && gb == 2) {
        if (hgss_count(n, nn) == hgss_count(x, nn)) {
            slot = nn;
            res = SAVE4_LOAD_RECOVERED;
        }
    } else if (gn == 2 && gb == 1) {
        if (hgss_count(n, nn) == hgss_count(x, nn)) {
            slot = nn;
        } else if (hgss_count(n, on) == hgss_count(x, on)) {
            slot = on;
            res = SAVE4_LOAD_RECOVERED;
        }
    } else if (nn == nb) {
        slot = nn;
    }
    if (slot < 0)
        return SAVE4_ERR_CHECKSUM;
    n->active = x->active = slot;
    s->load_result = res;
    return SAVE4_OK;
}

/* HG and SS share the layout; PlayerProfile.version (set by
 * PlayerProfile_Init to GAME_VERSION, pokeheartgold src/player_data.c) tells
 * them apart. Read from the active copy, or the primary when none loads. */
static save4_game hgss_version(const save4 *s, int copy)
{
    switch (s->img[copy * SAVE4_COPY_SIZE + s->layout->player + PL_GAMECODE]) {
    case VERSION_HEARTGOLD: return SAVE4_GAME_HG;
    case VERSION_SOULSILVER: return SAVE4_GAME_SS;
    default: return SAVE4_GAME_UNKNOWN;
    }
}

static save4_status detect(save4 *s)
{
    s->layout = NULL;
    s->game = SAVE4_GAME_UNKNOWN;
    for (size_t i = 0; i < sizeof(kLayouts) / sizeof(kLayouts[0]) && !s->layout; i++) {
        for (int c = 0; c < 2; c++) {
            if (footer_shape_ok(s->img, &kLayouts[i], c, SAVE4_BLOCK_GENERAL) ||
                footer_shape_ok(s->img, &kLayouts[i], c, SAVE4_BLOCK_STORAGE)) {
                s->layout = &kLayouts[i];
                break;
            }
        }
    }
    if (!s->layout) {
        for (size_t i = 0; i < SAVE4_IMAGE_SIZE; i++)
            if (s->img[i] != 0xFF && s->img[i] != 0x00)
                return SAVE4_ERR_UNKNOWN_GAME;
        return SAVE4_ERR_EMPTY;
    }
    s->game = s->layout->game;
    for (int b = 0; b < SAVE4_BLOCK_COUNT; b++)
        check_block(s, b);
    if (!s->layout->hgss)
        return select_copies(s);
    save4_status st = select_copies_hgss(s);
    s->game = hgss_version(s, st == SAVE4_OK ? s->blocks[SAVE4_BLOCK_GENERAL].active : 0);
    if (st == SAVE4_OK && s->game == SAVE4_GAME_UNKNOWN)
        return SAVE4_ERR_UNKNOWN_GAME;
    return st;
}

save4_status save4_load(save4 *s, const uint8_t *data, size_t len)
{
    memset(s, 0, sizeof(*s));
    if (!data)
        return SAVE4_ERR_ARG;
    if (len < SAVE4_IMAGE_SIZE)
        return SAVE4_ERR_SIZE;
    s->img = malloc(len);
    if (!s->img)
        return SAVE4_ERR_NOMEM;
    memcpy(s->img, data, len);
    s->len = len;
    save4_status st = detect(s);
    if (st != SAVE4_OK) {
        /* keep detection results for diagnostics, but drop the copy */
        free(s->img);
        s->img = NULL;
        s->len = 0;
    }
    return st;
}

save4_status save4_revalidate(save4 *s)
{
    if (!s->img)
        return SAVE4_ERR_ARG;
    return detect(s);
}

save4_status save4_clone(const save4 *src, save4 *dst)
{
    *dst = *src;
    dst->img = malloc(src->len);
    if (!dst->img)
        return SAVE4_ERR_NOMEM;
    memcpy(dst->img, src->img, src->len);
    return SAVE4_OK;
}

void save4_free(save4 *s)
{
    free(s->img);
    memset(s, 0, sizeof(*s));
}

const uint8_t *save4_image(const save4 *s, size_t *len)
{
    if (len)
        *len = s->len;
    return s->img;
}

uint32_t save4_block_base(const save4 *s, save4_block_id b)
{
    return (uint32_t)s->blocks[b].active * SAVE4_COPY_SIZE + s->blocks[b].offset;
}

void save4_commit_block(save4 *s, save4_block_id b)
{
    save4_block_state *st = &s->blocks[b];
    uint8_t *base = s->img + save4_block_base(s, b);
    uint16_t crc = save4_crc16(base, st->size - footer_size(s->layout));
    s16(base + st->size - 2, crc); /* the footer's last field in every game */
    st->stored_crc[st->active] = crc;
    st->valid[st->active] = true;
}

static const uint8_t *gen_c(const save4 *s) { return s->img + save4_block_base(s, SAVE4_BLOCK_GENERAL); }
static uint8_t *gen_m(save4 *s) { return s->img + save4_block_base(s, SAVE4_BLOCK_GENERAL); }
static const uint8_t *sto_c(const save4 *s) { return s->img + save4_block_base(s, SAVE4_BLOCK_STORAGE); }
static uint8_t *sto_m(save4 *s) { return s->img + save4_block_base(s, SAVE4_BLOCK_STORAGE); }

#define REQUIRE_LOADED(s)          \
    do {                           \
        if (!(s) || !(s)->img)     \
            return SAVE4_ERR_ARG;  \
    } while (0)

/* ------------------------------------------------------------ trainer */

static void decode_codes(const uint8_t *src, int n, char *out, size_t cap)
{
    uint16_t buf[32];
    for (int i = 0; i < n; i++)
        buf[i] = g16(src + i * 2);
    g4_text_decode(buf, (size_t)n, out, cap);
}

save4_status save4_get_trainer(const save4 *s, save4_trainer *t)
{
    REQUIRE_LOADED(s);
    const uint8_t *p = gen_c(s) + s->layout->player;
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < 8; i++)
        t->name_raw[i] = g16(p + PL_NAME + i * 2);
    decode_codes(p + PL_NAME, 8, t->name, sizeof(t->name));
    t->tid = g16(p + PL_ID);
    t->sid = g16(p + PL_ID + 2);
    t->money = g32(p + PL_MONEY);
    t->gender = p[PL_GENDER];
    t->language = p[PL_LANGUAGE];
    t->badges = p[PL_BADGES];
    t->game_code = p[PL_GAMECODE];
    t->kanto_badges = s->layout->hgss ? p[PL_KANTO_BADGES] : 0;
    t->main_story_cleared = p[PL_STORYFLAGS] & 1;
    t->has_national_dex = (p[PL_STORYFLAGS] >> 1) & 1;
    t->coins = g16(p + PL_COINS);
    t->play_hours = g16(p + PL_HOURS);
    t->play_minutes = p[PL_MINUTES];
    t->play_seconds = p[PL_SECONDS];
    return SAVE4_OK;
}

static uint8_t *player_m(save4 *s) { return gen_m(s) + s->layout->player; }

save4_status save4_set_money(save4 *s, uint32_t money)
{
    REQUIRE_LOADED(s);
    if (money > SAVE4_MONEY_MAX)
        return SAVE4_ERR_RANGE;
    s32(player_m(s) + PL_MONEY, money);
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return SAVE4_OK;
}

save4_status save4_set_coins(save4 *s, uint16_t coins)
{
    REQUIRE_LOADED(s);
    if (coins > SAVE4_COINS_MAX)
        return SAVE4_ERR_RANGE;
    s16(player_m(s) + PL_COINS, coins);
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return SAVE4_OK;
}

static save4_status write_name(uint8_t *dst, int slots, int max_chars, const char *utf8)
{
    uint16_t buf[32];
    size_t n;
    nd_status st = g4_text_encode(utf8, buf, (size_t)max_chars + 1, &n);
    if (st == ND_ERR_RANGE)
        return SAVE4_ERR_RANGE;
    if (st != ND_OK)
        return SAVE4_ERR_ENCODE;
    if (n == 0)
        return SAVE4_ERR_RANGE;
    for (int i = 0; i < slots; i++)
        s16(dst + i * 2, (size_t)i < n ? buf[i] : 0xFFFF);
    return SAVE4_OK;
}

save4_status save4_set_trainer_name(save4 *s, const char *utf8)
{
    REQUIRE_LOADED(s);
    save4_status st = write_name(player_m(s) + PL_NAME, 8, 7, utf8); /* TRAINER_NAME_LEN */
    if (st == SAVE4_OK)
        save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return st;
}

save4_status save4_set_trainer_ids(save4 *s, uint16_t tid, uint16_t sid)
{
    REQUIRE_LOADED(s);
    s16(player_m(s) + PL_ID, tid);
    s16(player_m(s) + PL_ID + 2, sid);
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return SAVE4_OK;
}

save4_status save4_set_gender(save4 *s, uint8_t gender)
{
    REQUIRE_LOADED(s);
    if (gender > 1)
        return SAVE4_ERR_RANGE;
    player_m(s)[PL_GENDER] = gender;
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return SAVE4_OK;
}

save4_status save4_set_badges(save4 *s, uint8_t mask)
{
    REQUIRE_LOADED(s);
    player_m(s)[PL_BADGES] = mask;
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return SAVE4_OK;
}

save4_status save4_set_kanto_badges(save4 *s, uint8_t mask)
{
    REQUIRE_LOADED(s);
    if (!s->layout->hgss)
        return SAVE4_ERR_UNSUPPORTED;
    player_m(s)[PL_KANTO_BADGES] = mask;
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return SAVE4_OK;
}

save4_status save4_set_play_time(save4 *s, uint16_t hours, uint8_t minutes, uint8_t seconds)
{
    REQUIRE_LOADED(s);
    /* PlayTime_Increment caps at 999:59:59 */
    if (hours > 999 || minutes > 59 || seconds > 59)
        return SAVE4_ERR_RANGE;
    uint8_t *p = player_m(s);
    s16(p + PL_HOURS, hours);
    p[PL_MINUTES] = minutes;
    p[PL_SECONDS] = seconds;
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return SAVE4_OK;
}

/* ------------------------------------------------------------- party */

uint8_t save4_party_count(const save4 *s)
{
    if (!s || !s->img)
        return 0;
    uint32_t n = g32(gen_c(s) + s->layout->party + 4);
    return (uint8_t)(n > SAVE4_PARTY_MAX ? SAVE4_PARTY_MAX : n);
}

save4_status save4_get_party(const save4 *s, int slot, pkm4 *out)
{
    REQUIRE_LOADED(s);
    if (slot < 0 || slot >= SAVE4_PARTY_MAX)
        return SAVE4_ERR_RANGE;
    return pkm4_decrypt(gen_c(s) + s->layout->party + 8 + slot * PKM4_PARTY_SIZE, PKM4_PARTY_SIZE, out);
}

save4_status save4_set_party(save4 *s, int slot, pkm4 *p)
{
    REQUIRE_LOADED(s);
    if (slot < 0 || slot >= SAVE4_PARTY_MAX)
        return SAVE4_ERR_RANGE;
    save4_status st = pkm4_encrypt(p, gen_m(s) + s->layout->party + 8 + slot * PKM4_PARTY_SIZE, PKM4_PARTY_SIZE);
    if (st == SAVE4_OK)
        save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return st;
}

save4_status save4_set_party_count(save4 *s, uint8_t count)
{
    REQUIRE_LOADED(s);
    if (count > SAVE4_PARTY_MAX)
        return SAVE4_ERR_RANGE;
    s32(gen_m(s) + s->layout->party + 4, count);
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return SAVE4_OK;
}

/* -------------------------------------------------------------- boxes */

static save4_status box_slot_ok(int box, int slot)
{
    return (box < 0 || box >= SAVE4_BOX_COUNT || slot < 0 || slot >= SAVE4_BOX_SLOTS) ? SAVE4_ERR_RANGE : SAVE4_OK;
}

static uint32_t box_mon_offset(const save4_layout *L, int box, int slot)
{
    return L->box_mons + (uint32_t)box * L->box_stride + (uint32_t)slot * PKM4_BOX_SIZE;
}

save4_status save4_get_box_mon(const save4 *s, int box, int slot, pkm4 *out)
{
    REQUIRE_LOADED(s);
    if (box_slot_ok(box, slot) != SAVE4_OK)
        return SAVE4_ERR_RANGE;
    return pkm4_decrypt(sto_c(s) + box_mon_offset(s->layout, box, slot), PKM4_BOX_SIZE, out);
}

save4_status save4_set_box_mon(save4 *s, int box, int slot, pkm4 *p)
{
    REQUIRE_LOADED(s);
    if (box_slot_ok(box, slot) != SAVE4_OK)
        return SAVE4_ERR_RANGE;
    const save4_layout *L = s->layout;
    save4_status st = pkm4_encrypt(p, sto_m(s) + box_mon_offset(L, box, slot), PKM4_BOX_SIZE);
    if (st != SAVE4_OK)
        return st;
    /* HG/SS write only the boxes flagged here (plus last save's) to the other
     * copy on the next save (src/save.c Save_CalcPCBoxModifiedFlags,
     * Save_WriteNextPCBox); every box change sets its bit
     * (PCStorage_SetBoxModified, src/pokemon_storage_system.c). */
    if (L->box_modified) {
        uint8_t *f = sto_m(s) + L->box_modified;
        s32(f, g32(f) | 1u << box);
    }
    save4_commit_block(s, SAVE4_BLOCK_STORAGE);
    return SAVE4_OK;
}

save4_status save4_clear_box_mon(save4 *s, int box, int slot)
{
    /* BoxPokemon_Init (HG/SS ZeroBoxMonData): zero, then encrypt with checksum 0. */
    pkm4 empty;
    memset(&empty, 0, sizeof(empty));
    return save4_set_box_mon(s, box, slot, &empty);
}

save4_status save4_get_box_name(const save4 *s, int box, char *utf8, size_t cap)
{
    REQUIRE_LOADED(s);
    if (box < 0 || box >= SAVE4_BOX_COUNT)
        return SAVE4_ERR_RANGE;
    decode_codes(sto_c(s) + s->layout->box_names + box * BOX_NAME_CODES * 2, BOX_NAME_CODES, utf8, cap);
    return SAVE4_OK;
}

save4_status save4_set_box_name(save4 *s, int box, const char *utf8)
{
    REQUIRE_LOADED(s);
    if (box < 0 || box >= SAVE4_BOX_COUNT)
        return SAVE4_ERR_RANGE;
    save4_status st =
        write_name(sto_m(s) + s->layout->box_names + box * BOX_NAME_CODES * 2, BOX_NAME_CODES, BOX_NAME_MAX, utf8);
    if (st == SAVE4_OK)
        save4_commit_block(s, SAVE4_BLOCK_STORAGE);
    return st;
}

uint32_t save4_current_box(const save4 *s)
{
    if (!s || !s->img)
        return 0;
    return g32(sto_c(s) + s->layout->box_current);
}

/* ---------------------------------------------------------------- bag */

const char *save4_pocket_name(save4_pocket p)
{
    return (unsigned)p < SAVE4_POCKET_COUNT ? kPocketNames[p] : NULL;
}

int save4_pocket_capacity(const save4 *s, save4_pocket p)
{
    return s && s->layout && (unsigned)p < SAVE4_POCKET_COUNT ? s->layout->pocket_cap[p] : 0;
}

static int pocket_offset(const save4_layout *L, save4_pocket p)
{
    int off = 0;
    for (int i = 0; i < (int)p; i++)
        off += L->pocket_cap[i] * 4;
    return off;
}

save4_status save4_get_bag_slot(const save4 *s, save4_pocket p, int slot, uint16_t *item, uint16_t *qty)
{
    REQUIRE_LOADED(s);
    if (slot < 0 || slot >= save4_pocket_capacity(s, p))
        return SAVE4_ERR_RANGE;
    const uint8_t *e = gen_c(s) + s->layout->bag + pocket_offset(s->layout, p) + slot * 4;
    *item = g16(e);
    *qty = g16(e + 2);
    return SAVE4_OK;
}

save4_status save4_set_bag_slot(save4 *s, save4_pocket p, int slot, uint16_t item, uint16_t qty)
{
    REQUIRE_LOADED(s);
    if (slot < 0 || slot >= save4_pocket_capacity(s, p))
        return SAVE4_ERR_RANGE;
    if (qty > 999 || (item == 0) != (qty == 0))
        return SAVE4_ERR_RANGE;
    uint8_t *e = gen_m(s) + s->layout->bag + pocket_offset(s->layout, p) + slot * 4;
    s16(e, item);
    s16(e + 2, qty);
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return SAVE4_OK;
}

save4_status save4_get_registered_item(const save4 *s, uint16_t *item)
{
    REQUIRE_LOADED(s);
    *item = (uint16_t)g32(gen_c(s) + s->layout->bag + pocket_offset(s->layout, SAVE4_POCKET_COUNT));
    return SAVE4_OK;
}

/* ------------------------------------------------------------ Pokédex */

save4_status save4_dex_get(const save4 *s, uint16_t species, bool *seen, bool *caught)
{
    REQUIRE_LOADED(s);
    if (species == 0 || species > SAVE4_DEX_MAX)
        return SAVE4_ERR_RANGE;
    const uint8_t *d = gen_c(s) + s->layout->dex;
    if (g32(d) != DEX_MAGIC)
        return SAVE4_ERR_LAYOUT;
    unsigned bit = species - 1u; /* ReadBit_2Forms: bitIndex - 1 */
    *caught = (d[DEX_CAUGHT + (bit >> 3)] >> (bit & 7)) & 1;
    *seen = (d[DEX_SEEN + (bit >> 3)] >> (bit & 7)) & 1;
    return SAVE4_OK;
}

save4_status save4_dex_set(save4 *s, uint16_t species, bool seen, bool caught)
{
    REQUIRE_LOADED(s);
    if (species == 0 || species > SAVE4_DEX_MAX)
        return SAVE4_ERR_RANGE;
    uint8_t *d = gen_m(s) + s->layout->dex;
    if (g32(d) != DEX_MAGIC)
        return SAVE4_ERR_LAYOUT;
    seen = seen || caught; /* the game never records a catch without a sighting */
    unsigned bit = species - 1u;
    uint8_t m = (uint8_t)(1u << (bit & 7));
    uint8_t *c = &d[DEX_CAUGHT + (bit >> 3)], *v = &d[DEX_SEEN + (bit >> 3)];
    *c = (uint8_t)(caught ? (*c | m) : (*c & ~m));
    *v = (uint8_t)(seen ? (*v | m) : (*v & ~m));
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return SAVE4_OK;
}

/* ------------------------------------------------------- vars & flags */

int save4_num_flags(const save4 *s) { return s && s->layout ? SAVE4_NUM_FLAGS : 0; }
int save4_num_vars(const save4 *s) { return s && s->layout ? s->layout->num_vars : 0; }

save4_status save4_flag_get(const save4 *s, uint16_t id, bool *value)
{
    REQUIRE_LOADED(s);
    if (id == 0 || id >= save4_num_flags(s))
        return SAVE4_ERR_RANGE;
    *value = (gen_c(s)[s->layout->flags + id / 8] >> (id % 8)) & 1;
    return SAVE4_OK;
}

save4_status save4_flag_set(save4 *s, uint16_t id, bool value)
{
    REQUIRE_LOADED(s);
    if (id == 0 || id >= save4_num_flags(s))
        return SAVE4_ERR_RANGE;
    uint8_t *b = gen_m(s) + s->layout->flags + id / 8;
    uint8_t m = (uint8_t)(1u << (id % 8));
    *b = (uint8_t)(value ? (*b | m) : (*b & ~m));
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return SAVE4_OK;
}

save4_status save4_var_get(const save4 *s, uint16_t id, uint16_t *value)
{
    REQUIRE_LOADED(s);
    if (id < SAVE4_VARS_START || id >= SAVE4_VARS_START + save4_num_vars(s))
        return SAVE4_ERR_RANGE;
    *value = g16(gen_c(s) + s->layout->vars + (id - SAVE4_VARS_START) * 2);
    return SAVE4_OK;
}

save4_status save4_var_set(save4 *s, uint16_t id, uint16_t value)
{
    REQUIRE_LOADED(s);
    if (id < SAVE4_VARS_START || id >= SAVE4_VARS_START + save4_num_vars(s))
        return SAVE4_ERR_RANGE;
    s16(gen_m(s) + s->layout->vars + (id - SAVE4_VARS_START) * 2, value);
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return SAVE4_OK;
}

/* Location: u32 mapId, warpId (-1 for a position), x, z, direction. */
save4_status save4_get_location(const save4 *s, save4_location *loc)
{
    REQUIRE_LOADED(s);
    const uint8_t *p = gen_c(s) + s->layout->location;
    loc->map = g32(p);
    loc->warp = (int32_t)g32(p + 4);
    loc->x = g32(p + 8);
    loc->z = g32(p + 12);
    loc->dir = g32(p + 16);
    return SAVE4_OK;
}

/* SystemData / SaveSysInfo / SysInfo: rtcOffset 8 + MAC 6 + birth month,
 * day, then GameTime / SysInfo_RTC at 0x10: canary u32, RTCDate {year,
 * month, day, week} u32 each at 0x14, RTCTime {hour, minute, second} u32
 * each at 0x24. */
save4_status save4_get_game_time(const save4 *s, save4_game_time *t)
{
    REQUIRE_LOADED(s);
    const uint8_t *p = gen_c(s) + 0x10;
    t->year = (uint16_t)(2000 + g32(p + 0x04));
    t->month = (uint8_t)g32(p + 0x08);
    t->day = (uint8_t)g32(p + 0x0C);
    t->hour = (uint8_t)g32(p + 0x14);
    t->minute = (uint8_t)g32(p + 0x18);
    t->second = (uint8_t)g32(p + 0x1C);
    return SAVE4_OK;
}

/* Byte 0 bit 0 poketchEnabled / isGiven; appCount, appIndex; then the
 * 32-byte app registry at 3. */
save4_status save4_get_poketch(const save4 *s, save4_poketch *p)
{
    REQUIRE_LOADED(s);
    if (!s->layout->poketch)
        return SAVE4_ERR_UNSUPPORTED;
    const uint8_t *k = gen_c(s) + s->layout->poketch;
    p->given = (k[0] & 1) != 0;
    for (int i = 0; i < SAVE4_POKETCH_APPS; i++)
        p->apps[i] = k[3 + i] != 0;
    return SAVE4_OK;
}

int save4_pt_lookup_name(const char *name, uint16_t *id)
{
    for (size_t i = 0; i < save4_pt_flag_names_count; i++)
        if (!strcmp(save4_pt_flag_names[i].name, name)) {
            *id = save4_pt_flag_names[i].id;
            return 0;
        }
    for (size_t i = 0; i < save4_pt_var_names_count; i++)
        if (!strcmp(save4_pt_var_names[i].name, name)) {
            *id = save4_pt_var_names[i].id;
            return 0;
        }
    return -1;
}

const char *save4_pt_flag_name(uint16_t id)
{
    size_t lo = 0, hi = save4_pt_flag_names_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (save4_pt_flag_names[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < save4_pt_flag_names_count && save4_pt_flag_names[lo].id == id ? save4_pt_flag_names[lo].name : NULL;
}

/* ------------------------------------------------------- Mystery Gift */
/*
 * MysteryGift: u8 received[256] (bit per Wonder Card id; bit 2047 = Mystery
 * Gift unlocked on the main menu), PGT pgts[8] (0x104 each: the deliveryman
 * hands these out), WonderCard wonderCards[3] (0x358 each).
 *
 * Pt (pokeplatinum include/mystery_gift.h, src/mystery_gift.c): a slot is
 * used while its type is valid; PGT.wondercardSlot is the card slot, 3 for a
 * gift without card. The entry ends in a CRC-16 over the body that
 * SaveData_Checksum verifies (src/savedata.c) when the game opens it, so it
 * is refreshed after every edit, like SaveData_SetChecksum.
 *
 * D/P (pokediamond arm9/asm/unk_0202AC20.s): u32 pgtUsed[8] / cardUsed[3]
 * hold 0xEDB88320 for a used slot (0 = free); PGT.wondercardSlot is card
 * slot + 1, 0 for a gift without card; no entry CRC.
 *
 * HG/SS (pokeheartgold include/mystery_gift.h, src/mystery_gift.c): Pt's
 * shape (MysteryGift.flag = the card link) plus specialWonderCard at
 * 0x1328; the CRC-16 after the body is asserted on every access
 * (Save_MysteryGift_Get -> SaveSubstruct_AssertCRC).
 */
#define MG_RECEIVED 0
#define MG_SLOT_USED 0xEDB88320u
#define MG_UNLOCK_BIT 2047
#define WC_HEADER 0x104
#define WC_ID (WC_HEADER + 0x4C)
#define WC_FLAGS (WC_HEADER + 0x4E)

static uint8_t *mg_m(save4 *s) { return gen_m(s) + s->layout->mystery; }
static const uint8_t *mg_c(const save4 *s) { return gen_c(s) + s->layout->mystery; }

static void mg_commit(save4 *s)
{
    uint16_t crc = s->layout->mg_crc;
    if (crc) {
        uint8_t *mg = mg_m(s);
        s16(mg + crc, save4_crc16(mg, crc));
    }
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
}

/* Any game's MysteryGiftType (the format check for .pgt/.pcd files). */
static bool mg_type_any(uint16_t type) { return type > 0 && type < SAVE4_MG_TYPE_MAX; }
/* A slot of this game holds a gift (Pt CheckIsValidWcType, HG/SS
 * MysteryGiftTagIsValid). */
static bool mg_type_ok(const save4_layout *L, uint16_t type) { return type > 0 && type < L->mg_tag_end; }

static const save4_layout *layout_for(save4_game game)
{
    if (game == SAVE4_GAME_SS)
        game = SAVE4_GAME_HG; /* one layout */
    for (size_t i = 0; i < sizeof(kLayouts) / sizeof(kLayouts[0]); i++)
        if (kLayouts[i].game == game)
            return &kLayouts[i];
    return NULL;
}

bool save4_mg_type_supported(save4_game game, uint16_t type)
{
    const save4_layout *L = layout_for(game);
    return L && type < 32 && ((L->mg_types >> type) & 1);
}

static uint8_t *mg_pgt(uint8_t *mg, const save4_layout *L, int i) { return mg + L->mg_pgts + i * SAVE4_PGT_SIZE; }
static uint8_t *mg_card(uint8_t *mg, const save4_layout *L, int i)
{
    return mg + L->mg_cards + i * SAVE4_WONDERCARD_SIZE;
}

static bool mg_pgt_used(const save4 *s, int i)
{
    const save4_layout *L = s->layout;
    const uint8_t *mg = mg_c(s);
    return L->mg_pgt_used ? g32(mg + L->mg_pgt_used + i * 4) != 0
                          : mg_type_ok(L, g16(mg + L->mg_pgts + i * SAVE4_PGT_SIZE));
}

static bool mg_card_used(const save4 *s, int i)
{
    const save4_layout *L = s->layout;
    const uint8_t *mg = mg_c(s);
    return L->mg_card_used ? g32(mg + L->mg_card_used + i * 4) != 0
                           : mg_type_ok(L, g16(mg + L->mg_cards + i * SAVE4_WONDERCARD_SIZE));
}

/* Stores a PGT in slot i linked to `link` (D/P sub_0202AC98, Pt
 * MysteryGift_TrySavePgt). */
static void mg_put_pgt(save4 *s, int i, const uint8_t *pgt, unsigned link)
{
    const save4_layout *L = s->layout;
    uint8_t *mg = mg_m(s), *dst = mg_pgt(mg, L, i);
    memcpy(dst, pgt, SAVE4_PGT_SIZE);
    s16(dst + 2, (uint16_t)((g16(dst + 2) & ~3u) | (link & 3u)));
    if (L->mg_pgt_used)
        s32(mg + L->mg_pgt_used + i * 4, MG_SLOT_USED);
}

save4_status save4_mg_get_card(const save4 *s, int slot, uint8_t card[SAVE4_WONDERCARD_SIZE], bool *used)
{
    REQUIRE_LOADED(s);
    if (slot < 0 || slot >= SAVE4_WONDERCARD_SLOTS)
        return SAVE4_ERR_RANGE;
    memcpy(card, mg_c(s) + s->layout->mg_cards + slot * SAVE4_WONDERCARD_SIZE, SAVE4_WONDERCARD_SIZE);
    *used = mg_card_used(s, slot);
    return SAVE4_OK;
}

save4_status save4_mg_pgt_count(const save4 *s, int *count)
{
    REQUIRE_LOADED(s);
    *count = 0;
    for (int i = 0; i < SAVE4_PGT_SLOTS; i++)
        *count += mg_pgt_used(s, i);
    return SAVE4_OK;
}

static int mg_free_pgt(const save4 *s)
{
    for (int i = 0; i < SAVE4_PGT_SLOTS; i++)
        if (!mg_pgt_used(s, i))
            return i;
    return -1;
}

static void mg_set_received(save4 *s, uint16_t id, bool on)
{
    uint8_t *b = mg_m(s) + MG_RECEIVED + (id & 0x7FF) / 8, m = (uint8_t)(1u << (id & 7));
    *b = (uint8_t)(on ? (*b | m) : (*b & ~m));
}

save4_status save4_mg_validate(const uint8_t *data, size_t len, const char **why)
{
    if (len != SAVE4_PGT_SIZE && len != SAVE4_WONDERCARD_SIZE) {
        *why = "not a .pgt (260 bytes) or .pcd (856 bytes) file";
        return SAVE4_ERR_SIZE;
    }
    if (!mg_type_any(g16(data))) {
        *why = "unknown gift type";
        return SAVE4_ERR_ARG;
    }
    if (len == SAVE4_WONDERCARD_SIZE) {
        bool ended = false;
        for (int i = 0; i < SAVE4_WC_TITLE_LEN && !ended; i++)
            ended = g16(data + WC_HEADER + i * 2) == 0xFFFF;
        if (!ended) {
            *why = "the card title is not terminated";
            return SAVE4_ERR_ARG;
        }
        if (g16(data + WC_ID) >= SAVE4_MG_ID_MAX) {
            *why = "the card id is out of range";
            return SAVE4_ERR_RANGE;
        }
    }
    *why = NULL;
    return SAVE4_OK;
}

/* The reception bookkeeping of the Mystery Gift app (Pt mystery_gift_app.c
 * + MysteryGift_TrySaveWondercard; D/P overlay 83 ov83_0222FCE4 +
 * sub_0202AD08; HG/SS overlay 74 ov74_0222A1BC + SaveMysteryGift_TryInsertCard):
 * received flag, the card, and its PGT linked to it. */
save4_status save4_mg_add(save4 *s, const uint8_t *data, size_t len)
{
    REQUIRE_LOADED(s);
    const char *why;
    save4_status st = save4_mg_validate(data, len, &why);
    if (st != SAVE4_OK)
        return st;
    if (!save4_mg_type_supported(s->game, g16(data)))
        return SAVE4_ERR_UNSUPPORTED;
    const save4_layout *L = s->layout;
    uint8_t *mg = mg_m(s);
    if (len == SAVE4_PGT_SIZE) {
        int p = mg_free_pgt(s);
        if (p < 0)
            return SAVE4_ERR_NOSPACE;
        mg_put_pgt(s, p, data, L->mg_link_none);
        mg_commit(s);
        return SAVE4_OK;
    }
    if (L->mg_special && ((data[WC_FLAGS] >> 2) & 1) && g16(data) == SAVE4_MG_ITEM &&
        g32(data + 4) == ITEM_LOCK_CAPSULE) {
        /* HG/SS: the Lock Capsule card goes to the special slot
         * (SaveMysteryGift_TrySetSpecialCard keeps an occupied one). */
        uint8_t *special = mg + L->mg_special;
        if (mg_type_ok(L, g16(special)))
            return SAVE4_ERR_NOSPACE;
        memcpy(special, data, SAVE4_WONDERCARD_SIZE);
        mg_set_received(s, g16(data + WC_ID), true);
        mg_commit(s);
        return SAVE4_OK;
    }
    bool save_pgt = (data[WC_FLAGS] >> 3) & 1;
    int slot = -1;
    for (int i = 0; i < SAVE4_WONDERCARD_SLOTS && slot < 0; i++)
        if (!mg_card_used(s, i))
            slot = i;
    int p = save_pgt ? mg_free_pgt(s) : 0;
    if (slot < 0 || p < 0)
        return SAVE4_ERR_NOSPACE;
    memcpy(mg_card(mg, L, slot), data, SAVE4_WONDERCARD_SIZE);
    if (L->mg_card_used)
        s32(mg + L->mg_card_used + slot * 4, MG_SLOT_USED);
    if (save_pgt)
        mg_put_pgt(s, p, data, L->mg_link_card0 + (unsigned)slot);
    mg_set_received(s, g16(data + WC_ID), true);
    mg_commit(s);
    return SAVE4_OK;
}

/* Pt MysteryGift_FreeWcErasePgt: the card's type, its received flag and its
 * PGT. D/P sub_0202ADC8: cardUsed and the PGT linked to slot + 1
 * (sub_0202AEC4 -> sub_0202AD94); the type and received flag stay. HG/SS
 * (ov74 card toss): with a linked gift
 * SaveMysteryGift_ReceiveGiftAndClearCardByIndex (type, received flag and
 * the gift), else SaveMysteryGift_DeleteWonderCardByIndex (the type only). */
save4_status save4_mg_remove_card(save4 *s, int slot)
{
    REQUIRE_LOADED(s);
    if (slot < 0 || slot >= SAVE4_WONDERCARD_SLOTS)
        return SAVE4_ERR_RANGE;
    if (!mg_card_used(s, slot))
        return SAVE4_OK;
    const save4_layout *L = s->layout;
    uint8_t *mg = mg_m(s), *card = mg_card(mg, L, slot);
    unsigned link = L->mg_link_card0 + (unsigned)slot;
    int linked = -1;
    for (int i = 0; i < SAVE4_PGT_SLOTS && linked < 0; i++) {
        const uint8_t *p = mg_pgt(mg, L, i);
        if ((g16(p + 2) & 3u) == link && (L->mg_pgt_used || mg_type_ok(L, g16(p))))
            linked = i;
    }
    if (L->mg_card_used) {
        s32(mg + L->mg_card_used + slot * 4, 0);
    } else {
        s16(card, 0);
        if (!L->hgss || linked >= 0)
            mg_set_received(s, g16(card + WC_ID), false);
    }
    if (linked >= 0) {
        uint8_t *p = mg_pgt(mg, L, linked);
        if (L->mg_pgt_used)
            s32(mg + L->mg_pgt_used + linked * 4, 0);
        else
            s16(p, 0);
        s16(p + 2, (uint16_t)(g16(p + 2) & ~3u));
    }
    mg_commit(s);
    return SAVE4_OK;
}

save4_status save4_mg_get_unlocked(const save4 *s, bool *unlocked)
{
    REQUIRE_LOADED(s);
    *unlocked = gen_c(s)[s->layout->mg_unlocked] ||
                ((mg_c(s)[MG_RECEIVED + MG_UNLOCK_BIT / 8] >> (MG_UNLOCK_BIT & 7)) & 1);
    return SAVE4_OK;
}

/* Both switches the main menu reads (Pt RenderMysteryGiftOption, D/P
 * ov83_0222DF40, HG/SS MainMenu_PrintMysteryGiftButton:
 * mysteryGiftActive == 1 or received bit 2047). */
save4_status save4_mg_set_unlocked(save4 *s, bool unlocked)
{
    REQUIRE_LOADED(s);
    gen_m(s)[s->layout->mg_unlocked] = unlocked ? 1 : 0;
    mg_set_received(s, MG_UNLOCK_BIT, unlocked);
    mg_commit(s);
    return SAVE4_OK;
}

save4_status save4_dex_get_obtained(const save4 *s, bool *obtained)
{
    REQUIRE_LOADED(s);
    const uint8_t *d = gen_c(s) + s->layout->dex;
    if (g32(d) != DEX_MAGIC)
        return SAVE4_ERR_LAYOUT;
    *obtained = d[s->layout->dex_obtained] != 0;
    return SAVE4_OK;
}

save4_status save4_dex_set_obtained(save4 *s, bool obtained)
{
    REQUIRE_LOADED(s);
    uint8_t *d = gen_m(s) + s->layout->dex;
    if (g32(d) != DEX_MAGIC)
        return SAVE4_ERR_LAYOUT;
    d[s->layout->dex_obtained] = obtained ? 1 : 0;
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return SAVE4_OK;
}

save4_status save4_dex_get_national(const save4 *s, bool *obtained)
{
    REQUIRE_LOADED(s);
    const uint8_t *d = gen_c(s) + s->layout->dex;
    if (g32(d) != DEX_MAGIC)
        return SAVE4_ERR_LAYOUT;
    *obtained = d[s->layout->dex_national] != 0;
    return SAVE4_OK;
}

/* Both halves the game's own award writes (the National Dex script command:
 * Pt scrcmd.c Pokedex_ObtainNationalDex + TrainerInfo_GiveNationalDex; D/P
 * ScrCmd_NationalDex Pokedex_SetNatDexFlag + PlayerProfile_SetNatDexFlag,
 * the same two bytes: dex + 0x139 = 1, PlayerData 0x21 bit 1). */
save4_status save4_dex_set_national(save4 *s, bool obtained)
{
    REQUIRE_LOADED(s);
    uint8_t *d = gen_m(s) + s->layout->dex;
    if (g32(d) != DEX_MAGIC)
        return SAVE4_ERR_LAYOUT;
    d[s->layout->dex_national] = obtained ? 1 : 0;
    uint8_t *flags = player_m(s) + PL_STORYFLAGS;
    *flags = (uint8_t)(obtained ? (*flags | 0x2) : (*flags & ~0x2));
    save4_commit_block(s, SAVE4_BLOCK_GENERAL);
    return SAVE4_OK;
}

static save4_status put_text(uint8_t *dst, int slots, const char *utf8)
{
    uint16_t buf[SAVE4_WC_DESC_LEN];
    size_t n;
    nd_status st = g4_text_encode(utf8, buf, (size_t)slots, &n);
    if (st == ND_ERR_RANGE)
        return SAVE4_ERR_RANGE;
    if (st != ND_OK)
        return SAVE4_ERR_ENCODE;
    for (int i = 0; i < slots; i++)
        s16(dst + i * 2, (size_t)i < n ? buf[i] : 0xFFFF);
    return SAVE4_OK;
}

/* A Wonder Card like the ones the game builds itself (ranger_link.c): the
 * card is shown under MYSTERY GIFT > CHECK CARD and its PGT waits for the
 * Poke Mart deliveryman. */
save4_status save4_mg_build_card(const save4_card_spec *spec, uint8_t card[SAVE4_WONDERCARD_SIZE])
{
    memset(card, 0, SAVE4_WONDERCARD_SIZE);
    if (!mg_type_any(spec->type) || spec->id >= SAVE4_MG_ID_MAX)
        return SAVE4_ERR_ARG;
    s16(card, spec->type);
    if (spec->type == SAVE4_MG_ITEM) {
        s32(card + 4, spec->item); /* MysteryGiftItemData.item */
        s32(card + 8, 1);          /* shouldPlayAnimation */
    }
    save4_status st = put_text(card + WC_HEADER, SAVE4_WC_TITLE_LEN, spec->title);
    if (st != SAVE4_OK)
        return st;
    s32(card + WC_HEADER + 0x48, 0x7); /* validGames: Diamond, Pearl, Platinum */
    s16(card + WC_HEADER + 0x4C, spec->id);
    card[WC_FLAGS] = (1u << 2) | (1u << 3); /* hasWonderCard, savePgt */
    st = put_text(card + 0x154, SAVE4_WC_DESC_LEN, spec->description);
    if (st != SAVE4_OK)
        return st;
    for (int i = 0; i < 3; i++)
        s16(card + 0x34A + i * 2, spec->sprites[i]);
    s32(card + 0x354, (uint32_t)spec->received_day);
    return SAVE4_OK;
}
