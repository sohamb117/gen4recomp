/*
 * PC_PROBE_2D=N or N-M: at the end of those frames, print the 2D sprite
 * state the renderer is about to consume, OAM entries that are enabled,
 * the standard OBJ palettes, the extended OBJ palettes, and a digest of
 * each engine's OBJ character VRAM window. The reader this exists for is
 * a person chasing "the sprite draws wrong": one line per live sprite
 * says which tiles and which palette it asked for, and the palette sums
 * say whether anything ever loaded there.
 *
 * Reads go straight through the identity-mapped guest addresses, which is
 * the same view pc_gpu2d.c has. Env-gated, free when unset.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "armrec_rt.h"
#include "pc_gpu3d.h"

/* The game's own system block, for the VBlank-task probe below: gSystem's
 * task managers are where per-VBlank work (sprite-sheet uploads among it)
 * queues, and "did the task ever run" is a question about this list. The
 * game headers want the SDK's fixed-width names in scope first. */
#include <nitro/types.h>

#include "sys_task_manager.h"
#include "system.h"

#define OAM_A   0x07000000u
#define OAM_B   0x07000400u
#define PAL_A   0x05000000u   /* BG 0x000, OBJ 0x200 */
#define PAL_B   0x05000400u
#define OBJ_A   0x06400000u   /* engine A OBJ window, up to 256 KB */
#define OBJ_B   0x06600000u   /* engine B OBJ window, up to 128 KB */

static uint64_t probe_lo, probe_hi;
static int probe_parsed;

/*
 * PC_TRACE_LCDC=N: from frame N on, every MI copy whose destination lies in
 * the LCDC window (0x06800000..0x068A3FFF, where GX_LoadTex and friends
 * write texture and palette data through) prints one line. The copy sites in
 * pc_mi.c and pc_dma.c call pc_trace_lcdc(); this is the shared gate.
 */
uint64_t pc_probe_frame;
static int64_t trace_lcdc_from = -1;
static int trace_lcdc_parsed;

void pc_trace_lcdc(const char *who, const void *src, uint32_t dst, uint32_t size) {
    if (!trace_lcdc_parsed) {
        const char *s = getenv("PC_TRACE_LCDC");
        trace_lcdc_parsed = 1;
        trace_lcdc_from = (s != NULL && *s != '\0') ? (int64_t)strtoull(s, NULL, 0)
                                                    : -1;
    }
    if (trace_lcdc_from < 0 || pc_probe_frame < (uint64_t)trace_lcdc_from) return;
    if (dst < 0x06800000u || dst >= 0x068A4000u) return;
    fprintf(stderr, "pc-lcdc: f=%u %s src=%p dst=%08X size=%u\n",
            (unsigned)pc_probe_frame, who, src, dst, size);
}

static uint16_t rd16(uint32_t a) { return *(volatile uint16_t *)(uintptr_t)a; }

static uint32_t sum_range(uint32_t a, uint32_t bytes) {
    uint32_t s = 0, i;
    for (i = 0; i < bytes; i += 2) s += rd16(a + i);
    return s;
}

static void probe_engine(const char *tag, uint32_t oam, uint32_t pal,
                         uint32_t obj, uint32_t objbytes, int extpal_which) {
    int i, live = 0;

    fprintf(stderr, "pc-probe2d: [%s] OBJ palette rows (sum of 16 colours):", tag);
    for (i = 0; i < 16; i++)
        fprintf(stderr, " %04X", sum_range(pal + 0x200u + i * 32u, 32) & 0xFFFF);
    fprintf(stderr, "\n");

    {
        void *xp = armrec_vram_extpal(extpal_which, 0);
        if (xp == NULL) {
            fprintf(stderr, "pc-probe2d: [%s] OBJ ext palette: unmapped\n", tag);
        } else {
            uint32_t s = 0;
            const uint16_t *p = (const uint16_t *)xp;
            for (i = 0; i < 8192 / 2; i++) s += p[i];
            fprintf(stderr, "pc-probe2d: [%s] OBJ ext palette: sum %08X "
                            "(first row: %04X %04X %04X %04X)\n",
                    tag, s, p[0], p[1], p[2], p[3]);
        }
    }

    fprintf(stderr, "pc-probe2d: [%s] OBJ vram sum %08X (window %u KB)\n",
            tag, sum_range(obj, objbytes), objbytes / 1024u);

    for (i = 0; i < 128; i++) {
        uint16_t a0 = rd16(oam + i * 8u + 0);
        uint16_t a1 = rd16(oam + i * 8u + 2);
        uint16_t a2 = rd16(oam + i * 8u + 4);

        /* not rotscale and disabled -> off */
        if (!(a0 & 0x0100) && (a0 & 0x0200)) continue;
        live++;
        fprintf(stderr, "pc-probe2d: [%s] oam %3d: y=%3d x=%4d "
                        "shape=%d size=%d mode=%d 256c=%d tile=%4u pal=%2u "
                        "prio=%d raw=%04X %04X %04X\n",
                tag, i, a0 & 0xFF, a1 & 0x1FF,
                (a0 >> 14) & 3, (a1 >> 14) & 3, (a0 >> 10) & 3,
                (a0 >> 13) & 1, a2 & 0x3FF, (a2 >> 12) & 0xF,
                (a2 >> 10) & 3, a0, a1, a2);
        if (live >= 24) { fprintf(stderr, "pc-probe2d: [%s] ...\n", tag); break; }
    }
    if (live == 0)
        fprintf(stderr, "pc-probe2d: [%s] no enabled OAM entries\n", tag);
}

