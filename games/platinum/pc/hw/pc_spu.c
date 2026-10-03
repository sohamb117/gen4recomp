/*
 * The SPU: sixteen channels, PCM8, PCM16, IMA-ADPCM, PSG and noise, the volume
 * divider, panning, the two capture units, master volume, SOUNDBIAS and the
 * clamp.
 *
 * Derived from melonDS's src/SPU.cpp and src/SPU.h (Copyright 2016-2026
 * melonDS team, GPLv3-or-later). This port's licence permits lifting melonDS's
 * hardware cores and asks for them to be quarantined with their provenance
 * recorded, which is what pc/hw/ is. What is not taken is upstream's output
 * path: blip_buf resampling, the interpolation modes, and the audio thread.
 * The DS produces one 16-bit stereo sample every 1024 ARM7 cycles and that is
 * what this file produces; what a host does with the stream afterwards is
 * pc/src/pc_audio.c's problem.
 *
 * The oracle is `pcdiff-melon --spu-selftest`, which drives melonDS's own SPU
 * over a sweep of register writes and reports every register the SPU answers
 * and what the two capture units wrote into main RAM. The capture units are
 * the whole reason this is arbitrable: SNDCAP0 captures the left mixer and
 * SNDCAP1 the right, so the sweep can read the mixer's own output back out of
 * memory rather than out of melonDS's internals.
 *
 * The oracle stops one stage short of the speaker. Capture takes `left >> 8`
 * clamped, which is before master volume, the SOUNDCNT output selector and
 * SOUNDBIAS, and melonDS's own final sample is a local no public call returns.
 * So those last three steps are transcribed from upstream with no emulator to
 * arbitrate them, and pc/tests/test_spu.c checks them the only way that is
 * honest: against closed-form arithmetic over a DC input, plus the requirement
 * that each of the three actually moves the output.
 *
 * Nothing here is a guess about something untested. The two places the model
 * could invent an answer abort with a message instead: an access at a width
 * the sweep never measured, and a capture unit's source-select bits, which
 * upstream does not model either.
 *
 * How guest code reaches it: pc_spu_latch(). Nothing in arm7/asm/ names an
 * address in the SPU's window; the writers are decompiled C, so there is no
 * assembly to hook and the port keeps its hook-free store model. The latch
 * diffs the ARM7's I/O page against a shadow once per mix step and replays
 * what changed. A write and an overwrite inside one step collapse to the later
 * value, so a channel keyed on and off between two latches is never heard.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "armrec_rt.h"
#include "pc_spu.h"

/*
 * Diamond's copy of this file carries a Windows-only wave-address rebase
 * here (its sound heap is a host object whose address loses its top bits in
 * SOUNDxSAD's 27). Platinum does not: pc/patches/src/sound_system.c.patch
 * moves the SoundSystem into the guest window at 0x02A00000 (settled
 * decision 17), so every wave address the driver programs is a real guest
 * address on every platform and there is nothing to invert.
 */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

struct spu_channel {
    uint32_t cnt;
    uint32_t srcaddr;
    uint16_t timer_reload;
    uint32_t looppos;
    uint32_t length;

    uint8_t volume;
    uint8_t volshift;
    uint8_t pan;

    int      keyon;
    uint32_t timer;
    int32_t  pos;
    int16_t  cursample;
    uint16_t noiseval;

    int32_t  adpcm_val;
    int32_t  adpcm_index;
    int32_t  adpcm_val_loop;
    int32_t  adpcm_index_loop;
    uint8_t  adpcm_cur_byte;

    uint32_t fifo[8];
    uint32_t fifo_read_pos;
    uint32_t fifo_write_pos;
    uint32_t fifo_read_offset;
    uint32_t fifo_level;
};

struct spu_capture {
    uint8_t  cnt;
    uint32_t dstaddr;
    uint16_t timer_reload;
    uint32_t length;

    uint32_t timer;
    int32_t  pos;

    uint32_t fifo[4];
    uint32_t fifo_read_pos;
    uint32_t fifo_write_pos;
    uint32_t fifo_write_offset;
    uint32_t fifo_level;
};

static struct spu_channel chan[PC_SPU_CHANNELS];

static void pc_spu_trace_keyon(int idx, const char *how)
{
    static int trace = -1;

    if (trace < 0) {
        trace = getenv("PC_SPU_TRACE") != NULL;
    }
    if (trace) {
        extern uint32_t pc_os_vblank_count;
        fprintf(stderr, "pc-spu: keyon ch=%d %s sad=%08x tmr=%04x f=%u\n",
                idx, how, (unsigned)chan[idx].srcaddr,
                (unsigned)(uint16_t)chan[idx].timer_reload,
                (unsigned)pc_os_vblank_count);
    }
}

static struct spu_capture cap[2];
static uint16_t spu_cnt;
static uint8_t  master_volume;
static uint16_t spu_bias;
static int      spu_mute;

static uint64_t spu_sample_count;

/*
 * The latch's shadow of the ARM7's register window. Zeroed by pc_spu_reset()
 * rather than primed from the page on first use, and that is not a detail: a
 * lazy prime swallows everything written between the reset and the first mix
 * step, which is exactly where a boot writes its channel setup. The zero is
 * right because armrec_mem_init() zeroes the I/O page and pc_spu_reset() runs
 * before any guest code.
 */
static uint8_t  latch_shadow[PC_SPU_END - PC_SPU_BASE];

/*
 * Keyons the driver has named since the last latch, one bit per channel.
 *
 * The latch cannot see a retrigger, by construction, and this is the eye it
 * Is given. The driver starts a note by writing the whole control word with
 * the start bit clear and then ORing the start bit back in, all inside one
 * sequencer tick; one virtual instant. Hardware sees the bit fall and
 * rise; a diff of two snapshots sees 1 at both ends and replays nothing, so
 * a channel retriggered while still sounding kept its sample position and
 * its ADPCM state, one-shot waves ran off their ends into silence while the
 * envelope went on being written, and a new wave's address landed on a
 * decoder mid-stream. Audibly: melody notes that never restruck, cries that
 * decoded as static. The driver's two start sites (SND_exChannel.c,
 * SND_command.c, via reg_SOUNDxCNT_KEYON) name the event here instead; the
 * next latch turns each named channel's keyon on whether or not its control
 * word looks different.
 */
static uint16_t keyon_pending;

