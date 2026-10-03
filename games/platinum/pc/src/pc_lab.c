/*
 * The save lab: manufacture a save that puts the player anywhere, with any
 * party, bag, badges, money and flags, in seconds instead of an evening.
 *
 * Why it mutates through the game and not through Python. The save format is
 * open here, so a Python writer was a real option and was rejected on one
 * measurement: the page layout is not a constant anywhere in the tree, it is
 * computed at boot by SavePageInfo_Init calling each entry's own size
 * function, and those are sizeof() over 38 structs that upstream changes. A
 * Python writer would restate every one of them and be wrong, silently, with a
 * valid CRC over the wrong bytes, the first time a merge moved a field.
 * Mutating in-port through the game's own setters makes every layout fact,
 * every checksum and both copies correct by construction.
 *
 * How a run goes. PC_LAB=FILE makes the boot skip the title screen and the
 * intro: main.c's patch calls pc_lab_boot(), which runs the game's own
 * StartNewSave and then enqueues the new-save application. That runs
 * InitializeNewSave and hands off to the field system's new-game template,
 * which runs the INIT_NEW_GAME script, the ~250 SetFlag and SetVar lines that
 * hide every story NPC. Skipping that script is the trap this design avoids: a
 * save minted without it puts thirty one-shot NPCs on every map at once.
 *
 * Then at frame PC_LAB_AT the recipe is applied and SaveData_Save runs twice,
 * because the game alternates sectors and one call leaves the other copy
 * erased. Two calls leave both copies valid with a consistent counter pair.
 *
 * The recipe reaching this file is numeric. Names are resolved by
 * pc/tests/pc_lab.py out of the tree's own headers, so there is no name table
 * in the port.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Game headers below this line need what the ROM build force-includes into
 * every game translation unit (ALIGN_4, nelems, <nitro.h>); pc/src is not on
 * that force-include, so it asks for the header by name. */
#include "pch/global_pch.h"

#include "constants/charcode.h"
#include "constants/heap.h"

#include "generated/battle_terrains.h"

#include "constants/items.h"

#include "bag.h"
#include "berry_patches.h"
#include "daycare_save.h"
#include "overlay005/daycare.h"
#include "evolution.h"
#include "field_bgm.h"
#include "field_transition.h"
#include "item_use_pokemon.h"
#include "map_header.h"
#include "applications/pokemon_summary_screen/main.h"
#include "sound.h"
#include "sound_playback.h"
#include "savedata_misc.h"
#include "special_encounter.h"
#include "field/field_system.h"
#include "field_system.h"          /* FieldSystem_HasChildProcess */
#include "overlay005/ov5_021EA714.h" /* FieldSystem_SendPoketchEvent */
#include "field_map_change.h"
#include "map_object.h"
#include "map_tile_behavior.h"
#include "terrain_collision_manager.h"
#include "unk_0203C954.h"
#include "field_overworld_state.h"
#include "heap.h"
#include "encounter.h"
#include "field_battle_data_transfer.h"
#include "system_flags.h"
#include "overlay006/wild_encounters.h"
#include "field_task.h"
#include "location.h"
#include "party.h"
#include "player_avatar.h"
#include "applications/poketch/poketch_system.h"
#include "poketch.h"
#include "pokedex.h"
#include "pokemon.h"
#include "save_player.h"
#include "savedata.h"
#include "constants/contests.h"
#include "constants/flavor.h"
#include "contest.h"
#include "poffin.h"
#include "applications/poffin_case/main.h"
#include "chatot_cry.h"
#include "string_gf.h"
#include "unk_020298BC.h"
#include "struct_defs/contest_player_mon_dto.h"
#include "generated/goods.h"
#include "generated/sphere_types.h"
#include "generated/traps.h"
#include "generated/vars_flags.h"
#include "underground.h"
#include "underground/mining.h"
#include "trainer_info.h"
#include "overlay005/save_info_window.h"
#include "unk_02054884.h"
#include "vars_flags.h"

/*
 * The battle lab (PC_LAB_BATTLE), and why it is a second entry point rather
 * than another recipe verb.
 *
 * A recipe mints a save and exits; a battle is something that happens to a
 * save already minted. Folding "fight this" into the recipe language would
 * mean the recipe both built the state and consumed it, and the digest a
 * station pins would be of a run that never loaded its own save. So a battle
 * station is the ordinary corpus shape, a recipe for the save, a spec for
 * the run, with one more input on the run: PC_LAB_BATTLE names the fight,
 * PC_LAB_BATTLE_AT says when, and the spec's input script plays it.
 *
 * Determinism was measured, not assumed, and the answer is that no seed
 * override is needed. The battle RNG is one LCG over BattleSystem.seedRandNext
 * (src/battle/battle_system.c), which battle_main.c seeds from
 * FieldBattleDTO.seed and nothing else re-seeds. That field is built in
 * FieldBattleDTO_New from the RTC date and time plus gSystem.vblankCounter,
 * and in this port BOTH are already inputs: PC_RTC fixes the clock and the
 * vblank counter is the frame number. Same save, same script, same frame in,
 * same battle out. A PC_BATTLE_SEED input would have been a second source of
 * truth for something the existing inputs already pin, so there is not one.
 */
enum lab_battle_kind {
    LAB_BATTLE_NONE = 0,
    LAB_BATTLE_WILD,      /* species, level: Encounter_NewVsSpeciesAtLevel */
    LAB_BATTLE_LEGENDARY, /* the same, with BATTLE_STATUS_LEGENDARY */
    LAB_BATTLE_TRAINER,   /* trainer, trainer2, partner: singles/doubles/tag */
    LAB_BATTLE_SAFARI,    /* species, level, balls: the safari zone's own path */
    LAB_BATTLE_FIRST,     /* the scripted first battle */
    LAB_BATTLE_TUTORIAL,  /* the catching tutorial */
};

#define LAB_MAX_OPS 512
#define LAB_HEAP    HEAP_ID_APPLICATION
/* Longer than the map-name popup, which is on screen for about three
 * seconds after a map change. */
#define LAB_SETTLE_FRAMES 360

enum lab_verb {
    LAB_MAP,        /* map header, x, z, facing */
    LAB_WARP,       /* map header, warp id: the game resolves the tile */
    LAB_MONEY,
    LAB_BADGE,
    LAB_NAME,       /* stored in the string field, not the numbers */
    LAB_GENDER,
    LAB_TRAINER_ID,
    LAB_PARTY,      /* species, level, held item */
    LAB_PARTY_MOVE, /* slot, move slot, move */
    LAB_ITEM,       /* item, count */
    LAB_FLAG,
    LAB_CLEAR_FLAG,
    LAB_VAR,        /* var, value */
    LAB_NATIONAL_DEX,
    LAB_STORY_CLEARED,
    LAB_REGISTER_ITEM,
    /*
     * Two separate flags with two separate consequences: the national one is
     * which list the app shows, and this one is whether the player has a
     * Pokedex at all, StartMenu_GetNormalHiddenOptions hides the whole entry
     * on Pokedex_IsObtained, so a station that wants to open the app has to
     * set it or the start menu it navigates has one fewer row.
     */
    LAB_POKEDEX,
    /*
     * The three below exist for the daily and per-minute systems, which are
     * driven off the clock rather than off anything a player presses: the
     * field runs sub_020559DC on every map load, works out how much time has
     * passed since the save was written, and hands the difference to the berry
     * patches, the honey-tree timers and Shaymin's form. Each one needs state
     * in the save BEFORE the boot that advances it; an empty patch does not
     * grow and an unslathered tree has nothing to count down, and none of
     * that state has a recipe verb otherwise.
     */
    LAB_BERRY,        /* patch index, berry item: planted and growing */
    LAB_HONEY,        /* tree index, minutes left on the slather */
    LAB_SHAYMIN_FORM, /* party slot, form */
    /*
     * The growth loop, the day care, the egg it produces and the walk that
     * hatches it. All six go through the game's own daycare functions; what
     * they exist for is the COUNTERS, because every one of those systems is
     * gated on a number of steps a headless station cannot afford to walk.
     * Daycare_Update ticks once per step, an egg appears when the second
     * parent's own counter wraps at 256, and a party egg only loses a cycle
     * when the shared counter reaches 255, so a station starting from zero
     * would walk 255 steps for the first event and 255 more for each cycle
     * after it. Setting the counters near their thresholds leaves the event
     * itself to the game, on a real step from a real walk.
     */
    LAB_DAYCARE,         /* party slot: deposited the lady's own way */
    LAB_DAYCARE_STEPS,   /* daycare slot, steps that mon has walked */
    LAB_DAYCARE_COUNTER, /* the shared step counter, 0..254 */
    LAB_DAYCARE_EGG,     /* offspring personality: an egg is waiting */
    LAB_TAKE_EGG,        /* the waiting egg, into the party */
    LAB_EGG_CYCLES,      /* party slot, cycles left before it hatches */
    /*
     * A contest entry's whole starting position. The five condition stats and
     * the sheen are what a poffin raises, pokemon_IncreaseValue on
     * MON_DATA_COOL..MON_DATA_TOUGH, capped by MON_DATA_SHEEN at
     * MAX_POKEMON_SHEEN, and the visual round's score is read off them. A
     * station that wanted a contest-ready mon by feeding it would have to cook
     * and feed a few dozen poffins, one minute-long minigame at a time; this
     * sets the number the feeding would have arrived at and leaves the contest
     * itself to the game.
     */
    LAB_CONDITION,       /* party slot, MON_DATA_* param, value */
    /*
     * The other way to a contest-ready entry, and the honest one: cook a
     * poffin into the case and feed it. Both go through the game,
     * Poffin_MakePoffin is what the cooking application calls when the pot
     * stops, and PoffinCase_UpdateMonContestStats is the feeding arithmetic
     * itself, nature preference and sheen included. What is skipped is the
     * poffin case's screen: choosing the poffin and choosing the mouth.
     */
    LAB_POFFIN,      /* flavor, how much of it, smoothness */
    LAB_FEED_POFFIN, /* party slot, poffin slot */
    /*
     * The Underground's bags. These go through the same TryAdd* the vendors
     * and the digging minigame call, so a station that wants a trap in the
     * bag or a sphere of a known size does not have to play the vendor or
     * finish a dig first. secret-base is SecretBase_SetEntrance; the write
     * a successful Digger Drill ends on, so capture is a save fact the
     * decoration path can then see.
     */
    LAB_SPHERE,      /* type, size */
    LAB_TRAP,        /* trap id */
    LAB_GOOD,        /* good id, into the bag */
    LAB_GOOD_PC,     /* good id, into the secret-base PC */
    LAB_SECRET_BASE, /* entrance x, z, dir: marks the base active */
    /*
     * The Poketch. Init already registers the digital watch and leaves the
     * device off; a station that wants the lower screen has to turn it on
     * and name the app that should be showing when the field comes up.
     * appIndex is the App ID, not a slot in the registry,
     * Poketch_CurrentAppID returns it and the overlay table is keyed by it.
     */
    LAB_POKETCH,         /* app id: enable, register, select */
    LAB_POKETCH_STEPS,   /* pedometer count; registers the app so the write sticks */
    LAB_POKETCH_HISTORY, /* party slot, enqueued the way a catch is */
};

