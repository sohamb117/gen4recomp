/*
 * Diamond/Pearl's save lab: Platinum's pc/src/pc_lab.c recipe language
 * (tests/gameplay/labc.py compiles the names) applied to a played save, so
 * a gameplay scenario can start anywhere without an evening of input.
 *
 *   PC_LAB=FILE | inline:VERB ARGS;...   the recipe (numbers only)
 *   PC_LAB_AT=N                          the first frame it may apply
 *
 * Platinum's lab can also mint a save from nothing, by booting straight into
 * the new-save application. D's is the derived station only: it needs a save
 * on the chip and the run's input to CONTINUE it (tests/gameplay/mint.sh
 * does both, from the new-game save tests/dp/first_save.sh makes), because
 * D's new-game flow is still assembly with nothing to hook it by. A played
 * base also carries the story state INIT_NEW_GAME and the intro leave, which
 * is the trap Platinum's comment describes.
 *
 * Once the player is free in the field at or after PC_LAB_AT, every line is
 * applied through the game's own setters (GiveMon, Bag_AddItem,
 * PlayerProfile_*, Save_VarsFlags_*, Save_Poketch_*); a `warp` or `map` line
 * then runs the field's own fade-and-load map change (sub_02049274, the one
 * ScrCmd's teleport uses) and the lab waits for the destination to be up and
 * settled. Then Field_SaveGame, the start menu's save (map objects synced,
 * the avatar's position copied into the location), and exit.
 *
 * Verbs: name TEXT, gender G, trainer-id N, money N, badge B, var V N,
 * flag F, clear-flag F, party SPECIES LEVEL ITEM, party-move SLOT MOVESLOT
 * MOVE, item ITEM QTY, register-item ITEM, poketch APP, warp MAP WARP,
 * map MAP X Z DIR. Anything else stops the run: a recipe line that silently
 * did nothing is the failure the verb table exists to prevent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"

#include "bag.h"
#include "constants/charcode.h"
#include "constants/heap.h"
#include "field_system.h"
#include "map_header.h"
#include "party.h"
#include "player_data.h"
#include "poketch.h"
#include "save.h"
#include "save_vars_flags.h"
#include "script_pokemon_util.h"

extern FieldSystem *UNK_021C5A08;                     /* arm9/asm/unk_020372D4.s */
extern u16 Field_SaveGame(FieldSystem *fieldSystem);  /* overlay 5 */
extern void sub_02049274(FieldSystem *fieldSystem, u16 mapId, s32 warpId, u16 x, u16 y, u32 direction);
extern int pc_dp_field_ready(FieldSystem *fs);        /* pc_dp_field.c */
extern int pc_dp_on_guest_stack(void (*fn)(void));   /* pc_dp_field.c */

enum {
    LAB_NAME, LAB_GENDER, LAB_TRAINER_ID, LAB_MONEY, LAB_BADGE, LAB_VAR, LAB_FLAG,
    LAB_CLEAR_FLAG, LAB_PARTY, LAB_PARTY_MOVE, LAB_ITEM, LAB_REGISTER_ITEM,
    LAB_POKETCH, LAB_WARP, LAB_MAP,
};

static const struct {
    const char *name;
    int verb, nargs;
} sVerbs[] = {
    { "name", LAB_NAME, 0 },          { "gender", LAB_GENDER, 1 },
    { "trainer-id", LAB_TRAINER_ID, 1 }, { "money", LAB_MONEY, 1 },
    { "badge", LAB_BADGE, 1 },        { "var", LAB_VAR, 2 },
    { "flag", LAB_FLAG, 1 },          { "clear-flag", LAB_CLEAR_FLAG, 1 },
    { "party", LAB_PARTY, 3 },        { "party-move", LAB_PARTY_MOVE, 3 },
    { "item", LAB_ITEM, 2 },          { "register-item", LAB_REGISTER_ITEM, 1 },
    { "poketch", LAB_POKETCH, 1 },    { "warp", LAB_WARP, 2 },
    { "map", LAB_MAP, 4 },
};

#define LAB_MAX_OPS 64
#define LAB_TIMEOUT 6000      /* frames past PC_LAB_AT before giving up */
#define LAB_SETTLE 60         /* frames the destination must stay free first */

static struct lab_op {
    int verb;
    long a[4];
    char text[8];
} sOps[LAB_MAX_OPS];
static int sNumOps, sActive, sApplied, sWarpPending, sWarpStarted, sPoketch;
static unsigned long long sApplyAt, sSettledAt;
static Location sWarpTo;

static void lab_fail(const char *why, const char *line) {
    fprintf(stderr, "pc_lab: %s: %s\n", why, line);
    fflush(NULL);
    exit(2);
}