/*
 * PC_PROBE_VB=1: one line whenever the VBlank task manager's population
 * changes, task count and each callback pointer. The upload task that
 * feeds sprite sheets to texture VRAM lives (or fails to live) here, and
 * "when did it appear/disappear" is a transition log, not a snapshot.
 */
static void probe_vbtasks(uint64_t frame) {
    static int enabled = -1;
    static int last_n = -1;
    static void *last_cbs[8];
    void *cbs[8] = { 0 };
    SysTask *t;
    int n = 0, changed;

    if (enabled < 0) enabled = getenv("PC_PROBE_VB") != NULL;
    if (!enabled || !armrec_mem_ready || gSystem.vBlankTaskMgr == NULL) return;

    for (t = gSystem.vBlankTaskMgr->sentinelTask.nextTask;
         t != &gSystem.vBlankTaskMgr->sentinelTask && n < 8;
         t = t->nextTask, n++)
        cbs[n] = (void *)t->callback;

    changed = (n != last_n) || (memcmp(cbs, last_cbs, sizeof cbs) != 0);
    if (changed) {
        int i;
        fprintf(stderr, "pc-vbtask: f=%u n=%d:", (unsigned)frame, n);
        for (i = 0; i < n; i++) fprintf(stderr, " %p", cbs[i]);
        fprintf(stderr, "\n");
        last_n = n;
        memcpy(last_cbs, cbs, sizeof cbs);
    }
}

/*
 * PC_TRACE_SCRIPT=N: from frame N on, one line per field-script opcode
 * dispatched (the hook is a pc/patches diff in field_script_context.c).
 * The stream is the ground truth for "which bytecode actually ran" , 
 * cheaper and more honest than a debugger replaying 43,000 frames.
 */
void pc_script_trace(unsigned op, const void *at)
{
    static int64_t from = -2;
    extern void pc_scrcov_mark(int engine, unsigned op);

    /* Coverage first, and unconditionally: it is accumulated for every run
     * rather than from a frame onwards, and PC_TRACE_SCRIPT's window must not
     * decide what the corpus is measured to have executed. */
    pc_scrcov_mark(0, op);

    if (from == -2) {
        const char *s = getenv("PC_TRACE_SCRIPT");
        from = (s != NULL && *s != '\0') ? (int64_t)strtoull(s, NULL, 0) : -1;
    }
    if (from < 0 || pc_probe_frame < (uint64_t)from) return;
    fprintf(stderr, "pc-script: f=%u op=%04x at=%p\n",
            (unsigned)pc_probe_frame, op, at);
}

/*
 * The frontier engine's dispatch loop (src/overlay104/frontier_script_context.c,
 * a table of its own) reached through the same pair of hooks. It is a separate
 * instruction set with its own numbering, so it gets its own engine index
 * rather than being folded into the field counts.
 */
void pc_frontier_script_trace(unsigned op, const void *at)
{
    static int64_t from = -2;
    extern void pc_scrcov_mark(int engine, unsigned op);

    pc_scrcov_mark(1, op);

    if (from == -2) {
        const char *s = getenv("PC_TRACE_SCRIPT");
        from = (s != NULL && *s != '\0') ? (int64_t)strtoull(s, NULL, 0) : -1;
    }
    if (from < 0 || pc_probe_frame < (uint64_t)from) return;
    fprintf(stderr, "pc-frscript: f=%u op=%04x at=%p\n",
            (unsigned)pc_probe_frame, op, at);
}