/*
 * PC_SPU_TRACE=1: one line per keyon the mixer acts on, channel, how it
 * was seen (control-word edge or the driver's named note), wave address,
 * stamped with the frame counter. Free when unset. Added chasing "the hit
 * sound sometimes plays twice": the command trace shows one START per SE,
 * so a double is below the command layer, and this is the layer below.
 */
static void pc_spu_trace_keyon(int idx, const char *how);

/*
 * How each keyon was seen, exported for --watch and the tests: [0] counts
 * keyons the latch found as a 0->1 edge of bit 31, [1] counts the ones only
 * the driver's note recovered; the retriggers the edge can never show.
 * A port where [1] stays zero has lost its eye on retriggers again.
 */
uint32_t pc_spu_keyons[2];

/*
 * Every register the latch has ever replayed, added up. The return value alone
 * answers "was the latch reached this tick"; a running total answers "has the
 * driver ever written a sound register at all", which is the first question to
 * ask of a port that is silent and the one the 3DS had no way to ask.
 */
uint64_t pc_spu_latched;

void pc_spu_keyon_note(int idx)
{
    if (idx >= 0 && idx < PC_SPU_CHANNELS) {
        keyon_pending |= (uint16_t)(1u << idx);
    }
}

/* ------------------------------------------------------------------ */
/* Tables, melonDS's, which are the DS's                             */
/* ------------------------------------------------------------------ */

static const int8_t adpcm_index_table[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static const uint16_t adpcm_table[89] = {
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E,
    0x0010, 0x0011, 0x0013, 0x0015, 0x0017, 0x0019, 0x001C, 0x001F,
    0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042,
    0x0049, 0x0050, 0x0058, 0x0061, 0x006B, 0x0076, 0x0082, 0x008F,
    0x009D, 0x00AD, 0x00BE, 0x00D1, 0x00E6, 0x00FD, 0x0117, 0x0133,
    0x0151, 0x0173, 0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292,
    0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583,
    0x0610, 0x06AB, 0x0756, 0x0812, 0x08E0, 0x09C3, 0x0ABD, 0x0BD0,
    0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE, 0x1706, 0x1954,
    0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B,
    0x3BB9, 0x41B2, 0x4844, 0x4F7E, 0x5771, 0x602F, 0x69CE, 0x7462,
    0x7FFF
};

static const int16_t psg_table[8][8] = {
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF,  0x7FFF },
    { -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF, -0x7FFF }
};

/* ------------------------------------------------------------------ */
/* Sound DMA's view of memory                                          */
/* ------------------------------------------------------------------ */

/*
 * The channel FIFO and the capture FIFO reach guest memory as the *ARM7* does,
 * which upstream spells NDS::ARM7Read32 / ARM7Write32. Identity mapping makes
 * that a plain access here, but only for an address a
 * region covers: an unmapped one would segfault the port where melonDS answers
 * zero and logs. So the region table arbitrates, and the port answers zero for
 * the same addresses melonDS does, including everything below 0x00004000,
 * which upstream refuses explicitly because sound DMA cannot read the ARM7's
 * BIOS.
 *
 * And on the 3DS there is no identity mapping, which is the same hole the host map
 * closed in pc/hw/pc_gpu2d.c and the same macro that closes it. Guest memory
 * there is a malloc'd slab, so a source address a channel was keyed with is a
 * number and not a pointer: dereferencing it reads whatever ARM11 userland has
 * at 0x02Axxxxx, which is nothing. The region table above still arbitrates;
 * it answers in guest addresses on both ports, so the only thing that
 * changes is the load, and hostmap_ptr() is what the rasterizer already uses
 * for it: armrec_host_ptr() memoised by 16 KB block, which suits a FIFO burst
 * that reads sixteen consecutive bytes. It never answers NULL, and nothing
 * here asks it to: both callers have already been told the address is mapped.
 */
#if defined(__3DS__)
void *hostmap_ptr(uint32_t guest);      /* 3ds/src/3ds_hostmap.h */
#define SPU_HOST(a) hostmap_ptr((uint32_t)(a))
#else
#define SPU_HOST(a) ((void *)(uintptr_t)(a))
#endif

static int sound_dma_mapped(uint32_t addr)
{
    int n = armrec_region_count();
    int i;

    for (i = 0; i < n; i++) {
        uint32_t base, size;
        if (!armrec_region_at(i, &base, &size, NULL)) {
            continue;
        }
        if (addr >= base && addr - base < size) {
            return 1;
        }
    }
    return 0;
}

static uint32_t sound_read32(uint32_t addr)
{
    addr &= ~3u;
    if (!sound_dma_mapped(addr)) {
        return 0;
    }
    return *(volatile uint32_t *)SPU_HOST(addr);
}

static void sound_write32(uint32_t addr, uint32_t val)
{
    addr &= ~3u;
    if (!sound_dma_mapped(addr)) {
        return;
    }
    *(volatile uint32_t *)SPU_HOST(addr) = val;
}

/* ------------------------------------------------------------------ */
/* Channel registers                                                   */
/* ------------------------------------------------------------------ */

static void chan_set_cnt(struct spu_channel *c, uint32_t val)
{
    static const uint8_t volshift[4] = { 4, 3, 2, 0 };
    uint32_t oldcnt = c->cnt;

    c->cnt = val & 0xFF7F837Fu;

    c->volume = (uint8_t)(c->cnt & 0x7F);
    if (c->volume == 127) {
        c->volume++;
    }
    c->volshift = volshift[(c->cnt >> 8) & 3];

    c->pan = (uint8_t)((c->cnt >> 16) & 0x7F);
    if (c->pan == 127) {
        c->pan++;
    }

    if ((val & 0x80000000u) && !(oldcnt & 0x80000000u)) {
        c->keyon = 1;
        pc_spu_keyons[0]++;
        pc_spu_trace_keyon((int)(c - chan), "edge");
    }
}

/*
 * What the SPU cannot read, counted. See pc/include/pc_spu.h
 * for why the two kinds are counted apart. Classified here rather than in
 * chan_fifo_buffer() because a keyon reads the same address sixteen times and
 * what is wanted is one count per *setup*, and because the raw register value
 * is only visible before the 27-bit mask below has been applied to it.
 */
uint32_t pc_spu_unreadable[2];
uint32_t pc_spu_unreadable_first[2];

/* The distinct ones. See pc/include/pc_spu.h for the row's shape and for why
 * the first value on its own turned out not to be enough. */
uint32_t pc_spu_unread_log[PC_SPU_UNREAD_LOG][4];
uint32_t pc_spu_unread_log_n;