struct lab_op {
    enum lab_verb verb;
    int a, b, c, d;
    char text[16];
};

static struct lab_op sOps[LAB_MAX_OPS];
static int sNumOps;
static int sActive;      /* PC_LAB was given and the file parsed */
static int sApplied;
static int sPoketchEnable; /* Enable() after the map change, not during it */
static unsigned long sApplyFrame = 600;
static char sScriptPath[512];
static Location sWarpTo;
static int sWarpPending;
static int sWarpStarted;
static int sWarpSettled;

/*
 * THE SWEEP: one boot, many maps.
 *
 * Visiting every map the warp graph reaches means hundreds of map changes, and
 * doing them one save and one process each costs about a minute apiece,
 * nearly all of it spent booting the game again to throw the boot away. The
 * boot is the expensive part and nothing about it is per-map, so this walks
 * the list inside a single run: change map, wait for the field to say it
 * arrived, sample the screen, write the verdict, next.
 *
 * The verdict line is flushed as it is written, which is what keeps the sweep
 * resumable across the one thing it cannot survive: a map that takes the
 * process down. The last line in the file names the map before the one that
 * did it, so a harness restarts after that and loses nothing.
 */
#define SWEEP_MAX      1024
#define SWEEP_TIMEOUT  3000   /* frames a single map change may take */

struct sweep_entry { int map, warp; };
static struct sweep_entry sSweep[SWEEP_MAX];
static int sSweepCount;
extern void pc_input_hold_idle(int on);
extern void pc_lab_start_map_change(FieldSystem *fs, const Location *loc);
static int sSweepAt = -1;      /* which entry is in flight, -1 before the first */
static FILE *sSweepOut;
static unsigned long sSweepStarted;
static int sSweepRecipeApplied;

/*
 * The Latin block of the game's charcode enum is contiguous and this reads it
 * from the enum rather than from a table of its own, so a charset change
 * upstream moves this with it.
 */
static charcode_t lab_charcode(char c)
{
    if (c >= '0' && c <= '9') return (charcode_t)(CHAR_0 + (c - '0'));
    if (c >= 'A' && c <= 'Z') return (charcode_t)(CHAR_A + (c - 'A'));
    if (c >= 'a' && c <= 'z') return (charcode_t)(CHAR_a + (c - 'a'));
    return (charcode_t)CHAR_SPACE;
}

static void lab_fail(const char *why, const char *what, int line)
{
    fprintf(stderr, "pc_lab: %s:%d: %s: %s\n", sScriptPath, line, why, what);
    exit(2);
}

struct lab_verb_row {
    const char *name;
    enum lab_verb verb;
    int args;      /* how many numbers follow */
    int text;      /* 1 = takes a word instead */
};

/*
 * The verb table IS the recipe language, the same way pc_args.c's table is
 * the command line. An unknown verb is a hard error rather than a skipped
 * line: a recipe whose typo silently did nothing would mint a save that looks
 * right and is not, which is the one failure this whole lab exists to keep
 * out of the corpus.
 */
static const struct lab_verb_row LAB_VERBS[] = {
    { "map",           LAB_MAP,           4, 0 },
    { "warp",          LAB_WARP,          2, 0 },
    { "money",         LAB_MONEY,         1, 0 },
    { "badge",         LAB_BADGE,         1, 0 },
    { "name",          LAB_NAME,          0, 1 },
    { "gender",        LAB_GENDER,        1, 0 },
    { "trainer-id",    LAB_TRAINER_ID,    1, 0 },
    { "party",         LAB_PARTY,         3, 0 },
    { "party-move",    LAB_PARTY_MOVE,    3, 0 },
    { "item",          LAB_ITEM,          2, 0 },
    { "register-item", LAB_REGISTER_ITEM, 1, 0 },
    { "flag",          LAB_FLAG,          1, 0 },
    { "clear-flag",    LAB_CLEAR_FLAG,    1, 0 },
    { "var",           LAB_VAR,           2, 0 },
    { "national-dex",  LAB_NATIONAL_DEX,  1, 0 },
    { "pokedex",       LAB_POKEDEX,       1, 0 },
    { "story-cleared", LAB_STORY_CLEARED, 1, 0 },
    { "berry",         LAB_BERRY,         2, 0 },
    { "honey",         LAB_HONEY,         2, 0 },
    { "shaymin-form",  LAB_SHAYMIN_FORM,  2, 0 },
    { "daycare",         LAB_DAYCARE,         1, 0 },
    { "daycare-steps",   LAB_DAYCARE_STEPS,   2, 0 },
    { "daycare-counter", LAB_DAYCARE_COUNTER, 1, 0 },
    { "daycare-egg",     LAB_DAYCARE_EGG,     1, 0 },
    { "take-egg",        LAB_TAKE_EGG,        0, 0 },
    { "egg-cycles",      LAB_EGG_CYCLES,      2, 0 },
    { "condition",       LAB_CONDITION,       3, 0 },
    { "poffin",          LAB_POFFIN,          3, 0 },
    { "feed-poffin",     LAB_FEED_POFFIN,     2, 0 },
    { "sphere",          LAB_SPHERE,          2, 0 },
    { "trap",            LAB_TRAP,            1, 0 },
    { "good",            LAB_GOOD,            1, 0 },
    { "good-pc",         LAB_GOOD_PC,         1, 0 },
    { "secret-base",     LAB_SECRET_BASE,     3, 0 },
    { "poketch",         LAB_POKETCH,         1, 0 },
    { "poketch-steps",   LAB_POKETCH_STEPS,   1, 0 },
    { "poketch-history", LAB_POKETCH_HISTORY, 1, 0 },
};

static void lab_parse(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int lineno = 0;

    if (f == NULL) {
        fprintf(stderr, "pc_lab: cannot open %s\n", path);
        exit(2);
    }

    while (fgets(line, sizeof line, f) != NULL) {
        char *tok, *save = NULL;
        unsigned i;
        const struct lab_verb_row *row = NULL;
        struct lab_op *op;

        lineno++;
        tok = strchr(line, '#');
        if (tok != NULL) *tok = '\0';

        tok = strtok_r(line, " \t\r\n", &save);
        if (tok == NULL) continue;

        for (i = 0; i < sizeof LAB_VERBS / sizeof LAB_VERBS[0]; i++) {
            if (strcmp(tok, LAB_VERBS[i].name) == 0) { row = &LAB_VERBS[i]; break; }
        }
        if (row == NULL) lab_fail("unknown verb", tok, lineno);
        if (sNumOps >= LAB_MAX_OPS) lab_fail("too many lines", tok, lineno);

        op = &sOps[sNumOps++];
        memset(op, 0, sizeof *op);
        op->verb = row->verb;

        if (row->text) {
            tok = strtok_r(NULL, " \t\r\n", &save);
            if (tok == NULL) lab_fail("missing argument", row->name, lineno);
            snprintf(op->text, sizeof op->text, "%s", tok);
        } else {
            int *dst[4] = { &op->a, &op->b, &op->c, &op->d };
            int n;
            for (n = 0; n < row->args; n++) {
                tok = strtok_r(NULL, " \t\r\n", &save);
                if (tok == NULL) lab_fail("missing argument", row->name, lineno);
                *dst[n] = (int)strtol(tok, NULL, 0);
            }
        }

        if (strtok_r(NULL, " \t\r\n", &save) != NULL) {
            lab_fail("trailing argument", row->name, lineno);
        }
    }

    fclose(f);
}

/*
 * PC_SAVE_LAYOUT=PATH: where each of the 38 save-table pages lands, as the
 * game itself just computed it in SavePageInfo_Init.
 *
 * The reader on the other side (pc/tests/pc_save.py) needs these offsets and
 * this is the only honest way for it to get them: they are not a constant
 * anywhere, they are the running sum of 38 size functions, and a reader that
 * hardcoded them would go quietly wrong on the next upstream struct change
 * while still reporting valid CRCs. So the port states them and nothing else
 * does.
 */
