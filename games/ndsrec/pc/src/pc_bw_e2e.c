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
 * Nothing here writes game memory, so a run without PC_E2E is the run it
 * always was, and with it the game's frames and hashes are unchanged too.
 */
#include <stdint.h>
#include <string.h>

#include "armrec_rt.h"
#include "pc_np_options.h"

#if defined(PC_BW_VER_WHITE)
#define BW_GAMESYS_PTR 0x02146268u
#define BW_GRID_QUERY ov21_0218DB0C
#define BW_OFF_MAP ov21_0218DC24
#define BW_HIT_CHECK ov10_021638EC
#else
#define BW_GAMESYS_PTR 0x02146248u
#define BW_GRID_QUERY ov21_0218DAEC
#define BW_OFF_MAP ov21_0218DC04
#define BW_HIT_CHECK ov10_021638CC
#endif

extern uint64_t BW_GRID_QUERY(uint32_t mapper, uint32_t pos, uint32_t out, uint32_t unused);
extern uint64_t BW_OFF_MAP(uint32_t mapper, uint32_t pos, uint32_t unused2, uint32_t unused3);
extern uint64_t BW_HIT_CHECK(uint32_t mmdl, uint32_t from, uint32_t x, uint32_t y);
extern uint32_t armrec_sp;

/* the field overlays the two functions live in */
#define BW_OVERLAY_MMDL 10
#define BW_OVERLAY_FIELD 21

#define GAMESYS_FIELDMAP 0x14
#define GAMESYS_EVENT 0x18
#define GAMESYS_GAMEDATA 0x1C
#define GAMEDATA_ZONE 0x114
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

/* ---- the frame */

static void bw_e2e_frame(const bw_field *f, int field, int ready)
{
    uint32_t arr;
    unsigned i, n;

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

static void bw_frame(void)
{
    bw_field f;
    const int field = bw_field_get(&f);
    const int ready = field && f.event == 0 && !bw_player_moving(&f);

    pc_np_stat.field_ready = (unsigned)ready;
    pc_np_stat.map_id = field ? f.zone : 0;
    /* Black/White have no quick save: a milestone saves through the game's
     * own menu (tests/e2e bots, `save`), so a request is refused at once. */
    if (pc_np_opt.quicksave_seq != pc_np_stat.quicksave_seq) {
        pc_np_stat.quicksave_seq = pc_np_opt.quicksave_seq;
        pc_np_stat.quicksave_result = PC_NP_QS_REFUSED;
    }
    if (pc_e2e_on()) bw_e2e_frame(&f, field, ready);
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