static void unread_log(uint32_t v, uint32_t ch, uint32_t cnt)
{
    uint32_t i;

    for (i = 0; i < pc_spu_unread_log_n; i++) {
        if (pc_spu_unread_log[i][0] == v && pc_spu_unread_log[i][1] == ch) {
            pc_spu_unread_log[i][3]++;
            return;
        }
    }
    if (pc_spu_unread_log_n >= PC_SPU_UNREAD_LOG) {
        return;
    }
    pc_spu_unread_log[pc_spu_unread_log_n][0] = v;
    pc_spu_unread_log[pc_spu_unread_log_n][1] = ch;
    pc_spu_unread_log[pc_spu_unread_log_n][2] = cnt;
    pc_spu_unread_log[pc_spu_unread_log_n][3] = 1;
    pc_spu_unread_log_n++;
}

uint64_t pc_spu_unreadable_host(void) { return pc_spu_unreadable[0]; }
uint64_t pc_spu_unreadable_lost(void) { return pc_spu_unreadable[1]; }
uint32_t pc_spu_unreadable_host_first(void) { return pc_spu_unreadable_first[0]; }
uint32_t pc_spu_unreadable_lost_first(void) { return pc_spu_unreadable_first[1]; }

static void chan_set_srcaddr(struct spu_channel *c, uint32_t v)
{
    /*
     * The mask is hardware's, SOUNDxSAD is bits 26..2 of a main-RAM address,
     * and it is applied to `v` rather than widened, which is the whole of
     * the decision: a DS address field is a fact about the console this port
     * transcribes, so the object at the far end of it moves into guest memory
     * instead..
     */
    c->srcaddr = v & 0x07FFFFFCu;

    if (!sound_dma_mapped(c->srcaddr)) {
#if defined(_WIN32)
        /* PE has no __executable_start/_end; __ImageBase is the loader's
         * own name for where the image begins, and the classifier only
         * needs "is this a host image address", so the image's whole
         * possible span past the base does. */
        extern char __ImageBase;
        uint32_t lo = (uint32_t)(uintptr_t)&__ImageBase;
        uint32_t hi = lo + 0x10000000u;
#else
        extern char __executable_start, _end;
        uint32_t lo = (uint32_t)(uintptr_t)&__executable_start;
        uint32_t hi = (uint32_t)(uintptr_t)&_end;
#endif

        /* `v`, not `c->srcaddr`: a host pointer is recognisable before the
         * mask and unrecognisable after it. */
        int kind = (v >= lo && v < hi) ? 0 : 1;
        if (pc_spu_unreadable[kind]++ == 0) {
            pc_spu_unreadable_first[kind] = v;
        }
        unread_log(v, (uint32_t)(c - chan), c->cnt);
    }
}

static void chan_set_timer(struct spu_channel *c, uint32_t v)
{
    c->timer_reload = (uint16_t)v;
}

static void chan_set_looppos(struct spu_channel *c, uint32_t v)
{
    c->looppos = (v & 0xFFFFu) << 2;
}

static void chan_set_length(struct spu_channel *c, uint32_t v)
{
    c->length = (v & 0x001FFFFFu) << 2;
}

/* ------------------------------------------------------------------ */
/* The channel FIFO, sound DMA's prefetch                            */
/* ------------------------------------------------------------------ */

/*
 * Modelled rather than short-circuited to a direct memory read, even though the
 * sweep could not tell the difference: the prefetch is real hardware, a game
 * that rewrites a sample buffer under a playing channel hears the old bytes on
 * a console, and "unobservable today" is how a divergence gets left in. Two
 * bursts at keyon and a refill whenever the level falls to 16 bytes, which is
 * upstream's shape.
 */
static void chan_fifo_buffer(struct spu_channel *c)
{
    uint32_t totallen = c->looppos + c->length;
    uint32_t burstlen = 16;
    uint32_t i;

    if (c->fifo_read_offset >= totallen) {
        uint32_t repeatmode = (c->cnt >> 27) & 3;
        if (repeatmode & 1) {
            c->fifo_read_offset = c->looppos;
        } else if (repeatmode & 2) {
            return;                     /* one-shot, and it is done */
        }
    }

    if ((c->fifo_read_offset + 16) > totallen) {
        burstlen = totallen - c->fifo_read_offset;
    }

    for (i = 0; i < burstlen; i += 4) {
        /* sound DMA cannot read the ARM7 BIOS; upstream's own bound */
        c->fifo[c->fifo_write_pos] =
            (c->srcaddr + c->fifo_read_offset) >= 0x00004000u
                ? sound_read32(c->srcaddr + c->fifo_read_offset) : 0;
        c->fifo_read_offset += 4;
        c->fifo_write_pos = (c->fifo_write_pos + 1) & 7;
    }

    c->fifo_level += burstlen;
}

static uint32_t chan_fifo_read(struct spu_channel *c, int bytes)
{
    const uint8_t *p = (const uint8_t *)c->fifo;
    uint32_t ret = 0;
    int i;

    for (i = 0; i < bytes; i++) {
        ret |= (uint32_t)p[(c->fifo_read_pos + (uint32_t)i) & 0x1F] << (8 * i);
    }
    c->fifo_read_pos = (c->fifo_read_pos + (uint32_t)bytes) & 0x1F;
    c->fifo_level -= (uint32_t)bytes;

    if (c->fifo_level <= 16) {
        chan_fifo_buffer(c);
    }
    return ret;
}

/*
 * What the SPU was pointed at, as a log.
 *
 * Recorded at keyon and nowhere else, because that is the one moment at which
 * the source address, the loop point and the length are all in place and agree
 * with each other, SND_command.c's StartTimer sets bit 31 as a separate byte
 * store after SND_SetupChannelPcm has written the other four.
 *
 * It exists so that the *bytes* the mixer reads have an oracle, which the
 * mixer itself does not give: pc/tests/test_spu.c proves this file agrees with
 * melonDS register for register and pixel for pixel, and it proved that
 * perfectly while the game was silent, because what was wrong was the address
 * rather than the arithmetic. run_tests.py reads this with `--watch`, reads
 * the span each entry names with `--watch ADDR:LEN`, and requires those bytes
 * to be bytes of `data/sound/sound_data.sdat` in the dump, so the whole path
 * from NitroFS through the sound heap, the bank and SND_NoteOn is arbitrated
 * by the ROM.
 *
 * Distinct entries, capped, and the count is exported too: a log that silently
 * stopped recording would make the test pass on the first note for ever.
 */
