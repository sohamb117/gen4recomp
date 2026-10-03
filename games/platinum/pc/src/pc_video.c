/*
 * The output surface and the frame dumps, ported from the sibling diamond
 * port and trimmed to what this tree has.
 *
 * The PNG encoder moved to pc/src/pc_png.h when the viewer needed to write
 * screenshots with it. It is the same code, so the frame dumps this file
 * produces are the bytes they always were; what stayed is the part that is
 * about DS surfaces rather than about PNG, the fixed dump size, the two
 * static buffers, and turning two engines into scanlines. The encoder is
 * checked against an independent decoder (Python's zlib) in the test runner.
 *
 * What is trimmed from the diamond original. The differential-trace hook
 * (pc_diff_framebuffer); there is no melonDS oracle harness in this tree
 * yet. The --input/--state-digest plumbing, no CLI layer exists; the dump is
 * driven by PC_DUMP_FRAMES / PC_FRAMES environment variables from pc_main.c.
 * pc_view_publish survives as a weak extern so a window layer can hook the
 * same seam diamond's does.
 */

#include "pc_png.h"
#include "pc_bench.h"

extern void pc_polydump_frame(uint64_t frame);
#include "pc_gpu3d.h"
#include "pc_video.h"
#include "pc_state.h"
#include "pc_sym.h"

#include "armrec_rt.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
static int pc_mkdir(const char *d) { return _mkdir(d); }
#else
static int pc_mkdir(const char *d) { return mkdir(d, 0777); }
#endif

/*
 * The window layer's publish hook, weak so this file links before any
 * window layer exists. pc_video_frame_end calls it when present; a viewer
 * that is slow, stopped or absent must cost the port nothing.
 */
extern void pc_view_publish(uint64_t frame) __attribute__((weak));
extern void pc_probe2d(uint64_t frame);

/* ------------------------------------------------------------------ */
/* The surfaces                                                       */
/* ------------------------------------------------------------------ */

static uint32_t pc_surface[2][PC_VIDEO_PIXELS];

static pc_video_renderer_fn pc_renderer;
static const char *pc_renderer_label;

uint32_t *pc_video_surface(int engine) {
    return pc_surface[engine == PC_VIDEO_SUB ? 1 : 0];
}

void pc_video_set_renderer(pc_video_renderer_fn fn, const char *name) {
    pc_renderer = fn;
    pc_renderer_label = fn ? (name ? name : "unnamed") : NULL;
}

const char *pc_video_renderer_name(void) {
    return pc_renderer_label;
}

/*
 * POWCNT1 bit 15 is the display swap: set means engine A drives the upper
 * LCD, which is what GX_Init establishes on its first line. Guarded on
 * armrec_mem_ready because a test that calls the encoder before
 * armrec_mem_init() would fault, not read.
 *
 * A guest address is a host address only on the PC port. On the 3DS the I/O
 * page is a row of the guest slab, so these three go through its base the way
 * pc/hw/pc_gpu2d.c's accessors do, found by running the game there, where
 * the first frame read guest 0x04000304 straight through and the emulator
 * reported an unmapped access every frame.
 */
#if defined(__3DS__)
extern unsigned char *armrec_io_base;
#define PC_IOREG(a, t)     (*(volatile t *)(armrec_io_base + ((a) - 0x04000000u)))
#else
#define PC_IOREG(a, t)     (*(volatile t *)(uintptr_t)(a))
#endif

#define PC_REG_POWCNT      PC_IOREG(0x04000304u, uint16_t)
#define PC_REG_DISPCNT_A   PC_IOREG(0x04000000u, uint32_t)
#define PC_REG_DISPCNT_B   PC_IOREG(0x04001000u, uint32_t)
#define PC_POWCNT_DSEL     0x8000u

int pc_video_upper_engine(void) {
    if (!armrec_mem_ready) return PC_VIDEO_SUB;
    return (PC_REG_POWCNT & PC_POWCNT_DSEL) ? PC_VIDEO_MAIN : PC_VIDEO_SUB;
}

