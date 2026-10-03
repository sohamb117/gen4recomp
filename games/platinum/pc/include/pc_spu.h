/*
 * The SPU. Sixteen channels, PCM8/PCM16/IMA-ADPCM/PSG and
 * noise, the volume divider, panning, the two capture units, master volume and
 * SOUNDBIAS.
 *
 * Why there is a register interface here and not a hook. Nothing in arm7/asm/
 * touches 0x04000400-0x0400051F, measured, not assumed: the only writers in
 * this tree are arm7/lib/src/SND_channel.c and SND_capture.c, which are
 * decompiled C already, plus their macros in arm7/lib/include/registers.h. So
 * there is no assembly to give an ARMREC_*_HOOK to, and the port keeps its
 * hook-free store model: guest code writes the ARM7's I/O page as plain memory
 * and pc_spu_latch() replays what changed through pc_spu_write(). See
 * pc/hw/pc_spu.c's header for what that costs and what it buys.
 *
 * pc_spu_write/pc_spu_read are the *precise* interface, the one the oracle
 * and pc/tests/test_spu.c drive, and the one that is bit-exact against
 * melonDS's own SPU. Everything else here is the port's own plumbing.
 */

#ifndef PC_SPU_H
#define PC_SPU_H

#include <stdint.h>

#define PC_SPU_BASE      0x04000400u
#define PC_SPU_END       0x04000520u   /* one past SNDCAP1LEN's halfword */
#define PC_SPU_CHANNELS  16

/* One mix step. melonDS's MixInterval at its default sample rate, and the
 * divisor that turns the port's guest cycle counter into a sample rate:
 * 33,513,982 / 1024 = 32,728 Hz. */
#define PC_SPU_MIX_CYCLES 1024u

/* Reset every channel, both capture units, SOUNDCNT and SOUNDBIAS, and drop
 * whatever is in the output ring. */
void pc_spu_reset(void);

/* The register interface. `size` is 1, 2 or 4 bytes; anything else aborts,
 * because a width no oracle has measured is a guess. */
void     pc_spu_write(uint32_t addr, uint32_t val, int size);
uint32_t pc_spu_read(uint32_t addr, int size);

/* Produce one stereo sample, advancing every channel and capture unit by
 * PC_SPU_MIX_CYCLES. `out[0]` is left, `out[1]` right, after master volume,
 * SOUNDBIAS and the clamp. */
void pc_spu_mix(int16_t out[2]);

/* POWCNT2 bit 0. Zero mutes the mixer's output; the channels still run and
 * the capture units still write, which is what the hardware does. */
void pc_spu_set_power(int on);

/* A channel keyon, named by the ARM7 sound driver's own start sites (see
 * arm7/lib/include/registers.h, reg_SOUNDxCNT_KEYON). The latch diffs
 * register state between two instants and cannot see a start bit that fell
 * and rose in between (a retrigger) so the driver says so instead. */
void pc_spu_keyon_note(int idx);

/*
 * The bridge between the plain-memory I/O page and the interface above. Diffs
 * the ARM7's 0x04000400-0x0400051F window against a shadow, replays every
 * changed word through pc_spu_write(), and writes back the fields hardware
 * owns, each channel's masked control word (bit 31 included, which the SPU
 * clears when a one-shot sample ends) and both capture control bytes, so that
 * SND_IsChannelActive() and SND_GetChannelControl() read what a console would.
 *
 * Returns the number of registers it replayed, which is what makes "the latch
 * is being reached at all" checkable rather than assumed.
 */
unsigned pc_spu_latch(void);

/* Every register the latch has ever replayed. A silent port's first question
 * is whether the driver wrote anything at all, and one tick's return value
 * does not answer it. */
extern uint64_t pc_spu_latched;

/* How many mix steps have been produced. For the tests. */
uint64_t pc_spu_samples(void);

/*
 * A wave the SPU cannot read is counted, not guessed at.
 *
 * Sound DMA reaches guest memory by address, so a source address in no mapped
 * region is not a wave: it is a host pointer that got into SOUNDxSAD, and the
 * mixer answers zero for it exactly as melonDS does for an unmapped address.
 * That is why the sound heap work existed at all; the sequencer was running and every one of
 * its eleven source addresses was inside `struct SoundData`, a `static` in
 * decompiled C, so the whole game was silent with nothing anywhere saying so.
 *
 * These count what arrives at chan_set_srcaddr() and cannot be read, split by
 * what the address turns out to be, because the two have different causes and
 * different fixes:
 *
 *   `host` , inside the port's own image, i.e. a decompiled-C object that
 *              needs to move to pc_guest_window_alloc() the way struct
 *              SoundData did.
 *   `lost` , in neither the port nor a guest region, i.e. an address that
 *              was truncated on the way. SOUNDxSAD is 27 bits and
 *              SND_command.c unpacks the ARM9's command with
 *              `& 0x7FFFFFFu`, so a host pointer crossing that loses its top
 *              bits and lands nowhere at all.
 *
 * run_tests.py requires both to be zero over a real boot, so the next such
 * object is a named failing build rather than another quiet silence. This is
 * the same arrangement pc_pxi_wide_sends[] uses: count, do not
 * judge, and let the test decide what the population should be.
 */
uint64_t pc_spu_unreadable_host(void);
uint64_t pc_spu_unreadable_lost(void);

/* The first address of each kind, or 0. What --watch and gdb need to find the
 * object; a count alone says something is wrong and not what. */
