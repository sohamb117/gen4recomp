/*
 * 3ds/src/3ds_audio.h: the mixer's samples on the console's DSP.
 *
 * pc/src/pc_audio.c produces stereo s16 at the DS SPU's own rate and holds it
 * in a ring; this is the consumer that hands it to NDSP. See the .c for why
 * the hand-off happens on the frame thread and not on NDSP's.
 */

#ifndef POKEPLATINUM_3DS_AUDIO_H
#define POKEPLATINUM_3DS_AUDIO_H

/*
 * Bring the DSP up and, if the port's mixer is in this link, register the
 * sink it calls when it has produced samples. 0 on success; -1 when the
 * console has no DSP firmware to load, which is not fatal; the game runs
 * silent and says so.
 */
int audio_init(void);

/* Stop the channel and release the DSP. Safe with no init. */
void audio_exit(void);

/*
 * The system is taking the foreground. Nothing produces samples while
 * the guest is frozen, so what is already queued, up to about 67 ms of the
 * game's music, would otherwise play out on top of the Home Menu. Both are
 * safe with no init and safe to repeat.
 */
void audio_suspend(void);
void audio_resume(void);

/* Whether the DSP came up. */
int audio_ready(void);

/* Sample frames handed to the DSP, and wave buffers queued. */
unsigned long audio_samples(void);
unsigned long audio_buffers(void);

/*
 * Frames where the sink had samples and no free wave buffer to put them in.
 * They stay in the ring; pc_audio_overruns() is what counts the ones the ring
 * then dropped.
 */
unsigned long audio_stalls(void);

/*
 * How long the DSP took to play a buffer of a known length, in console
 * frames, measured at start-up in the binary that has no game to make sound.
 * 0 when the probe did not run.
 */
int audio_probe_frames(void);

/*
 * The console-side check: the DSP is up, the probe played its buffer, and it
 * took the number of frames the buffer's length says it should.
 */
int audio_selftest(int *ranOut);

#endif /* POKEPLATINUM_3DS_AUDIO_H */