/* FNV-1a over the surfaces, for the manifest's digest column. One
 * implementation in the port (pc_state.c), so that a digest in a frame manifest
 * and a digest from --state-digest are the same arithmetic and mean the same
 * thing. The constants were already identical, so no manifest value moved when
 * this stopped being a second copy. */
static uint64_t fnv1a(const void *data, uint32_t len) {
    return pc_state_fnv1a(PC_STATE_FNV64_OFFSET, data, len);
}


/* ------------------------------------------------------------------ */
/* PNG                                                                 */
/* ------------------------------------------------------------------ */

#define PNG_W      PC_VIDEO_WIDTH
#define PNG_H      (PC_VIDEO_HEIGHT * 2)
#define PNG_STRIDE (1 + PNG_W * 3)          /* one filter byte, then RGB */
#define PNG_RAW    ((size_t)PNG_H * PNG_STRIDE)

static unsigned char png_raw[PNG_RAW];
static unsigned char png_z[PNG_RAW + PNG_RAW / 2 + 64];

/*
 * The two surfaces, upper screen first, as PNG filter-type-0 scanlines.
 */
static void png_build_raw(void) {
    int upper = pc_video_upper_engine();
    const uint32_t *half[2];
    size_t o = 0;
    int h, y, x;

    half[0] = pc_video_surface(upper);
    half[1] = pc_video_surface(upper == PC_VIDEO_MAIN ? PC_VIDEO_SUB
                                                      : PC_VIDEO_MAIN);
    for (h = 0; h < 2; h++) {
        for (y = 0; y < PC_VIDEO_HEIGHT; y++) {
            const uint32_t *row = half[h] + (size_t)y * PC_VIDEO_WIDTH;

            png_raw[o++] = 0;
            for (x = 0; x < PC_VIDEO_WIDTH; x++) {
                uint32_t p = row[x];

                png_raw[o++] = (unsigned char)(p >> 16);
                png_raw[o++] = (unsigned char)(p >> 8);
                png_raw[o++] = (unsigned char)(p);
            }
        }
    }
}

int pc_video_write_png(const char *path) {
    png_build_raw();
    return pc_png_write_raw(path, PNG_W, PNG_H, png_raw, PNG_RAW,
                            png_z, sizeof png_z, "pc-video");
}

/* ------------------------------------------------------------------ */
/* The dump                                                            */
/* ------------------------------------------------------------------ */

#define PC_VIDEO_PATH_MAX 1024

static char dump_dir[PC_VIDEO_PATH_MAX];
static FILE *dump_manifest;
static uint64_t dump_written;   /* PNGs actually on disk */
static uint64_t dump_rows;      /* manifest rows, which is the frame count */
static uint64_t dump_limit;
static uint64_t dump_from;      /* PC_DUMP_FROM: skip PNGs before this frame */
static uint64_t dump_stride;    /* ...and keep only every Nth after it */
static int dump_digest;         /* PC_DUMP_DIGEST: hash every frame, PNG or not */
static int dump_headless;
static int dump_closed;
static int dump_atexit_armed;

/*
 * What is on screen right now, as one number and one count. The count is what
 * separates "drew a map" from "drew a blank": a screen the game never painted
 * is one or two colours, and a digest alone cannot tell those apart from a
 * screen it painted identically twice.
 */
void pc_video_surface_stats(uint64_t *digest, unsigned *colours)
{
    if (digest != NULL) {
        *digest = fnv1a(pc_surface, (uint32_t)sizeof pc_surface);
    }
    if (colours != NULL) {
        /* A 4096-entry direct-mapped set over the low bits of each pixel.
         * Exact counting would want a hash of 393,216 pixels every time this
         * is asked; this is a lower bound that separates one colour from many,
         * which is the only distinction the caller makes. */
        static unsigned char seen[4096];
        unsigned n = 0, i;
        memset(seen, 0, sizeof seen);
        for (i = 0; i < 2 * PC_VIDEO_PIXELS; i++) {
            uint32_t px = ((const uint32_t *)pc_surface)[i];
            unsigned slot = (unsigned)((px ^ (px >> 12)) & 0xFFFu);
            if (!seen[slot]) { seen[slot] = 1; n++; }
        }
        *colours = n;
    }
}

