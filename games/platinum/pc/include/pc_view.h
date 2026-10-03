/*
 * The window channel, pokediamond's architecture at platinum's scale.
 *
 * Why a second process. The port is -m32 (guest struct layouts demand it)
 * and the host's SDL2 is a 64-bit ELF a 32-bit process can neither link nor
 * dlopen. So the port publishes frames into a shared-memory page and a
 * native 64-bit viewer (pc/sdl/pcview.c, built separately) presents them.
 *
 * The port never blocks on the viewer. Publishing is a seqlock write: it
 * costs the same whether pcview is running, stopped in a debugger, or was
 * never started. A frontend that could stall the guest would make host
 * speed an input to the run.
 *
 * THE SEQLOCK. `seq` is even when the frame data is stable, odd while the
 * port is writing. A reader snapshots seq (retrying while odd), copies the
 * pixels, and rereads seq; a change means a torn read and another try. The
 * input back-channel runs the other way and needs no lock at all: the
 * viewer owns those words, the port samples them once per frame, and a
 * torn 16-bit read of a key mask is a one-frame glitch a human cannot
 * produce or perceive.
 *
 * What is deliberately absent from diamond's protocol: the HD 3D margins
 * and the input recorder. Each comes back with the subsystem that feeds
 * it. The audio ring came back with the mixer, version 3 carries the
 * wide picture (`width` on the way out, the window's shape on the way
 * back), version 5 the held fast-forward key, and version 6 the internal
 * resolution: `height` beside `width`, and pixel arrays big enough for both
 * to be multiplied. Version 7 adds `touch_wanted`, which is the game saying
 * whether the pen is wanted at all.
 */

#ifndef POKEPLATINUM_PC_VIEW_H
#define POKEPLATINUM_PC_VIEW_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_VIEW_MAGIC   0x50504C56u     /* 'PPLV' */
#define PC_VIEW_VERSION 7u

/*
 * The audio ring, diamond's design, whole (see that tree's pc_view.h for
 * the full derivation). The port writes stereo frames at the head and
 * advances it; the reader owns its tail in its own memory, so a viewer
 * paused in a debugger costs the port nothing and a reader that falls a
 * full ring behind skips itself forward. One stereo frame is one
 * uint32_t, left in the low half, so a frame is indivisible. A full
 * second of ring absorbs a load-stall burst; the steady-state fill is the
 * viewer's prebuffer, which is where the latency lives (188 ms).
 */
#define PC_VIEW_AUDIO_FRAMES 32768u
/* The guest's own rate, 33,513,982 / 1024, pc_audio.c's WAV_RATE; the
 * rounder 32,768 would play 0.1% fast. */
#define PC_VIEW_AUDIO_RATE   32728u

#define PC_VIEW_W 256
#define PC_VIEW_H 192

/*
 * Wide rendering. A screen may carry up to this many columns when the port
 * widens the 3D field of view; 342 is 16:9 against 192 rows, and nothing
 * asks for more because the margins are drawn by the rasterizer and a
 * frustum much wider than that starts showing the seams of scenes that
 * were built for a 4:3 camera.
 *
 * The pixel arrays are sized for the maximum and rows are packed at the
 * FRAME'S OWN `width`, so a native frame is byte-for-byte the frame this
 * protocol always carried and a wide one simply uses more of the array.
 * A reader that ignores `width` reads a native frame correctly and a wide
 * one skewed, which is why the viewer checks the version.
 */
#define PC_VIEW_WIDE_MAX 342u

/*
 * Internal resolution. The 3D layer can be rasterized at this many times the
 * DS's pixel count on each axis, and the published frame carries the result
 * rather than a point-sample of it, so the pixel arrays are sized for the
 * widest frame at the highest scale, and `width` and `height` say what is
 * actually in them.
 *
 * 4, which is 1024x768 native and 1368x768 at 16:9. The old ceiling of 2 was
 * written down as arithmetic, "the geometry engine masks final positions to
 * 10 and 9 bits and there is no room above that"; which was a fact about
 * two field widths in pc/hw/pc_gpu3d.c rather than about the DS. Both are
 * sized from the surface now; see PC_GPU3D_HD_MAX for the one that was
 * actually load-bearing.
 *
 * The page is sized from this, so each step costs 4x the pixels (525 KB
 * native, 8.4 MB at 4) on top of N^2 the rasterizer's work.
 */
#define PC_VIEW_HD_MAX 4u

/* One screen's pixel array, in words: the widest frame at the highest
 * internal resolution. Readers size their scratch from this. */
#define PC_VIEW_FRAME_WORDS (PC_VIEW_WIDE_MAX * PC_VIEW_HD_MAX \
                             * PC_VIEW_H * PC_VIEW_HD_MAX)

