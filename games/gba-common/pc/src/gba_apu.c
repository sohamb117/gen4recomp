/*
 * GBA sound output at 32768 Hz: the four Game Boy PSG channels, emulated
 * from their registers (square with sweep, square, wave, noise; length,
 * envelope and sweep clocked by a 512 Hz frame sequencer), plus DirectSound
 * A/B, which the m4a mixer (gba_m4a.c) feeds one frame of 8-bit samples at
 * a time in place of the FIFO DMA. Mixed per SOUNDCNT_L/H/X and scaled by
 * the host's volume options (NP_OPT_BGM_VOLUME for everything the
 * cartridge plays as music: the PSG and the BGM player's DirectSound).
 */
#include <string.h>

#include "gba_port.h"
#include "np_guest_abi.h"

void gba_audio_push(int16_t l, int16_t r);
uint32_t gba_option(uint32_t opt);

#define RATE 32768u
#define CYC_PER_SAMPLE 512u /* 16777216 / 32768 */

typedef struct {
    int on;
    uint32_t len;       /* length counter */
    int len_on;
    int vol, env_dir, env_period, env_timer;
    double phase;       /* in waveform steps */
    /* square */
    int duty;
    /* sweep (ch1) */
    int sw_period, sw_dir, sw_shift, sw_timer;
    uint32_t shadow;
    /* noise */
    uint32_t lfsr;
} psg_chan;

static psg_chan ch[4];
static uint8_t wave_bank[2][16];
static uint32_t seq_count, seq_step;
static uint64_t sample_acc; /* fractional output samples, 1/280896 units */

/* DirectSound queue: one frame of samples from the mixer at its rate */
#define DS_QUEUE 4096
static int8_t ds_r[DS_QUEUE], ds_l[DS_QUEUE];
static uint32_t ds_head, ds_tail, ds_rate = 13379;
static uint32_t ds_frac;
static int8_t ds_cur_r, ds_cur_l;

void gba_apu_reset(void) {
    memset(ch, 0, sizeof ch);
    memset(wave_bank, 0, sizeof wave_bank);
    seq_count = seq_step = 0;
    ds_head = ds_tail = ds_frac = 0;
    ds_cur_l = ds_cur_r = 0;
    ch[3].lfsr = 0x7FFF;
}

static uint32_t freq_of(int c) {
    return IO16(c == 0 ? 0x64 : c == 1 ? 0x6C : 0x74) & 0x7FF;
}

static void trigger(int c) {
    psg_chan *p = &ch[c];
    uint16_t envreg = IO16(c == 0 ? 0x62 : c == 1 ? 0x68 : 0x78);
    p->on = 1;
    if (c == 2) {
        if (p->len == 0) p->len = 256;
        p->phase = 0;
        return;
    }
    if (p->len == 0) p->len = 64;
    p->vol = envreg >> 12;
    p->env_dir = (envreg >> 11) & 1;
    p->env_period = (envreg >> 8) & 7;
    p->env_timer = p->env_period;
    if ((envreg & 0xF800) == 0) p->on = 0; /* DAC off */
    if (c == 3) p->lfsr = 0x7FFF;
    if (c == 0) {
        uint16_t sw = IO16(0x60);
        p->shadow = freq_of(0);
        p->sw_period = (sw >> 4) & 7;
        p->sw_dir = (sw >> 3) & 1;
        p->sw_shift = sw & 7;
        p->sw_timer = p->sw_period ? p->sw_period : 8;
    }
}

void gba_apu_write(uint32_t off, uint16_t v) {
    switch (off) {
    case 0x62: case 0x68: case 0x78: {
        int c = off == 0x62 ? 0 : off == 0x68 ? 1 : 3;
        ch[c].len = 64 - (v & 63);
        ch[c].duty = (v >> 6) & 3;
        if ((v & 0xF800) == 0) ch[c].on = 0;
        break;
    }
    case 0x72:
        ch[2].len = 256 - (v & 255);
        break;
    case 0x70:
        if (!(v & 0x80)) ch[2].on = 0;
        break;
    case 0x64: case 0x6C: case 0x74: case 0x7C: {
        int c = off == 0x64 ? 0 : off == 0x6C ? 1 : off == 0x74 ? 2 : 3;
        ch[c].len_on = (v >> 14) & 1;
        if (v & 0x8000) {
            trigger(c);
            IO16(off) = v & 0x7FFF;
        }
        break;
    }
    case 0x84:
        if (!(v & 0x80)) {
            for (int c = 0; c < 4; c++) ch[c].on = 0;
        }
        break;
    default:
        if (off >= R_WAVE_RAM && off < R_WAVE_RAM + 16) {
            int bank = (IO16(0x70) >> 6) & 1;
            memcpy(&wave_bank[bank ^ 1][off - R_WAVE_RAM], &v, 2);
        }
        break;
    }
}

static void clock_length(void) {
    for (int c = 0; c < 4; c++)
        if (ch[c].len_on && ch[c].len && --ch[c].len == 0) ch[c].on = 0;
}

static void clock_envelope(void) {
    for (int c = 0; c < 4; c++) {
        if (c == 2) continue;
        psg_chan *p = &ch[c];
        if (!p->env_period) continue;
        if (--p->env_timer > 0) continue;
        p->env_timer = p->env_period;
        if (p->env_dir && p->vol < 15) p->vol++;
        else if (!p->env_dir && p->vol > 0) p->vol--;
    }
}

