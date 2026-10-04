/*
 * NP_RULE_FIX_BUGS on Diamond/Pearl: the cartridge bugs Platinum's
 * pc/patches fix behind the bit (Platinum's docs/bugs_and_glitches.md),
 * where D/P has them.
 *
 *   Fire Fang     MoveIsOnDamagingTurn, ov11_0224C6D4 (arm9/overlays/11/
 *                 asm/ov11_02242B78.s), lists the effects that only hit on
 *                 the last turn of a multi-turn move, for the Wonder Guard
 *                 checks. D/P's list is Platinum's, including Fire Fang's
 *                 effect 0x111 where Shadow Force's 0x110 belongs, so Fire
 *                 Fang always passes Wonder Guard and Shadow Force's charge
 *                 turn is blocked by it. Both calls (ov11_02242B78.s) go to
 *                 PcDp_MoveIsOnDamagingTurn through
 *                 pc/patches/arm9/overlays/11/asm/ov11_02242B78.s.patch.
 *   Rage          the pre-move pass (ov11_0223D1DC.s, Platinum's
 *                 BattleControllerPlayer_CheckPreMoveActions): a raging
 *                 battler that picked another move gets
 *                 `statusVolatile &= VOLATILE_CONDITION_RAGE`, every other
 *                 volatile cleared and rage kept. The `mov r0, #2; lsl r0,
 *                 r0, #0x16; and r1, r0` there is replaced, size for size,
 *                 by `bl PcDp_RageAfterOtherMove; nop`
 *                 (pc/patches/arm9/overlays/11/asm/ov11_0223D1DC.s.patch).
 *   form stats    not in D/P: their trainer parties have no form field
 *                 (arm9/src/trainer_data.c CreateNPCTrainerParty creates
 *                 every mon from its species alone), so there is no form
 *                 whose stats could be wrong.
 *
 * With the bit off both hooks do exactly what the cartridge does (the first
 * calls the original routine); the bit only ever changes the two outcomes
 * the documented fixes change.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"

#include "constants/moves.h"
#include "move_data.h"

#include "pc_np_options.h"

extern BOOL ov11_0224C6D4(void *battleCtx, int move); /* MoveIsOnDamagingTurn */

/* BattleContext offsets ov11_0224C6D4 reads: the move table (struct
 * WazaTbl[], effect first) and battleStatusMask. */
#define CTX_MOVE_TABLE    0x3DE
#define CTX_STATUS_MASK   0x213C
#define SYSCTL_LAST_OF_MULTI_TURN 0x200
#define VOLATILE_CONDITION_RAGE   (1u << 23)

static int rules_on(void) {
    return (pc_np_opt.rules & PC_NP_RULE_FIX_BUGS) != 0;
}

BOOL PcDp_MoveIsOnDamagingTurn(void *battleCtx, int move) {
    u8 *ctx = battleCtx;
    u16 effect;

    if (!rules_on()) {
        return ov11_0224C6D4(battleCtx, move);
    }
    effect = *(u16 *)(ctx + CTX_MOVE_TABLE + move * (int)sizeof(struct WazaTbl));
    switch (effect) {
    case 0x01A: /* BIDE */
    case 0x027: /* CHARGE_TURN_HIGH_CRIT */
    case 0x04B: /* CHARGE_TURN_HIGH_CRIT_FLINCH */
    case 0x091: /* CHARGE_TURN_DEF_UP */
    case 0x097: /* SKIP_CHARGE_TURN_IN_SUN */
    case 0x09B: /* FLY */
    case 0x0FF: /* DIVE */
    case 0x100: /* DIG */
    case 0x107: /* BOUNCE */
    case 0x110: /* SHADOW_FORCE (the fix; the cartridge has 0x111 FLINCH_BURN_HIT, Fire Fang) */
        return (*(u32 *)(ctx + CTX_STATUS_MASK) & SYSCTL_LAST_OF_MULTI_TURN) != 0;
    }
    return TRUE;
}

