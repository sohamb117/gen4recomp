/*
 * The ARM7 BIOS sound tables, pokediamond's, carried whole for 4.1.
 *
 * Its own translation unit rather than part of the pokediamond port's SWI layer (here: pc_arm7snd.c's SVC shims), which is
 * where the SWIs that reach it live: these are data, they need nothing else in
 * the port, and pc/tests/test_stkargs.c links this file alone to check them
 * against a second implementation of the ARM7 BIOS. A table only a SWI number
 * can reach is a table no test can hold to anything.
 */
#include <math.h>
#include <stdint.h>

#define PC_SND_PITCH_ENTRIES  768
#define PC_SND_VOLUME_ENTRIES 724

uint32_t pc_snd_pitch_table(int index);
uint32_t pc_snd_volume_table(int index);

/*
 * The pitch table is exact and it is a statement rather than a fit.
 * round(65536 * 2^(i/768)) - 65536, one entry per 1/768th of an octave, which
 * is what SND_CalcTimer's own octave normalisation says the index means. All
 * 768 entries reproduce the only second implementation of the ARM7 BIOS on this
 * machine, and floor() instead of round() disagrees on 379 of them, so the
 * rounding rule is pinned rather than plausible. pc/tests/test_stkargs.c is
 * where both claims are checked.
 *
 * Built once and kept, so a run cannot depend on how many times the game asked:
 * The entries are a function of the index and of nothing else, which is what
 * keeps this a declared input rather than host state reaching the guest
 *'s rule, applied to a table).
 */
uint32_t pc_snd_pitch_table(int index) {
    static uint16_t table[PC_SND_PITCH_ENTRIES];
    static int built;

    if (index < 0 || index >= PC_SND_PITCH_ENTRIES) return 0;
    if (!built) {
        int i;
        for (i = 0; i < PC_SND_PITCH_ENTRIES; i++) {
            double v = 65536.0 * pow(2.0, (double)i / (double)PC_SND_PITCH_ENTRIES);
            table[i] = (uint16_t)((uint32_t)(v + 0.5) - 65536u);
        }
        built = 1;
    }
    return table[index];
}

/*
 * The volume table is not exact, and that is measured rather than hoped.
 * Read this before changing anything here.
 *
 * The structure is this repository's own. arm7/lib/src/SND_util.c's
 * SND_CalcChannelVolume splits at value < -240, < -120 and < -60 and returns
 * `table | (shift << 8)`, so the index (value - SND_VOL_DB_MIN) breaks at 483,
 * 603 and 663, and the table in the second implementation drops at exactly
 * 483, 603 and 663 and nowhere else. Four segments, each ending at 127, each
 * paired with one more halving. That is the tree and the BIOS agreeing about
 * the shape, from two directions, with nothing fitted.
 *
 * The *rate* is first principles: one index step is 0.1 dB, so an entry is
 * 127 * 10^(-0.1 * steps-from-the-segment-end / 20). At that rate 604 of the
 * 724 entries are exactly right and **every one of the other 120 is off by
 * exactly one step out of 127**, 0.07 dB, and no entry anywhere is off by
 * two. Nine closed forms and every offset were tried; none reproduces the table
 * exactly, because it is dumped data with its own rounding.
 *
 * So why not trap, which is this project's usual answer to no oracle. Because
 * the trap is not free here and MIi_UncompressBackward's is: this is on the
 * live boot path from the first tick of SndThread, so trapping does not defer a
 * wrong answer, it deletes the port. The trade taken is decision 42's and 45's,
 * where two implementations disagree, measure the disagreement and keep it
 * visible rather than pretending, and pc/tests/test_stkargs.c is what keeps
 * it visible: it fails if the exact count falls below 604 or if any entry ever
 * differs by more than one.
 */
static const int pc_snd_vol_segment_end[] = { 482, 602, 662, 723 };

uint32_t pc_snd_volume_table(int index) {
    static uint8_t table[PC_SND_VOLUME_ENTRIES];
    static int built;

    if (index < 0 || index >= PC_SND_VOLUME_ENTRIES) return 0;
    if (!built) {
        int i, s = 0;
        for (i = 0; i < PC_SND_VOLUME_ENTRIES; i++) {
            double v;
            while (i > pc_snd_vol_segment_end[s]) s++;
            v = 127.0 * pow(10.0, (double)(i - pc_snd_vol_segment_end[s]) / 200.0);
            table[i] = (uint8_t)(uint32_t)(v + 0.5);
        }
        built = 1;
    }
    return table[index];
}