uint32_t pc_spu_unreadable_host_first(void);
uint32_t pc_spu_unreadable_lost_first(void);

/* The same four as plain arrays ([0] host, [1] lost) so that a *linked
 * port* can be asked over a real boot with `--watch pc_spu_unreadable:8`.
 * The accessors above are for a test that is in the process. */
extern uint32_t pc_spu_unreadable[2];
extern uint32_t pc_spu_unreadable_first[2];

/*
 * ...and the distinct ones, because a count with one example is not enough to
 * name an object. The first value alone said `00000000` on the 3DS port over a
 * boot where the conversion had refused nothing, which narrows to "something
 * put a zero in a source-address lane" and no further. Each row is
 *
 *   [0] the raw register word, before the 27-bit mask
 *   [1] the channel it was written to
 *   [2] that channel's control word at the time, so a lane written as part of
 *       a real setup is distinguishable from one that arrived on its own
 *   [3] how many times this exact (word, channel) pair arrived
 *
 * Distinct pairs, not events: the interesting number is how many different
 * addresses are wrong, and a channel refilled every frame would otherwise fill
 * the log with one of them. Read by `--watch pc_spu_unread_log:128`.
 */
#define PC_SPU_UNREAD_LOG 16
extern uint32_t pc_spu_unread_log[PC_SPU_UNREAD_LOG][4];
extern uint32_t pc_spu_unread_log_n;

/*
 * Every distinct (source address, byte length) a channel has been keyed on
 * with, so that what the mixer *reads* has an oracle and not just what it
 * computes. See pc/hw/pc_spu.c. Read by `--watch pc_spu_src_log:128`.
 */
#define PC_SPU_SRC_LOG 1024
extern uint32_t pc_spu_src_log[PC_SPU_SRC_LOG][2];
extern uint32_t pc_spu_src_log_n;
extern uint32_t pc_spu_src_log_dropped;

/* ------------------------------------------------------------------ */
/* What the sequencer asked the hardware to do.       */
/* ------------------------------------------------------------------ */

/*
 * The SPU's latched register state, for the differential trace.
 *
 * Not the i/o page, and that is the point. Guest code writes
 * 0x04000400-0x0400051F as plain memory and pc_spu_latch() replays it, so the
 * page holds whatever the driver *stored*, an unmasked word, a half-written
 * pair, a source address with its top bits still on. What the SPU holds is the
 * masked value, and that is what the other side of the comparison holds too:
 * melonDS's SPUChannel keeps `Cnt & 0xFF7F837F`, `SrcAddr & 0x07FFFFFC`,
 * `LoopPos << 2` and `Length << 2`, and every mask here is the same one. So
 * this is the register domain both programs agree to describe, rather than one
 * program's storage.
 *
 * `looppos` and `length` are in *bytes*, i.e. already shifted, because that is
 * how both models store them and an unshift would not round-trip a capture
 * length of zero.
 */
struct pc_spu_regs {
    struct {
        uint32_t cnt;       /* SOUNDxCNT */
        uint32_t srcaddr;   /* SOUNDxSAD, bits 26..2 */
        uint16_t timer;     /* SOUNDxTMR: the pitch */
        uint32_t looppos;   /* SOUNDxPNT, in bytes */
        uint32_t length;    /* SOUNDxLEN, in bytes */
    } chan[PC_SPU_CHANNELS];
    struct {
        uint8_t  cnt;       /* SNDCAPxCNT */
        uint32_t dstaddr;   /* SNDCAPxDAD */
        uint16_t timer;     /* shared with channel 1 and 3's SOUNDxTMR */
        uint32_t length;    /* SNDCAPxLEN, in bytes */
    } cap[2];
    uint16_t soundcnt;      /* SOUNDCNT */
    uint8_t  master_volume; /* its low seven bits, with 127 raised to 128 */
    uint16_t bias;          /* SOUNDBIAS */
};

void pc_spu_regs(struct pc_spu_regs *out);

/* ------------------------------------------------------------------ */
/* The output surface, pc/src/pc_audio.c                             */
/* ------------------------------------------------------------------ */

/*
 * Headless-first, exactly as the renderer required of the renderer: the SPU fills an
 * in-memory ring and the WAV writer is one consumer of it, so a second consumer
 * (a viewer's audio device) is an addition rather than a refactor of the output
 * path.
 */

/* Called from pc_irq.c's frame boundary: latch the ARM7's registers, then
 * produce the samples `cycles` of guest time is worth. */
void pc_audio_advance(uint64_t cycles);

/* --dump-audio path. A riff/wave file, 16-bit stereo at 32,728 Hz, with its
 * length fields fixed up at close. Returns 0 on success. */
int  pc_audio_open_dump(const char *path);
void pc_audio_close(void);

/* The ring, for whoever wants the samples without a file. Returns how many
 * stereo frames were copied into `dst` (2 int16_t each). */
unsigned pc_audio_read(int16_t *dst, unsigned frames);

/* A sink pc_audio_advance() calls whenever it has produced samples, so a
 * consumer can take them the moment they exist rather than at the frame
 * boundary, pc/src/pc_view.c registers its shared-ring publisher here.
 * NULL (the default) means nobody is listening between frames. */
void pc_audio_set_sink(void (*sink)(void));

/* How many stereo frames the sink has taken, and how many it overwrote for
 * want of a reader. */
uint64_t pc_audio_frames(void);
uint64_t pc_audio_overruns(void);

#endif /* PC_SPU_H */
