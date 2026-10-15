/*
 * NP_RULE_FIX_BUGS on Black/White: the documented Generation V cartridge bug
 * that sits at one decision point in the battle engine.
 *
 *   0 damage    Bulbapedia's "0 damage glitch" (List of battle glitches in
 *               Generation V): the damage calculation raises a 0 to 1 before
 *               the last ("other") modifier, so a 1 that modifier lowers
 *               (a resist Berry, Multiscale, Filter...) ends as 0 damage.
 *               Overlay 93's damage routine (Black ov93_021C1E74, White
 *               ov93_021C1E94): base damage (ov93_021D7A44), spread, weather,
 *               critical, random, STAB, type, burn, then `cmp r4, #0; bne;
 *               mov r4, #1` (0x021C1FEC), then the handler event 0x47 sets
 *               the other modifier (work 0x35, 4096 = x1, kept 0x29..0x20000)
 *               and the result is damage x other through the fx multiply
 *               (Black ov93_021D7B10, White ov93_021D7B30: (a * b) >> 12,
 *               up only when the remainder is above one half). That last
 *               call goes to PcBw_DamageOther
 *               (pc/patches/<VER>/arm9/overlays/93/asm/ndsrec_ov093_002.s.patch),
 *               which with the bit on keeps a non-zero result at 1, as the
 *               game's own ov93_021D7B2C does and as later generations do.
 *               The other modifier is never 0, so only the glitch's case
 *               changes.
 *
 * With the bit off the hook returns the cartridge's own result. The other
 * documented B/W battle bugs (Sky Drop, Choice lock under Klutz/Embargo/
 * Magic Room, confusion and pinch items, Trick Room's speed wrap, Shed Shell)
 * are state across turns or handlers, not one decision point, and are left
 * as the cartridge has them.
 *
 * PC_NP_RULES_CHECK: on the first battle frame with overlay 93 resident, the
 * hook runs on the real fx multiply with the bit off and on (1 x 1/2, the
 * glitch; 2 x 1/2 and 1 x 1, unaffected) and logs the verdicts.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "armrec_rt.h"
#include "pc_np_options.h"

#if defined(PC_BW_VER_WHITE)
#define BW_FX_MUL ov93_021D7B30
#else
#define BW_FX_MUL ov93_021D7B10
#endif

extern uint64_t BW_FX_MUL(uint32_t a, uint32_t b, uint32_t unused2, uint32_t unused3);

#define FX_HALF 0x800u
#define FX_ONE 0x1000u

static int rules_on(void)
{
    return (pc_np_opt.rules & PC_NP_RULE_FIX_BUGS) != 0;
}

uint32_t PcBw_DamageOther(uint32_t damage, uint32_t other)
{
    uint32_t r = (uint32_t)ARMREC_CALL(BW_FX_MUL, damage, other, 0, 0);

    if (r == 0 && damage != 0 && rules_on()) r = 1;
    return r;
}

static void rules_check(void)
{
    const unsigned saved = pc_np_opt.rules;
    uint32_t glitch[2], twoHalf[2], oneOne[2];
    unsigned bit;
    int pass;

    for (bit = 0; bit < 2; bit++) {
        pc_np_opt.rules = bit ? (saved | PC_NP_RULE_FIX_BUGS) : (saved & ~PC_NP_RULE_FIX_BUGS);
        glitch[bit] = PcBw_DamageOther(1, FX_HALF);
        twoHalf[bit] = PcBw_DamageOther(2, FX_HALF);
        oneOne[bit] = PcBw_DamageOther(1, FX_ONE);
    }
    pc_np_opt.rules = saved;
    fprintf(stderr, "pc-np: rules check: 1 damage x other 1/2: off %u, on %u\n", (unsigned)glitch[0],
            (unsigned)glitch[1]);
    fprintf(stderr, "pc-np: rules check: 2 x 1/2: off %u, on %u; 1 x 1: off %u, on %u\n", (unsigned)twoHalf[0],
            (unsigned)twoHalf[1], (unsigned)oneOne[0], (unsigned)oneOne[1]);
    pass = glitch[0] == 0 && glitch[1] == 1 && twoHalf[0] == 1 && twoHalf[1] == 1 && oneOne[0] == 1 && oneOne[1] == 1;
    fprintf(stderr, "pc-np: rules check: %s\n", pass ? "PASS (bug with the bit off, fixed with it on)" : "FAIL");
}

void pc_bw_rules_frame(int in_battle)
{
    static int done;

    if (done || !in_battle || !armrec_overlay_resident(93)) return;
    done = 1;
    if (getenv("PC_NP_RULES_CHECK") != NULL) rules_check();
}
