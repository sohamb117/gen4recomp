/*
 * The sprite lab: load every species through pokemon_sprite.c's real path and
 * digest what came out.
 *
 * A story play finds a texture or decrypt wall one evening at a time, when a
 * species finally appears. This walks the same load the battle, the Pokedex
 * and the summary screen use, and fingerprints the page. A few thousand
 * renders, minutes of compute, and a decode that would have trapped in a fight
 * traps here with a species number on the line.
 *
 * It does not start a battle and does not need a field: the sprite manager
 * needs only a heap and the filesystem, both of which are up before the first
 * VBlank.
 *
 * Genders. The archive stores six files per species and the template builder
 * always picks a male or female character index. Male-only, female-only and
 * genderless species have one sprite; everything else is loaded as both. Two
 * loads that hash the same are still two loads; the second file was reached.
 *
 * Forms. Pokemon_SanitizeFormId is the source of truth: the species list below
 * is the same switch that function has, and the count is whatever form it first
 * refuses. Restating the counts from constants/forms.h would be a second table
 * that drifted the first time a form was added to one side.
 *
 * Spinda. TryDrawSpindaSpots only fires on a front-face Spinda, and the spots
 * are a function of the personality nibbles. Two fixed PIDs are the ends of
 * that function, so their character digests must differ; if they do not, the
 * spots did not apply and the lab exits 2 rather than pin an identical pair.
 *
 * The digest is FNV-1a 64 over the manager's character buffer and slot 0's
 * 16-colour palette.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
static int lab_mkdir(const char *d) { return _mkdir(d); }
#else
static int lab_mkdir(const char *d) { return mkdir(d, 0755); }
#endif

#include "pch/global_pch.h"

#include "constants/forms.h"
#include "constants/graphics.h"
#include "constants/heap.h"
#include "constants/narc.h"
#include "constants/species.h"

#include "generated/gender_ratios.h"
#include "generated/genders.h"
#include "generated/species_data_params.h"

#include "heap.h"
#include "narc.h"
#include "pokemon.h"
#include "pokemon_sprite.h"

#include "pc_png.h"
#include "pc_state.h"

#define LAB_SPRITE_HEAP     HEAP_ID_POKEDEX
#define LAB_SPRITE_HEAP_SZ  0x20000
#define LAB_SPRITE_AT_DEF   2
#define LAB_ROW_STRIDE      0x80 /* MAN_Y_OFFSET in src/pokemon_sprite.c */
#define LAB_SPRITE_PX_W     (MON_SPRITE_FRAME_WIDTH * 2)
#define LAB_SPRITE_PX_H     MON_SPRITE_FRAME_HEIGHT

#define SPINDA_PID_A  0x00000000u
#define SPINDA_PID_B  0xFFFFFFFFu

#define LAB_MAX_LIST  32

enum lab_sprite_mode {
    LAB_SPRITE_NONE = 0,
    LAB_SPRITE_ALL,
    LAB_SPRITE_PIN,
    LAB_SPRITE_LIST
};

static enum lab_sprite_mode sMode;
static unsigned long sAtFrame = LAB_SPRITE_AT_DEF;
static int sDone;
static int sParsed;
static u16 sList[LAB_MAX_LIST];
static int sListN;
static FILE *sOut;
static const char *sDumpDir;
static int sLoaded;
static int sGenderSame;
static int sFailed;
static uint64_t sSpindaChar[2];
static int sSpindaGot[2];

static int lab_has_forms(u16 species)
{
    switch (species) {
    case SPECIES_BURMY:
    case SPECIES_WORMADAM:
    case SPECIES_SHELLOS:
    case SPECIES_GASTRODON:
    case SPECIES_CHERRIM:
    case SPECIES_ARCEUS:
    case SPECIES_CASTFORM:
    case SPECIES_DEOXYS:
    case SPECIES_UNOWN:
    case SPECIES_EGG:
    case SPECIES_SHAYMIN:
    case SPECIES_ROTOM:
    case SPECIES_GIRATINA:
        return 1;
    default:
        return 0;
    }
}