void pc_video_set_frame_limit(uint64_t frames) { dump_limit = frames; }
void pc_video_set_headless(int headless)       { dump_headless = headless; }
int  pc_video_headless(void)                   { return dump_headless; }
uint64_t pc_video_frames_written(void)         { return dump_written; }

int pc_video_open_dump(const char *dir) {
    char path[PC_VIDEO_PATH_MAX + 32];

    if (dir == NULL || dir[0] == '\0') {
        fprintf(stderr, "pc-video: PC_DUMP_FRAMES needs a directory.\n");
        return 0;
    }
    if (strlen(dir) >= sizeof dump_dir) {
        fprintf(stderr, "pc-video: PC_DUMP_FRAMES path is too long "
                        "(limit %u).\n", (unsigned)sizeof dump_dir - 1);
        return 0;
    }
    if (pc_mkdir(dir) != 0 && errno != EEXIST) {
        fprintf(stderr, "pc-video: cannot create %s: %s\n",
                dir, strerror(errno));
        return 0;
    }

    strcpy(dump_dir, dir);
    snprintf(path, sizeof path, "%s/frames.txt", dump_dir);
    /* "wb": on Windows text mode would write \r\n and the manifest would
     * differ from the Linux run's byte for byte. */
    dump_manifest = fopen(path, "wb");
    if (dump_manifest == NULL) {
        fprintf(stderr, "pc-video: cannot write %s: %s\n",
                path, strerror(errno));
        dump_dir[0] = '\0';
        return 0;
    }
    dump_written = 0;
    dump_rows = 0;
    dump_closed = 0;

    /* PC_DUMP_FROM=N[:S]: PNG files only from frame N on, and with a stride
     * only every Sth frame; the manifest still gets every frame. The PNG
     * encoder is
     * ~10x the cost of a frame, so an iterating harness that only needs
     * to SEE the end of a deterministic run replays the start at full
     * speed, and one that needs to find WHERE a long run diverged
     * samples the whole stretch in a single pass. */
    {
        const char *from = getenv("PC_DUMP_FROM");
        char *end = NULL;
        dump_from = from != NULL ? strtoull(from, &end, 0) : 0;
        dump_stride = (end != NULL && *end == ':')
                          ? strtoull(end + 1, NULL, 0) : 1;
        if (dump_stride == 0) dump_stride = 1;
        dump_digest = getenv("PC_DUMP_DIGEST") != NULL;
    }

    /*
     * The header goes out now, before any guest code runs: a dump directory
     * holding a manifest and no frames means "the boot never reached a
     * VBlank", which is a different thing from a directory the port never
     * touched.
     */
    fprintf(dump_manifest,
            "# pc frame dump, pokeplatinum port\n"
            "# renderer: %s\n"
            "# frames: %u x %u, upper screen on top of lower\n"
            "# frame N is the Nth VBlank\n"
            "# every frame gets a row; file and digest are '-' on a frame\n"
            "#   PC_DUMP_FROM held back, so the row count is the frame count\n"
            "# columns: frame file powcnt dispcnt-main dispcnt-sub"
            " upper digest\n",
            pc_renderer_label ? pc_renderer_label
                              : "none (no 2D engine installed; these frames "
                                "are what the port has drawn, which is nothing)",
            (unsigned)PC_VIDEO_WIDTH, (unsigned)PC_VIDEO_HEIGHT);
    fflush(dump_manifest);

    if (!dump_atexit_armed) {
        dump_atexit_armed = 1;
        atexit(pc_video_close_dump);
    }
    return 1;
}