/* PAD_Read-positive button bits, the SDK's own values. */
#define PC_VIEW_KEY_A      0x0001u
#define PC_VIEW_KEY_B      0x0002u
#define PC_VIEW_KEY_SELECT 0x0004u
#define PC_VIEW_KEY_START  0x0008u
#define PC_VIEW_KEY_RIGHT  0x0010u
#define PC_VIEW_KEY_LEFT   0x0020u
#define PC_VIEW_KEY_UP     0x0040u
#define PC_VIEW_KEY_DOWN   0x0080u
#define PC_VIEW_KEY_R      0x0100u
#define PC_VIEW_KEY_L      0x0200u
#define PC_VIEW_KEY_X      0x0400u
#define PC_VIEW_KEY_Y      0x0800u

struct pc_view_shm {
    uint32_t magic;
    uint32_t version;
    /* The process publishing here, so a second port that finds this name
     * already in use can say whose it is. One page has exactly one writer:
     * Two ports on one channel produce a picture assembled from two
     * different games, and on a version boundary it is not even a picture. */
    volatile uint32_t publisher;

    /* Frame channel: port writes, viewer reads. Seqlock as above. */
    volatile uint32_t seq;
    volatile uint32_t frame_lo;
    volatile uint32_t frame_hi;
    volatile uint32_t upper_engine;    /* 0 = engine A on top, 1 = engine B */
    volatile uint32_t width;           /* columns per screen this frame:
                                        * PC_VIEW_W native, more when wide,
                                        * multiplied again when HD */
    volatile uint32_t height;          /* rows per screen: PC_VIEW_H, or a
                                        * multiple of it when HD */

    /*
     * Whether the game is asking for the pen. 1 while the game has the
     * SDK's touch auto-sampling running, which it starts when a screen
     * wants touch input and stops when it stops wanting it, the game
     * saying so in its own words, from inside the process, which is
     * something no frontend outside one can see.
     *
     * The window layer uses it to decide which screen deserves the space.
     * It is advisory and nothing but the layout reads it, so a viewer that
     * ignores it is simply a viewer with a fixed layout.
     */
    volatile uint32_t touch_wanted;
    uint32_t pix[2][PC_VIEW_FRAME_WORDS];           /* 0x00RRGGBB, rows
                                        * packed at `width`, [0]=A, [1]=B */

    /* Input channel: viewer writes, port samples at each frame boundary.
     * in_seq bumps on every change; 0 means "no viewer has ever spoken",
     * which is what keeps a scripted run scripted when no window is up. */
    volatile uint32_t in_seq;
    volatile uint32_t in_keys;         /* PC_VIEW_KEY_* mask, positive */
    volatile uint32_t in_touch;        /* 1 = pen down */
    volatile uint32_t in_touch_x;      /* 0..255, lower-screen pixels */
    volatile uint32_t in_touch_y;      /* 0..191 */

    /* The window's shape, as the ratio of the area one screen is drawn
     * into. The viewer writes it whenever the window is resized; a port
     * asked for an adaptive aspect turns it into a width, and any other
     * port ignores it. A ratio rather than a width because the policy , 
     * what is too wide, how it rounds, belongs to the side that renders.
     * Zero in either word means "no viewer has said", which is what keeps
     * a windowless run native. */
    volatile uint32_t in_aspect_n;     /* width of that area, in pixels  */
    volatile uint32_t in_aspect_d;     /* height of that area, in pixels */

    /*
     * Who is watching, and whether they are still there. The port has no
     * window of its own: the viewer holds it, so closing that window is a
     * player ending the session, and a port that kept running would be a
     * process nobody can see and nobody thinks is there. They accumulate,
     * and the next run finds its channel already taken.
     *
     * Two fields because there are two ways a window goes away. `in_quit`
     * is the viewer saying so on its way out, which is the common case and
     * lets the port end the run the same way a frame limit does.
     * `in_viewer_pid` covers every other way, a crash, a kill, a machine
     * putting the process down, because a pid can be asked whether it is
     * still alive and a dead viewer cannot write anything.
     *
     * Zero in either means nothing: a headless run has no viewer and must
     * not be ended by one.
     */
    volatile uint32_t in_viewer_pid;   /* the process presenting this page */
    volatile uint32_t in_quit;         /* 1 = that window has closed       */

    /*
     * Fast-forward, held. 1 while the player is holding the key; the port
     * skips the frame pacer for exactly as long as it stays 1, and runs at
     * whatever rate the host manages.
     *
     * Not part of in_seq. That counter exists so the port can tell a live
     * player from a script and only overrides the script once someone has
     * actually spoken; a held key that is not a game input has no business
     * in that decision, and gating it on in_seq would make releasing the
     * key at the same moment as a button press lose one of them.
     *
     * It changes the SPEED and nothing else: the frames a fast-forwarded
     * run produces are the frames it would have produced anyway, which is
     * what makes it safe to hold through a battle.
     */
    volatile uint32_t in_turbo;