static int lab_form_count(u16 species)
{
    int f;

    if (!lab_has_forms(species)) return 1;
    for (f = 1; f < 32; f++) {
        if (Pokemon_SanitizeFormId(species, (u8)f) != (u8)f) return f;
    }
    return 32;
}

static void lab_genders(u16 species, int *out, int *n)
{
    u32 ratio;

    *n = 0;
    if (species == SPECIES_EGG || species == SPECIES_BAD_EGG
            || species > SPECIES_BAD_EGG) {
        /* Personal NARC stops at BAD_EGG. A new species has no
         * member there; do not read OOB to pick a gender. */
        out[(*n)++] = GENDER_MALE;
        return;
    }
    ratio = SpeciesData_GetSpeciesValue((int)species, SPECIES_DATA_GENDER_RATIO);
    if (ratio == GENDER_RATIO_FEMALE_ONLY) {
        out[(*n)++] = GENDER_FEMALE;
        return;
    }
    out[(*n)++] = GENDER_MALE;
    if (ratio != GENDER_RATIO_MALE_ONLY && ratio != GENDER_RATIO_NO_GENDER) {
        out[(*n)++] = GENDER_FEMALE;
    }
}

static void lab_say(const char *fmt, ...)
{
    va_list ap;
    va_list aq;

    va_start(ap, fmt);
    va_copy(aq, ap);
    vfprintf(stderr, fmt, ap);
    if (sOut != NULL) vfprintf(sOut, fmt, aq);
    va_end(aq);
    va_end(ap);
    fflush(stderr);
    if (sOut != NULL) fflush(sOut);
}

static int lab_dump_png(PokemonSpriteManager *man, const char *path)
{
    unsigned char *raw, *z;
    size_t rawn, zcap;
    unsigned y, x;
    int ok;
    const u8 *charData = man->charRawData;
    const u16 *pltt = man->plttRawData;

    rawn = (size_t)LAB_SPRITE_PX_H * (1 + 3 * LAB_SPRITE_PX_W);
    zcap = rawn + rawn / 2 + 256;
    raw = malloc(rawn);
    z = malloc(zcap);
    if (raw == NULL || z == NULL) {
        free(raw);
        free(z);
        fprintf(stderr, "pc_lab: sprite: out of host memory writing %s\n", path);
        return 0;
    }
    for (y = 0; y < LAB_SPRITE_PX_H; y++) {
        unsigned char *row = raw + y * (1 + 3 * LAB_SPRITE_PX_W);
        const u8 *src = charData + y * LAB_ROW_STRIDE;

        row[0] = 0;
        for (x = 0; x < LAB_SPRITE_PX_W; x++) {
            u8 packed = src[x / 2];
            u8 idx = (x & 1) ? (u8)(packed >> 4) : (u8)(packed & 0x0F);
            u16 c = pltt[idx];

            row[1 + 3 * x + 0] = (unsigned char)(((c >> 0) & 31) * 255 / 31);
            row[1 + 3 * x + 1] = (unsigned char)(((c >> 5) & 31) * 255 / 31);
            row[1 + 3 * x + 2] = (unsigned char)(((c >> 10) & 31) * 255 / 31);
        }
    }
    ok = pc_png_write_raw(path, LAB_SPRITE_PX_W, LAB_SPRITE_PX_H, raw, rawn,
                          z, zcap, "pc_lab: sprite");
    free(raw);
    free(z);
    return ok;
}

