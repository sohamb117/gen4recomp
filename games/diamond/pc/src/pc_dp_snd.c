/*
 * Which sound-archive player drives each ARM7 driver player, for
 * Diamond/Pearl: the lookup pc_np_options.c's music / effects volume asks
 * every frame (pc_np_seq_player_group, weak there).
 *
 * Platinum answers it from NitroSystem's own player.c (a patch adds the
 * function beside sSeqPlayer[]). D's NitroSystem is assembly
 * (arm9/asm/NNS_SND_player.s, recompiled), so the same two arrays are read
 * here at their guest addresses: the bridge turns the asm data labels
 * sSeqPlayer and sPlayer into those addresses for whichever version is
 * being built. Their layout is NitroSystem 071126's, which the ROM link
 * confirms: sSeqPlayer spans 0x440 bytes (16 x 68) and sPlayer 0x480
 * (32 x 36) in both xMAPs, the sizes asserted below.
 *
 * D's SDAT names its players as Platinum's does (0 PV, 1 FIELD, 2 ME,
 * 3..6 SE_1..4, 7 BGM; files/data/sound/sound_data.sdat's SYMB block), so
 * pc_np_options.c's music/effects split applies unchanged.
 */
#include <nitro.h>
#include <nnsys/snd/player.h>

_Static_assert(sizeof(NNSSndSeqPlayer) == 68, "sSeqPlayer stride (ROM: 0x440 / 16)");
_Static_assert(sizeof(NNSSndPlayer) == 36, "sPlayer stride (ROM: 0x480 / 32)");

extern NNSSndSeqPlayer sSeqPlayer[SND_PLAYER_NUM];
extern NNSSndPlayer sPlayer[NNS_SND_PLAYER_NUM];

int pc_np_seq_player_group(int i)
{
    if (i < 0 || i >= SND_PLAYER_NUM || sSeqPlayer[i].player == NULL) {
        return -1;
    }
    return (int)(sSeqPlayer[i].player - sPlayer);
}
