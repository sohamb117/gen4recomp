/*
 * The runtime's player options, host side (pc/include/pc_np_options.h).
 *
 * Nothing here knows the game: the values, their range checks, and the one
 * option that has to be pushed into another component every frame, the
 * music/effects volume. The ARM7 sound driver applies it per driver player
 * (pc/arm7snd/src/SND_seq.c, TrackUpdateChannel, as SND_HostPlayerDecay;
 * the driver's symbols carry the arm7_ prefix on this side of the namespace
 * wall, see pc/Makefile's SND7NS), but a driver player is allocated per
 * sequence, so which sound-archive player (music or effects) each one is
 * playing for is asked of NitroSystem every frame
 * (pc_np_seq_player_group, pc/patches/.../snd/src/player.c.patch).
 */
#include "pc_np_options.h"

#include <math.h>

pc_np_options pc_np_opt = {
    .bgm_volume = 256,
    .se_volume = 256,
    .render_scale = 1,
    .widescreen = 0,
    .camera_zoom = 256,
    .camera_tilt = 0,
    .quicksave_seq = 0,
    .rules = 0,
    .text_instant = 0,
};

pc_np_status pc_np_stat;

/* SND_PLAYER_NUM driver players; the decay is added to the sum of the
 * track's, expression's and player's SNDi_DecibelSquareTable entries,
 * which are tenths of a decibel (0 = full, -723 = the table's floor). */
#define PC_NP_SND_PLAYERS 16
extern short arm7_SND_HostPlayerDecay[PC_NP_SND_PLAYERS];
extern int pc_np_seq_player_group(int driverPlayer) __attribute__((weak));

/* Platinum's SDAT players (res/sound/pl_sound_data.naix): 0 PV (cries),
 * 1 FIELD and 7 BGM (music), 2 ME (fanfares, music too), 3..6 SE. Players
 * the archive does not define are effects. */
static int player_is_music(int player)
{
    return player == 1 || player == 2 || player == 7;
}

/* 0..256 -> tenths of a dB; 256 is exactly 0 so the default adds nothing. */
static short volume_decay(unsigned v)
{
    if (v >= 256) return 0;
    if (v == 0) return -0x7FFF;   /* the driver clamps to silence */
    return (short)lrint(200.0 * log10((double)v / 256.0));
}

void pc_np_options_set(const pc_np_options *o)
{
    static int decay_live;
    pc_np_options n = *o;
    int i;

    if (n.bgm_volume > 256) n.bgm_volume = 256;
    if (n.se_volume > 256) n.se_volume = 256;
    if (n.render_scale < 1) n.render_scale = 1;
    if (n.render_scale > 4) n.render_scale = 4;
    n.widescreen = n.widescreen != 0;
    if (n.camera_zoom < 64) n.camera_zoom = 64;       /* 1/4 .. */
    if (n.camera_zoom > 1024) n.camera_zoom = 1024;   /* .. 4x the distance */
    if (n.camera_tilt < -45 * 16) n.camera_tilt = -45 * 16;
    if (n.camera_tilt > 45 * 16) n.camera_tilt = 45 * 16;
    n.text_instant = n.text_instant != 0;
    pc_np_opt = n;

    /* At full volume nothing is written, so the driver mixes exactly as it
     * always has; once a volume has moved, every frame re-reads which
     * sound-archive player each driver player is serving. */
    if (n.bgm_volume == 256 && n.se_volume == 256 && !decay_live) return;
    {
        const short music = volume_decay(n.bgm_volume), effects = volume_decay(n.se_volume);

        for (i = 0; i < PC_NP_SND_PLAYERS; i++) {
            const int group = pc_np_seq_player_group != 0 ? pc_np_seq_player_group(i) : i;

            arm7_SND_HostPlayerDecay[i] = group >= 0 && player_is_music(group) ? music : effects;
        }
        decay_live = music != 0 || effects != 0;
    }
}