void pc_video_close_dump(void) {
    if (dump_manifest == NULL || dump_closed) return;
    dump_closed = 1;
    fprintf(dump_manifest, "# wrote %u frames, %u image(s)\n",
            (unsigned)dump_rows, (unsigned)dump_written);
    fclose(dump_manifest);
    dump_manifest = NULL;
}

void pc_video_frame_end(uint64_t frame) {
    char path[PC_VIDEO_PATH_MAX + 32];
    char name[32];

    /*
     * SWAP_BUFFERS takes effect at VBlank on hardware rather than when the
     * command runs, so this is where the geometry engine's polygon list
     * becomes the one a renderer sees. Before the renderer, and
     * unconditional for the same reason it is. (A weak no-op in armrec_rt.c
     * until the 3D engine is linked in.)
     */
    pc_gpu3d_vblank();
    pc_bench_frame();
    /* The polygon list is published by now and the rasterizer has just
     * consumed it, so the numbers the instrument prints are the numbers this
     * frame was drawn with, and they carry the same frame number the PNG
     * beside them does. */
    pc_polydump_frame(frame);

    /* The save lab, if this run is one: it mints its save and exits from
     * here, at a frame boundary, so the field system it is reading out of is
     * between frames rather than mid-task. A run with no PC_LAB returns
     * immediately. */
    {
        extern void pc_lab_frame(unsigned long long frame);
        extern void pc_lab_battle_frame(unsigned long long frame);
        extern void pc_lab_save_frame(unsigned long long frame);
        extern void pc_lab_grow_frame(unsigned long long frame);
        extern void pc_lab_contest_frame(unsigned long long frame);
        extern void pc_lab_underground_frame(unsigned long long frame);
        extern void pc_lab_sprite_frame(unsigned long long frame);
        extern void pc_lab_text_frame(unsigned long long frame);
        extern void pc_lab_audio_frame(unsigned long long frame);
        pc_lab_frame((unsigned long long)frame);
        /* The battle lab is the other half and does not need PC_LAB: a battle
         * station boots from a save the recipe already minted. */
        pc_lab_battle_frame((unsigned long long)frame);
        /* And PC_LAB_SAVE_AT writes the save again for a station whose whole
         * claim is about what the run did rather than what it started from. */
        pc_lab_save_frame((unsigned long long)frame);
        /* And PC_LAB_USE_ITEM uses a bag item on a party member, which is what
         * an evolution station has instead of a menu to walk through. */
        pc_lab_grow_frame((unsigned long long)frame);
        /* And PC_LAB_CONTEST enters a Super Contest, which is the same shape:
         * The lobby's four screens are menu-driving, the contest is not. */
        pc_lab_contest_frame((unsigned long long)frame);
        /* And PC_LAB_UNDERGROUND skips the first-visit Roark intro after the
         * Explorer Kit arrives, and can start the digging minigame. */
        pc_lab_underground_frame((unsigned long long)frame);
        /* And PC_LAB_SPRITE loads every species through pokemon_sprite.c
         * and exits, no field, no save; the manager only needs a heap. */
        pc_lab_sprite_frame((unsigned long long)frame);
        /* And PC_LAB_TEXT walks every message bank through the printer
         * and exits, same shape: fonts and the filesystem, no field. */
        pc_lab_text_frame((unsigned long long)frame);
        /* And PC_LAB_AUDIO plays every named sequence through the
         * game's own player and exits, SoundSystem_Init is enough. */
        pc_lab_audio_frame((unsigned long long)frame);
    }

    /*
     * Unconditional: a renderer that only ran when someone was watching
     * would make PC_DUMP_FRAMES change the run. The VRAM brackets are no-ops
     * where the views really alias (everywhere on this host).
     */
    armrec_vram_render_begin();
    if (pc_renderer) pc_renderer(frame);
    armrec_vram_render_end();

    /* The window, when a window layer exists, a second consumer of the same
     * surfaces, never a gate on them. */
    if (pc_view_publish != NULL) pc_view_publish(frame);

    /* PC_PROBE_2D=N[-M]: sprite-state probe, pc_probe2d.c. */
    pc_probe2d(frame);
    /* --watch prints here, at the same frame boundary the probes use, so a
     * watch line and a probe line from one frame describe the same instant. */
    pc_sym_frame(frame);

    /*
     * A row per frame, whether or not this frame gets a PNG. The row is what
     * a harness counts to learn how far a run got, and counting rows beats
     * counting PNGs twice over: it is exact when PC_DUMP_FROM holds the
     * images back, and it does not make the answer cost a PNG encode per
     * frame; which on gameplay frames is 5x the run (194 -> 38 frames/s,
     * measured over the replay; the attract loop's flatter pictures compress
     * so much better that it is only 1.5x there).
     *
     * The digest stays tied to the PNG, because it is the expensive half:
     * FNV-1a byte-at-a-time over the 384 KB surface, with a 64-bit multiply
     * on a 32-bit host. A frame with no image reports '-' for both columns
     * rather than paying for a hash nothing asked for.
     *
     * PC_DUMP_DIGEST=1 unties them: every row gets a digest and no PNG is
     * written unless PC_DUMP_FROM asks for one. That is what proves two
     * builds draw the same picture; a renderer change that must not move a
     * pixel is checked by replaying a scene under both and diffing the
     * manifests, and at one hash a frame that costs about a third of a frame
     * instead of the five frames an image costs.
     */
    if (dump_manifest != NULL) {
        int want_png = frame >= dump_from &&
                       (frame - dump_from) % (dump_stride ? dump_stride : 1) == 0;
        const char *file = "-";
        char digest[20];

        strcpy(digest, "-");
        if (want_png) {
            snprintf(name, sizeof name, "frame-%06u.png", (unsigned)frame);
            snprintf(path, sizeof path, "%s/%s", dump_dir, name);
            if (pc_video_write_png(path)) {
                dump_written++;
                file = name;
            }
        }
        if (dump_digest || (want_png && file != NULL && file[0] != '-')) {
            snprintf(digest, sizeof digest, "%016llX",
                     (unsigned long long)fnv1a(pc_surface,
                                               (uint32_t)sizeof pc_surface));
        }
        fprintf(dump_manifest,
                "%06u %s %04X %08X %08X %s %s\n",
                (unsigned)frame, file,
                armrec_mem_ready ? (unsigned)PC_REG_POWCNT : 0u,
                armrec_mem_ready ? (unsigned)PC_REG_DISPCNT_A : 0u,
                armrec_mem_ready ? (unsigned)PC_REG_DISPCNT_B : 0u,
                pc_video_upper_engine() == PC_VIDEO_MAIN ? "main" : "sub",
                digest);
        fflush(dump_manifest);
        dump_rows++;
    }

    /*
     * PC_FRAMES=N ends the run here rather than by unwinding, because there
     * is nothing to unwind to: this is called from OS_Halt on whichever
     * guest thread gave up the CPU. exit() still runs the atexit handlers,
     * so the manifest is closed.
     */
    if (dump_limit != 0 && frame >= dump_limit) {
        pc_video_close_dump();
        fprintf(stderr, "pc-video: PC_FRAMES %u reached.\n",
                (unsigned)dump_limit);
        exit(0);
    }
}

void pc_video_reset(void) {
    pc_video_close_dump();
    memset(pc_surface, 0, sizeof pc_surface);
    pc_renderer = NULL;
    pc_renderer_label = NULL;
    dump_dir[0] = '\0';
    dump_written = 0;
    dump_rows = 0;
    dump_limit = 0;
    dump_headless = 0;
    dump_closed = 0;
}