/*
 * PC_TRACE_FIELD=N: from frame N on, one line per attempt by the player to
 * move, the tile stood on, the direction asked for, the collision mask
 * that came back, and the tile behavior byte under the player and under the
 * tile being walked into. The hook is a pc/patches diff at the end of
 * PlayerAvatar_CheckCollision.
 *
 * Why the collision mask is the interesting part. Stairs and doors are
 * COLLISIONS in this engine: the player pushes into them, the move is
 * refused, and PLAYER_COLLISION_WARP riding along on that refusal is what
 * starts the map change. So "the story will not leave this room" and "the
 * player walked into a wall" are the same event with one bit between them,
 * and reading that bit is otherwise a debugger question tens of thousands
 * of frames into a run.
 *
 * A held key re-asks the same question every frame, so only a changed
 * answer prints; the line is emitted when any of tile, direction or mask
 * moves.
 */
void pc_field_trace_move(int x, int z, int dir, unsigned collision,
                         unsigned here, unsigned next)
{
    static int64_t from = -2;
    static int lastX = -1, lastZ = -1, lastDir = -1;
    static unsigned lastCollision = ~0u;

    if (from == -2) {
        const char *s = getenv("PC_TRACE_FIELD");
        from = (s != NULL && *s != '\0') ? (int64_t)strtoull(s, NULL, 0) : -1;
    }
    if (from < 0 || pc_probe_frame < (uint64_t)from) return;
    if (x == lastX && z == lastZ && dir == lastDir && collision == lastCollision) return;
    lastX = x, lastZ = z, lastDir = dir, lastCollision = collision;

    fprintf(stderr, "pc-field: f=%u at=(%d,%d) dir=%d collision=%08x "
                    "behavior=%02x next=%02x\n",
            (unsigned)pc_probe_frame, x, z, dir, collision, here, next);
}

/*
 * PC_TRACE_JOURNAL=1: one line per journal event a script records, from the
 * hook at the end of ScrCmd_CreateJournalEvent.
 *
 * What it is for. Every field move, Cut, Surf, Strength, Rock Smash,
 * Waterfall, Rock Climb, Defog, Flash, ends its script with that command, so
 * this stream says which move the game believes it performed and on what map.
 * Without it a station that walked past its tree draws the same overworld,
 * pins a digest and passes; it is the "battle over" line's equivalent for a
 * run with no battle in it.
 *
 * `entry` is whether there was a journal to store the event IN, which a
 * station minted by the save lab need not have. The move ran either way, and
 * conflating the two would make the instrument silent exactly where it is
 * being used.
 *
 * Not frame-gated like the traces above: these are rare, a handful in a whole
 * playthrough, and the interesting one is usually the first.
 */
void pc_journal_trace(int eventType, int param, int hasEntry)
{
    static int on = -1;

    if (on < 0) {
        const char *s = getenv("PC_TRACE_JOURNAL");
        on = (s != NULL && *s != '\0' && *s != '0');
    }
    if (!on) return;

    fprintf(stderr, "pc-journal: f=%u event=%d param=%d entry=%d\n",
            (unsigned)pc_probe_frame, eventType, param, hasEntry);
    fflush(stderr);
}

/*
 * PC_TRACE_FISH=1: one line per state change of the fishing task, from the
 * hook in GoFish (pc/patches/src/overlay005/fishing.c.patch).
 *
 * What it is for. The bite window is the only input in this game a scripted
 * run cannot aim at by reading the tree. FishingTask_SetFishWait draws the
 * delay before the bite from the RNG, ((LCRNG_Next() % 4) + 1) * 30 frames , 
 * and an A press that lands before the window opens is FishingTask_
 * ReeledInEarly, so mashing A fails by construction and no amount of reading
 * res/ says when to press. The `bite` line's frame IS that answer: the run is
 * deterministic, so once it has been measured the press is a constant like
 * every other input here.
 *
 * The states worth naming are the six the outcome hangs on. `wait` carries the
 * delay the RNG picked and the rod's window length; `bite` is the frame the
 * window opens, and it stays open for `window` frames from there; `caught`,
 * `early`, `away` and `none` are the four ways a cast can end. A station
 * asserts one of those words, which is a claim about what the run DID that no
 * digest can make, a missed window draws the same overworld.
 *
 * Only a CHANGE prints. GoFish runs every frame and most of those frames sit
 * in one state, so the raw stream would be one line per frame for a couple of
 * hundred frames a cast.
 *
 * Not frame-gated: a run casts a handful of times at most, and the first cast
 * is usually the interesting one.
 */