uint32_t pc_spu_src_log[PC_SPU_SRC_LOG][2];
uint32_t pc_spu_src_log_n;
uint32_t pc_spu_src_log_dropped;

static void src_log(const struct spu_channel *c)
{
    uint32_t len = c->looppos + c->length;
    uint32_t i;

    if (len == 0) {
        return;
    }
    for (i = 0; i < pc_spu_src_log_n; i++) {
        if (pc_spu_src_log[i][0] == c->srcaddr &&
            pc_spu_src_log[i][1] == len) {
            return;
        }
    }
    if (pc_spu_src_log_n >= PC_SPU_SRC_LOG) {
        pc_spu_src_log_dropped++;
        return;
    }
    pc_spu_src_log[pc_spu_src_log_n][0] = c->srcaddr;
    pc_spu_src_log[pc_spu_src_log_n][1] = len;
    pc_spu_src_log_n++;
}

static void chan_start(struct spu_channel *c)
{
    c->timer = c->timer_reload;
    c->pos = (((c->cnt >> 29) & 3) == 3) ? -1 : -3;

    c->noiseval = 0x7FFF;
    c->cursample = 0;

    c->fifo_read_pos = 0;
    c->fifo_write_pos = 0;
    c->fifo_read_offset = 0;
    c->fifo_level = 0;

    if (((c->cnt >> 29) & 3) != 3) {
        src_log(c);
        chan_fifo_buffer(c);
        chan_fifo_buffer(c);
    }
}

/* ------------------------------------------------------------------ */
/* The four sample generators                                          */
/* ------------------------------------------------------------------ */

static void next_pcm8(struct spu_channel *c)
{
    c->pos++;
    if (c->pos < 0) {
        return;
    }
    if ((uint32_t)c->pos >= (c->looppos + c->length)) {
        uint32_t repeat = (c->cnt >> 27) & 3;
        if (repeat & 1) {
            c->pos = (int32_t)c->looppos;
        } else if (repeat & 2) {
            c->cursample = 0;
            c->cnt &= ~0x80000000u;
            return;
        }
    }
    c->cursample = (int16_t)((int16_t)(int8_t)chan_fifo_read(c, 1) << 8);
}

static void next_pcm16(struct spu_channel *c)
{
    c->pos++;
    if (c->pos < 0) {
        return;
    }
    if ((uint32_t)(c->pos << 1) >= (c->looppos + c->length)) {
        uint32_t repeat = (c->cnt >> 27) & 3;
        if (repeat & 1) {
            c->pos = (int32_t)(c->looppos >> 1);
        } else if (repeat & 2) {
            c->cursample = 0;
            c->cnt &= ~0x80000000u;
            return;
        }
    }
    c->cursample = (int16_t)chan_fifo_read(c, 2);
}

static void next_adpcm(struct spu_channel *c)
{
    c->pos++;
    if (c->pos < 8) {
        if (c->pos == 0) {
            uint32_t header = chan_fifo_read(c, 4);
            c->adpcm_val = (int32_t)(int16_t)(header & 0xFFFF);
            c->adpcm_index = (int32_t)((header >> 16) & 0x7F);
            if (c->adpcm_index > 88) {
                c->adpcm_index = 88;
            }
            c->adpcm_val_loop = c->adpcm_val;
            c->adpcm_index_loop = c->adpcm_index;
        }
        return;
    }

    if ((uint32_t)(c->pos >> 1) >= (c->looppos + c->length)) {
        uint32_t repeat = (c->cnt >> 27) & 3;
        if (repeat & 1) {
            c->pos = (int32_t)(c->looppos << 1);
            c->adpcm_val = c->adpcm_val_loop;
            c->adpcm_index = c->adpcm_index_loop;
            c->adpcm_cur_byte = (uint8_t)chan_fifo_read(c, 1);
        } else if (repeat & 2) {
            c->cursample = 0;
            c->cnt &= ~0x80000000u;
            return;
        }
    } else {
        uint16_t val, diff;

        if (!(c->pos & 1)) {
            c->adpcm_cur_byte = (uint8_t)chan_fifo_read(c, 1);
        } else {
            c->adpcm_cur_byte = (uint8_t)(c->adpcm_cur_byte >> 4);
        }

        val = adpcm_table[c->adpcm_index];
        diff = (uint16_t)(val >> 3);
        if (c->adpcm_cur_byte & 1) diff = (uint16_t)(diff + (val >> 2));
        if (c->adpcm_cur_byte & 2) diff = (uint16_t)(diff + (val >> 1));
        if (c->adpcm_cur_byte & 4) diff = (uint16_t)(diff + val);

        if (c->adpcm_cur_byte & 8) {
            c->adpcm_val -= diff;
            if (c->adpcm_val < -0x7FFF) c->adpcm_val = -0x7FFF;
        } else {
            c->adpcm_val += diff;
            if (c->adpcm_val > 0x7FFF) c->adpcm_val = 0x7FFF;
        }

        c->adpcm_index += adpcm_index_table[c->adpcm_cur_byte & 7];
        if (c->adpcm_index < 0)       c->adpcm_index = 0;
        else if (c->adpcm_index > 88) c->adpcm_index = 88;

        if ((uint32_t)c->pos == (c->looppos << 1)) {
            c->adpcm_val_loop = c->adpcm_val;
            c->adpcm_index_loop = c->adpcm_index;
        }
    }

    c->cursample = (int16_t)c->adpcm_val;
}

static void next_psg(struct spu_channel *c)
{
    c->pos++;
    c->cursample = psg_table[(c->cnt >> 24) & 7][c->pos & 7];
}

static void next_noise(struct spu_channel *c)
{
    if (c->noiseval & 1) {
        c->noiseval = (uint16_t)((c->noiseval >> 1) ^ 0x6000);
        c->cursample = -0x7FFF;
    } else {
        c->noiseval = (uint16_t)(c->noiseval >> 1);
        c->cursample = 0x7FFF;
    }
}

/* ------------------------------------------------------------------ */
/* Running a channel                                                   */
/* ------------------------------------------------------------------ */

/*
 * `type` is 0 PCM8, 1 PCM16, 2 ADPCM, 3 PSG, 4 noise, upstream's numbering,
 * which is also the SOUNDxCNT format field's except that format 3 means PSG on
 * channels 8-13, noise on 14-15 and silence below 8.
 */