static void lab_dump_layout(SaveData *saveData, const char *path)
{
    FILE *f = fopen(path, "w");
    int i;

    if (f == NULL) {
        fprintf(stderr, "pc_lab: cannot write %s\n", path);
        exit(2);
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"sector_size\": %d,\n", SAVE_SECTOR_SIZE);
    fprintf(f, "  \"page_max\": %d,\n", SAVE_PAGE_MAX);
    fprintf(f, "  \"primary_sector_start\": %d,\n", PRIMARY_SECTOR_START);
    fprintf(f, "  \"backup_sector_start\": %d,\n", BACKUP_SECTOR_START);
    fprintf(f, "  \"signature\": %u,\n", (unsigned)SECTOR_SIGNATURE);
    fprintf(f, "  \"footer_size\": %u,\n", (unsigned)sizeof(SaveBlockFooter));
    fprintf(f, "  \"pages\": [\n");
    for (i = 0; i < SAVE_TABLE_ENTRY_MAX; i++) {
        fprintf(f, "    {\"id\": %d, \"size\": %u, \"location\": %u, \"block\": %u}%s\n",
                saveData->pageInfo[i].pageID,
                (unsigned)saveData->pageInfo[i].size,
                (unsigned)saveData->pageInfo[i].location,
                (unsigned)saveData->pageInfo[i].blockID,
                i + 1 == SAVE_TABLE_ENTRY_MAX ? "" : ",");
    }
    fprintf(f, "  ],\n  \"blocks\": [\n");
    for (i = 0; i < SAVE_BLOCK_ID_MAX; i++) {
        fprintf(f, "    {\"id\": %d, \"offset\": %u, \"size\": %u, \"sectors\": %u}%s\n",
                saveData->blockInfo[i].saveBlockID,
                (unsigned)saveData->blockInfo[i].offset,
                (unsigned)saveData->blockInfo[i].size,
                (unsigned)saveData->blockInfo[i].sectorsInUse,
                i + 1 == SAVE_BLOCK_ID_MAX ? "" : ",");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

/*
 * PC_LAB_MAPSCAN=PATH: what is actually on the map the recipe just loaded.
 *
 * Why this exists. A field-move station has to put the player on a specific
 * tile facing a specific thing; the tree it will cut, the water it will surf
 * onto, the wall it will climb, and only half of that is answerable from the
 * tree. Obstacles are object events, so res/field/events/ gives their exact
 * coordinates; water, waterfalls and rock-climb walls are TILE BEHAVIOURS
 * inside the compiled map, and nothing outside the running game knows where
 * they are. Guessing a tile produces a station that boots, draws an overworld,
 * pins a digest and proves nothing, which is the one failure mode the corpus
 * is built to keep out. So the port says where they are, once, and the recipes
 * are written from the answer.
 *
 * It runs at the same moment the lab saves, the destination map loaded, the
 * player standing on it, so what it reports is the station's own map and not
 * an approximation of it.
 */
/*
 * Order matters and it is not alphabetical. These predicates overlap: a
 * waterfall tile is also surfable; it has to be, you surf up to the foot of
 * it, so asking "is it water" first labels every waterfall in the game as
 * water. Twenty-four maps were scanned for a waterfall and reported none
 * before that was noticed. The specific behaviors are asked about first.
 */
static const char *lab_tile_label(u8 behavior)
{
    if (TileBehavior_IsWaterfall(behavior)) return "waterfall";
    if (TileBehavior_IsRockClimbNorthSouth(behavior)) return "rockclimb-ns";
    if (TileBehavior_IsRockClimbEastWest(behavior)) return "rockclimb-ew";
    if (TileBehavior_IsSurfable(behavior)) return "surf";
    if (TileBehavior_IsShallowWater(behavior)) return "shallow";
    if (TileBehavior_HasEncounters(behavior)) return "encounters";
    if (TileBehavior_IsBikeSlope(behavior)) return "bike-slope";
    if (TileBehavior_IsBikeParking(behavior)) return "bike-parking";
    return NULL;
}

static void lab_scan_map(FieldSystem *fs, const char *path)
{
    const char *radius = getenv("PC_LAB_MAPSCAN_R");
    int r = (radius != NULL && radius[0] != '\0') ? (int)strtol(radius, NULL, 0) : 24;
    int px = PlayerAvatar_GetXPos(fs->playerAvatar);
    int pz = PlayerAvatar_GetZPos(fs->playerAvatar);
    MapObject *facing = NULL;
    int idx = 0;
    int x, z;
    FILE *f = fopen(path, "w");

    if (f == NULL) {
        fprintf(stderr, "pc_lab: cannot write %s\n", path);
        exit(2);
    }

    fprintf(f, "map %d\n", (int)fs->location->mapHeaderID);
    fprintf(f, "player %d %d dir %d behavior %02x\n", px, pz,
            PlayerAvatar_GetFacingDir(fs->playerAvatar),
            TerrainCollisionManager_GetTileBehavior(fs, px, pz));

    /* The same question the party menu asks before it offers Cut or Strength:
     * what object is the player facing. A station whose recipe is right has a
     * non-empty answer here and one whose coordinates are off by a tile does
     * not, which is the check worth having before a run is ever driven. */
    sub_0203C9D4(fs, &facing);
    if (facing != NULL) {
        fprintf(f, "facing-object gfx %u script %u at %d %d\n",
                (unsigned)MapObject_GetGraphicsID(facing),
                (unsigned)MapObject_GetScript(facing),
                MapObject_GetX(facing), MapObject_GetZ(facing));
    } else {
        fprintf(f, "facing-object none\n");
    }

    {
        MapObject *obj = NULL;
        while (MapObjectMan_FindObjectWithStatus(fs->mapObjMan, &obj, &idx,
                                                 MAP_OBJ_STATUS_0)) {
            fprintf(f, "object gfx %u script %u at %d %d\n",
                    (unsigned)MapObject_GetGraphicsID(obj),
                    (unsigned)MapObject_GetScript(obj),
                    MapObject_GetX(obj), MapObject_GetZ(obj));
        }
    }

    /*
     * Two views of the same window, and both earn their place. The `tile`
     * lines are the game's own classification, the predicates the field
     * engine itself branches on, and are what a person reads. The `row`
     * lines are every behavior byte, which is what a script needs: "a land
     * tile with water in front of it" is an adjacency question, and it cannot
     * be answered from a list that only contains the water.
     */
    for (z = pz - r; z <= pz + r; z++) {
        for (x = px - r; x <= px + r; x++) {
            u8 b = TerrainCollisionManager_GetTileBehavior(fs, x, z);
            const char *label = lab_tile_label(b);
            if (label != NULL) {
                fprintf(f, "tile %d %d %02x %s\n", x, z, b, label);
            }
        }
    }
    for (z = pz - r; z <= pz + r; z++) {
        fprintf(f, "row %d %d ", z, px - r);
        for (x = px - r; x <= px + r; x++) {
            fprintf(f, "%02x", TerrainCollisionManager_GetTileBehavior(fs, x, z));
        }
        fprintf(f, "\n");
    }

    /*
     * And passability, which is NOT the behavior byte and cost a station to
     * learn: the tiles either side of a Strength boulder both read behavior 00
     * and one of them is solid, so the boulder had nowhere to go and the push
     * silently did not happen. Behaviors say what a tile IS; this says whether
     * anything can stand on it.
     */
    for (z = pz - r; z <= pz + r; z++) {
        fprintf(f, "blocked %d %d ", z, px - r);
        for (x = px - r; x <= px + r; x++) {
            fprintf(f, "%d", TerrainCollisionManager_CheckCollision(fs, x, z) ? 1 : 0);
        }
        fprintf(f, "\n");
    }

    fclose(f);
    fprintf(stderr, "pc_lab: map scan written to %s\n", path);
}

/*
 * Called from main.c's patch, at the point the boot decides which application
 * to enter. Returns 1 to mean "the lab owns this run": the caller enters the
 * new-save application instead of the opening cutscene.
 */
int pc_lab_boot(void *saveDataVoid)
{
    SaveData *saveData = saveDataVoid;
    const char *path = getenv("PC_LAB");
    const char *at = getenv("PC_LAB_AT");
    const char *layout = getenv("PC_SAVE_LAYOUT");
    const char *sweep = getenv("PC_SWEEP");
    const char *sweepout = getenv("PC_SWEEP_OUT");
    extern void pc_lab_start_new_save(SaveData *saveData); /* pc/patches/src/game_start.c.patch */

    if (layout != NULL && layout[0] != '\0') {
        lab_dump_layout(saveData, layout);
        fprintf(stderr, "pc_lab: save layout written to %s\n", layout);
        exit(0);
    }

    if (path == NULL || path[0] == '\0') return 0;

    if (sweep != NULL && sweep[0] != '\0') {
        FILE *f = fopen(sweep, "r");
        char line[128];

        if (f == NULL) {
            fprintf(stderr, "pc_lab: cannot open %s\n", sweep);
            exit(2);
        }
        while (fgets(line, sizeof line, f) != NULL && sSweepCount < SWEEP_MAX) {
            int map, warp;
            if (line[0] == '#') continue;
            if (sscanf(line, "%d %d", &map, &warp) != 2) continue;
            sSweep[sSweepCount].map = map;
            sSweep[sSweepCount].warp = warp;
            sSweepCount++;
        }
        fclose(f);

        sSweepOut = sweepout != NULL && sweepout[0] != '\0'
                  ? fopen(sweepout, "w") : stdout;
        if (sSweepOut == NULL) {
            fprintf(stderr, "pc_lab: cannot write %s\n", sweepout);
            exit(2);
        }
        fprintf(stderr, "pc_lab: sweeping %d map(s) in one boot\n", sSweepCount);
    }

    snprintf(sScriptPath, sizeof sScriptPath, "%s", path);
    lab_parse(path);

    if (at != NULL && at[0] != '\0') {
        sApplyFrame = strtoul(at, NULL, 0);
    }

    sActive = 1;

    /*
     * Two bases, and which one is in force is decided by what is on the chip
     * rather than by another input. A recipe run against an empty save is a
     * new game; run against a save that already exists it is a DERIVED
     * station, the same recipe language applied on top of a played save,
     * which is how a station gets story state no amount of SetFlag would
     * reproduce faithfully.
     */
    if (SaveData_DataExists(saveData)) {
        fprintf(stderr, "pc_lab: %s, %d line(s) on the existing save,"
                " applying at frame %lu\n", path, sNumOps, sApplyFrame);
        return 2;
    }

    pc_lab_start_new_save(saveData);
    fprintf(stderr, "pc_lab: %s, %d line(s) on a new game, applying at frame %lu\n",
            path, sNumOps, sApplyFrame);
    return 1;
}

static void lab_give_mon(SaveData *saveData, int species, int level, int item)
{
    /* The game's own give-a-Pokemon path: it sets the OT to the player, marks
     * the dex, and overflows into the boxes when the party is full, all of
     * which a hand-built Pokemon_InitWith would have to redo by eye. */
    Pokemon_GiveMonFromScript(LAB_HEAP, saveData, (u16)species, (u8)level,
                              (u16)item, 0, TERRAIN_MAX);
}

static void lab_apply(SaveData *saveData, int pass)
{
    TrainerInfo *info = SaveData_GetTrainerInfo(saveData);
    VarsFlags *varsFlags = SaveData_GetVarsFlags(saveData);
    Bag *bag = SaveData_GetBag(saveData);
    Party *party = SaveData_GetParty(saveData);
    FieldOverworldState *fieldState = SaveData_GetFieldOverworldState(saveData);
    int i;

    for (i = 0; i < sNumOps; i++) {
        const struct lab_op *op = &sOps[i];

        if (op->verb == LAB_MAP && pass == 0) continue;
        if (op->verb != LAB_MAP && pass == 1) continue;

        switch (op->verb) {
        case LAB_WARP: {
            /* By warp index instead of by tile. The map change resolves the
             * index through the destination's own warp table, which is where
             * the game puts a player arriving through that door, so a
             * station can name any map something warps into without anyone
             * having to know a walkable coordinate on it. */
            Location loc;
            Location_Set(&loc, (enum MapHeaderID)op->a, op->b, 0, 0, FACE_DOWN);
            *FieldOverworldState_GetPlayerLocation(fieldState) = loc;
            *FieldOverworldState_GetEntranceLocation(fieldState) = loc;
            *FieldOverworldState_GetPrevLocation(fieldState) = loc;
            sWarpTo = loc;
            sWarpPending = 1;
            break;
        }
        case LAB_MAP: {
            /* The destination is written into the save and then loaded, and
             * the loading is the part that matters. Writing a Location and
             * saving looked right and was not: the saved map objects still
             * belonged to the map the player was standing on, and the
             * CONTINUE path does not survive that mismatch. So the lab writes
             * the location and then performs the same map change a script's
             * warp performs, which loads the destination for real, after
             * which its objects are the ones the save carries. */
            Location loc;
            Location_Set(&loc, (enum MapHeaderID)op->a, WARP_ID_NONE,
                         op->b, op->c, op->d);
            *FieldOverworldState_GetPlayerLocation(fieldState) = loc;
            *FieldOverworldState_GetEntranceLocation(fieldState) = loc;
            *FieldOverworldState_GetPrevLocation(fieldState) = loc;
            sWarpTo = loc;
            sWarpPending = 1;
            break;
        }
        case LAB_MONEY:
            TrainerInfo_SetMoney(info, (u32)op->a);
            break;
        case LAB_BADGE:
            TrainerInfo_SetBadge(info, op->a);
            break;
        case LAB_NAME: {
            charcode_t name[8];
            int n;
            for (n = 0; n < 7 && op->text[n] != '\0'; n++) {
                name[n] = lab_charcode(op->text[n]);
            }
            name[n] = CHAR_EOS;
            TrainerInfo_SetName(info, name);
            break;
        }
        case LAB_GENDER:
            TrainerInfo_SetGender(info, op->a);
            break;
        case LAB_TRAINER_ID:
            TrainerInfo_SetID(info, (u32)op->a);
            break;
        case LAB_PARTY:
            lab_give_mon(saveData, op->a, op->b, op->c);
            break;
        case LAB_PARTY_MOVE: {
            Pokemon *mon = Party_GetPokemonBySlotIndex(party, op->a);
            if (mon != NULL) Pokemon_SetMoveSlot(mon, (u16)op->c, (u8)op->b);
            break;
        }
        case LAB_ITEM:
            if (!Bag_TryAddItem(bag, (u16)op->a, (u16)op->b, LAB_HEAP)) {
                fprintf(stderr, "pc_lab: bag refused item %d x%d\n", op->a, op->b);
                exit(2);
            }
            break;
        case LAB_REGISTER_ITEM:
            Bag_RegisterItem(bag, (u32)op->a);
            break;
        case LAB_FLAG:
            VarsFlags_SetFlag(varsFlags, (u16)op->a);
            break;
        case LAB_CLEAR_FLAG:
            VarsFlags_ClearFlag(varsFlags, (u16)op->a);
            break;
        case LAB_VAR: {
            u16 *var = VarsFlags_GetVarAddress(varsFlags, (u16)op->a);
            if (var != NULL) *var = (u16)op->b;
            break;
        }
        case LAB_NATIONAL_DEX:
            if (op->a) {
                Pokedex_ObtainNationalDex(SaveData_GetPokedex(saveData));
                TrainerInfo_GiveNationalDex(info);
            }
            break;
        case LAB_POKEDEX:
            if (op->a) Pokedex_ObtainPokedex(SaveData_GetPokedex(saveData));
            break;
        case LAB_STORY_CLEARED:
            if (op->a) TrainerInfo_SetMainStoryCleared(info);
            break;
        case LAB_BERRY: {
            /*
             * Through the game's own planting path, with the growth table read
             * out of the berry NARC, so the patch gets the stage length the
             * berry actually has rather than one written down here. A new save
             * Already has berries and they do not move: InitializeNewSave runs
             * BerryPatches_Init, which leaves every patch at the fruit stage
             * with isGrowing FALSE, and BerryPatches_ElapseMinutes skips those.
             * Planting is what makes a patch tick.
             */
            BerryGrowthData *growth = BerryGrowthData_Init(LAB_HEAP);
            BerryPatch *patches = MiscSaveBlock_GetBerryPatches(saveData);
            BerryPatches_PlantInPatch(patches, op->a, growth,
                                      op->b - FIRST_BERRY_IDX + 1);
            Heap_Free(growth);
            break;
        }
        case LAB_HONEY: {
            /*
             * The minute counter is written straight, because the game's own
             * HoneyTree_SlatherTree needs a field system standing on one of the
             * 21 honey-tree maps and this runs before the recipe's map change.
             * It is also the whole precondition: the timer path reads nothing
             * but this field, and 24 hours' worth of minutes is what a slather
             * leaves behind.
             */
            PlayerHoneyTreeStates *trees = SpecialEncounter_GetPlayerHoneyTreeStates(
                SaveData_GetSpecialEncounters(saveData));
            SpecialEncounter_GetHoneyTree((u8)op->a, trees)->minutesRemaining = op->b;
            SpecialEncounter_SetLastSlatheredTreeId((u8)op->a, trees);
            break;
        }
        case LAB_SHAYMIN_FORM: {
            /* Named for the one species it works on rather than pretending to
             * be a general form setter: BoxPokemon_SetShayminForm returns
             * silently for anything else, and a recipe line that quietly did
             * nothing is what the verb table's hard errors exist to prevent. */
            Pokemon *mon = Party_GetPokemonBySlotIndex(party, op->a);
            if (mon == NULL
                || Pokemon_GetValue(mon, MON_DATA_SPECIES, NULL) != SPECIES_SHAYMIN) {
                fprintf(stderr, "pc_lab: shaymin-form: party slot %d is not a"
                        " Shaymin\n", op->a);
                exit(2);
            }
            Pokemon_SetShayminForm(mon, op->b);
            break;
        }
        case LAB_DAYCARE:
            /* The day-care lady's own deposit, which also moves the mon out of
             * the party and bumps the deposit record. */
            if (Party_GetCurrentCount(party) <= op->a) {
                fprintf(stderr, "pc_lab: daycare: party slot %d is empty\n", op->a);
                exit(2);
            }
            Daycare_MoveToEmptySlotFromParty(party, op->a,
                                             SaveData_GetDaycare(saveData), saveData);
            break;
        case LAB_DAYCARE_STEPS:
            DaycareMon_SetSteps(
                Daycare_GetDaycareMon(SaveData_GetDaycare(saveData), op->a), op->b);
            break;
        case LAB_DAYCARE_COUNTER:
            Daycare_SetStepCounter(SaveData_GetDaycare(saveData), op->a);
            break;
        case LAB_DAYCARE_EGG:
            /* What the 256th step does when the parents are compatible: an
             * offspring personality is the whole of "an egg is waiting", and
             * Daycare_HasEgg is that field being non-zero. */
            if (op->a == 0) {
                fprintf(stderr, "pc_lab: daycare-egg: 0 is the no-egg value\n");
                exit(2);
            }
            Daycare_SetOffspringPersonality(SaveData_GetDaycare(saveData), op->a);
            break;
        case LAB_TAKE_EGG: {
            Daycare *daycare = SaveData_GetDaycare(saveData);
            if (!Daycare_HasEgg(daycare)) {
                fprintf(stderr, "pc_lab: take-egg: no egg is waiting, a recipe"
                        " needs two parents and a daycare-egg line first\n");
                exit(2);
            }
            /* The species, the inherited IVs, the moveset and the reset of the
             * daycare's counters are all the game's; this is the same call the
             * day-care man's script makes when the egg is accepted. */
            Daycare_GiveEggFromDaycare(daycare, party, info);
            break;
        }
        case LAB_EGG_CYCLES: {
            /* An egg's remaining cycles live in the friendship byte, which is
             * where Egg_CreateEgg puts the species' hatch counter and where
             * Daycare_Update decrements. Zero is hatch-on-the-next-step. */
            Pokemon *mon = Party_GetPokemonBySlotIndex(party, op->a);
            u8 cycles = (u8)op->b;
            if (mon == NULL || !Pokemon_GetValue(mon, MON_DATA_IS_EGG, NULL)) {
                fprintf(stderr, "pc_lab: egg-cycles: party slot %d is not an egg\n",
                        op->a);
                exit(2);
            }
            Pokemon_SetValue(mon, MON_DATA_FRIENDSHIP, &cycles);
            break;
        }
        case LAB_CONDITION: {
            Pokemon *mon = Party_GetPokemonBySlotIndex(party, op->a);
            u8 value = (u8)op->c;
            if (mon == NULL) {
                fprintf(stderr, "pc_lab: condition: no party slot %d\n", op->a);
                exit(2);
            }
            Pokemon_SetValue(mon, op->b, &value);
            break;
        }
        case LAB_POFFIN: {
            /* One flavor, which is what a single-berry cook produces: the
             * flavor's amount and the smoothness are the two numbers the pot
             * hands Poffin_MakePoffin, and the type it returns is derived. */
            u8 flavors[FLAVOR_MAX] = { 0 };
            Poffin poffin;

            if (op->a < 0 || op->a >= FLAVOR_MAX) {
                fprintf(stderr, "pc_lab: poffin: no flavor %d\n", op->a);
                exit(2);
            }
            flavors[op->a] = (u8)op->b;
            Poffin_Clear(&poffin);
            Poffin_MakePoffin(&poffin, flavors, (u8)op->c, FALSE);
            if (PoffinCase_AddPoffin(SaveData_GetPoffinCase(saveData), &poffin)
                    == POFFIN_NONE) {
                fprintf(stderr, "pc_lab: poffin: the case is full\n");
                exit(2);
            }
            break;
        }
        case LAB_FEED_POFFIN: {
            Pokemon *mon = Party_GetPokemonBySlotIndex(party, op->a);
            PoffinCase *poffinCase = SaveData_GetPoffinCase(saveData);
            Poffin poffin;

            PoffinCase_CopyPoffinFromSlot(poffinCase, (u16)op->b, &poffin);
            if (mon == NULL || !Poffin_HasValidFlavor(&poffin)) {
                fprintf(stderr, "pc_lab: feed-poffin: party slot %d or poffin"
                        " slot %d is empty\n", op->a, op->b);
                exit(2);
            }
            PoffinCase_UpdateMonContestStats(&poffin, mon);
            /* Eaten, the way the case's own screen leaves it. */
            PoffinCase_ClearSlot(poffinCase, (u16)op->b);
            PoffinCase_Compact(poffinCase);
            break;
        }
        case LAB_SPHERE: {
            Underground *ug = SaveData_GetUnderground(saveData);

            if (!Underground_TryAddSphere(ug, (enum SphereType)op->a, op->b)) {
                fprintf(stderr, "pc_lab: sphere: the bag is full\n");
                exit(2);
            }
            break;
        }
        case LAB_TRAP: {
            Underground *ug = SaveData_GetUnderground(saveData);

            if (!Underground_TryAddTrap(ug, (enum Trap)op->a)) {
                fprintf(stderr, "pc_lab: trap: the bag is full\n");
                exit(2);
            }
            break;
        }
        case LAB_GOOD: {
            Underground *ug = SaveData_GetUnderground(saveData);

            if (!Underground_TryAddGoodBag(ug, (enum Good)op->a)) {
                fprintf(stderr, "pc_lab: good: the bag is full\n");
                exit(2);
            }
            break;
        }
        case LAB_GOOD_PC: {
            Underground *ug = SaveData_GetUnderground(saveData);

            if (!Underground_TryAddGoodPC(ug, (enum Good)op->a)) {
                fprintf(stderr, "pc_lab: good-pc: the PC is full\n");
                exit(2);
            }
            break;
        }
        case LAB_SECRET_BASE: {
            SecretBase *base = SaveData_GetSecretBase(saveData);

            SecretBase_SetEntrance(base, op->a, op->b, op->c);
            break;
        }
        case LAB_POKETCH: {
            Poketch *poketch = SaveData_GetPoketch(saveData);

            if (op->a < 0 || op->a >= POKETCH_APPID_MAX) {
                fprintf(stderr, "pc_lab: poketch: no app %d\n", op->a);
                exit(2);
            }
            /* Enable is deferred until after any map change. The field that
             * is already up loaded the unavailable overlay; flipping the
             * flag now makes teardown call PoketchSystem_StartShutdown on a
             * NULL system. Register and select are save bytes only. */
            Poketch_RegisterApp(poketch, (enum PoketchAppID)op->a);
            poketch->appIndex = (s8)op->a;
            sPoketchEnable = 1;
            break;
        }
        case LAB_POKETCH_STEPS: {
            /* SetStepCount is a no-op until the pedometer is registered;
             * that flag is what the field's step callback also gates on. */
            Poketch *poketch = SaveData_GetPoketch(saveData);

            Poketch_RegisterApp(poketch, POKETCH_APPID_PEDOMETER);
            Poketch_SetStepCount(poketch, (u32)op->a);
            break;
        }
        case LAB_POKETCH_HISTORY: {
            Pokemon *mon = Party_GetPokemonBySlotIndex(party, op->a);

            if (mon == NULL) {
                fprintf(stderr, "pc_lab: poketch-history: no party slot %d\n",
                        op->a);
                exit(2);
            }
            Poketch_PokemonHistoryEnqueue(SaveData_GetPoketch(saveData),
                                          (const BoxPokemon *)mon);
            break;
        }
        }
    }
}

/*
 * One step of the sweep, per frame. Returns 1 while the sweep owns the run.
 *
 * A map that never settles is recorded as a refusal and ends the run rather
 * than being skipped: the field is left mid-change and nothing here can put it
 * back, so the honest move is to write the verdict and let the harness start
 * the next boot after it.
 */
static int sweep_frame(FieldSystem *fs, unsigned long long frame)
{
    uint64_t digest = 0;
    unsigned colours = 0;
    extern void pc_video_surface_stats(uint64_t *digest, unsigned *colours);
    Location loc;

    if (sSweepCount == 0) return 0;

    /* The sweep owns the run and never reaches the recipe's save, so pass-0
     * verbs (party, flags, vars) would otherwise never run. The Hall of Fame
     * display walks the party and Sprite_SetPositions monSprites[0] even
     * when monCount is 0, a new-game sweep with no party is a NULL deref
     * that has nothing to do with the map. */
    if (!sSweepRecipeApplied) {
        lab_apply(SaveData_Ptr(), 0);
        sSweepRecipeApplied = 1;
    }

    if (sSweepAt >= 0) {
        int arrived = fs->location->mapHeaderID == sSweep[sSweepAt].map
                   && fs->task == NULL;

        /* Stop driving A the moment the map is up. The script presses it so
         * arrival scenes finish; pressing it on a settled overworld starts
         * conversations and battles, and a battle that crashes is a finding
         * about battles rather than about the map underneath it. */
        pc_input_hold_idle(arrived);

        if (arrived && sWarpSettled == 0) sWarpSettled = (int)frame;
        if (!arrived || (int)frame < sWarpSettled + LAB_SETTLE_FRAMES) {
            if (frame > sSweepStarted + SWEEP_TIMEOUT) {
                fprintf(sSweepOut, "%d %d refused - - map=%d task=%s\n",
                        sSweep[sSweepAt].map, sSweep[sSweepAt].warp,
                        (int)fs->location->mapHeaderID,
                        fs->task ? "busy" : "idle");
                fflush(sSweepOut);
                fprintf(stderr, "pc_lab: map %d never settled; the sweep ends "
                        "here and the next boot resumes after it\n",
                        sSweep[sSweepAt].map);
                exit(0);
            }
            return 1;
        }

        pc_video_surface_stats(&digest, &colours);
        fprintf(sSweepOut, "%d %d %s %016llX %u map=%d at=%d,%d\n",
                sSweep[sSweepAt].map, sSweep[sSweepAt].warp,
                colours <= 2 ? "black" : "clean",
                (unsigned long long)digest, colours,
                (int)fs->location->mapHeaderID,
                fs->location->x, fs->location->z);
        fflush(sSweepOut);
    }

    sSweepAt++;
    if (sSweepAt >= sSweepCount) {
        fprintf(stderr, "pc_lab: sweep done, %d map(s)\n", sSweepCount);
        fflush(NULL);
        exit(0);
    }

    pc_input_hold_idle(0);
    Location_Set(&loc, (enum MapHeaderID)sSweep[sSweepAt].map,
                 sSweep[sSweepAt].warp, 0, 0, FACE_DOWN);
    *FieldOverworldState_GetPlayerLocation(
        SaveData_GetFieldOverworldState(SaveData_Ptr())) = loc;
    sWarpTo = loc;
    sWarpSettled = 0;
    sSweepStarted = (unsigned long)frame;
    pc_lab_start_map_change(fs, &loc);
    return 1;
}

/*
 * Every frame, from pc_video_frame_end. Does nothing until the frame the
 * recipe asked for; the wait exists because the field system's new-game
 * template needs to have run INIT_NEW_GAME and loaded the spawn map before
 * anything here writes to the save it owns.
 */
void pc_lab_frame(unsigned long long frame)
{
    SaveData *saveData;
    FieldSystem *fs;
    int result;
    extern FieldSystem *pc_lab_field_system(void); /* pc/patches/src/field_system.c.patch */

    if (!sActive || sApplied) return;
    if (frame < sApplyFrame) return;

    fs = pc_lab_field_system();
    if (fs == NULL) {
        if (frame > sApplyFrame + 6000) {
            fprintf(stderr, "pc_lab: no field system by frame %llu\n", frame);
            exit(2);
        }
        return;
    }

    /* A sweep owns the run from here: it walks its own list of maps and never
     * reaches the recipe's save. */
    if (sweep_frame(fs, frame)) return;

    /*
     * A recipe that names a map loads it first and finishes afterwards. The
     * wait is on the field system's own task pointer going idle, which is
     * what every other caller in the tree waits on, rather than on a frame
     * count that would be a guess about how long a map load takes.
     */
    if (sWarpStarted) {
        /* Done when the field's own location says the destination is loaded
         * and no task is still running on top of it. The settle that follows
         * is for the map-name popup: it runs as a system task with a window
         * of its own, and touching the field while it is still on screen
         * crashed on two of the four cities tried (Oreburgh, Veilstone) and
         * not on the other two, which is what a race looks like. */
        int arrived = fs->location->mapHeaderID == sWarpTo.mapHeaderID
                   && fs->task == NULL;
        if (arrived && sWarpSettled == 0) sWarpSettled = (int)frame;
        if (!arrived || (int)frame < sWarpSettled + LAB_SETTLE_FRAMES) {
            if (frame > sApplyFrame + 6000) {
                fprintf(stderr, "pc_lab: the map change never finished"
                        " (map %d, task %s)\n",
                        (int)fs->location->mapHeaderID, fs->task ? "busy" : "idle");
                exit(2);
            }
            return;
        }
    } else {
        saveData = SaveData_Ptr();
        lab_apply(saveData, 0);
        lab_apply(saveData, 1);
        if (sWarpPending) {
            sWarpStarted = 1;
            pc_lab_start_map_change(fs, &sWarpTo);
            return;
        }
    }

    sApplied = 1;
    saveData = SaveData_Ptr();

    /*
     * What the game does before every save it makes from the field, and the
     * reason an early version of this lab produced saves the CONTINUE path
     * refused: FieldSystem_Save syncs the live map objects into the save
     * (FieldOverworldSave's 64 slots) and copies the player avatar's position
     * into the saved location. A save with no saved map objects has no player
     * object to load, and the continue path does not survive it, measured by
     * splicing that one page from a played save into a lab save, which fixed
     * it and no other page did.
     *
     * By the time this runs the recipe has been applied and any fly warp it
     * asked for has landed, so the avatar is standing where the station wants
     * to be and the objects are the destination's own.
     */
    {
        const char *scan = getenv("PC_LAB_MAPSCAN");
        if (scan != NULL && scan[0] != '\0') lab_scan_map(fs, scan);
    }

    if (sPoketchEnable) {
        Poketch_Enable(SaveData_GetPoketch(saveData));
    }

    FieldSystem_SaveObjects(fs);
    FieldSystem_SendPoketchEvent(fs, POKETCH_EVENT_SAVE, 0);
    fs->location->x = PlayerAvatar_GetXPos(fs->playerAvatar);
    fs->location->z = PlayerAvatar_GetZPos(fs->playerAvatar);
    fs->location->warpId = WARP_ID_NONE;
    fs->location->faceDirection = PlayerAvatar_GetFacingDir(fs->playerAvatar);

    result = SaveData_Save(saveData);

    fprintf(stderr, "pc_lab: applied %d line(s) at frame %llu, save %s\n",
            sNumOps, frame, result == SAVE_RESULT_OK ? "ok" : "FAILED");
    fflush(NULL);
    exit(result == SAVE_RESULT_OK ? 0 : 2);
}

/*
 * "The field is standing still and nothing is on top of it", the question
 * both saves below have to answer before they touch the map objects.
 *
 * FieldSystem_SaveObjects recomputes every map object's height through the
 * terrain collision manager, so a save taken while a map change or an
 * application is in flight reaches that with no terrain loaded and faults. An
 * idle task pointer alone is not enough: a whiteout hands control to an
 * application and the field's task is NULL the whole time it runs.
 *
 * The child process is the one to ask about, not `IsRunningApplication`, and
 * getting that wrong is why this is a function now. That predicate is
 * `parent != NULL || child != NULL`, and on the overworld the field map IS the
 * parent, so it is true whenever the field is up, and the battle lab's
 * post-battle save, which required it to be FALSE, could never fire. It never
 * did: every battle station has been asserting the save its recipe minted
 * rather than the one its fight produced, silently, because the fields those
 * specs assert (name, map, money, badge count, party size) are the ones a won
 * battle does not change. The condition wanted was "no application ON TOP of
 * the field map", which is exactly HasChildProcess.
 */
static int lab_field_settled(FieldSystem *fs)
{
    return fs != NULL
        && fs->task == NULL
        && FieldSystem_IsRunningFieldMap(fs)
        && !FieldSystem_HasChildProcess(fs);
}

/* ------------------------------------------------------- the end-of-run save
 *
 * PC_LAB_SAVE_AT=N: write the save again at frame N, through the game's own
 * FieldSystem_Save.
 *
 * Why a station needs one. The corpus asserts a station's save BEFORE it boots,
 * because for a station that only stands somewhere the save is all there is.
 * A field move is the opposite: the whole point of the station is what the run
 * DID, the tree cut and walked through, the water surfed onto, the boulder
 * pushed, and none of that is in the save the recipe minted. The battle lab
 * hit this first and solved it for battles only (PC_LAB_BATTLE_SAVE); this is
 * the same answer for a run that has no battle in it.
 *
 * The settle is the battle lab's, and it is there for the battle lab's reason:
 * FieldSystem_SaveObjects recomputes every map object's height through the
 * terrain collision manager, so saving while a map change is in flight reaches
 * that with no terrain loaded. All three questions have to say yes.
 */
static unsigned long sSaveAtFrame;
static int sSaveAtParsed;
static int sSaveAtSettling;
static int sSaveAtDone;

void pc_lab_save_frame(unsigned long long frame)
{
    FieldSystem *fs;
    extern FieldSystem *pc_lab_field_system(void);

    if (!sSaveAtParsed) {
        const char *at = getenv("PC_LAB_SAVE_AT");
        sSaveAtParsed = 1;
        sSaveAtFrame = (at != NULL && at[0] != '\0') ? strtoul(at, NULL, 0) : 0;
    }
    if (sSaveAtFrame == 0 || sSaveAtDone) return;
    if (frame < sSaveAtFrame) return;

    fs = pc_lab_field_system();
    if (!lab_field_settled(fs)) {
        sSaveAtSettling = (int)frame;   /* still moving; restart the settle */
        if (frame > sSaveAtFrame + 8000) {
            /* Said out loud rather than waited on forever, and the station
             * fails rather than asserting the save its recipe minted, which
             * would pass while proving nothing. */
            fprintf(stderr, "pc_lab: the field never settled for the end-of-run "
                    "save (8000 frames after %lu)\n", sSaveAtFrame);
            fflush(NULL);
            sSaveAtDone = 1;
        }
        return;
    }
    if ((int)frame < sSaveAtSettling + LAB_SETTLE_FRAMES) return;

    sSaveAtDone = 1;
    fprintf(stderr, "pc_lab: end-of-run save at frame %llu, %s, at (%d,%d) map %d\n",
            frame, FieldSystem_Save(fs) ? "ok" : "FAILED",
            PlayerAvatar_GetXPos(fs->playerAvatar),
            PlayerAvatar_GetZPos(fs->playerAvatar),
            (int)fs->location->mapHeaderID);
    fflush(NULL);
}

/* ------------------------------------------------------------ the battle lab
 *
 * One field task, created from outside any other, that calls the game's own
 * encounter entry point and then waits for it. Every one of those entries
 * takes a FieldTask to call from, startEncounter is FieldTask_InitCall, a
 * push onto the caller, and a station has none, so this supplies one. It is
 * the same shape pc_lab_start_map_change needed and for the same reason.
 *
 * Nothing here is a reimplementation of a battle: the parties, the trainer
 * data, the seed and the bag all come from FieldBattleDTO_Init reading the
 * save the station minted, exactly as a script's `trainerbattle` would.
 */

static enum lab_battle_kind sBattleKind;
static int sBattleArg[3];
static unsigned long sBattleFrame = 900;
static int sBattleResult = -1;
static int sBattleStarted;
static int sBattleDone;
static int sBattleEnded;
static const char *sBattleSavePath;
static enum lab_battle_kind sBattle2Kind;
static int sBattle2Arg[3];
static unsigned long sBattle2Frame;
static int sBattle2Armed;

static const struct { const char *name; enum lab_battle_kind kind; int args; }
LAB_BATTLES[] = {
    { "wild",      LAB_BATTLE_WILD,      2 },
    { "legendary", LAB_BATTLE_LEGENDARY, 2 },
    { "trainer",   LAB_BATTLE_TRAINER,   3 },
    { "safari",    LAB_BATTLE_SAFARI,    3 },
    { "first",     LAB_BATTLE_FIRST,     1 },
    { "tutorial",  LAB_BATTLE_TUTORIAL,  0 },
};

/*
 * The battle's own task. State 0 starts the encounter and then gets out of
 * the way.
 *
 * It does not detect the end, and that is not tidiness. The encounter entries
 * do not agree about what they do to the caller: the trainer and scripted-wild
 * ones push (FieldTask_InitCall) so this function is entered again afterwards,
 * while the safari path jumps (FieldTask_InitJump), which REPLACES this
 * function with the encounter's and means it is never entered again. A
 * completion test written against the pushing half looks correct and silently
 * never fires for the other; which is what the first version of this did,
 * leaving a safari station sitting on an overworld it had never left. So the
 * end of a battle is observed from outside, as the field going idle, which is
 * true of both.
 */
static BOOL lab_battle_task(FieldTask *task)
{
    int *state = FieldTask_GetState(task);

    switch (*state) {
    case 0:
        switch (sBattleKind) {
        case LAB_BATTLE_WILD:
            Encounter_NewVsSpeciesAtLevel(task, (u16)sBattleArg[0],
                                          (u8)sBattleArg[1], &sBattleResult, FALSE);
            break;
        case LAB_BATTLE_LEGENDARY:
            Encounter_NewVsSpeciesAtLevel(task, (u16)sBattleArg[0],
                                          (u8)sBattleArg[1], &sBattleResult, TRUE);
            break;
        case LAB_BATTLE_TRAINER:
            Encounter_NewVsTrainer(task, sBattleArg[0], sBattleArg[1],
                                   sBattleArg[2], HEAP_ID_FIELD2, &sBattleResult);
            break;
        case LAB_BATTLE_FIRST:
            Encounter_NewVsFirstBattle(task, sBattleArg[0], HEAP_ID_FIELD2,
                                       &sBattleResult);
            break;
        case LAB_BATTLE_SAFARI: {
            /* The safari path is chosen by the SAVE, not by the caller:
             * Encounter_NewVsWild branches on SystemFlag_CheckSafariGameActive,
             * so this sets the flag the way entering the zone does and then
             * takes the ordinary wild entry. Building a safari DTO while the
             * flag was clear would give a battle whose menu and whose escape
             * rules disagreed with its type. */
            FieldSystem *fs = FieldTask_GetFieldSystem(task);
            FieldBattleDTO *dto;

            SystemFlag_SetSafariGameActive(SaveData_GetVarsFlags(fs->saveData));
            dto = FieldBattleDTO_NewSafari(HEAP_ID_FIELD2, sBattleArg[2]);
            FieldBattleDTO_Init(dto, fs);
            CreateWildMon_Scripted(fs, (u16)sBattleArg[0], (u8)sBattleArg[1], dto);
            Encounter_StartVsWild(fs, task, dto);
            break;
        }
        case LAB_BATTLE_TUTORIAL:
            /* The tutorial reports nothing: it is a scripted demonstration
             * with no outcome the field cares about, so resultMask stays at
             * the -1 this file set and the station asserts on the save. */
            Encounter_NewCatchingTutorial(task);
            break;
        default:
            return TRUE;
        }
        (*state)++;
        return FALSE;

    default:
        return TRUE;
    }
}

/*
 * Parse one "KIND ARG..." into kind/args. Returns 1 on success. The
 * same grammar as a station's `battle:` line, used for both fights.
 */
static int lab_battle_parse_one(const char *s, const char *what,
                                enum lab_battle_kind *kind, int *args)
{
    char buf[128], *tok, *save = NULL;
    unsigned i;
    int n;

    if (s == NULL || s[0] == '\0') return 0;

    snprintf(buf, sizeof buf, "%s", s);
    tok = strtok_r(buf, " \t", &save);
    if (tok == NULL) return 0;

    for (i = 0; i < sizeof LAB_BATTLES / sizeof LAB_BATTLES[0]; i++) {
        if (strcmp(tok, LAB_BATTLES[i].name) != 0) continue;

        for (n = 0; n < LAB_BATTLES[i].args; n++) {
            tok = strtok_r(NULL, " \t", &save);
            if (tok == NULL) {
                fprintf(stderr, "pc_lab: %s '%s' wants %d argument(s)\n",
                        what, LAB_BATTLES[i].name, LAB_BATTLES[i].args);
                exit(2);
            }
            args[n] = (int)strtol(tok, NULL, 0);
        }
        *kind = LAB_BATTLES[i].kind;
        return 1;
    }

    /* Loud, like an unknown recipe verb: a station whose battle silently
     * did not happen would pin a digest of the overworld and pass. */
    fprintf(stderr, "pc_lab: %s: unknown kind '%s'\n", what, s);
    exit(2);
}

/*
 * PC_LAB_BATTLE="KIND ARG...", parsed once, at the first frame that asks.
 * PC_LAB_BATTLE2 is the same grammar, fired after the first fight ends and
 * the field is back, a second load of the battle and field overlays.
 */
static void lab_battle_parse(void)
{
    const char *at = getenv("PC_LAB_BATTLE_AT");
    const char *at2 = getenv("PC_LAB_BATTLE2_AT");

    if (lab_battle_parse_one(getenv("PC_LAB_BATTLE"), "PC_LAB_BATTLE",
                             &sBattleKind, sBattleArg)) {
        if (at != NULL && at[0] != '\0') sBattleFrame = strtoul(at, NULL, 0);
    }
    if (lab_battle_parse_one(getenv("PC_LAB_BATTLE2"), "PC_LAB_BATTLE2",
                             &sBattle2Kind, sBattle2Arg)) {
        if (at2 != NULL && at2[0] != '\0') {
            sBattle2Frame = strtoul(at2, NULL, 0);
        }
        sBattle2Armed = 1;
    }
    sBattleSavePath = getenv("PC_LAB_BATTLE_SAVE");
}

/*
 * Every frame, alongside pc_lab_frame. Independent of PC_LAB: a battle station
 * boots from a save the recipe already made, so the recipe machinery above is
 * not running at all.
 */
void pc_lab_battle_frame(unsigned long long frame)
{
    static int parsed;
    FieldSystem *fs;
    extern FieldSystem *pc_lab_field_system(void);

    if (!parsed) {
        parsed = 1;
        lab_battle_parse();
    }
    if (sBattleKind == LAB_BATTLE_NONE) return;

    if (sBattleDone) {
        fs = pc_lab_field_system();
        if (!lab_field_settled(fs)) {
            /* An idle task pointer is not enough; see lab_field_settled, which
             * also carries the correction to what this used to ask. */
            sBattleDone = (int)frame;   /* still moving; restart the settle */
            if (frame > (unsigned long long)sBattleEnded + 8000) {
                /* Said out loud rather than waited on forever. A LOST battle
                 * whites the player out, and that hands control to an
                 * application this has no way to wait through; the station
                 * still pins its picture and its result, and the save it
                 * asserts on is the one the recipe minted. */
                fprintf(stderr, "pc_lab: the field never settled after the "
                        "battle (8000 frames)%s\n",
                        sBattle2Armed ? ", so the second fight never started"
                                      : ", so no post-battle save was written");
                fflush(NULL);
                sBattle2Armed = 0;
                sBattleSavePath = NULL;
            }
            return;
        }

        /*
         * A second fight starts once the field is back, not after the long
         * save-settle. That settle is for FieldSystem_SaveObjects; starting
         * an encounter only needs the map running and idle. The overlay
         * reload has already happened: StartFieldMapInner loads overlay 5
         * as the parent process comes back.
         */
        if (sBattle2Armed) {
            if (sBattle2Frame != 0 && frame < sBattle2Frame) return;
            sBattleKind = sBattle2Kind;
            sBattleArg[0] = sBattle2Arg[0];
            sBattleArg[1] = sBattle2Arg[1];
            sBattleArg[2] = sBattle2Arg[2];
            sBattleFrame = (unsigned long)frame;
            sBattleStarted = 0;
            sBattleDone = 0;
            sBattleEnded = 0;
            sBattleResult = -1;
            sBattle2Armed = 0;
            fprintf(stderr, "pc_lab: arming second battle kind %d (%d,%d,%d) "
                    "at frame %llu\n",
                    (int)sBattleKind, sBattleArg[0], sBattleArg[1],
                    sBattleArg[2], frame);
            fflush(NULL);
            return;
        }

        /*
         * Written once, after the fight, so the save reader can be asked what
         * the battle did, party HP and experience, money, the dex. The run
         * does NOT end here: the station's pinned frame is PC_FRAMES like every
         * other, and ending early would make the digest depend on how long the
         * battle happened to take.
         *
         * And it waits for the field to settle first, which cost a crash to
         * learn. FieldSystem_SaveObjects recomputes every map object's height
         * through the terrain collision manager, and a LOST battle whites the
         * player out; which starts a map change. Saving on the frame the
         * battle ended reached that recomputation with the destination's
         * terrain not yet loaded and segfaulted inside the height manager. The
         * nine stations that WIN never leave their map, which is exactly how a
         * bug like this hides: it is in the losing path only.
         */
        if (sBattleSavePath == NULL) return;
        if ((int)frame < sBattleDone + LAB_SETTLE_FRAMES) return;

        FieldSystem_SaveObjects(fs);
        FieldSystem_SendPoketchEvent(fs, POKETCH_EVENT_SAVE, 0);
        fprintf(stderr, "pc_lab: post-battle save at frame %llu, %s\n", frame,
                SaveData_Save(SaveData_Ptr()) == SAVE_RESULT_OK ? "ok" : "FAILED");
        fflush(NULL);
        sBattleSavePath = NULL;
        return;
    }

    if (sBattleStarted) {
        /* The field is idle again: the encounter task, however it was
         * attached, has run to completion. */
        fs = pc_lab_field_system();
        if (fs != NULL && fs->task == NULL && frame > sBattleStarted + 60) {
            fprintf(stderr, "pc_lab: battle over at frame %llu, resultMask=%d\n",
                    frame, sBattleResult);
            fflush(NULL);
            sBattleDone = sBattleEnded = (int)frame;
        }
        return;
    }
    if (frame < sBattleFrame) return;

    fs = pc_lab_field_system();
    if (fs == NULL || fs->task != NULL) {
        /* The field has to be up and idle: starting an encounter on top of a
         * running task is what the sweep's A-mashing did, and it is the
         * difference between a battle station and a crash report. */
        if (frame > sBattleFrame + 6000) {
            fprintf(stderr, "pc_lab: no idle field system by frame %llu, so the "
                    "battle never started\n", frame);
            exit(2);
        }
        return;
    }

    sBattleStarted = (int)frame;
    fprintf(stderr, "pc_lab: starting battle kind %d (%d,%d,%d) at frame %llu\n",
            (int)sBattleKind, sBattleArg[0], sBattleArg[1], sBattleArg[2], frame);
    FieldSystem_CreateTask(fs, lab_battle_task, NULL);
}

/* ------------------------------------------------------------ the growth lab
 *
 * PC_LAB_USE_ITEM="<party slot> <item>": use a bag item on a party member at
 * PC_LAB_USE_AT, then evolve whatever that made evolvable, a rare candy into
 * a level-up evolution, a stone into an item one, and neither for a species
 * that only evolves by trade.
 *
 * What it skips and what it does not. Everything here is the game's: the bag
 * removal is Bag_TryRemoveItem, the effect is Party_ApplyItemEffectsToMember
 * (the party menu's own item call), the question "does this evolve now" is
 * Pokemon_GetEvolutionTargetSpecies with the same class and the same map
 * evolution method the menu passes, and the animation, the species change, the
 * dex entry and the stat recalculation are Evolution_Begin's. What is skipped
 * is the NAVIGATION, opening the bag, picking the item, picking the mon;
 * which is a menu-driving problem and belongs with the rest of the menus.
 *
 * The move-learn prompt is on the other side of that line and is worth naming:
 * It is not part of evolution at all. Levelling up into a new move is handled
 * by the party menu's own callback (PartyMenuCB_LevelUp, LEVELUP_STATE_*)
 * BEFORE it hands over to the evolution app, so a hook that starts at the
 * evolution app cannot reach it. The post-battle path is the same shape.
 */
static int sUseSlot = -1;
static int sUseItem;
static unsigned long sUseFrame = 2600;
static int sUseStarted;
static int sUseParsed;
static EvolutionData *sEvoData;
static int sEvoClass;
static int sEvoMethod;
static int sEvoTarget;

/*
 * The task, and it is a state machine because every step of it is a call that
 * suspends: FieldTransition_* each push a sub-task and return, and the
 * evolution app runs for hundreds of frames.
 *
 * The fade and the map teardown are not decoration. The evolution app wants
 * 192 KB of HEAP_ID_APPLICATION and the loaded field map is holding it, so
 * calling Evolution_Begin with the overworld still up fails the allocation;
 * which in this game is not a NULL, it is ErrorMessageReset_PrintErrorAndReset
 * putting "a communication error has occurred" on screen and waiting for A.
 * That is what the first version of this did. Every game path that runs a
 * full-screen application from the field does fade out, finish the map, run,
 * start the map, fade in, in that order; this is the one in
 * FieldTask_ProcessNPCTrade, which is the closest relative.
 */
static BOOL lab_grow_task(FieldTask *task)
{
    FieldSystem *fs = FieldTask_GetFieldSystem(task);
    int *state = FieldTask_GetState(task);
    SaveData *saveData = fs->saveData;
    Party *party = SaveData_GetParty(saveData);
    Pokemon *mon;

    switch (*state) {
    case 0: {
        int before, after;

        mon = Party_GetPokemonBySlotIndex(party, sUseSlot);
        if (mon == NULL) {
            fprintf(stderr, "pc_lab: use-item: party slot %d is empty\n", sUseSlot);
            exit(2);
        }
        if (!Bag_TryRemoveItem(SaveData_GetBag(saveData), (u16)sUseItem, 1, LAB_HEAP)) {
            /* Loud: the item is the station's input and a run that used one it
             * did not have would be measuring a different game. */
            fprintf(stderr, "pc_lab: use-item: the bag holds no item %d\n", sUseItem);
            exit(2);
        }

        before = Pokemon_GetValue(mon, MON_DATA_LEVEL, NULL);
        Party_ApplyItemEffectsToMember(party, (u16)sUseItem, (u8)sUseSlot, 0,
                                       MapHeader_GetMapLabelTextID(
                                           fs->location->mapHeaderID),
                                       LAB_HEAP);
        after = Pokemon_GetValue(mon, MON_DATA_LEVEL, NULL);

        /*
         * Which question to ask is decided by what the item DID, which is how
         * the party menu decides it too: a level-up item runs the level-up
         * callback and asks EVO_CLASS_BY_LEVEL, and anything else asks
         * EVO_CLASS_BY_ITEM with the item's own id.
         */
        if (after != before) {
            sEvoClass = EVO_CLASS_BY_LEVEL;
            sEvoMethod = MapHeader_GetMapEvolutionMethod(fs->location->mapHeaderID);
        } else {
            sEvoClass = EVO_CLASS_BY_ITEM;
            sEvoMethod = sUseItem;
        }
        sEvoTarget = Pokemon_GetEvolutionTargetSpecies(party, mon, sEvoClass,
                                                       sEvoMethod, &sEvoMethod);
        fprintf(stderr, "pc_lab: item %d on slot %d: level %d -> %d, species %d,"
                " evolution class %d gives species %d\n",
                sUseItem, sUseSlot, before, after,
                (int)Pokemon_GetValue(mon, MON_DATA_SPECIES, NULL),
                sEvoClass, sEvoTarget);

        if (sEvoTarget == SPECIES_NONE) {
            /*
             * The honest refusal, and the reason the trade answer is printed
             * beside it: a station whose mon simply did not evolve looks
             * identical to one whose evolution is broken. Naming the species
             * this WOULD become if it were traded says which of the two this
             * is, the evolution exists, the game will not reach it without a
             * link, and that is the pin.
             */
            fprintf(stderr, "pc_lab: no evolution: species %d stays, and by trade"
                    " it would be species %d\n",
                    (int)Pokemon_GetValue(mon, MON_DATA_SPECIES, NULL),
                    (int)Pokemon_GetEvolutionTargetSpecies(party, mon,
                                                           EVO_CLASS_BY_TRADE,
                                                           ITEM_NONE, NULL));
            fflush(NULL);
            return TRUE;
        }
        fflush(NULL);
        (*state)++;
        return FALSE;
    }
    case 1:
        FieldTransition_FadeOut(task);
        (*state)++;
        return FALSE;
    case 2:
        FieldTransition_FinishMap(task);
        (*state)++;
        return FALSE;
    case 3:
        Sound_StopWaveOutAndSequences();
        Heap_Create(HEAP_ID_APPLICATION, HEAP_ID_EVOLUTION, HEAP_SIZE_EVOLUTION);
        mon = Party_GetPokemonBySlotIndex(party, sUseSlot);
        sEvoData = Evolution_Begin(party, mon, sEvoTarget,
                                   SaveData_GetOptions(saveData),
                                   PokemonSummaryScreen_ShowContestData(saveData),
                                   SaveData_GetPokedex(saveData),
                                   SaveData_GetBag(saveData),
                                   SaveData_GetGameRecords(saveData),
                                   SaveData_GetPoketch(saveData),
                                   sEvoMethod,
                                   sEvoClass == EVO_CLASS_BY_LEVEL ? 0x1 : 0,
                                   HEAP_ID_EVOLUTION);
        (*state)++;
        return FALSE;
    case 4:
        if (!Evolution_IsDone(sEvoData)) return FALSE;

        /* Printed before the map comes back, and separately from the line at
         * the end, because the two answers differing is what would separate an
         * app that did nothing from a field restart that undid it. It is how
         * the B-cancel above was localised. */
        fprintf(stderr, "pc_lab: evolution app done: slot %d is species %d\n",
                sUseSlot,
                (int)Pokemon_GetValue(Party_GetPokemonBySlotIndex(party, sUseSlot),
                                      MON_DATA_SPECIES, NULL));
        Evolution_Free(sEvoData);
        Heap_Destroy(HEAP_ID_EVOLUTION);
        Sound_StopBGM(SEQ_SHINKA_sseq, 0);
        Sound_SetScene(SOUND_SCENE_NONE);
        (*state)++;
        return FALSE;
    case 5:
        FieldTransition_StartMap(task);
        (*state)++;
        return FALSE;
    case 6:
        FieldBGM_PlayEffectiveForMapHeader(fs, fs->location->mapHeaderID);
        FieldTransition_FadeIn(task);
        (*state)++;
        return FALSE;
    default:
        mon = Party_GetPokemonBySlotIndex(party, sUseSlot);
        fprintf(stderr, "pc_lab: evolution over: slot %d is species %d level %d\n",
                sUseSlot, (int)Pokemon_GetValue(mon, MON_DATA_SPECIES, NULL),
                (int)Pokemon_GetValue(mon, MON_DATA_LEVEL, NULL));
        fflush(NULL);
        return TRUE;
    }
}

void pc_lab_grow_frame(unsigned long long frame)
{
    FieldSystem *fs;
    extern FieldSystem *pc_lab_field_system(void);

    if (!sUseParsed) {
        const char *s = getenv("PC_LAB_USE_ITEM");
        const char *at = getenv("PC_LAB_USE_AT");

        sUseParsed = 1;
        if (s != NULL && s[0] != '\0') {
            if (sscanf(s, "%d %d", &sUseSlot, &sUseItem) != 2) {
                fprintf(stderr, "pc_lab: PC_LAB_USE_ITEM wants '<party slot>"
                        " <item>', got '%s'\n", s);
                exit(2);
            }
        }
        if (at != NULL && at[0] != '\0') sUseFrame = strtoul(at, NULL, 0);
    }
    if (sUseSlot < 0 || sUseStarted) return;
    if (frame < sUseFrame) return;

    fs = pc_lab_field_system();
    if (!lab_field_settled(fs)) {
        if (frame > sUseFrame + 6000) {
            fprintf(stderr, "pc_lab: no idle field system by frame %llu, so the"
                    " item was never used\n", frame);
            exit(2);
        }
        return;
    }

    sUseStarted = 1;
    FieldSystem_CreateTask(fs, lab_grow_task, NULL);
}

/* ------------------------------------------------------------ the contest lab
 *
 * PC_LAB_CONTEST="<rank> <type> <party slot>": enter one official Super
 * Contest, at PC_LAB_CONTEST_AT, with the party slot named.
 *
 * Everything the contest is belongs to the game: Contest_Init builds the
 * field, seeds the RNG and picks the three NPC rivals,
 * FieldTask_InitRunContestTask runs the same five applications the script
 * runs, and Contest_EndContest awards the ribbon. What is skipped is the
 * lobby: the receptionist's conversation, the three text menus and the party
 * menu. Folding those in would make every contest station a twenty-thousand
 * frame walk through the same four screens before the thing under test began.
 *
 * Determinism was measured rather than assumed. Contest_Init reseeds the
 * global LCRNG from a clock read multiplied by the running seed. Both halves
 * are already inputs here: the clock is PC_RTC, and the running seed is a pure
 * function of the boot and the frame the contest starts on, which
 * PC_LAB_CONTEST_AT fixes. So a PC_CONTEST_SEED input would be a second source
 * of truth for something the existing inputs already pin.
 */
static int sContestRank = -1;
static int sContestType;
static int sContestSlot;
static unsigned long sContestFrame = 2600;
static int sContestStarted;
static unsigned long long sContestNow;
static Contest *sContest;

static BOOL lab_contest_task(FieldTask *task)
{
    FieldSystem *fs = FieldTask_GetFieldSystem(task);
    int *state = FieldTask_GetState(task);

    switch (*state) {
    case 0: {
        TrainerInfo *info = SaveData_GetTrainerInfo(fs->saveData);
        String *trainerName = TrainerInfo_NameNewString(info, HEAP_ID_FIELD1);
        PlayerMonContestDTO dto;

        memset(&dto, 0, sizeof dto);
        dto.contestType = (u8)sContestType;
        dto.contestRank = (u8)sContestRank;
        /* The official competition: all three rounds, in order. The practice
         * ones run a single round and award nothing, Contest_EndContest
         * returns on them before it reaches the ribbon. */
        dto.competitionType = CONTEST_COMPETITION_LINK_OR_OFFICIAL;
        dto.isGameCompleted = SystemFlag_CheckGameCompleted(SaveData_GetVarsFlags(fs->saveData));
        dto.isNatDexObtained = Pokedex_IsNationalDexObtained(SaveData_GetPokedex(fs->saveData));
        dto.monPartySlot = (u8)sContestSlot;
        dto.mon = Party_GetPokemonBySlotIndex(SaveData_GetParty(fs->saveData),
                                              sContestSlot);
        dto.trainerName = trainerName;
        dto.trainerInfo = info;
        dto.imageClips = SaveData_GetImageClips(fs->saveData);
        dto.options = SaveData_GetOptions(fs->saveData);
        dto.saveData = fs->saveData;
        dto.chatotCry = SaveData_GetChatotCry(fs->saveData);

        if (dto.mon == NULL) {
            fprintf(stderr, "pc_lab: contest: no party slot %d\n", sContestSlot);
            exit(2);
        }

        sContest = Contest_Init(&dto);
        String_Free(trainerName);
        /* A CALL, not a jump: the contest task is pushed in front of this one
         * and this one resumes at state 1 when it pops, which is the same
         * shape the script has around RunContestApplication. */
        FieldTask_InitRunContestTask(task, sContest);
        (*state)++;
        return FALSE;
    }
    default: {
        int placement = Contest_GetPlayerContestPlacement(sContest);

        Contest_EndContest(sContest, fs->saveData,
                           MapHeader_GetMapLabelTextID(fs->location->mapHeaderID),
                           fs->journalEntry);
        Contest_Free(sContest);
        sContest = NULL;
        /* First place is placement 0, the way the game counts it. */
        fprintf(stderr, "pc_lab: contest over at frame %llu, placement=%d\n",
                sContestNow, placement);
        fflush(NULL);
        return TRUE;
    }
    }
}

void pc_lab_contest_frame(unsigned long long frame)
{
    static int parsed;
    FieldSystem *fs;
    extern FieldSystem *pc_lab_field_system(void);

    if (!parsed) {
        const char *s = getenv("PC_LAB_CONTEST");
        const char *at = getenv("PC_LAB_CONTEST_AT");

        parsed = 1;
        if (s != NULL && s[0] != '\0') {
            if (sscanf(s, "%d %d %d", &sContestRank, &sContestType,
                       &sContestSlot) != 3) {
                fprintf(stderr, "pc_lab: PC_LAB_CONTEST wants '<rank> <type>"
                        " <party slot>', got '%s'\n", s);
                exit(2);
            }
        }
        if (at != NULL && at[0] != '\0') sContestFrame = strtoul(at, NULL, 0);
    }
    sContestNow = frame;
    if (sContestRank < 0 || sContestStarted) return;
    if (frame < sContestFrame) return;

    fs = pc_lab_field_system();
    if (!lab_field_settled(fs)) {
        if (frame > sContestFrame + 6000) {
            fprintf(stderr, "pc_lab: no idle field system by frame %llu, so the"
                    " contest never started\n", frame);
            exit(2);
        }
        return;
    }

    sContestStarted = 1;
    fprintf(stderr, "pc_lab: starting contest rank %d type %d slot %d at frame"
            " %llu\n", sContestRank, sContestType, sContestSlot, frame);
    fflush(NULL);
    FieldSystem_CreateTask(fs, lab_contest_task, NULL);
}

/* ------------------------------------------------------------ the underground
 *
 * PC_LAB_UNDERGROUND=enter|mine at PC_LAB_UNDERGROUND_AT.
 *
 * Arrival is the Explorer Kit's own field-use path, the same shape as a rod.
 * The comms confirm and the forced save are the game's. What this hook does
 * after arrival is two things the first-visit path otherwise sits on:
 *
 *   enter  set FLAG_HAS_SEEN_UNDERGROUND_ROARK_INTRO so WaitForRoarkScene
 *          proceeds. The flag is set after the underground init has raised
 *          doNotConnectUnderground, so the visit stays solo.
 *   mine   that, then Mining_StartGameForLab, the same task the confirm
 *          prompt would start.
 *
 * Determinism: MiningEnv_Init calls CommSys_Seed, off the same two inputs the
 * contest lab pins, PC_RTC and the frame the environment is created. The board
 * is MATH_Rand32 off that seed, so there is no PC_MINE_SEED.
 */
extern void Mining_StartGameForLab(void);

static int sUgKind; /* 0 none, 1 enter, 2 mine */
static unsigned long sUgFrame = 5600;
static int sUgStarted;
static unsigned long long sUgNow;

static void lab_ug_skip_roark(FieldSystem *fs)
{
    VarsFlags *vf = SaveData_GetVarsFlags(fs->saveData);

    if (!SystemFlag_CheckHasSeenUndergroundRoarkIntro(vf)) {
        VarsFlags_SetFlag(vf, FLAG_HAS_SEEN_UNDERGROUND_ROARK_INTRO);
        fprintf(stderr, "pc_lab: underground: skipped the Roark intro at frame"
                " %llu\n", sUgNow);
    }
}

void pc_lab_underground_frame(unsigned long long frame)
{
    static int parsed;
    FieldSystem *fs;
    extern FieldSystem *pc_lab_field_system(void);

    if (!parsed) {
        const char *s = getenv("PC_LAB_UNDERGROUND");
        const char *at = getenv("PC_LAB_UNDERGROUND_AT");

        parsed = 1;
        if (s != NULL && s[0] != '\0') {
            if (strcmp(s, "enter") == 0) {
                sUgKind = 1;
            } else if (strcmp(s, "mine") == 0) {
                sUgKind = 2;
            } else {
                fprintf(stderr, "pc_lab: PC_LAB_UNDERGROUND wants 'enter' or"
                        " 'mine', got '%s'\n", s);
                exit(2);
            }
        }
        if (at != NULL && at[0] != '\0') sUgFrame = strtoul(at, NULL, 0);
    }
    sUgNow = frame;
    if (sUgKind == 0 || sUgStarted) return;
    if (frame < sUgFrame) return;

    fs = pc_lab_field_system();
    /* The hole animation is on screen at frame ~4900 and a communication
     * error (leftover title WM, no PXI tag 10) covers it ~100 frames
     * later. The field task is still the map change, so lab_field_settled
     * is false for the whole window. Mining_StartGameForLab is a SysTask,
     * not a field task, and only needs the map and MiningEnv. */
    if (fs == NULL || fs->location == NULL
            || fs->location->mapHeaderID != MAP_HEADER_UNDERGROUND) {
        if (frame > sUgFrame + 6000) {
            fprintf(stderr, "pc_lab: not in the underground by frame %llu,"
                    " so the hook never ran\n", frame);
            exit(2);
        }
        return;
    }

    sUgStarted = 1;
    lab_ug_skip_roark(fs);
    if (sUgKind == 2) {
        /* MiningEnv is created by EnterUnderground, which is the last
         * step of the hole animation. This hook can fire earlier, while
         * the tunnels are already on screen. If the env is not up yet
         * the start is a no-op and we say so rather than hang. */
        fprintf(stderr, "pc_lab: starting the digging minigame at frame"
                " %llu\n", frame);
        fflush(NULL);
        Mining_StartGameForLab();
        if (!Mining_IsMiningGameTaskActive()) {
            fprintf(stderr, "pc_lab: Mining_StartGameForLab did not start"
                    " a game (MiningEnv is not up)\n");
            exit(2);
        }
        fprintf(stderr, "pc_lab: mining started at frame %llu\n", frame);
        fflush(NULL);
    } else {
        fprintf(stderr, "pc_lab: underground enter at frame %llu map=%d"
                " pos=%d,%d\n", frame, fs->location->mapHeaderID,
                fs->location->x, fs->location->z);
        fflush(NULL);
    }
}