    /* The audio ring; see PC_VIEW_AUDIO_FRAMES above. Both fields are
     * the port's; the reader's tail is the reader's own. audio_rate is 0
     * until the port has published once, which is how a viewer knows
     * whether this port produces sound at all. */
    volatile uint32_t audio_rate;
    volatile uint32_t audio_head;           /* stereo frames ever written */
    uint32_t audio[PC_VIEW_AUDIO_FRAMES];   /* left in the low half       */
};

/*
 * Take up to `frames` stereo frames out of the ring as interleaved 16-bit
 * left/right. Returns how many it got; the caller pads with silence,
 * because an audio callback must always fill its buffer. `*dropped`
 * counts frames lost to falling a full ring behind; the reader's own
 * bookkeeping, since the producer never inspects the tail. static inline
 * because the two callers are different ABIs and this header is the one
 * copy of the protocol either has.
 */
static inline unsigned pc_view_audio_read(const struct pc_view_shm *v,
                                          uint32_t *tailp,
                                          int16_t *dst, unsigned frames,
                                          uint32_t *dropped) {
    uint32_t head, tail, avail;
    unsigned n = 0;

    if (dropped) *dropped = 0;
    if (v == 0 || dst == 0 || tailp == 0) return 0;
    if (v->magic != PC_VIEW_MAGIC || v->version != PC_VIEW_VERSION) return 0;

    head = __atomic_load_n(&v->audio_head, __ATOMIC_ACQUIRE);
    tail = *tailp;

    /* Unsigned subtraction, so this stays right across the 2^32 wrap. */
    avail = head - tail;
    if (avail > PC_VIEW_AUDIO_FRAMES) {
        uint32_t lost = avail - PC_VIEW_AUDIO_FRAMES;
        if (dropped) *dropped = lost;
        tail += lost;
        avail = PC_VIEW_AUDIO_FRAMES;
    }

    while (n < frames && avail > 0) {
        uint32_t s = v->audio[tail % PC_VIEW_AUDIO_FRAMES];
        dst[n * 2 + 0] = (int16_t)(uint16_t)(s & 0xFFFFu);
        dst[n * 2 + 1] = (int16_t)(uint16_t)(s >> 16);
        tail++;
        avail--;
        n++;
    }

    *tailp = tail;
    return n;
}

/*
 * The width a window of n x d pixels asks for: the columns that fill that
 * shape at 192 rows, never narrower than the DS and never wider than the
 * cap, and always even so the two margins are the same size. static inline
 * beside the audio reader and for the same reason, the port applies this
 * and the viewer's selftest pins it, and they are different ABIs.
 */
static inline int pc_view_aspect_width(unsigned n, unsigned d) {
    unsigned w;

    if (n == 0 || d == 0) return PC_VIEW_W;
    w = (PC_VIEW_H * n + d / 2) / d;
    if (w < PC_VIEW_W) w = PC_VIEW_W;
    if (w > PC_VIEW_WIDE_MAX) w = PC_VIEW_WIDE_MAX;
    return (int)(w & ~1u);
}

/*
 * Port side. pc_view_init() maps $PC_VIEW (a /dev/shm name, created and
 * truncated here) and returns 0 if the env is unset; the seam costs
 * nothing when no window is asked for. pc_view_publish() overrides
 * pc_video.c's weak extern; it also samples the input channel and feeds
 * pc_input's live path.
 *
 * pc_view_set_aspect() chooses the width: PC_VIEW_W (or anything smaller)
 * is native, PC_VIEW_ASPECT_AUTO follows the window the viewer reports,
 * and anything between is that many columns whatever the window does.
 */
#define PC_VIEW_ASPECT_AUTO (-1)

int pc_view_init(void);
void pc_view_publish(uint64_t frame);
void pc_view_set_aspect(int width);
void pc_view_set_hd(int scale);

#if defined(__BIONIC__)
/*
 * The attach side, on the host where the reader is in this process.
 *
 * bionic has no shm_open and Android has no /dev/shm, so on that host the
 * channel is an anonymous mapping registered under its name inside the
 * process, and this is how a reader reaches it, an app's renderer calls it
 * where the desktop viewer opens the name. NULL means nobody has published
 * that channel yet, which is the same "wait" a POSIX attach-before-create
 * gives, not an error. *size, when asked for, is the published size.
 */
void *pc_view_local_page(const char *name, size_t *size);
#endif

#ifdef __cplusplus
}
#endif

#endif /* POKEPLATINUM_PC_VIEW_H */