static int32_t chan_run(struct spu_channel *c, int type, uint32_t cycles)
{
    int32_t val;

    if (!(c->cnt & 0x80000000u)) {
        return 0;
    }
    /* A sample source shorter than one FIFO burst never plays. */
    if (type < 3 && (c->length + c->looppos) < 16) {
        return 0;
    }
    if (c->keyon) {
        chan_start(c);
        c->keyon = 0;
    }

    c->timer += cycles;
    while (c->timer >> 16) {
        c->timer = (uint32_t)c->timer_reload + (c->timer - 0x10000u);
        switch (type) {
        case 0: next_pcm8(c);  break;
        case 1: next_pcm16(c); break;
        case 2: next_adpcm(c); break;
        case 3: next_psg(c);   break;
        default: next_noise(c); break;
        }
        if (!(c->cnt & 0x80000000u)) {
            break;
        }
    }

    val = c->cursample;
    val <<= c->volshift;
    val *= c->volume;
    return val;
}

static int32_t chan_do_run(struct spu_channel *c, unsigned num, uint32_t cycles)
{
    switch ((c->cnt >> 29) & 3) {
    case 0: return chan_run(c, 0, cycles);
    case 1: return chan_run(c, 1, cycles);
    case 2: return chan_run(c, 2, cycles);
    default:
        if (num >= 14) return chan_run(c, 4, cycles);
        if (num >= 8)  return chan_run(c, 3, cycles);
        return 0;
    }
}

static void chan_pan_output(const struct spu_channel *c, int32_t in,
                           int64_t *left, int64_t *right)
{
    *left  += ((int64_t)in * (128 - c->pan)) >> 10;
    *right += ((int64_t)in * c->pan) >> 10;
}

/* ------------------------------------------------------------------ */
/* The capture units                                                   */
/* ------------------------------------------------------------------ */

static void cap_start(struct spu_capture *u)
{
    u->timer = u->timer_reload;
    u->pos = 0;
    u->fifo_read_pos = 0;
    u->fifo_write_pos = 0;
    u->fifo_write_offset = 0;
    u->fifo_level = 0;
}

static void cap_set_cnt(struct spu_capture *u, uint8_t val)
{
    if ((val & 0x80) && !(u->cnt & 0x80)) {
        cap_start(u);
    }
    val = (uint8_t)(val & 0x8F);
    if (!(val & 0x80)) {
        val = (uint8_t)(val & ~0x01);
    }
    /*
     * Bits 0 and 1 are the source selectors, "add to channel output" and
     * "capture one channel rather than the mixer". Upstream logs
     * `!! Unsupported SPU capture mode` and models neither, so there is no
     * oracle for either and a plausible answer here would be a guess that
     * agreed with nothing. arm7/lib/src/SND_capture.c can encode them, so this
     * is reachable rather than theoretical.
     */
    if (val & 0x03) {
        armrec_trap("pc_spu_write",
                    "SNDCAPxCNT bits 0-1 select a capture source other than "
                    "the mixer. melonDS does not model either bit (its own "
                    "'!! UNSUPPORTED SPU CAPTURE MODE'), so --spu-selftest "
                    "cannot arbitrate them and this port will not invent an "
                    "answer.");
    }
    u->cnt = val;
}

static void cap_set_dstaddr(struct spu_capture *u, uint32_t v)
{
    u->dstaddr = v & 0x07FFFFFCu;
}

static void cap_set_length(struct spu_capture *u, uint32_t v)
{
    u->length = (v & 0xFFFFu) << 2;
    if (u->length == 0) {
        u->length = 4;
    }
}

static void cap_fifo_flush(struct spu_capture *u)
{
    uint32_t i;

    for (i = 0; i < 4; i++) {
        sound_write32(u->dstaddr + u->fifo_write_offset, u->fifo[u->fifo_read_pos]);
        u->fifo_read_pos = (u->fifo_read_pos + 1) & 3;
        u->fifo_level -= 4;
        u->fifo_write_offset += 4;
        if (u->fifo_write_offset >= u->length) {
            u->fifo_write_offset = 0;
            break;
        }
    }
}

static void cap_fifo_write(struct spu_capture *u, uint32_t val, int bytes)
{
    uint8_t *p = (uint8_t *)u->fifo;
    int i;

    for (i = 0; i < bytes; i++) {
        p[(u->fifo_write_pos + (uint32_t)i) & 0xF] = (uint8_t)(val >> (8 * i));
    }
    u->fifo_write_pos = (u->fifo_write_pos + (uint32_t)bytes) & 0xF;
    u->fifo_level += (uint32_t)bytes;

    if (u->fifo_level >= 16) {
        cap_fifo_flush(u);
    }
}

