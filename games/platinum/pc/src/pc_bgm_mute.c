/*
 * PC_MUTE_BGM=1, play sound effects, silence the music.
 *
 * The user's idea, and it works because this decomp names the two halves
 * apart. Every sequence plays on a numbered player (generated/sdat.h):
 * PLAYER_FIELD (1) and PLAYER_BGM (7) are the music; PLAYER_ME (2) is the
 * short fanfares; PLAYER_SE_1..4 (3..6) are the sound effects. The game's
 * own Sound_SetBGMPlayerPaused() pauses exactly the two music players (it
 * ignores any other id), so forcing both paused every frame stops their
 * sequencer advancing (no BGM notes, so no BGM keyons) while the SE
 * players are never touched.
 *
 * It is a player feature and a debugging instrument at once: with the music
 * gone, the SPU keyon trace (PC_SPU_TRACE) shows only sound effects, which
 * is what a "the hit plays twice" hunt needs to see through a battle theme
 * that legitimately retriggers a dozen channels a second.
 *
 * Called once per frame from the main-thread frame boundary (pc_os_lite.c),
 * the same context the game's own sound calls run in, so there is no race.
 * Idempotent, and safe before the sound system exists: Sound_SetBGMPlayerPaused
 * works through NNS sound handles that are inert until a sequence is bound,
 * so a pause on an idle handle does nothing. Forcing it EVERY frame is
 * deliberate, a fresh Sound_PlayBGM re-arms its player, and re-pausing the
 * next frame costs that start at most a single frame of sound.
 */
#include <nitro/types.h>

/* generated/sdat.h's player ids and sound_system.h's handle ids, stated
 * here so this file needs no game header. */
#define PC_PLAYER_FIELD          1
#define PC_PLAYER_BGM            7
#define PC_HANDLE_FIELD_BGM      0
#define PC_HANDLE_BGM            7

extern void Sound_SetBGMPlayerPaused(u8 playerID, int paused);
/* Sound_FadeVolumeForHandle(handleType, targetVolume, frames); frames 0 is
 * immediate. Zeroing the player's volume silences channels ALREADY ringing:
 * A looping BGM instrument keeps sounding through a sequencer pause, so
 * pause alone left ~a quarter of the music audible. */
extern void Sound_FadeVolumeForHandle(int handleType, int targetVolume, int frames);

static int mute = -1;

void pc_bgm_mute_apply(void)
{
    if (mute < 0) {
        extern char *getenv(const char *);
        const char *e = getenv("PC_MUTE_BGM");

        mute = (e != NULL && e[0] == '1');
    }
    if (mute) {
        Sound_SetBGMPlayerPaused(PC_PLAYER_FIELD, 1);
        Sound_SetBGMPlayerPaused(PC_PLAYER_BGM, 1);
        Sound_FadeVolumeForHandle(PC_HANDLE_FIELD_BGM, 0, 0);
        Sound_FadeVolumeForHandle(PC_HANDLE_BGM, 0, 0);
    }
}