void pc_fish_trace(int action, const char *what, int rod, int delay, int window)
{
    static int on = -1;
    static int last = -1;

    if (on < 0) {
        const char *s = getenv("PC_TRACE_FISH");
        on = (s != NULL && *s != '\0' && *s != '0');
    }
    if (!on) return;

    if (action == last) return;
    last = action;
    if (what == NULL) return;

    fprintf(stderr, "pc-fish: f=%u rod=%d %s delay=%d window=%d\n",
            (unsigned)pc_probe_frame, rod, what, delay, window);
    fflush(stderr);
}

void pc_probe2d(uint64_t frame) {
    pc_probe_frame = frame;
    probe_vbtasks(frame);
    if (!probe_parsed) {
        const char *s = getenv("PC_PROBE_2D");
        probe_parsed = 1;
        if (s == NULL || *s == '\0') { probe_lo = 1; probe_hi = 0; return; }
        {
            char *end;
            probe_lo = strtoull(s, &end, 0);
            probe_hi = (*end == '-') ? strtoull(end + 1, NULL, 0) : probe_lo;
        }
    }
    if (frame < probe_lo || frame > probe_hi) return;
    if (!armrec_mem_ready) return;

    fprintf(stderr, "pc-probe2d: ===== frame %u =====\n", (unsigned)frame);
    {
        SysTask *t;
        int n = 0;
        fprintf(stderr, "pc-probe2d: gSystem.frameCounter=%u vblankCallback=%p\n",
                (unsigned)gSystem.frameCounter, (void *)gSystem.vblankCallback);
        for (t = gSystem.vBlankTaskMgr->sentinelTask.nextTask;
             t != &gSystem.vBlankTaskMgr->sentinelTask && n < 16;
             t = t->nextTask, n++)
            fprintf(stderr, "pc-probe2d: vbtask %d: cb=%p state=%d prio=%u\n",
                    n, (void *)t->callback, (int)t->state, (unsigned)t->priority);
        fprintf(stderr, "pc-probe2d: vbtasks=%d locked=%d\n",
                n, (int)gSystem.vBlankTaskMgr->locked);
    }
    {
        int i;
        fprintf(stderr, "pc-probe2d: VRAMCNT A-I:");
        for (i = 0; i < 9; i++)
            fprintf(stderr, " %02X",
                    *(volatile uint8_t *)(uintptr_t)armrec_vram_cnt_addr(i));
        fprintf(stderr, "\n");
        for (i = 0; i < 4; i++) {
            const uint16_t *p = (const uint16_t *)armrec_vram_texture(i);
            uint32_t s = 0; int j;
            if (p) for (j = 0; j < 65536; j++) s += p[j];
            fprintf(stderr, "pc-probe2d: tex slot %d: %s sum %08X\n",
                    i, p ? "mapped," : "UNMAPPED", p ? s : 0);
        }
        for (i = 0; i < 8; i++) {
            const uint16_t *p = (const uint16_t *)armrec_vram_texpal(i);
            uint32_t s = 0; int j;
            if (p) for (j = 0; j < 8192; j++) s += p[j];
            fprintf(stderr, "pc-probe2d: texpal slot %d: %s sum %08X\n",
                    i, p ? "mapped," : "UNMAPPED", p ? s : 0);
        }
    }
    /* PC_PROBE_TEX=texaddr,palettebase: hex-dump 64 bytes of texture data
     * and the 32-byte palette row those polygons named. */
    {
        const char *s = getenv("PC_PROBE_TEX");
        if (s != NULL) {
            uint32_t ta = 0, pa = 0;
            if (sscanf(s, "%x,%x", &ta, &pa) == 2) {
                const uint8_t *t = (const uint8_t *)armrec_vram_texture((ta >> 17) & 3);
                const uint8_t *p = (const uint8_t *)armrec_vram_texpal((pa >> 14) & 7);
                int i;
                fprintf(stderr, "pc-probe2d: tex@%05X:", ta);
                for (i = 0; i < 64; i++)
                    fprintf(stderr, "%s%02X", (i % 32) ? "" : "\npc-probe2d:   ",
                            t ? t[(ta & 0x1FFFF) + i] : 0);
                fprintf(stderr, "\npc-probe2d: pal@%05X:", pa);
                for (i = 0; i < 32; i++)
                    fprintf(stderr, " %02X", p ? p[(pa & 0x3FFF) + i] : 0);
                fprintf(stderr, "\n");
            }
        }
    }
    probe_engine("A", OAM_A, PAL_A, OBJ_A, 256u * 1024u, ARMREC_EXTPAL_AOBJ);
    probe_engine("B", OAM_B, PAL_B, OBJ_B, 128u * 1024u, ARMREC_EXTPAL_BOBJ);

    /* The 3D polygon list SWAP_BUFFERS last published, one line per unique
     * texture image parameter: which format, which texture address, which
     * palette base; the question "did the data this polygon samples ever
     * load" needs exactly these numbers. */
    {
        uint32_t count = 0, i, printed = 0;
        PcGxPolygon **polys = pc_gpu3d_render_polygons(&count);
        uint32_t seen[64];
        uint32_t nseen = 0;

        fprintf(stderr, "pc-probe2d: 3D polygons: %u\n", count);
        for (i = 0; i < count && printed < 40; i++) {
            PcGxPolygon *p = polys[i];
            uint32_t j, dup = 0;
            for (j = 0; j < nseen; j++)
                if (seen[j] == p->TexParam) { dup = 1; break; }
            if (dup) continue;
            if (nseen < 64) seen[nseen++] = p->TexParam;
            printed++;
            fprintf(stderr,
                    "pc-probe2d: 3d tex: fmt=%u vram=%05X %ux%u c0t=%u "
                    "pal=%05X attr=%08X v0=(%d,%d) col=(%d,%d,%d)\n",
                    (unsigned)((p->TexParam >> 26) & 7),
                    (unsigned)((p->TexParam & 0xFFFF) << 3),
                    8u << ((p->TexParam >> 20) & 7),
                    8u << ((p->TexParam >> 23) & 7),
                    (unsigned)((p->TexParam >> 29) & 1),
                    (unsigned)(p->TexPalette << 4),
                    p->Attr,
                    p->Vertices[0] ? (int)p->Vertices[0]->FinalPosition[0] : -1,
                    p->Vertices[0] ? (int)p->Vertices[0]->FinalPosition[1] : -1,
                    p->Vertices[0] ? (int)p->Vertices[0]->FinalColor[0] : -1,
                    p->Vertices[0] ? (int)p->Vertices[0]->FinalColor[1] : -1,
                    p->Vertices[0] ? (int)p->Vertices[0]->FinalColor[2] : -1);
        }
    }
}