static void cap_run(struct spu_capture *u, uint32_t cycles, int32_t sample)
{
    int eight = (u->cnt & 0x08) != 0;

    u->timer += cycles;
    while (u->timer >> 16) {
        u->timer = (uint32_t)u->timer_reload + (u->timer - 0x10000u);
        if (eight) {
            cap_fifo_write(u, (uint32_t)(uint8_t)(int8_t)(sample >> 8), 1);
            u->pos += 1;
        } else {
            cap_fifo_write(u, (uint32_t)(uint16_t)(int16_t)sample, 2);
            u->pos += 2;
        }
        if ((uint32_t)u->pos >= u->length) {
            if (u->fifo_level >= 4) {
                cap_fifo_flush(u);
            }
            if (u->cnt & 0x04) {
                u->cnt = (uint8_t)(u->cnt & 0x7F);
                return;
            }
            u->pos = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* The register interface                                              */
/* ------------------------------------------------------------------ */

static void bad_width(const char *what, uint32_t addr, int size)
{
    static char msg[256];

    (void)snprintf(msg, sizeof msg,
                   "%s at %08X is %d byte(s) wide. --spu-selftest measures "
                   "8-, 16- and 32-bit accesses and nothing else, so any other "
                   "width would be a byte-lane rule this port invented.",
                   what, (unsigned)addr, size);
    armrec_trap("pc_spu", msg);
}

void pc_spu_write(uint32_t addr, uint32_t val, int size)
{
    if (size != 1 && size != 2 && size != 4) {
        bad_width("an SPU store", addr, size);
    }

    if (addr < 0x04000500u) {
        struct spu_channel *c = &chan[(addr >> 4) & 0xF];
        uint32_t lane = addr & 0xF;

        if (size == 1) {
            uint32_t sh = (lane & 3) * 8;
            if (lane < 4) {
                chan_set_cnt(c, (c->cnt & ~(0xFFu << sh)) | (val & 0xFF) << sh);
            }
            return;
        }
        if (size == 2) {
            switch (lane) {
            case 0x0: chan_set_cnt(c, (c->cnt & 0xFFFF0000u) | (val & 0xFFFF)); return;
            case 0x2: chan_set_cnt(c, (c->cnt & 0x0000FFFFu) | (val << 16)); return;
            case 0x8:
                chan_set_timer(c, val & 0xFFFF);
                if ((addr & 0xF0) == 0x10)      cap[0].timer_reload = (uint16_t)val;
                else if ((addr & 0xF0) == 0x30) cap[1].timer_reload = (uint16_t)val;
                return;
            case 0xA: chan_set_looppos(c, val); return;
            case 0xC: chan_set_length(c, ((c->length >> 2) & 0xFFFF0000u) | (val & 0xFFFF)); return;
            case 0xE: chan_set_length(c, ((c->length >> 2) & 0x0000FFFFu) | (val << 16)); return;
            default: return;
            }
        }
        switch (lane) {
        case 0x0: chan_set_cnt(c, val); return;
        case 0x4: chan_set_srcaddr(c, val); return;
        case 0x8:
            chan_set_looppos(c, val >> 16);
            chan_set_timer(c, val & 0xFFFF);
            if ((addr & 0xF0) == 0x10)      cap[0].timer_reload = (uint16_t)val;
            else if ((addr & 0xF0) == 0x30) cap[1].timer_reload = (uint16_t)val;
            return;
        case 0xC: chan_set_length(c, val); return;
        default: return;
        }
    }

    /* SOUNDCNT, SOUNDBIAS and the two capture units. */
    if (size == 1) {
        switch (addr) {
        case 0x04000500:
            spu_cnt = (uint16_t)((spu_cnt & 0xBF00u) | (val & 0x7Fu));
            master_volume = (uint8_t)(spu_cnt & 0x7F);
            if (master_volume == 127) master_volume++;
            return;
        case 0x04000501:
            spu_cnt = (uint16_t)((spu_cnt & 0x007Fu) | ((val & 0xBFu) << 8));
            return;
        case 0x04000508: cap_set_cnt(&cap[0], (uint8_t)val); return;
        case 0x04000509: cap_set_cnt(&cap[1], (uint8_t)val); return;
        default: return;
        }
    }

    switch (addr) {
    case 0x04000500:
        spu_cnt = (uint16_t)(val & 0xBF7Fu);
        master_volume = (uint8_t)(spu_cnt & 0x7F);
        if (master_volume == 127) master_volume++;
        return;
    case 0x04000504:
        spu_bias = (uint16_t)(val & 0x3FFu);
        return;
    case 0x04000508:
        cap_set_cnt(&cap[0], (uint8_t)(val & 0xFF));
        cap_set_cnt(&cap[1], (uint8_t)((val >> 8) & 0xFF));
        return;
    case 0x04000510: if (size == 4) cap_set_dstaddr(&cap[0], val); return;
    case 0x04000514: cap_set_length(&cap[0], val & 0xFFFF); return;
    case 0x04000518: if (size == 4) cap_set_dstaddr(&cap[1], val); return;
    case 0x0400051C: cap_set_length(&cap[1], val & 0xFFFF); return;
    default: return;
    }
}

uint32_t pc_spu_read(uint32_t addr, int size)
{
    if (size != 1 && size != 2 && size != 4) {
        bad_width("an SPU load", addr, size);
    }

    if (addr < 0x04000500u) {
        const struct spu_channel *c = &chan[(addr >> 4) & 0xF];
        uint32_t lane = addr & 0xF;

        if (size == 1) {
            return lane < 4 ? (c->cnt >> ((lane & 3) * 8)) & 0xFF : 0;
        }
        if (size == 2) {
            if (lane == 0x0) return c->cnt & 0xFFFF;
            if (lane == 0x2) return c->cnt >> 16;
            return 0;
        }
        return lane == 0 ? c->cnt : 0;
    }

    if (size == 1) {
        switch (addr) {
        case 0x04000500: return spu_cnt & 0x7Fu;
        case 0x04000501: return spu_cnt >> 8;
        case 0x04000508: return cap[0].cnt;
        case 0x04000509: return cap[1].cnt;
        default: return 0;
        }
    }
    switch (addr) {
    case 0x04000500: return spu_cnt;
    case 0x04000504: return spu_bias;
    case 0x04000508: return (uint32_t)cap[0].cnt | ((uint32_t)cap[1].cnt << 8);
    case 0x04000510: return size == 4 ? cap[0].dstaddr : 0;
    case 0x04000518: return size == 4 ? cap[1].dstaddr : 0;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* One mix step                                                        */
/* ------------------------------------------------------------------ */

/*
 * PC_SPU_TAP: per-channel debug taps. Set the environment variable to a
 * directory and every mix step appends each channel's post-run, pre-pan
 * sample to <dir>/chNN.s16 as mono little-endian s16, the sixteen stems
 * of the mix, for telling which channel a wrong sound lives on. Costs one
 * getenv on the first mix step and nothing when unset.
 */
static FILE *spu_tap[PC_SPU_CHANNELS];
static int   spu_tap_state;   /* 0 unchecked, 1 on, -1 off */

static void spu_tap_write(int i, int32_t v)
{
    int16_t s;

    if (spu_tap_state < 0) {
        return;
    }
    if (spu_tap_state == 0) {
        const char *dir = getenv("PC_SPU_TAP");
        int k;

        spu_tap_state = -1;
        if (dir == NULL || dir[0] == '\0') {
            return;
        }
        for (k = 0; k < PC_SPU_CHANNELS; k++) {
            char path[512];
            (void)snprintf(path, sizeof path, "%s/ch%02d.s16", dir, k);
            spu_tap[k] = fopen(path, "wb");
        }
        spu_tap_state = 1;
    }
    if (spu_tap[i] == NULL) {
        return;
    }
    /* A channel sample is up to sample * (1 << volshift) * volume, i.e.
     * 0x7FFF << 11 at full scale; keep the top 16 bits. */
    v >>= 11;
    if (v < -0x8000) v = -0x8000; else if (v > 0x7FFF) v = 0x7FFF;
    s = (int16_t)v;
    (void)fwrite(&s, 2, 1, spu_tap[i]);
}

void pc_spu_mix(int16_t out[2])
{
    /*
     * Half the interval, because the channel clock is half the system clock.
     * One sample spans PC_SPU_MIX_CYCLES = 1024 system-clock cycles, but
     * SOUNDxTMR counts at 16.756991 MHz (the system clock over two) so a
     * sample is 512 timer ticks. melonDS says the same thing in one line:
     * Its SPU event fires every MixInterval cycles and hands Mix() exactly
     * `MixInterval >> 1`. Passing the full 1024 here stepped every channel
     * and capture timer twice as fast as hardware, which transposed the
     * entire game up one octave, and the register-level instruments were
     * all blind to it, because the registers were right; only the tone that
     * came out of them was not. The oracle agreed because melon_host's
     * selftest drove melonDS's Mix() with the same doubled constant.
     */
    const uint32_t cycles = PC_SPU_MIX_CYCLES >> 1;
    int64_t left = 0, right = 0;
    int64_t leftout = 0, rightout = 0;
    int32_t ch0 = 0, ch1 = 0, ch3 = 0;

    if (spu_cnt & (1 << 15)) {
        int32_t ch2;
        int i;

        ch0 = chan_do_run(&chan[0], 0, cycles);
        ch1 = chan_do_run(&chan[1], 1, cycles);
        ch2 = chan_do_run(&chan[2], 2, cycles);
        ch3 = chan_do_run(&chan[3], 3, cycles);

        spu_tap_write(0, ch0);
        spu_tap_write(1, ch1);
        spu_tap_write(2, ch2);
        spu_tap_write(3, ch3);

        chan_pan_output(&chan[0], ch0, &left, &right);
        chan_pan_output(&chan[2], ch2, &left, &right);
        if (!(spu_cnt & (1 << 12))) chan_pan_output(&chan[1], ch1, &left, &right);
        if (!(spu_cnt & (1 << 13))) chan_pan_output(&chan[3], ch3, &left, &right);

        for (i = 4; i < PC_SPU_CHANNELS; i++) {
            int32_t v = chan_do_run(&chan[i], (unsigned)i, cycles);
            spu_tap_write(i, v);
            chan_pan_output(&chan[i], v, &left, &right);
        }

        /* Capture takes the mixer's output, eight bits down and clamped,
         * before master volume and before the bias, which is why it can be an
         * oracle for the mixer and cannot be one for the output stage. */
        if (cap[0].cnt & (1 << 7)) {
            int64_t v = left >> 8;
            if (v < -0x8000) v = -0x8000; else if (v > 0x7FFF) v = 0x7FFF;
            cap_run(&cap[0], cycles, (int32_t)v);
        }
        if (cap[1].cnt & (1 << 7)) {
            int64_t v = right >> 8;
            if (v < -0x8000) v = -0x8000; else if (v > 0x7FFF) v = 0x7FFF;
            cap_run(&cap[1], cycles, (int32_t)v);
        }

        switch (spu_cnt & 0x0300u) {
        case 0x0000: leftout = left; break;
        case 0x0100: leftout = ((int64_t)ch1 * (128 - chan[1].pan)) >> 10; break;
        case 0x0200: leftout = ((int64_t)ch3 * (128 - chan[3].pan)) >> 10; break;
        default:
            leftout = (((int64_t)ch1 * (128 - chan[1].pan)) >> 10)
                    + (((int64_t)ch3 * (128 - chan[3].pan)) >> 10);
            break;
        }
        switch (spu_cnt & 0x0C00u) {
        case 0x0000: rightout = right; break;
        case 0x0400: rightout = ((int64_t)ch1 * chan[1].pan) >> 10; break;
        case 0x0800: rightout = ((int64_t)ch3 * chan[3].pan) >> 10; break;
        default:
            rightout = (((int64_t)ch1 * chan[1].pan) >> 10)
                     + (((int64_t)ch3 * chan[3].pan) >> 10);
            break;
        }
    }

    leftout  = (leftout  * master_volume) >> 7;
    rightout = (rightout * master_volume) >> 7;
    leftout  >>= 8;
    rightout >>= 8;

    /* SOUNDBIAS. Every commercial title writes 0x200, which is exactly the
     * offset subtracted here, so the usual case is a no-op, but the register
     * is writable and the game's own SND_SetSoundBias reaches it. */
    leftout  += ((int64_t)spu_bias << 6) - 0x8000;
    rightout += ((int64_t)spu_bias << 6) - 0x8000;

    if (spu_mute) {
        out[0] = 0;
        out[1] = 0;
    } else {
        if (leftout  < -0x8000) leftout  = -0x8000; else if (leftout  > 0x7FFF) leftout  = 0x7FFF;
        if (rightout < -0x8000) rightout = -0x8000; else if (rightout > 0x7FFF) rightout = 0x7FFF;
        out[0] = (int16_t)leftout;
        out[1] = (int16_t)rightout;
    }

    spu_sample_count++;
}

void pc_spu_set_power(int on)
{
    spu_mute = !on;
}

/* ------------------------------------------------------------------ */
/* The latch                                                           */
/* ------------------------------------------------------------------ */

/*
 * The order below is the whole of what makes this work, so it is spelled out
 * rather than left to the loop. A channel's source address, timer, loop point
 * and length are replayed *before* its control word, because the control word's
 * bit 31 is the keyon and a channel started before its address is in place
 * would play whatever the previous sample was; SND_command.c's StartTimer sets
 * that bit as a separate byte store after SND_SetupChannelPcm has written the
 * other four, and the latch collapses both into one pass. The two capture
 * control bytes come last for the same reason.
 */
static const uint16_t latch_chan_lanes[4] = { 0x4, 0x8, 0xC, 0x0 };
static const uint16_t latch_global[7] = {
    0x500, 0x504, 0x510, 0x514, 0x518, 0x51C, 0x508
};

unsigned pc_spu_latch(void)
{
    /* pokeplatinum: the HLE driver (pc/arm7snd + pc_arm7snd.c) runs on the
     * ARM9's thread and writes its registers through plain absolute
     * addresses, which land in the LIVE page; there is no cpu-switched
     * ARM7 page in this port. The ARM9 side never stores to 0x400-0x51F
     * (its geometry writes go through the ioreg shadows into the staging
     * word), so the range is the SPU's alone. Diamond reads the suspended
     * ARM7 copy here instead; the paragraph below is its reasoning, kept
     * for provenance. */
    uint8_t *page = (uint8_t *)armrec_io_page(ARMREC_CPU_ARM9);
    uint8_t *io;
    unsigned replayed = 0;
    unsigned i;
    int k;

    if (page == NULL) {
        return 0;
    }
    /*
     * The ARM7's page, not the live mapping. This runs at the ARM9's frame
     * boundary, where 0x04000400 is the *geometry command FIFO*, the two
     * processors' I/O pages are separate hardware and
     * reading the live one here would latch matrix commands as channel control
     * words.
     */
    io = (uint8_t *)page + (PC_SPU_BASE - 0x04000000u);

#define LATCH_WORD(off)                                                       \
    do {                                                                      \
        uint32_t o_ = (uint32_t)(off);                                        \
        uint32_t now_, was_;                                                   \
        memcpy(&now_, io + o_, 4);                                            \
        memcpy(&was_, latch_shadow + o_, 4);                                  \
        if (now_ != was_) {                                                   \
            memcpy(latch_shadow + o_, &now_, 4);                              \
            pc_spu_write(PC_SPU_BASE + o_, now_, 4);                          \
            replayed++;                                                       \
        }                                                                     \
    } while (0)

    for (i = 0; i < PC_SPU_CHANNELS; i++) {
        for (k = 0; k < 4; k++) {
            LATCH_WORD(i * 0x10u + latch_chan_lanes[k]);
        }
    }
    for (k = 0; k < 7; k++) {
        LATCH_WORD(latch_global[k] - (PC_SPU_BASE - 0x04000000u));
    }
#undef LATCH_WORD

    /*
     * The named keyons; see keyon_pending above. After the replay, so the
     * channel's address, timer, loop point and length are already this
     * tick's; gated on the enable bit, because a note keyed on and off
     * inside one interval would have sounded for less than a scanline and
     * keying it now would play it late instead. A channel whose replayed
     * control word already produced the 0->1 edge just has its keyon set
     * twice, which is once.
     */
    if (keyon_pending != 0) {
        for (i = 0; i < PC_SPU_CHANNELS; i++) {
            if ((keyon_pending & (1u << i)) &&
                (chan[i].cnt & 0x80000000u)) {
                if (!chan[i].keyon) {
                    pc_spu_keyons[1]++;
                    pc_spu_trace_keyon(i, "note");
                }
                chan[i].keyon = 1;
            }
        }
        keyon_pending = 0;
    }

    /*
     * And back the other way, for the three fields hardware owns. Each
     * channel's control word: the write masks are the SPU's, and bit 31 is
     * cleared by the SPU itself when a one-shot sample runs out, which is what
     * SND_IsChannelActive() reads. Both capture control bytes: bit 7 clears the
     * same way at the end of a one-shot capture.
     *
     * The shadow is updated to match, or the next pass would read the port's
     * own write-back as a guest store and replay it, which for a channel
     * whose bit 31 the SPU had just cleared would be harmless, and for one it
     * had just set would be a spurious keyon.
     */
    for (i = 0; i < PC_SPU_CHANNELS; i++) {
        uint32_t cnt = chan[i].cnt;
        memcpy(io + i * 0x10u, &cnt, 4);
        memcpy(latch_shadow + i * 0x10u, &cnt, 4);
    }
    {
        uint32_t caps = (uint32_t)cap[0].cnt | ((uint32_t)cap[1].cnt << 8);
        uint32_t off = 0x508u - (PC_SPU_BASE - 0x04000000u);
        uint32_t keep;
        memcpy(&keep, io + off, 4);
        keep = (keep & 0xFFFF0000u) | caps;
        memcpy(io + off, &keep, 4);
        memcpy(latch_shadow + off, &keep, 4);
    }

    pc_spu_latched += replayed;
    return replayed;
}

/* ------------------------------------------------------------------ */
/* Reset and counters                                                  */
/* ------------------------------------------------------------------ */

void pc_spu_reset(void)
{
    memset(chan, 0, sizeof chan);
    memset(cap, 0, sizeof cap);
    spu_cnt = 0;
    master_volume = 0;
    /*
     * Exactly melonDS's SPU::Reset(), which is what --spu-selftest compares
     * against: bias zero and the mixer muted. The 0x200 bias and the unmute are
     * *SetupDirectBoot's*, i.e. the firmware's, and belong beside
     * pc/src/pc_firmware.c's other stand-ins, pc_main.c does them. A reset
     * that quietly did the firmware's job here would make this model and the
     * oracle disagree at step zero of every configuration.
     */
    spu_bias = 0;
    spu_mute = 1;
    spu_sample_count = 0;
    memset(pc_spu_unreadable, 0, sizeof pc_spu_unreadable);
    memset(pc_spu_unreadable_first, 0, sizeof pc_spu_unreadable_first);
    memset(pc_spu_unread_log, 0, sizeof pc_spu_unread_log);
    pc_spu_unread_log_n = 0;
    memset(pc_spu_keyons, 0, sizeof pc_spu_keyons);
    memset(pc_spu_src_log, 0, sizeof pc_spu_src_log);
    pc_spu_src_log_n = 0;
    pc_spu_src_log_dropped = 0;
    keyon_pending = 0;
    memset(latch_shadow, 0, sizeof latch_shadow);
}

uint64_t pc_spu_samples(void) { return spu_sample_count; }

/*
 * The latched register state, for pc/src/pc_diff.c.
 *
 * Straight out of the model rather than out of the I/O page: see the comment
 * on struct pc_spu_regs. Nothing here reads guest memory, so it is safe from
 * the ending hook, which is where pc_diff_at_ending() calls it from.
 */
void pc_spu_regs(struct pc_spu_regs *out)
{
    unsigned i;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    for (i = 0; i < PC_SPU_CHANNELS; i++) {
        out->chan[i].cnt = chan[i].cnt;
        out->chan[i].srcaddr = chan[i].srcaddr;
        out->chan[i].timer = chan[i].timer_reload;
        out->chan[i].looppos = chan[i].looppos;
        out->chan[i].length = chan[i].length;
    }
    for (i = 0; i < 2; i++) {
        out->cap[i].cnt = cap[i].cnt;
        out->cap[i].dstaddr = cap[i].dstaddr;
        out->cap[i].timer = cap[i].timer_reload;
        out->cap[i].length = cap[i].length;
    }
    out->soundcnt = spu_cnt;
    out->master_volume = master_volume;
    out->bias = spu_bias;
}