static int lab_one(PokemonSpriteManager *man, NARC *pokegra, NARC *otherpoke,
                   u16 species, u8 form, u8 face, u8 gender, u8 shiny, u32 pid)
{
    PokemonSpriteTemplate tmpl;
    PokemonSprite *sprite;
    uint64_t ch, pl;
    u8 yoff;
    u16 files;
    NARC *narc;
    char dump[512];

    memset(&tmpl, 0, sizeof tmpl);
    BuildPokemonSpriteTemplate(&tmpl, species, gender, face, shiny, form, pid);

    narc = (tmpl.narcID == NARC_INDEX_POKETOOL__POKEGRA__PL_OTHERPOKE)
         ? otherpoke : pokegra;
    files = NARC_GetFileCount(narc);
    if (tmpl.character >= files || tmpl.palette >= files) {
        lab_say("pc_lab: sprite FAIL species=%u form=%u face=%u gender=%u"
                " shiny=%u pid=0x%08x narc=%u char=%u pltt=%u files=%u\n",
                species, form, face, gender, shiny, pid,
                tmpl.narcID, tmpl.character, tmpl.palette, files);
        sFailed++;
        return 0;
    }

    sprite = PokemonSpriteManager_CreateSprite(man, &tmpl, 128, 96, 0, 0,
                                               NULL, NULL);
    if (sprite == NULL) {
        lab_say("pc_lab: sprite FAIL create species=%u form=%u\n",
                species, form);
        sFailed++;
        return 0;
    }

    PokemonSpriteManager_DrawSprites(man);
    PokemonSpriteManager_UpdateCharAndPltt(man);

    ch = pc_state_fnv1a(PC_STATE_FNV64_OFFSET, man->charRawData, man->charSize);
    pl = pc_state_fnv1a(PC_STATE_FNV64_OFFSET, man->plttRawData,
                        PALETTE_SIZE_BYTES);
    /* height.narc is species*4 and stops at Arceus. Icon and cry
     * are not this fixture; do not OOB the height table. */
    if (species > SPECIES_BAD_EGG) {
        yoff = 0;
    } else {
        yoff = LoadPokemonSpriteYOffset(species, gender, face, form, pid);
    }

    lab_say("pc_lab: sprite species=%u form=%u face=%u gender=%u shiny=%u"
            " pid=0x%08x narc=%u char=%u pltt=%u yoff=%u spots=%u"
            " char_fnv=%016llx pltt_fnv=%016llx\n",
            species, form, face, gender, shiny, pid,
            tmpl.narcID, tmpl.character, tmpl.palette, yoff,
            tmpl.spindaSpots,
            (unsigned long long)ch, (unsigned long long)pl);

    if (species == SPECIES_SPINDA && face == FACE_FRONT && shiny == 0
            && form == 0 && gender == GENDER_MALE) {
        if (pid == SPINDA_PID_A) {
            sSpindaChar[0] = ch;
            sSpindaGot[0] = 1;
        } else if (pid == SPINDA_PID_B) {
            sSpindaChar[1] = ch;
            sSpindaGot[1] = 1;
        }
    }

    if (sDumpDir != NULL) {
        snprintf(dump, sizeof dump,
                 "%s/sp-%u-f%u-face%u-g%u-s%u-%08x.png",
                 sDumpDir, species, form, face, gender, shiny, pid);
        if (!lab_dump_png(man, dump)) {
            PokemonSprite_Delete(sprite);
            sFailed++;
            return 0;
        }
    }

    PokemonSprite_Delete(sprite);
    sLoaded++;
    return 1;
}

static int lab_species(PokemonSpriteManager *man, NARC *pokegra, NARC *otherpoke,
                       u16 species, int both_pids)
{
    int forms = lab_form_count(species);
    int genders[2], ng, form, gi, face, shiny;
    uint64_t maleChar = 0;
    int haveMale;

    lab_genders(species, genders, &ng);
    for (form = 0; form < forms; form++) {
        for (face = 0; face <= FACE_FRONT; face += FACE_FRONT) {
            haveMale = 0;
            for (gi = 0; gi < ng; gi++) {
                u8 gender = (u8)genders[gi];
                u32 pids[2];
                int np = 1;
                int pi;

                pids[0] = 0;
                if (both_pids && species == SPECIES_SPINDA
                        && face == FACE_FRONT && form == 0) {
                    pids[0] = SPINDA_PID_A;
                    pids[1] = SPINDA_PID_B;
                    np = 2;
                }
                for (pi = 0; pi < np; pi++) {
                    for (shiny = 0; shiny < 2; shiny++) {
                        if (!lab_one(man, pokegra, otherpoke, species,
                                     (u8)form, (u8)face, gender, (u8)shiny,
                                     pids[pi])) {
                            return 0;
                        }
                        if (gender == GENDER_MALE && shiny == 0 && pi == 0) {
                            maleChar = pc_state_fnv1a(PC_STATE_FNV64_OFFSET,
                                                      man->charRawData,
                                                      man->charSize);
                            haveMale = 1;
                        } else if (gender == GENDER_FEMALE && shiny == 0
                                && pi == 0 && haveMale) {
                            uint64_t femaleChar = pc_state_fnv1a(
                                PC_STATE_FNV64_OFFSET, man->charRawData,
                                man->charSize);
                            if (femaleChar == maleChar) sGenderSame++;
                        }
                    }
                }
            }
        }
    }
    return 1;
}

