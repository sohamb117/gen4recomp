/*
 * The GBA BIOS calls the decomps make (libagbsyscall's SWI wrappers), as C:
 * the same names, so the bridge gives them the cartridge's addresses.
 *
 * ArcTan/ArcTan2 and the sine table behind BgAffineSet/ObjAffineSet follow
 * the BIOS algorithms as GBATEK and mGBA (src/gba/bios.c, MPL-2.0) document
 * them, so angles come out exactly as on the console.
 */
#include <math.h>
#include <string.h>

#include "gba_port.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;

/* ------------------------------------------------------------ resets */

void RegisterRamReset(u32 flags) {
    if (flags & 0x01) memset(GBA_PTR(GBA_EWRAM), 0, 0x40000);
    if (flags & 0x02) memset(GBA_PTR(GBA_IWRAM), 0, 0x7E00);
    if (flags & 0x04) memset(GBA_PTR(GBA_PAL), 0, 0x400);
    if (flags & 0x08) memset(GBA_PTR(GBA_VRAM), 0, 0x18000);
    if (flags & 0x10) memset(GBA_PTR(GBA_OAM), 0, 0x400);
    if (flags & 0x20) {
        IO16(R_RCNT) = 0x8000;
        IO16(R_SIOCNT) = 0;
    }
    if (flags & 0x40) {
        memset(GBA_PTR(GBA_IO + R_SOUND1CNT_L), 0, 0x48);
        IO16(R_SOUNDBIAS) = 0x200;
        gba_apu_reset();
    }
    if (flags & 0x80) {
        uint16_t keep_ime = IO16(R_IME);
        memset(GBA_PTR(GBA_IO), 0, 0x60);
        memset(GBA_PTR(GBA_IO + 0xB0), 0, 0x100 - 0xB0 + 0x10);
        IO16(R_BG2PA) = IO16(R_BG2PA + 6) = 0x100;
        IO16(R_BG3PA) = IO16(R_BG3PA + 6) = 0x100;
        IO16(R_IE) = 0;
        IO16(R_IF) = 0;
        IO16(R_IME) = keep_ime & 0;
    }
    IO16(R_DISPCNT) = 0x80;
}

void SoftReset(u32 flags) {
    RegisterRamReset(flags);
    memset(GBA_PTR(0x03007E00u), 0, 0x200);
    gba_soft_reset();
}

void SoftResetRom(u32 flags) { SoftReset(flags); }
void SoftResetExram(u32 flags) { SoftReset(flags); }

/* -------------------------------------------------------- interrupts */

void VBlankIntrWait(void) { gba_wait_irq(1, 1); }
void IntrWait(u32 discard, u32 flags) { gba_wait_irq((u16)flags, (int)discard); }
void Halt(void) { gba_step_line(); }

/* --------------------------------------------------------------- math */

s32 Div(s32 num, s32 denom) {
    if (denom == 0) return num < 0 ? -1 : 1;
    if (denom == -1 && num == INT32_MIN) return INT32_MIN;
    return num / denom;
}

s32 Mod(s32 num, s32 denom) {
    if (denom == 0) return num;
    if (denom == -1) return 0;
    return num % denom;
}

s32 DivArm(s32 denom, s32 num) { return Div(num, denom); }
s32 ModArm(s32 denom, s32 num) { return Mod(num, denom); }

