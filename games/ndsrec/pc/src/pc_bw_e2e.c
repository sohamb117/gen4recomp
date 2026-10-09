/*
 * Black/White's game-side frame work (pc_np_options.h pc_np_frame): the
 * field status every frame, and the end-to-end probe (core/include/
 * np_e2e.h, tests/e2e) under PC_E2E=1.
 *
 * There is no game source: everything here reads the recompiled cartridge's
 * own structures in guest memory at the frame boundary, at addresses and
 * offsets docs/BW_RAM.md lists with how each was found, and asks two of the
 * game's own functions about the terrain. The only per-version differences
 * are the GAMESYS pointer and the two field overlays' function addresses
 * (Makefile.wasm compiles this file with -DPC_BW_VER_WHITE for White).
 *
 *   GAMESYS   *(0x02146248 / 0x02146268): +0x14 FIELDMAP (0 outside the
 *             field), +0x18 the running GMEVENT (a script, a menu, a warp;
 *             0 when the player is free), +0x1C GAMEDATA
 *   GAMEDATA  +0x114 the player's zone id (u16), +0x1A8 MMDLSYS
 *   MMDLSYS   +0x04 u16 object slots, +0x18 the MMDL array (0x100 bytes
 *             each), +0x34 the G3D mapper
 *   MMDL      +0x00 status (bit 0: in use), +0x08 u16 object id (0xFF the
 *             player), +0x0C u16 graphics id, +0x18 s16 facing (0 up,
 *             1 down, 2 left, 3 right), +0x3C s16 x/y/z tile, +0x44 the
 *             VecFx32 position (a tile is 16 units, its centre +8)
 *
 * The terrain: the mapper's grid query (Black ov21_0218DAEC, White
 * ov21_0218DB0C; (mapper, VecFx32 *pos, out[16])) answers the place to
 * stand nearest pos.y on the tile under pos: out+8 the MAPATTR (value in the
 * low half, flags in the high half, flag bit 0 the "hitch": blocked; value
 * 0xFF or the whole word 0xFFFFFFFF: no attribute), out+12 its height. The
 * step check is the object movement check every MMDL step asks (Black
 * ov10_021638CC, White ov10_021638EC; (mmdl, VecFx32 *from, x, y, [z, dir])
 * -> bits 1 move limit, 2 attribute, 4 another object, 8 height, 0x10 off
 * the map), asked for the player from each tile of the flood; objects are
 * left to the planner, which reads them from objects[], as on D/P.
 *
 * The functions run on a guest stack of their own (as D/P's pc_dp_field.c
 * does): the frame boundary is the idle thread's, whose stack is tiny.
 * The probe writes no game memory, so a run without PC_E2E is the run it
 * always was, and with it the game's frames and hashes are unchanged too.
 *
 * F1 quick save (NP_OPT_QUICKSAVE_SEQ) is the game's own save without the
 * X menu, as the menu's SAVE and the script SAVE command do it (Black
 * ov10_02169AB8 / ov10_02159A64; addresses are the same in White's static
 * code below 0x02013100, the only code used here):
 *
 *   sub_020071F0(savecontrol) == 1  the menu answers "cannot save" (a new
 *                                   adventure over an existing file)
 *   GAMEDATA+0x1CE                  set while a save runs
 *   sub_02008DF0(sub_02012F2C(gd), 1)   the records' "times saved" + 1
 *   sub_02012DAC(gd)                start: the live GAMEDATA written back
 *                                   into the save blocks, the async write
 *   sub_02012DD0(gd)                each frame: 0 / 1 writing, 2 saved,
 *                                   3 failed
 *
 * The save is asynchronous (about 230 frames), so the player is held for it
 * by a GMEVENT of the game's own (sub_020122C0 create, sub_02012108 make it
 * the running one): its function is ov10_02161340 ("done once seq is not
 * 0", Black 0x02161341 / White 0x02161361), so setting its seq to 1 when
 * the save ends lets the game's event runner free it and give the player
 * back. It starts only where field_ready holds: the field running, no
 * event, the player on a tile centre.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "armrec_rt.h"
#include "pc_np_options.h"

#if defined(PC_BW_VER_WHITE)
#define BW_GAMESYS_PTR 0x02146268u
#define BW_BATTLE_VIEW 0x021F63B8u
#define BW_GRID_QUERY ov21_0218DB0C
#define BW_OFF_MAP ov21_0218DC24
#define BW_HIT_CHECK ov10_021638EC
#define BW_EVENT_UNTIL_SEQ 0x02161361u /* ov10_02161360, Thumb */
#else
#define BW_GAMESYS_PTR 0x02146248u
#define BW_BATTLE_VIEW 0x021F6398u
#define BW_GRID_QUERY ov21_0218DAEC
#define BW_OFF_MAP ov21_0218DC04
#define BW_HIT_CHECK ov10_021638CC
#define BW_EVENT_UNTIL_SEQ 0x02161341u /* ov10_02161340, Thumb */
#endif