static void lab_parse(void)
{
    const char *s = getenv("PC_LAB_SPRITE");
    const char *at = getenv("PC_LAB_SPRITE_AT");
    const char *out = getenv("PC_LAB_SPRITE_OUT");
    const char *dump = getenv("PC_LAB_SPRITE_DUMP");

    sParsed = 1;
    if (s == NULL || s[0] == '\0') return;
    if (strcmp(s, "all") == 0) {
        sMode = LAB_SPRITE_ALL;
    } else if (strcmp(s, "pin") == 0) {
        sMode = LAB_SPRITE_PIN;
    } else {
        const char *p = s;

        sMode = LAB_SPRITE_LIST;
        while (*p != '\0' && sListN < LAB_MAX_LIST) {
            char *end;
            unsigned long v = strtoul(p, &end, 0);

            if (end == p) {
                fprintf(stderr, "pc_lab: PC_LAB_SPRITE wants 'all', 'pin'"
                        " or a comma-separated species list, got '%s'\n", s);
                exit(2);
            }
            /* SPECIES_BAD_EGG is 495. A content package can allocate
             * 496+; BuildPokemonSpriteTemplate's default arm has no
             * ceiling. 494 is SPECIES_EGG and is not a new species. */
            if (v == 0 || v > 65535ul) {
                fprintf(stderr, "pc_lab: PC_LAB_SPRITE species %lu is out of"
                        " range 1..65535\n", v);
                exit(2);
            }
            sList[sListN++] = (u16)v;
            p = (*end == ',') ? end + 1 : end;
        }
        if (sListN == 0) {
            fprintf(stderr, "pc_lab: PC_LAB_SPRITE list was empty\n");
            exit(2);
        }
    }
    if (at != NULL && at[0] != '\0') sAtFrame = strtoul(at, NULL, 0);
    if (out != NULL && out[0] != '\0') {
        sOut = fopen(out, "w");
        if (sOut == NULL) {
            fprintf(stderr, "pc_lab: cannot write %s\n", out);
            exit(2);
        }
    }
    if (dump != NULL && dump[0] != '\0') {
        sDumpDir = dump;
        if (lab_mkdir(dump) != 0 && errno != EEXIST) {
            fprintf(stderr, "pc_lab: cannot create %s: %s\n", dump,
                    strerror(errno));
            exit(2);
        }
    }
}

static void lab_finish(void)
{
    lab_say("pc_lab: sprite done loaded=%d gender_same=%d failed=%d\n",
            sLoaded, sGenderSame, sFailed);
    if (sSpindaGot[0] && sSpindaGot[1]) {
        lab_say("pc_lab: sprite pin spinda pid=0x%08x char_fnv=%016llx\n",
                SPINDA_PID_A, (unsigned long long)sSpindaChar[0]);
        lab_say("pc_lab: sprite pin spinda pid=0x%08x char_fnv=%016llx\n",
                SPINDA_PID_B, (unsigned long long)sSpindaChar[1]);
        if (sSpindaChar[0] == sSpindaChar[1]) {
            lab_say("pc_lab: sprite FAIL Spinda PIDs 0x%08x and 0x%08x"
                    " hashed the same, TryDrawSpindaSpots did not apply\n",
                    SPINDA_PID_A, SPINDA_PID_B);
            if (sOut != NULL) fclose(sOut);
            exit(2);
        }
    } else if (sMode == LAB_SPRITE_PIN || sMode == LAB_SPRITE_ALL) {
        lab_say("pc_lab: sprite FAIL Spinda front was not loaded under both"
                " pin PIDs\n");
        if (sOut != NULL) fclose(sOut);
        exit(2);
    }
    if (sOut != NULL) fclose(sOut);
    if (sFailed) exit(2);
    exit(0);
}