/*
 * PC_TRACE_BATTLE=1: one line per HP change inside a battle.
 *
 * The hook is a pc/patches diff at BtlCmd_UpdateHealthBarValue, which is the
 * one command every damage, drain, recoil and residual tick in the battle
 * engine passes through. What it buys is an oracle the differential runner
 * cannot give: the game's own arithmetic, stated per event, so a test can
 * compute what a move OUGHT to deal, Seismic Toss deals the user's level,
 * burn takes an eighth, and compare against it, rather than running the port
 * twice and calling the agreement a proof.
 *
 * `delta` is signed as the engine stores it: negative is damage, positive is
 * healing. `hp` is the value BEFORE the delta is applied, which is what makes
 * a fraction-of-current-HP rule (Super Fang, Pain Split) checkable from one
 * line.
 */
void pc_battle_trace(int battler, int move, int delta, int hp, int maxHP,
                     int species, int level)
{
    static int on = -1;
    if (on < 0) {
        const char *s = getenv("PC_TRACE_BATTLE");
        on = (s != NULL && *s != '\0' && *s != '0');
    }
    if (!on) return;
    fprintf(stderr, "pc-battle: f=%u battler=%d move=%d delta=%d hp=%d/%d "
            "species=%d level=%d\n", (unsigned)pc_probe_frame, battler, move,
            delta, hp, maxHP, species, level);
}

/*
 * The trainer AI's dispatch bounds check, called from a pc/patches hook in
 * trainer_ai.c before every AI opcode.
 *
 * ALWAYS ON, unlike every other instrument in this file, and that is the
 * decision rather than an oversight. The AI script is 91,724 bytes of bytecode
 * lifted out of the ROM's own assembled section, and the interpreter has 109
 * handlers; an index past the end calls whatever follows the table, which
 * faults somewhere with no connection to the AI at all. That is a defect nobody
 * would diagnose from the crash site, and the check that prevents it is one
 * compare against a constant per evaluated move, against a battle turn that
 * already costs thousands of instructions. Gating it behind an environment
 * variable would mean the runs that hit it are exactly the runs without it on.
 */
void pc_ai_dispatch_check(unsigned op, unsigned max, int attacker, int species)
{
    if (op < max) return;

    fprintf(stderr,
            "pokeplatinum-pc: trainer AI dispatched opcode %u with only %u "
            "handlers, for battler %d (species %d). The AI script is data "
            "lifted from the ROM; an index past the table means the script or "
            "the table is wrong, and calling it would fault somewhere with no "
            "connection to either.\n", op, max, attacker, species);
    fflush(NULL);
    abort();
}