extern uint64_t BW_GRID_QUERY(uint32_t mapper, uint32_t pos, uint32_t out, uint32_t unused);
extern uint64_t BW_OFF_MAP(uint32_t mapper, uint32_t pos, uint32_t unused2, uint32_t unused3);
extern uint64_t BW_HIT_CHECK(uint32_t mmdl, uint32_t from, uint32_t x, uint32_t y);
/* the save and the event, at the same addresses in both games */
extern uint64_t sub_020071F0(uint32_t savecontrol, uint32_t u1, uint32_t u2, uint32_t u3);
extern uint64_t sub_02012F2C(uint32_t gamedata, uint32_t u1, uint32_t u2, uint32_t u3);
extern uint64_t sub_02008DF0(uint32_t records, uint32_t id, uint32_t u2, uint32_t u3);
extern uint64_t sub_02012DAC(uint32_t gamedata, uint32_t u1, uint32_t u2, uint32_t u3);
extern uint64_t sub_02012DD0(uint32_t gamedata, uint32_t u1, uint32_t u2, uint32_t u3);
extern uint64_t sub_020122C0(uint32_t gamesys, uint32_t parent, uint32_t func, uint32_t worksize);
extern uint64_t sub_02012108(uint32_t gamesys, uint32_t event, uint32_t u2, uint32_t u3);
extern uint32_t armrec_sp;

/* the field overlays the two functions live in */
#define BW_OVERLAY_MMDL 10
#define BW_OVERLAY_FIELD 21
/* the battle overlay; BW_BATTLE_VIEW is a static of its .bss (docs/BW_RAM.md, Battle) */
#define BW_OVERLAY_BATTLE 93

/* The battle: BW_BATTLE_VIEW +0x00 the battle main module, +0x04 its POKECON (main+0xC8, whose +0x00
 * points back to main). POKECON +0x04 + 0x1C*client: a client's party, BattleMon pointers by slot
 * (slot 0 in front) and the u8 count at +0x18. BattleMon: +0x0C species, +0x0E max HP, +0x10 HP,
 * +0x18 level, +0xF8/+0xF9 the current types, +0x104 four 0x0E-byte move slots {u16 move, u8 PP}. */
#define BATTLE_MAIN_POKECON 0xC8
#define POKECON_PARTY 0x04
#define POKECON_PARTY_SIZE 0x1C
#define PARTY_COUNT 0x18
#define BTL_CLIENTS 4
#define BTL_PARTY_MAX 6
#define BPP_SPECIES 0x0C
#define BPP_MAX_HP 0x0E
#define BPP_HP 0x10
#define BPP_LEVEL 0x18
#define BPP_TYPES 0xF8
#define BPP_MOVES 0x104
#define BPP_MOVE_SIZE 0x0E