static const u16 sPinSpecies[] = {
    SPECIES_PIKACHU,   /* gender-different sprite */
    SPECIES_TURTWIG,   /* ordinary pl_pokegra, both faces */
    SPECIES_SPINDA,    /* PID spots */
    SPECIES_UNOWN,
    SPECIES_CASTFORM,
    SPECIES_DEOXYS,
    SPECIES_BURMY,
    SPECIES_WORMADAM,
    SPECIES_SHELLOS,
    SPECIES_GASTRODON,
    SPECIES_CHERRIM,
    SPECIES_GIRATINA,
    SPECIES_SHAYMIN,
    SPECIES_ROTOM,
    SPECIES_ARCEUS,
    SPECIES_EGG,
};

void pc_lab_sprite_frame(unsigned long long frame)
{
    PokemonSpriteManager *man;
    NARC *pokegra, *otherpoke;
    int i;

    if (!sParsed) lab_parse();
    if (sMode == LAB_SPRITE_NONE || sDone) return;
    if (frame < sAtFrame) return;
    sDone = 1;

    fprintf(stderr, "pc_lab: sprite starting at frame %llu mode=%s\n",
            frame,
            sMode == LAB_SPRITE_ALL ? "all"
            : sMode == LAB_SPRITE_PIN ? "pin" : "list");
    fflush(stderr);

    if (!Heap_Create(HEAP_ID_APPLICATION, LAB_SPRITE_HEAP, LAB_SPRITE_HEAP_SZ)) {
        fprintf(stderr, "pc_lab: sprite: Heap_Create(%d, %u) failed --"
                " APPLICATION is too full at frame %llu\n",
                LAB_SPRITE_HEAP, LAB_SPRITE_HEAP_SZ, frame);
        exit(2);
    }

    man = PokemonSpriteManager_New(LAB_SPRITE_HEAP);
    if (man == NULL) {
        fprintf(stderr, "pc_lab: sprite: PokemonSpriteManager_New failed\n");
        exit(2);
    }
    pokegra = NARC_ctor(NARC_INDEX_POKETOOL__POKEGRA__PL_POKEGRA,
                        LAB_SPRITE_HEAP);
    otherpoke = NARC_ctor(NARC_INDEX_POKETOOL__POKEGRA__PL_OTHERPOKE,
                          LAB_SPRITE_HEAP);
    if (pokegra == NULL || otherpoke == NULL) {
        fprintf(stderr, "pc_lab: sprite: could not open pl_pokegra /"
                " pl_otherpoke\n");
        exit(2);
    }
    lab_say("pc_lab: sprite archives pl_pokegra=%u pl_otherpoke=%u\n",
            NARC_GetFileCount(pokegra), NARC_GetFileCount(otherpoke));

    if (sMode == LAB_SPRITE_ALL) {
        for (i = SPECIES_BULBASAUR; i <= SPECIES_BAD_EGG; i++) {
            if (!lab_species(man, pokegra, otherpoke, (u16)i, 1)) break;
            if ((sLoaded % 200) == 0) {
                fprintf(stderr, "pc_lab: sprite progress loaded=%d species=%d\n",
                        sLoaded, i);
                fflush(stderr);
            }
        }
    } else if (sMode == LAB_SPRITE_PIN) {
        for (i = 0; i < (int)(sizeof sPinSpecies / sizeof sPinSpecies[0]); i++) {
            if (!lab_species(man, pokegra, otherpoke, sPinSpecies[i], 1)) break;
        }
    } else {
        for (i = 0; i < sListN; i++) {
            if (!lab_species(man, pokegra, otherpoke, sList[i], 1)) break;
        }
    }

    NARC_dtor(pokegra);
    NARC_dtor(otherpoke);
    PokemonSpriteManager_Free(man);
    Heap_Destroy(LAB_SPRITE_HEAP);
    lab_finish();
}