static void lab_parse_line(char *line) {
    char *argv[6];
    int argc = 0, i;
    char *tok = strtok(line, " \t\r\n");

    if (tok == NULL || tok[0] == '#') return;
    while (tok != NULL && argc < 6) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }
    for (i = 0; i < (int)(sizeof sVerbs / sizeof sVerbs[0]); i++) {
        if (strcmp(argv[0], sVerbs[i].name) == 0) break;
    }
    if (i == (int)(sizeof sVerbs / sizeof sVerbs[0])) lab_fail("unknown verb", argv[0]);
    if (sNumOps == LAB_MAX_OPS) lab_fail("too many lines", argv[0]);
    if (sVerbs[i].verb == LAB_NAME) {
        if (argc != 2) lab_fail("name takes one word", argv[0]);
        snprintf(sOps[sNumOps].text, sizeof sOps[sNumOps].text, "%s", argv[1]);
    } else {
        int k;
        if (argc - 1 != sVerbs[i].nargs) lab_fail("wrong argument count", argv[0]);
        for (k = 0; k < sVerbs[i].nargs; k++) {
            char *end;
            sOps[sNumOps].a[k] = strtol(argv[k + 1], &end, 0);
            if (*end != '\0') lab_fail("not a number (compile names with tests/gameplay/labc.py)", argv[k + 1]);
        }
    }
    sOps[sNumOps].verb = sVerbs[i].verb;
    sNumOps++;
}

static void lab_parse(const char *spec) {
    char line[256];

    if (strncmp(spec, "inline:", 7) == 0) {
        const char *p = spec + 7;
        while (*p != '\0') {
            size_t n = strcspn(p, ";\n");
            if (n >= sizeof line) n = sizeof line - 1;
            memcpy(line, p, n);
            line[n] = '\0';
            lab_parse_line(line);
            p += n;
            if (*p != '\0') p++;
        }
    } else {
        FILE *f = fopen(spec, "r");
        if (f == NULL) lab_fail("cannot open", spec);
        while (fgets(line, sizeof line, f) != NULL) lab_parse_line(line);
        fclose(f);
    }
}

/* The Latin block of D's charcode table is contiguous, like Platinum's. */
static u16 lab_charcode(char c) {
    if (c >= '0' && c <= '9') return (u16)(CHAR_0 + (c - '0'));
    if (c >= 'A' && c <= 'Z') return (u16)(CHAR_A + (c - 'A'));
    if (c >= 'a' && c <= 'z') return (u16)(CHAR_a + (c - 'a'));
    return CHAR_SPACE;
}

static void lab_apply(FieldSystem *fs) {
    SaveData *save = fs->saveData;
    PlayerProfile *profile = Save_PlayerData_GetProfile(save);
    SaveVarsFlags *vf = Save_VarsFlags_Get(save);
    Bag *bag = Save_Bag_Get(save);
    struct Party *party = SaveArray_Party_Get(save);
    struct Poketch *poketch = Save_Poketch_Get(save);
    int i;

    for (i = 0; i < sNumOps; i++) {
        const struct lab_op *op = &sOps[i];

        switch (op->verb) {
        case LAB_NAME: {
            u16 *name = PlayerProfile_GetNamePtr(profile);
            int n;
            for (n = 0; n < 7 && op->text[n] != '\0'; n++) name[n] = lab_charcode(op->text[n]);
            name[n] = EOS;
            break;
        }
        case LAB_GENDER:
            PlayerProfile_SetTrainerGender(profile, (u32)op->a[0]);
            break;
        case LAB_TRAINER_ID:
            PlayerProfile_SetTrainerID(profile, (u32)op->a[0]);
            break;
        case LAB_MONEY:
            PlayerProfile_SetMoney(profile, (u32)op->a[0]);
            break;
        case LAB_BADGE:
            PlayerProfile_SetBadgeFlag(profile, (u32)op->a[0]);
            break;
        case LAB_VAR: {
            u16 *var = Save_VarsFlags_GetVarAddr(vf, (u16)op->a[0]);
            if (var == NULL) lab_fail("no such var", "var");
            *var = (u16)op->a[1];
            break;
        }
        case LAB_FLAG:
            Save_VarsFlags_SetFlagInArray(vf, (u16)op->a[0]);
            break;
        case LAB_CLEAR_FLAG:
            Save_VarsFlags_ClearFlagInArray(vf, (u16)op->a[0]);
            break;
        case LAB_PARTY:
            /* The game's own give-a-Pokemon path (ScrCmd_GiveMon): OT, dex,
             * met data, and the boxes when the party is full. */
            if (!GiveMon(HEAP_ID_FIELD, save, (u16)op->a[0], (u8)op->a[1], (u16)op->a[2],
                         MapHeader_GetMapSec(fs->location->mapId), 12)) {
                lab_fail("GiveMon refused", "party");
            }
            break;
        case LAB_PARTY_MOVE:
            if (op->a[0] >= Party_GetCount(party)) lab_fail("no such party slot", "party-move");
            PartyMonSetMoveInSlot(party, (int)op->a[0], (int)op->a[1], (u16)op->a[2]);
            break;
        case LAB_ITEM:
            if (!Bag_AddItem(bag, (u16)op->a[0], (u16)op->a[1], HEAP_ID_FIELD)) lab_fail("the bag refused", "item");
            break;
        case LAB_REGISTER_ITEM:
            Bag_SetRegisteredItem(bag, (u32)op->a[0]);
            break;
        case LAB_POKETCH:
            if (op->a[0] < 0 || op->a[0] >= NUM_POKETCH_APPS) lab_fail("no such app", "poketch");
            /* Given (enabled) after any map change, as Platinum's lab does:
             * the field already up loaded no Poketch, and flipping the flag
             * under it changes what its teardown expects. */
            Save_Poketch_UnlockApp(poketch, (PoketchApp)op->a[0]);
            sPoketch = 1;
            break;
        case LAB_WARP:
            sWarpTo.mapId = (u32)op->a[0];
            sWarpTo.warpId = (u32)op->a[1];
            sWarpTo.x = 0;
            sWarpTo.y = 0;
            sWarpTo.direction = 1; /* down */
            sWarpPending = 1;
            break;
        case LAB_MAP:
            sWarpTo.mapId = (u32)op->a[0];
            sWarpTo.warpId = (u32)-1;
            sWarpTo.x = (u32)op->a[1];
            sWarpTo.y = (u32)op->a[2];
            sWarpTo.direction = (u32)op->a[3];
            sWarpPending = 1;
            break;
        }
    }
}