/* The battle menu (derived from the generated assembly, proven in the bedroom's two battles: docs/BW_RAM.md,
 * Battle menu; screens 3-7 and a double battle's chooser are not seen yet).
 * main+0x04 the view (BTLV_CORE: ov93_021E8F20 builds it, ov93_021CD95C hands it to each client),
 * main+0x10 + 4*id the clients, main+0x46C (u8) the player's client id. The view's +0x180 the bottom
 * screen's controller (overlay 95, mapped at 0x06898020: ov95_06899ED0 starts the action menu, ov95_06899F40
 * the moves), +0xBC the chooser's BattleMon (ov93_021E9760 / ov93_021E97BC store it). The controller's +0x60
 * the depth of its task stack (ov95_06899E48 pushes, ov95_06899E68 pops when a menu is done), +0xB0 the
 * input screen (overlay 94). The input screen's +0x58 the screen it shows (ov94_0220270C builds it:
 * 0 the standby screen, 1 the action menu FIGHT/BAG/POKEMON/RUN, 2 the four moves, 5 the moves of a
 * multi-battler turn, 3/4 others), +0x68 bits 5..8 the key cursor (ov94_02206140 moves it through the
 * screen's key table: action 0 FIGHT, 1 BAG, 2 POKEMON, 3 RUN; moves 0 top left, 1 top right, 2 bottom
 * left, 3 bottom right, 4 back), +0x274 a pointer to the byte that says the cursor is shown (0: the first
 * key press only shows it, ov94_02206140 at 0x0220636A). */
#define MAIN_VIEW 0x04
#define MAIN_CLIENTS 0x10
#define VIEW_SCU 0x180
#define VIEW_CHOOSER 0xBC
#define SCU_DEPTH 0x60
#define SCU_INPUT 0xB0
#define INPUT_SCREEN 0x58
#define INPUT_STATE 0x68
#define INPUT_KEY_MODE 0x274
#define SCREEN_ACTION 1
#define SCREEN_MOVES 2
#define SCREEN_MOVES_MULTI 5
#define BW_OVERLAY_BATTLE_INPUT 94
#define BW_OVERLAY_BATTLE_SCU 95
/* np_e2e.h ui_arg: 1 the action menu, 11 the moves (D/P's menu config numbers); other B/W screens report
 * 0x100 + the screen number; ui_cursor bit 8: the cursor is not shown yet */
#define UI_ARG_ACTION 1
#define UI_ARG_MOVES 11
#define UI_ARG_RAW 0x100
#define UI_CURSOR_HIDDEN 0x100

#define GAMESYS_FIELDMAP 0x14
#define GAMESYS_EVENT 0x18
#define GAMESYS_GAMEDATA 0x1C
#define GAMEDATA_ZONE 0x114
#define GAMEDATA_SAVING 0x1CE
#define GMEVENT_SEQ 0x08
#define RECORD_SAVES 1
#define GAMEDATA_MMDLSYS 0x1A8
#define MMDLSYS_SLOTS 0x04
#define MMDLSYS_ARRAY 0x18
#define MMDLSYS_MAPPER 0x34
#define MMDL_SIZE 0x100
#define MMDL_STATUS 0x00
#define MMDL_ID 0x08
#define MMDL_GFX 0x0C
#define MMDL_FACING 0x18
#define MMDL_TILE 0x3C
#define MMDL_POS 0x44
#define MMDL_ID_PLAYER 0xFF

#define HIT_ATTR 0x02u
#define HIT_HEIGHT 0x08u
#define HIT_OFF_MAP 0x10u

#define TILE_FX (16 << 12)
#define HALF_TILE_FX (8 << 12)

typedef struct {
    uint32_t gamesys, fieldmap, event, gamedata, mmdlsys, mapper, player;
    unsigned zone;
} bw_field;

static int bw_ram(uint32_t a)
{
    return a >= 0x02000000u && a < 0x02400000u && (a & 3) == 0;
}

static uint32_t rd32(uint32_t a)
{
    return *(const volatile uint32_t *)(uintptr_t)a;
}

static unsigned rd16(uint32_t a)
{
    return *(const volatile uint16_t *)(uintptr_t)a;
}

static int rds16(uint32_t a)
{
    return *(const volatile int16_t *)(uintptr_t)a;
}