/* r1 is the battler's statusVolatile; r0 comes back as the 0x800000 the
 * replaced instructions left in it, r1 as the new status. */
u64 PcDp_RageAfterOtherMove(u32 r0, u32 statusVolatile) {
    const u32 status = rules_on() ? statusVolatile & ~VOLATILE_CONDITION_RAGE
                                  : statusVolatile & VOLATILE_CONDITION_RAGE;

    (void)r0;
    return (u64)VOLATILE_CONDITION_RAGE | ((u64)status << 32);
}

/*
 * PC_NP_RULES_CHECK: both fixes run with the bit off and on, the cartridge's
 * own ov11_0224C6D4 (bit off) beside the fix, on the real move table
 * (LoadAllWazaTbl) in a zeroed BattleContext-sized buffer, and the verdicts
 * logged. No field state is needed, so it runs on the first frame.
 */
static void pc_dp_rules_check(void) {
    const unsigned saved = pc_np_opt.rules;
    const size_t ctxSize = CTX_STATUS_MASK + 4 > CTX_MOVE_TABLE + (NUM_MOVES + 1) * sizeof(struct WazaTbl)
                               ? CTX_STATUS_MASK + 4
                               : CTX_MOVE_TABLE + (NUM_MOVES + 1) * sizeof(struct WazaTbl);
    u8 *ctx = calloc(1, ctxSize);
    int ff[2], sf[2], pass;
    u32 rage[2];
    unsigned bit;

    if (ctx == NULL) {
        fprintf(stderr, "pc-np: rules check: out of memory\n");
        return;
    }
    LoadAllWazaTbl((struct WazaTbl *)(ctx + CTX_MOVE_TABLE));
    for (bit = 0; bit < 2; bit++) {
        pc_np_opt.rules = bit ? (saved | PC_NP_RULE_FIX_BUGS) : (saved & ~PC_NP_RULE_FIX_BUGS);
        *(u32 *)(ctx + CTX_STATUS_MASK) = 0; /* not the last turn */
        /* Wonder Guard is applied only on a damaging turn: off it, a not
         * very effective move gets through. So "blocked" = on one. */
        ff[bit] = PcDp_MoveIsOnDamagingTurn(ctx, MOVE_FIRE_FANG) != 0;
        sf[bit] = PcDp_MoveIsOnDamagingTurn(ctx, MOVE_SHADOW_FORCE) != 0;
        rage[bit] = (u32)(PcDp_RageAfterOtherMove(0, VOLATILE_CONDITION_RAGE | 1) >> 32);
    }
    pc_np_opt.rules = saved;
    free(ctx);

    fprintf(stderr, "pc-np: rules check: Fire Fang into Wonder Guard blocked: off %d, on %d\n", ff[0], ff[1]);
    fprintf(stderr, "pc-np: rules check: Shadow Force charge turn blocked by Wonder Guard: off %d, on %d\n", sf[0],
            sf[1]);
    fprintf(stderr, "pc-np: rules check: raging + confused battler after another move, volatile: off %#lx, on %#lx\n",
            (unsigned long)rage[0], (unsigned long)rage[1]);
    fprintf(stderr, "pc-np: rules check: trainer form stats: not applicable (D/P trainer parties carry no form)\n");
    pass = ff[0] == 0 && ff[1] == 1 && sf[0] == 1 && sf[1] == 0 && rage[0] == VOLATILE_CONDITION_RAGE && rage[1] == 1;
    fprintf(stderr, "pc-np: rules check: %s\n", pass ? "PASS (bugs with the bit off, fixed with it on)" : "FAIL");
}

void pc_dp_rules_frame(void) {
    static int done;

    if (done) return;
    done = 1;
    if (getenv("PC_NP_RULES_CHECK") != NULL) pc_dp_rules_check();
}
