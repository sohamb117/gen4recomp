/*
 * 3ds/src/3ds_frame.h: the frame boundary, in one place.
 *
 * Two callers reach it and they must not have two policies: the self-test
 * loop in 3ds_main.c, and the game, whose frame ends inside OS_Halt. See
 * 3ds_frame.c for the ordering and why the guest path exits instead of
 * returning.
 */

#ifndef POKEPLATINUM_3DS_FRAME_H
#define POKEPLATINUM_3DS_FRAME_H

#include <stdint.h>

/*
 * One host frame: sample the buttons, put the two surfaces on the two LCDs,
 * swap, and wait for the console's own VBlank. Records what apt answered;
 * ask frame_running() afterwards.
 */
/*
 * `upper` is the 2D engine feeding the top panel, POWCNT1's DSEL bit, which
 * the game owns. It matters only to the path that composes a screen out of
 * tiles rather than out of a surface, and the diagnostic loop, which
 * has no engines at all, passes 0.
 */
void frame_present(const uint32_t *top, const uint32_t *bottom,
                   const char *line_top, const char *line_bottom, int upper);

/* What apt said at the last frame_present(). 1 before the first one. */
int frame_running(void);

/* Frames presented so far. */
unsigned long frame_count(void);

/*
 * The pacing self-check, one step per frame, driven by the caller's
 * loop before frame_present(). It overruns one frame on purpose and then
 * watches for the frame that would have caught the time back up. Console
 * only: there is nothing to measure on a build machine.
 */
void pace_step(void);
int pace_ran(void);
int pace_bad(void);
int pace_first_failure(void);

/* Mean interval between presents, in ARM11 system ticks, over the window the
 * check measured. 0 until it has one. */
unsigned long pace_mean_ticks(void);

/* One display refresh in those ticks, 59.8261 Hz, the DS's rate and the
 * 3DS's. */
#define PACE_FRAME_TICKS 4481519ul

/*
 * The PC frontend's per-frame hook. pc_video.c calls it through a weak
 * extern at the end of pc_video_frame_end(), which OS_Halt calls; pc_view.c
 * answers it on PC by publishing into shared memory for a viewer process.
 * Here it is the game's frame boundary. Nothing in 3ds/src calls it.
 */
void pc_view_publish(uint64_t frame);

#endif /* POKEPLATINUM_3DS_FRAME_H */