static void clock_sweep(void) {
    psg_chan *p = &ch[0];
    if (--p->sw_timer > 0) return;
    p->sw_timer = p->sw_period ? p->sw_period : 8;
    if (!p->sw_period || !p->sw_shift) return;
    uint32_t delta = p->shadow >> p->sw_shift;
    uint32_t nf = p->sw_dir ? p->shadow - delta : p->shadow + delta;
    if (nf > 2047) {
        p->on = 0;
        return;
    }
    p->shadow = nf;
    IO16(0x64) = (uint16_t)((IO16(0x64) & ~0x7FF) | nf);
}

static const uint8_t k_duty[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 0}};

/* one PSG channel's output for this sample, -15..15 */
static int psg_sample(int c) {
    psg_chan *p = &ch[c];
    if (!p->on) return 0;
    if (c < 2) {
        uint32_t f = freq_of(c);
        double period = (2048.0 - f) * 16.0; /* cycles per duty step */
        p->phase += CYC_PER_SAMPLE / period;
        while (p->phase >= 8.0) p->phase -= 8.0;
        int high = k_duty[p->duty][(int)p->phase];
        return high ? p->vol : -p->vol;
    }
    if (c == 2) {
        uint16_t cnt = IO16(0x70), h = IO16(0x72);
        double period = (2048.0 - freq_of(2)) * 8.0; /* cycles per 4-bit sample */
        int len = (cnt & 0x20) ? 64 : 32;
        p->phase += CYC_PER_SAMPLE / period;
        while (p->phase >= len) p->phase -= len;
        int idx = (int)p->phase;
        int bank = (cnt >> 6) & 1;
        if (len == 64) bank = idx >= 32 ? bank ^ 1 : bank;
        idx &= 31;
        uint8_t b = wave_bank[bank][idx >> 1];
        int s = ((idx & 1) ? (b & 15) : (b >> 4)) * 2 - 15;
        int v = (h >> 13) & 3;
        if (h & 0x8000) return s * 3 / 4;
        return v == 0 ? 0 : s / (1 << (v - 1));
    }
    uint16_t n = IO16(0x7C);
    double r = (n & 7) ? (double)(n & 7) : 0.5;
    double freq = 524288.0 / r / (double)(2u << ((n >> 4) & 15));
    p->phase += freq / RATE;
    while (p->phase >= 1.0) {
        p->phase -= 1.0;
        uint32_t bit = (p->lfsr ^ (p->lfsr >> 1)) & 1;
        p->lfsr = (p->lfsr >> 1) | (bit << 14);
        if (n & 8) p->lfsr = (p->lfsr & ~0x40u) | (bit << 6);
    }
    return (p->lfsr & 1) ? -p->vol : p->vol;
}

void gba_apu_pcm(const int8_t *right, const int8_t *left, uint32_t n, uint32_t rate) {
    if (rate) ds_rate = rate;
    for (uint32_t i = 0; i < n; i++) {
        if (ds_head - ds_tail >= DS_QUEUE) ds_tail++; /* overrun: drop the oldest */
        ds_r[ds_head % DS_QUEUE] = right[i];
        ds_l[ds_head % DS_QUEUE] = left[i];
        ds_head++;
    }
}

static int16_t clamp16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* the frame's worth of output (280896 cycles = 548.6 samples) */
void gba_apu_frame(void) {
    sample_acc += 280896u;
    uint32_t n = (uint32_t)(sample_acc / CYC_PER_SAMPLE);
    sample_acc -= (uint64_t)n * CYC_PER_SAMPLE;
    uint16_t cl = IO16(R_SOUNDCNT_L), chh = IO16(R_SOUNDCNT_H), cx = IO16(R_SOUNDCNT_X);
    int32_t vol = (int32_t)gba_option(NP_OPT_BGM_VOLUME);
    if (vol > 256) vol = 256;
    for (uint32_t i = 0; i < n; i++) {
        if (++seq_count >= RATE / 512) {
            seq_count = 0;
            seq_step = (seq_step + 1) & 7;
            if (!(seq_step & 1)) clock_length();
            if (seq_step == 2 || seq_step == 6) clock_sweep();
            if (seq_step == 7) clock_envelope();
        }
        ds_frac += ds_rate;
        while (ds_frac >= RATE) {
            ds_frac -= RATE;
            if (ds_tail != ds_head) {
                ds_cur_r = ds_r[ds_tail % DS_QUEUE];
                ds_cur_l = ds_l[ds_tail % DS_QUEUE];
                ds_tail++;
            }
        }
        int32_t pl = 0, pr = 0;
        for (int c = 0; c < 4; c++) {
            int s = psg_sample(c);
            if (cl & (0x100 << c)) pr += s;
            if (cl & (0x1000 << c)) pl += s;
        }
        static const int k_ratio[4] = {1, 2, 4, 4};
        int ratio = k_ratio[chh & 3];
        pr = pr * (int32_t)((cl & 7) + 1) * ratio / 4;
        pl = pl * (int32_t)(((cl >> 4) & 7) + 1) * ratio / 4;
        int32_t a = ds_cur_r * ((chh & 4) ? 2 : 1), b = ds_cur_l * ((chh & 8) ? 2 : 1);
        int32_t outr = pr, outl = pl;
        if (chh & 0x100) outr += a;
        if (chh & 0x200) outl += a;
        if (chh & 0x1000) outr += b;
        if (chh & 0x2000) outl += b;
        if (!(cx & 0x80)) outr = outl = 0;
        gba_audio_push(clamp16(outl * 64 * vol / 256), clamp16(outr * 64 * vol / 256));
    }
}