/* The field's structures, 0 where there is no field (a menu application,
 * a battle, the title). */
static int bw_field_get(bw_field *f)
{
    uint32_t arr;
    unsigned i, n;

    memset(f, 0, sizeof *f);
    f->gamesys = rd32(BW_GAMESYS_PTR);
    if (!bw_ram(f->gamesys)) return 0;
    f->gamedata = rd32(f->gamesys + GAMESYS_GAMEDATA);
    if (!bw_ram(f->gamedata)) return 0;
    f->zone = rd16(f->gamedata + GAMEDATA_ZONE);
    f->fieldmap = rd32(f->gamesys + GAMESYS_FIELDMAP);
    f->event = rd32(f->gamesys + GAMESYS_EVENT);
    f->mmdlsys = rd32(f->gamedata + GAMEDATA_MMDLSYS);
    if (!bw_ram(f->fieldmap) || !bw_ram(f->mmdlsys)) return 0;
    if (!armrec_overlay_resident(BW_OVERLAY_MMDL) || !armrec_overlay_resident(BW_OVERLAY_FIELD)) return 0;
    f->mapper = rd32(f->mmdlsys + MMDLSYS_MAPPER);
    arr = rd32(f->mmdlsys + MMDLSYS_ARRAY);
    n = rd16(f->mmdlsys + MMDLSYS_SLOTS);
    if (!bw_ram(f->mapper) || !bw_ram(arr) || n > 0x100) return 0;
    for (i = 0; i < n; i++) {
        const uint32_t m = arr + i * MMDL_SIZE;

        if ((rd32(m + MMDL_STATUS) & 1) && rd16(m + MMDL_ID) == MMDL_ID_PLAYER) {
            f->player = m;
            break;
        }
    }
    return f->player != 0;
}

/* The player stands on a tile centre between steps; mid-step it does not. */
static int bw_player_moving(const bw_field *f)
{
    const int32_t px = (int32_t)rd32(f->player + MMDL_POS), pz = (int32_t)rd32(f->player + MMDL_POS + 8);

    return px != rds16(f->player + MMDL_TILE) * TILE_FX + HALF_TILE_FX
        || pz != rds16(f->player + MMDL_TILE + 4) * TILE_FX + HALF_TILE_FX;
}

/* ---- the game's terrain queries (on the probe's guest stack) */

static int32_t sPos[3];
static uint32_t sGrid[4]; /* normal (3 x s16, pad), MAPATTR, height */

/* The place to stand on tile (x, z) nearest height y: 1 and sGrid filled,
 * or 0 off the map / no terrain there. */
static int bw_grid(const bw_field *f, int x, int z, int32_t y)
{
    sPos[0] = x * TILE_FX + HALF_TILE_FX;
    sPos[1] = y;
    sPos[2] = z * TILE_FX + HALF_TILE_FX;
    if ((uint32_t)ARMREC_CALL(BW_OFF_MAP, f->mapper, (uint32_t)(uintptr_t)sPos, 0, 0)) return 0;
    return (uint32_t)ARMREC_CALL(BW_GRID_QUERY, f->mapper, (uint32_t)(uintptr_t)sPos, (uint32_t)(uintptr_t)sGrid, 0)
        != 0;
}

static unsigned bw_tile(void *ctx, int x, int z)
{
    const bw_field *f = ctx;
    uint32_t attr;

    if (!bw_grid(f, x, z, (int32_t)rd32(f->player + MMDL_POS + 4))) return 0;
    attr = sGrid[2];
    if (attr == 0xFFFFFFFFu || (attr & 0xFFFF) == 0xFF) return 0;
    return PC_E2E_TILE_KNOWN | (attr & 0xFF) | ((attr >> 16) & 1 ? PC_E2E_TILE_COLLISION : 0);
}

/* One step of the player from (x, z) at height y in direction dir, by the
 * game's own movement check; *ty the height of the place it lands on. */
