/*
 * Which sound-archive player drives each ARM7 driver player, for
 * Black/White: the lookup pc_np_options.c's music / effects volume asks
 * every frame (pc_np_seq_player_group, weak there), in place of D/P's
 * games/diamond/pc/src/pc_dp_snd.c (pc/Makefile.wasm links this instead).
 *
 * The two NitroSystem arrays are read the same way as on D/P: sSeqPlayer
 * and sPlayer are `data ref` primitives (tools/ndsrec/primitives.txt), placed
 * in the ROM by signature (Black 0x0214E024 / 0x0214E464, 0x440 = 16 x 68
 * bytes apart, D's stride). What differs is the sound archive. B/W's SDAT
 * (Black and White alike) names its players 0 PLAYER_BGM, 1 PLAYER_SE_SYS
 * (cries too), 2 SE_1, 3 SE_2, 4 SE_PSG, 5 SE_3, 6 PLAYER_BGM2 (the title
 * music), where pc_np_options.c classes Platinum's: 1 FIELD, 2 ME and
 * 7 BGM music, the rest effects. So B/W's music players answer 7 and every
 * other player 3 (an SE player), and the shared code stays Platinum's.
 */
#include <nitro.h>
#include <nnsys/snd/player.h>

_Static_assert(sizeof(NNSSndSeqPlayer) == 68, "sSeqPlayer stride (ROM: 0x440 / 16)");
_Static_assert(sizeof(NNSSndPlayer) == 36, "sPlayer stride");

extern NNSSndSeqPlayer sSeqPlayer[SND_PLAYER_NUM];
extern NNSSndPlayer sPlayer[NNS_SND_PLAYER_NUM];

enum { PLATINUM_PLAYER_BGM = 7, PLATINUM_PLAYER_SE = 3 };
enum { BW_PLAYER_BGM = 0, BW_PLAYER_BGM2 = 6 };

int pc_np_seq_player_group(int i)
{
    int player;

    if (i < 0 || i >= SND_PLAYER_NUM || sSeqPlayer[i].player == NULL) {
        return -1;
    }
    player = (int)(sSeqPlayer[i].player - sPlayer);
    return player == BW_PLAYER_BGM || player == BW_PLAYER_BGM2 ? PLATINUM_PLAYER_BGM : PLATINUM_PLAYER_SE;
}