static unsigned long long sFrame;

static void lab_step(void) {
    FieldSystem *fs = UNK_021C5A08;
    const unsigned long long frame = sFrame;
    u16 ok;

    if (!sActive) {
        const char *spec = getenv("PC_LAB");
        const char *at = getenv("PC_LAB_AT");
        if (spec == NULL || spec[0] == '\0' || sApplied) return;
        lab_parse(spec);
        sApplyAt = at != NULL ? strtoull(at, NULL, 0) : 0;
        sActive = 1;
        fprintf(stderr, "pc_lab: %d line(s), applying from frame %llu on the save being continued\n", sNumOps,
                sApplyAt);
    }
    if (sApplied || frame < sApplyAt) return;
    if (frame > sApplyAt + LAB_TIMEOUT) {
        fprintf(stderr, "pc_lab: the field was never free (map %u) by frame %llu\n",
                fs != NULL && fs->location != NULL ? (unsigned)fs->location->mapId : 0u, frame);
        fflush(NULL);
        exit(2);
    }
    if (!pc_dp_field_ready(fs)) {
        sSettledAt = 0;
        return;
    }
    if (!sWarpStarted) {
        lab_apply(fs);
        if (sWarpPending) {
            sWarpStarted = 1;
            sub_02049274(fs, (u16)sWarpTo.mapId, (s32)sWarpTo.warpId, (u16)sWarpTo.x, (u16)sWarpTo.y,
                         sWarpTo.direction);
            return;
        }
    } else {
        /* The destination is loaded and the player has been free on it for
         * a while: the map-name popup and the fade are system tasks the
         * field-ready gate does not see. */
        if (fs->location->mapId != sWarpTo.mapId) return;
        if (sSettledAt == 0) sSettledAt = frame;
        if (frame < sSettledAt + LAB_SETTLE) return;
    }

    sApplied = 1;
    sActive = 0;
    if (sPoketch) Save_Poketch_Give(Save_Poketch_Get(fs->saveData));
    ok = Field_SaveGame(fs);
    fprintf(stderr, "pc_lab: applied %d line(s) at frame %llu (map %u), save %s\n", sNumOps, frame,
            (unsigned)fs->location->mapId, ok ? "ok" : "FAILED");
    fflush(NULL);
    exit(ok ? 0 : 2);
}

/* Every frame, from pc_dp_hooks.c's pc_lab_frame (pc_video.c's frame end,
 * inside OS_Halt on the idle thread): the recipe and the save run
 * recompiled code, so they get pc_dp_field.c's guest stack, as the quick
 * save does. A frame boundary nested inside the save is skipped. */
void pc_dp_lab_frame(unsigned long long frame) {
    if (getenv("PC_LAB") == NULL) return;
    sFrame = frame;
    pc_dp_on_guest_stack(lab_step);
}