static int bw_step(void *ctx, int x, int z, int y, int dir, int *ty)
{
    static const int dx[4] = {0, 0, -1, 1}, dz[4] = {-1, 1, 0, 0};
    const bw_field *f = ctx;
    const int nx = x + dx[dir], nz = z + dz[dir];
    uint32_t hit;

    sPos[0] = x * TILE_FX + HALF_TILE_FX;
    sPos[1] = y;
    sPos[2] = z * TILE_FX + HALF_TILE_FX;
    /* arguments five and six (z, dir) on the guest stack, as a caller pushes them */
    armrec_sp -= 8;
    *(volatile uint32_t *)(uintptr_t)armrec_sp = (uint32_t)nz;
    *(volatile uint32_t *)(uintptr_t)(armrec_sp + 4) = (uint32_t)dir;
    hit = (uint32_t)ARMREC_CALL(BW_HIT_CHECK, f->player, (uint32_t)(uintptr_t)sPos, (uint32_t)nx, (uint32_t)(y >> 16));
    armrec_sp += 8;
    if (hit & (HIT_ATTR | HIT_HEIGHT | HIT_OFF_MAP)) return 0;
    if (!bw_grid(f, nx, nz, y)) return 0;
    *ty = (int)sGrid[3];
    return 1;
}

/* ---- the battle */

static void bw_mon(uint32_t bpp, pc_e2e_mon *m)
{
    int i;

    memset(m, 0, sizeof *m);
    m->species = (unsigned short)rd16(bpp + BPP_SPECIES);
    m->hp = (unsigned short)rd16(bpp + BPP_HP);
    m->max_hp = (unsigned short)rd16(bpp + BPP_MAX_HP);
    m->level = *(const volatile uint8_t *)(uintptr_t)(bpp + BPP_LEVEL);
    m->types[0] = *(const volatile uint8_t *)(uintptr_t)(bpp + BPP_TYPES);
    m->types[1] = *(const volatile uint8_t *)(uintptr_t)(bpp + BPP_TYPES + 1);
    for (i = 0; i < 4; i++) {
        m->moves[i] = (unsigned short)rd16(bpp + BPP_MOVES + i * BPP_MOVE_SIZE);
        m->pp[i] = *(const volatile uint8_t *)(uintptr_t)(bpp + BPP_MOVES + i * BPP_MOVE_SIZE + 2);
    }
}

/* The battle's POKECON, 0 outside a battle: the battle overlay resident, its view static naming a
 * main module whose POKECON points back at it, and no field (the field is torn down for a battle). */
static uint32_t bw_pokecon(int field)
{
    uint32_t main_, pokecon;

    if (field || !armrec_overlay_resident(BW_OVERLAY_BATTLE)) return 0;
    main_ = rd32(BW_BATTLE_VIEW);
    pokecon = rd32(BW_BATTLE_VIEW + 4);
    if (!bw_ram(main_) || pokecon != main_ + BATTLE_MAIN_POKECON || rd32(pokecon) != main_) return 0;
    return pokecon;
}

/* Each client's front Pokemon as battler 0..3 (0 the player, 1 the foe in a single battle) and the
 * player's party in slot order; the battler whose menu is up, from the view's chooser (its BattleMon
 * matched against the clients' front slots: client c, slot s -> battler c + 2s [INFERENCE: doubles]). */
static void bw_battle_report(uint32_t pokecon, uint32_t chooser)
{
    pc_e2e_mon battlers[BTL_CLIENTS], party[BTL_PARTY_MAX];
    unsigned c, i, nparty = 0, menu_battler = 0;

    memset(battlers, 0, sizeof battlers);
    for (c = 0; c < BTL_CLIENTS; c++) {
        const uint32_t p = pokecon + POKECON_PARTY + c * POKECON_PARTY_SIZE;
        unsigned n = *(const volatile uint8_t *)(uintptr_t)(p + PARTY_COUNT);

        if (n > BTL_PARTY_MAX) n = 0;
        for (i = 0; i < n; i++) {
            const uint32_t bpp = rd32(p + i * 4);

            if (!bw_ram(bpp)) continue;
            if (i == 0) bw_mon(bpp, &battlers[c]);
            if (c == 0) bw_mon(bpp, &party[nparty++]);
            if (bpp == chooser && i < 2) menu_battler = c + 2 * i;
        }
    }
    pc_e2e_battle(menu_battler, 0, battlers, BTL_CLIENTS, party, nparty);
}

