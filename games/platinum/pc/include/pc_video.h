/*
 * The port's output surface, and the frame dumps that read it.
 *
 * This is not a 2D engine. It is the surface a 2D engine writes and that
 * everything downstream reads, plus one consumer of it: `--dump-frames DIR`
 * writes a PNG per frame. Headless came first and SDL was added as a second
 * consumer of the same surface, because building SDL first and retrofitting
 * headless later is a refactor of the whole output path.
 *
 * Which screen is on top is the hardware's rule, not a convention here.
 * POWCNT1's DSEL bit selects which 2D engine drives the upper LCD, and the
 * SDK's GX_Init is what sets it. pc_video_upper_engine() reads that bit, so a
 * dump follows the game rather than an assumption, and pc/tests/test_video.c
 * drives it both ways.
 *
 * Dumping never changes what the port computes. The renderer runs at every
 * frame boundary whether or not anything is consuming its output, so
 * `--dump-frames` only ever writes files. That is what lets test_video require
 * an identical --state-digest with and without it. `--frames N` is the
 * exception and is meant to be: it ends the run.
 */

#ifndef POKEDIAMOND_PC_VIDEO_H
#define POKEDIAMOND_PC_VIDEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One DS screen. Both engines produce this and the dump stacks them. */
#define PC_VIDEO_WIDTH   256
#define PC_VIDEO_HEIGHT  192
#define PC_VIDEO_PIXELS  (PC_VIDEO_WIDTH * PC_VIDEO_HEIGHT)

/* Which 2D engine, not which screen; see pc_video_upper_engine(). */
enum pc_video_engine {
    PC_VIDEO_MAIN = 0,      /* engine A, reg_GX_DISPCNT at 0x04000000 */
    PC_VIDEO_SUB  = 1       /* engine B, reg_GXS_DB_DISPCNT at 0x04001000 */
};

/*
 * An engine's surface: PC_VIDEO_PIXELS words, row-major, 0x00RRGGBB.
 *
 * Host memory rather than guest memory on purpose. A DS framebuffer is not a
 * thing that exists in the guest address space; the picture is composed on
 * the fly from VRAM, OAM and the registers, so putting one at a guest address
 * would invent a memory the hardware does not have and pollute the guest
 * digest rests on.
 */
uint32_t *pc_video_surface(int engine);

/*
 * The renderer installs the 2D engine here. It is called once per frame with the
 * frame number, before the frame is consumed, and it must be a pure function of
 * guest state: it runs whether or not anyone is looking, so a renderer with a
 * side effect on guest memory would make --dump-frames change the run.
 */
typedef void (*pc_video_renderer_fn)(uint64_t frame);
void pc_video_set_renderer(pc_video_renderer_fn fn, const char *name);

/* NULL until the renderer installs one. */
const char *pc_video_renderer_name(void);

/*
 * PC_VIDEO_MAIN or PC_VIDEO_SUB, whichever engine POWCNT1's DSEL bit sends to
 * the upper screen. The dump puts that one in the top half of the PNG.
 */
int pc_video_upper_engine(void);

/*
 * --dump-frames DIR. Creates the directory if it is not there and opens
 * DIR/frames.txt, writing its header immediately: a run that reaches no frame
 * at all still leaves a manifest saying so, which is how a caller tells "the
 * flags worked and the boot never got there" from "the flags did nothing".
 * Returns 1 on success, 0 having printed why.
 */
int pc_video_open_dump(const char *dir);

/* --frames N: end the run once N frames have been displayed. 0 means no
 * limit, which is the default. */
void pc_video_set_frame_limit(uint64_t frames);

/* --headless. There is no window to suppress yet, so this records the mode the
 * dump was taken in rather than selecting it; the SDL frontend's SDL frontend is what
 * will have to honour it. */
void pc_video_set_headless(int headless);
int  pc_video_headless(void);

/*
 * A frame has finished scanning out. Called from pc/src/pc_irq.c at the VBlank
 * boundary, which is the one place that knows a frame has passed; `frame` is
 * the same number --input counts in, so frame N is the Nth VBlank.
 *
 * Runs the renderer, writes the PNG and the manifest line if a dump is open,
 * and ends the run if --frames says this was the last one.
 */
void pc_video_frame_end(uint64_t frame);

/* Frames written to the dump so far. */
uint64_t pc_video_frames_written(void);

/*
 * Write the two surfaces to `path` as a 256x384 8-bit RGB PNG, upper screen on
 * top. Returns 1 on success. Exposed so a test can check the encoder against an
 * independent decoder without going near the frame loop.
 */
int pc_video_write_png(const char *path);

/* Close the manifest with its summary line. Runs at exit; idempotent. */
void pc_video_close_dump(void);

/* Forget the renderer, the dump, the limit and the surfaces. For tests;
 * nothing in the port calls it. */
void pc_video_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* POKEDIAMOND_PC_VIDEO_H */