u16 Sqrt(u32 num) {
    u32 r = 0, bit = 1u << 30;
    while (bit > num) bit >>= 2;
    while (bit) {
        if (num >= r + bit) {
            num -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (u16)r;
}

static s32 arctan_core(s32 i) {
    s32 a = -((i * i) >> 14);
    s32 b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (i * b) >> 16;
}

u16 ArcTan(s16 tan) { return (u16)arctan_core(tan); }

u16 ArcTan2(s16 x, s16 y) {
    s32 X = x, Y = y;
    if (!Y) return X >= 0 ? 0 : 0x8000;
    if (!X) return Y >= 0 ? 0x4000 : 0xC000;
    if (Y >= 0) {
        if (X >= 0) {
            if (X >= Y) return (u16)arctan_core((Y << 14) / X);
        } else if (-X >= Y) {
            return (u16)(arctan_core((Y << 14) / X) + 0x8000);
        }
        return (u16)(0x4000 - arctan_core((X << 14) / Y));
    }
    if (X <= 0) {
        if (-X > -Y) return (u16)(arctan_core((Y << 14) / X) + 0x8000);
    } else if (X >= -Y) {
        return (u16)(arctan_core((Y << 14) / X) + 0x10000);
    }
    return (u16)(0xC000 - arctan_core((X << 14) / Y));
}

/* sin(i * 2pi / 256) in 1.14, the table the BIOS affine calls use */
static s16 sine(u32 i) {
    static s16 table[256];
    static int ready;
    if (!ready) {
        for (int k = 0; k < 256; k++) table[k] = (s16)lround(sin(k * 3.14159265358979323846 / 128.0) * 16384.0);
        ready = 1;
    }
    return table[i & 255];
}

void BgAffineSet(void *srcp, void *dstp, s32 count) {
    u8 *src = srcp, *dst = dstp;
    for (s32 n = 0; n < count; n++, src += 20, dst += 16) {
        s32 texX, texY;
        s16 scrX, scrY, sx, sy;
        u16 alpha;
        memcpy(&texX, src, 4);
        memcpy(&texY, src + 4, 4);
        memcpy(&scrX, src + 8, 2);
        memcpy(&scrY, src + 10, 2);
        memcpy(&sx, src + 12, 2);
        memcpy(&sy, src + 14, 2);
        memcpy(&alpha, src + 16, 2);
        s32 c = sine((alpha >> 8) + 64), s = sine(alpha >> 8);
        s16 pa = (s16)((sx * c) >> 14), pb = (s16)(-(sx * s) >> 14);
        s16 pc = (s16)((sy * s) >> 14), pd = (s16)((sy * c) >> 14);
        s32 dx = texX - (pa * scrX + pb * scrY), dy = texY - (pc * scrX + pd * scrY);
        memcpy(dst, &pa, 2);
        memcpy(dst + 2, &pb, 2);
        memcpy(dst + 4, &pc, 2);
        memcpy(dst + 6, &pd, 2);
        memcpy(dst + 8, &dx, 4);
        memcpy(dst + 12, &dy, 4);
    }
}

void ObjAffineSet(void *srcp, void *dstp, s32 count, s32 offset) {
    u8 *src = srcp, *dst = dstp;
    for (s32 n = 0; n < count; n++, src += 8) {
        s16 sx, sy;
        u16 alpha;
        memcpy(&sx, src, 2);
        memcpy(&sy, src + 2, 2);
        memcpy(&alpha, src + 4, 2);
        s32 c = sine((alpha >> 8) + 64), s = sine(alpha >> 8);
        s16 v[4] = {(s16)((sx * c) >> 14), (s16)(-(sx * s) >> 14), (s16)((sy * s) >> 14), (s16)((sy * c) >> 14)};
        for (int k = 0; k < 4; k++) {
            memcpy(dst, &v[k], 2);
            dst += offset;
        }
    }
}

/* ------------------------------------------------------------ copies */

void CpuSet(const void *src, void *dst, u32 ctl) {
    u32 n = ctl & 0x1FFFFF;
    int fixed = (ctl >> 24) & 1;
    if (ctl & (1u << 26)) {
        const u32 *s = (const u32 *)((uintptr_t)src & ~3u);
        u32 *d = (u32 *)((uintptr_t)dst & ~3u);
        for (u32 i = 0; i < n; i++) d[i] = fixed ? s[0] : s[i];
    } else {
        const u16 *s = (const u16 *)((uintptr_t)src & ~1u);
        u16 *d = (u16 *)((uintptr_t)dst & ~1u);
        for (u32 i = 0; i < n; i++) d[i] = fixed ? s[0] : s[i];
    }
}

void CpuFastSet(const void *src, void *dst, u32 ctl) {
    u32 n = ((ctl & 0x1FFFFF) + 7) & ~7u;
    int fixed = (ctl >> 24) & 1;
    const u32 *s = (const u32 *)((uintptr_t)src & ~3u);
    u32 *d = (u32 *)((uintptr_t)dst & ~3u);
    for (u32 i = 0; i < n; i++) d[i] = fixed ? s[0] : s[i];
}

/* ---------------------------------------------------- decompression */

static void lz77(const void *srcp, void *dstp) {
    const u8 *s = srcp;
    u8 *d = dstp;
    u32 size = (s[1] | s[2] << 8 | s[3] << 16);
    s += 4;
    u32 out = 0;
    while (out < size) {
        u8 flags = *s++;
        for (int b = 0; b < 8 && out < size; b++, flags <<= 1) {
            if (flags & 0x80) {
                u32 len = (s[0] >> 4) + 3, disp = ((s[0] & 15) << 8 | s[1]) + 1;
                s += 2;
                for (u32 k = 0; k < len && out < size; k++, out++) d[out] = d[out - disp];
            } else {
                d[out++] = *s++;
            }
        }
    }
}

void LZ77UnCompWram(const void *src, void *dst) { lz77(src, dst); }
void LZ77UnCompVram(const void *src, void *dst) { lz77(src, dst); }

static void rl(const void *srcp, void *dstp) {
    const u8 *s = srcp;
    u8 *d = dstp;
    u32 size = (s[1] | s[2] << 8 | s[3] << 16), out = 0;
    s += 4;
    while (out < size) {
        u8 f = *s++;
        if (f & 0x80) {
            u32 len = (f & 0x7F) + 3;
            u8 v = *s++;
            for (u32 k = 0; k < len && out < size; k++) d[out++] = v;
        } else {
            u32 len = (f & 0x7F) + 1;
            for (u32 k = 0; k < len && out < size; k++) d[out++] = *s++;
        }
    }
}

void RLUnCompWram(const void *src, void *dst) { rl(src, dst); }
void RLUnCompVram(const void *src, void *dst) { rl(src, dst); }

void HuffUnComp(const void *srcp, void *dstp) {
    const u8 *s = srcp;
    u32 hdr = s[0] | s[1] << 8 | s[2] << 16 | (u32)s[3] << 24;
    u32 bits = hdr & 15, size = hdr >> 8;
    const u8 *tree = s + 4;
    const u32 *stream = (const u32 *)(uintptr_t)(((uintptr_t)tree + (tree[0] + 1) * 2 + 3) & ~(uintptr_t)3);
    u32 *d = dstp;
    u32 acc = 0, accbits = 0, outw = 0;
    const u8 *node = tree + 1;
    while (outw * 4 < size) {
        u32 word = *stream++;
        for (int b = 31; b >= 0 && outw * 4 < size; b--) {
            int bit = (word >> b) & 1;
            u8 n = *node;
            const u8 *child = (const u8 *)(((uintptr_t)node & ~(uintptr_t)1) + ((n & 0x3F) + 1) * 2) + bit;
            if (n & (bit ? 0x40 : 0x80)) {
                acc |= (u32)*child << accbits;
                accbits += bits;
                node = tree + 1;
                if (accbits == 32) {
                    d[outw++] = acc;
                    acc = 0;
                    accbits = 0;
                }
            } else {
                node = child;
            }
        }
    }
}

void BitUnPack(const void *srcp, void *dstp, const void *infop) {
    const u8 *info = infop;
    u16 len = (u16)(info[0] | info[1] << 8);
    u8 sw = info[2], dw = info[3];
    u32 off = info[4] | info[5] << 8 | info[6] << 16 | (u32)info[7] << 24;
    int zero = off >> 31;
    off &= 0x7FFFFFFF;
    const u8 *s = srcp;
    u32 *d = dstp, acc = 0, accbits = 0;
    for (u32 i = 0; i < len; i++) {
        u8 byte = s[i];
        for (u32 b = 0; b < 8; b += sw) {
            u32 v = (byte >> b) & ((1u << sw) - 1);
            if (v || zero) v += off;
            acc |= v << accbits;
            accbits += dw;
            if (accbits >= 32) {
                *d++ = acc;
                acc = 0;
                accbits = 0;
            }
        }
    }
}

void Diff8bitUnFilterWram(const void *srcp, void *dstp) {
    const u8 *s = srcp;
    u8 *d = dstp;
    u32 size = s[1] | s[2] << 8 | s[3] << 16;
    u8 v = 0;
    for (u32 i = 0; i < size; i++) d[i] = v = (u8)(v + s[4 + i]);
}

void Diff8bitUnFilterVram(const void *src, void *dst) { Diff8bitUnFilterWram(src, dst); }

void Diff16bitUnFilter(const void *srcp, void *dstp) {
    const u8 *s = srcp;
    u16 *d = dstp;
    u32 size = s[1] | s[2] << 8 | s[3] << 16;
    u16 v = 0;
    for (u32 i = 0; i < size / 2; i++) d[i] = v = (u16)(v + (s[4 + 2 * i] | s[5 + 2 * i] << 8));
}

/* ------------------------------------------------------------- misc */

int MultiBoot(void *mp) {
    (void)mp;
    return 1; /* no link cable partner */
}

void SoundBiasReset(void) {}
void SoundBiasSet(void) {}
void SoundBiasChange(u32 v) { (void)v; }

u32 MidiKey2Freq(void *wa, u8 mk, u8 fp) {
    /* the BIOS: freq = wa->freq * 2^((mk - 60 + fp/256) / 12) */
    u32 base;
    memcpy(&base, (u8 *)wa + 4, 4);
    double f = (double)base * pow(2.0, ((double)mk - 180.0 + (double)fp / 256.0) / 12.0);
    return (u32)f;
}