/* The bottom screen's menu while the player chooses: the ui report (np_e2e.h) and the key cursor; 0 and
 * the chooser 0 when no menu waits. */
static uint32_t bw_battle_menu(uint32_t pokecon)
{
    const uint32_t main_ = rd32(pokecon); /* POKECON +0x00: the main module */
    uint32_t view, scu, input, key_mode;
    unsigned screen, cursor, arg;

    if (!armrec_overlay_resident(BW_OVERLAY_BATTLE_INPUT) || !armrec_overlay_resident(BW_OVERLAY_BATTLE_SCU))
        return 0;
    view = rd32(main_ + MAIN_VIEW);
    if (!bw_ram(view)) return 0;
    scu = rd32(view + VIEW_SCU);
    if (!bw_ram(scu)) return 0;
    input = rd32(scu + SCU_INPUT);
    if (!bw_ram(input) || rd32(scu + SCU_DEPTH) == 0) return 0;
    screen = rd32(input + INPUT_SCREEN);
    if (screen == 0 || screen > 7) return 0;
    arg = screen == SCREEN_ACTION ? UI_ARG_ACTION
        : screen == SCREEN_MOVES || screen == SCREEN_MOVES_MULTI ? UI_ARG_MOVES : UI_ARG_RAW + screen;
    cursor = (rd32(input + INPUT_STATE) >> 5) & 0xF;
    key_mode = rd32(input + INPUT_KEY_MODE);
    if (!bw_ram(key_mode & ~3u) || !*(const volatile uint8_t *)(uintptr_t)key_mode) cursor |= UI_CURSOR_HIDDEN;
    pc_e2e_ui(PC_E2E_UI_BATTLE_MENU, arg);
    pc_e2e_cursor(cursor);
    return rd32(view + VIEW_CHOOSER);
}

/* ---- the frame */

static void bw_e2e_frame(const bw_field *f, int field, int ready, uint32_t pokecon)
{
    uint32_t arr;
    unsigned i, n;

    if (pokecon) bw_battle_report(pokecon, bw_battle_menu(pokecon));
    if (!field) {
        pc_e2e_field(0, 0, 0, 0, 0, 0, 0);
        pc_e2e_end_frame();
        return;
    }
    pc_e2e_field(1, f->zone, rds16(f->player + MMDL_TILE), rds16(f->player + MMDL_TILE + 4),
                 rds16(f->player + MMDL_TILE + 2), (unsigned)rds16(f->player + MMDL_FACING) & 3,
                 bw_player_moving(f) ? 1 : 0);
    if (ready) {
        pc_e2e_steps(bw_step, (void *)f, (int)rd32(f->player + MMDL_POS + 4));
        pc_e2e_grid(bw_tile, (void *)f);
    }
    arr = rd32(f->mmdlsys + MMDLSYS_ARRAY);
    n = rd16(f->mmdlsys + MMDLSYS_SLOTS);
    for (i = 0; i < n; i++) {
        const uint32_t m = arr + i * MMDL_SIZE;

        if (!(rd32(m + MMDL_STATUS) & 1) || m == f->player) continue;
        pc_e2e_object(rds16(m + MMDL_TILE), rds16(m + MMDL_TILE + 4), rd16(m + MMDL_ID), rd16(m + MMDL_GFX));
    }
    pc_e2e_end_frame();
}

/* ---- F1 quick save */

/* A request waits this long for the player to be free (as on D/P and
 * HG/SS); the write itself takes about 230 frames, and one that has not
 * ended by QUICKSAVE_LIMIT gives the player back as failed. */
#define QUICKSAVE_PATIENCE 60
#define QUICKSAVE_LIMIT 1800

static unsigned sQsWait, sQsFrames;
static uint32_t sQsEvent, sQsGamedata;

static void quicksave_done(unsigned result)
{
    fprintf(stderr, "pc-np: quick save %u: %s\n", pc_np_opt.quicksave_seq,
            result == PC_NP_QS_SAVED     ? "saved"
            : result == PC_NP_QS_REFUSED ? "refused, the player is not free in the field or the game cannot save"
                                         : "FAILED");
    pc_np_stat.quicksave_seq = pc_np_opt.quicksave_seq;
    pc_np_stat.quicksave_result = result;
    sQsWait = 0;
    sQsFrames = 0;
    sQsEvent = 0;
}

static void quicksave_frame(const bw_field *f, int ready)
{
    uint32_t gd, sc, ev;

    if (sQsEvent != 0) {
        const uint32_t gamesys = rd32(BW_GAMESYS_PTR);
        unsigned r;

        /* A snapshot loaded over the save: the event is not ours any more. */
        if (!bw_ram(gamesys) || rd32(gamesys + GAMESYS_EVENT) != sQsEvent) {
            quicksave_done(PC_NP_QS_FAILED);
            return;
        }
        r = (unsigned)ARMREC_CALL(sub_02012DD0, sQsGamedata, 0, 0, 0);
        if (r >= 2 || ++sQsFrames > QUICKSAVE_LIMIT) {
            *(volatile uint32_t *)(uintptr_t)(sQsEvent + GMEVENT_SEQ) = 1; /* the event ends, the player is free */
            quicksave_done(r == 2 ? PC_NP_QS_SAVED : PC_NP_QS_FAILED);
        }
        return;
    }
    if (pc_np_opt.quicksave_seq == pc_np_stat.quicksave_seq) return;
    if (!ready) {
        if (++sQsWait > QUICKSAVE_PATIENCE) quicksave_done(PC_NP_QS_REFUSED);
        return;
    }
    gd = f->gamedata;
    sc = rd32(gd);
    if (!bw_ram(sc) || (uint32_t)ARMREC_CALL(sub_020071F0, sc, 0, 0, 0) == 1
        || *(const volatile uint8_t *)(uintptr_t)(gd + GAMEDATA_SAVING) != 0) {
        quicksave_done(PC_NP_QS_REFUSED);
        return;
    }
    ev = (uint32_t)ARMREC_CALL(sub_020122C0, f->gamesys, 0, BW_EVENT_UNTIL_SEQ, 4);
    if (!bw_ram(ev)) {
        quicksave_done(PC_NP_QS_FAILED);
        return;
    }
    ARMREC_CALL(sub_02012108, f->gamesys, ev, 0, 0);
    ARMREC_CALL(sub_02008DF0, (uint32_t)ARMREC_CALL(sub_02012F2C, gd, 0, 0, 0), RECORD_SAVES, 0, 0);
    ARMREC_CALL(sub_02012DAC, gd, 0, 0, 0);
    sQsEvent = ev;
    sQsGamedata = gd;
    sQsFrames = 0;
}

static void bw_frame(void)
{
    bw_field f;
    const int field = bw_field_get(&f);
    const int ready = field && f.event == 0 && !bw_player_moving(&f);
    uint32_t pokecon;

    pc_np_stat.field_ready = (unsigned)ready;
    pc_np_stat.map_id = field ? f.zone : 0;
    pokecon = bw_pokecon(field);
    pc_np_stat.in_battle_app = pokecon != 0;
    quicksave_frame(&f, ready);
    if (pc_e2e_on()) bw_e2e_frame(&f, field, ready, pokecon);
}

void pc_np_frame(void)
{
    static uint32_t stack[0x2000] __attribute__((aligned(8))); /* 32 KiB of guest stack */
    static int busy;
    uint32_t saved;

    if (busy) return;
    busy = 1;
    saved = armrec_sp;
    armrec_sp = (uint32_t)(uintptr_t)(stack + 0x2000);
    bw_frame();
    armrec_sp = saved;
    busy = 0;
}
