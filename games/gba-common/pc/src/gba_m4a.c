/*
 * The MusicPlayer2000 (m4a/MP2K) sound driver's assembly half, m4a_1.s, as
 * C: the sequencer (MPlayMain and the ply_* track commands), the channel
 * allocator (ply_note), and the DirectSound mixer behind SoundMain, which
 * fills the game's PCM double buffer exactly as the ARM routine does
 * (8-bit wrapping sums, envelope steps once per frame, linear interpolation
 * in 9.23 fixed point) and hands each frame's samples to the APU
 * (gba_apu.c) in place of the FIFO DMA.
 *
 * Written from the decomps' m4a_1.s; the C half of the driver (m4a.c) is
 * compiled from the decomp unchanged and calls into this one.
 */
#include <string.h>

#include "global.h"
#define SoundMainBTM SoundMainBTM_decl /* declared (void); m4a.c calls it with a pointer */
#include "gba/m4a_internal.h"
#undef SoundMainBTM
#include "gba_port.h"
#include "np_guest_abi.h"

#define SI (*(struct SoundInfo **)0x3007FF0)
#ifndef TONEDATA_TYPE_CMP
#define TONEDATA_TYPE_CMP 0x20 /* compressed (DPCM) sample */
#define TONEDATA_TYPE_REV 0x10 /* played backwards */
#endif

extern void *const gMPlayJumpTableTemplate[];
extern const u8 gClockTable[];
extern const s8 gDeltaEncodingTable[];

/* statusFlags bit the ARM mixer sets once a compressed/reversed voice's
 * position became a sample index (cleared again by a note start) */
#define SOUND_CHANNEL_SF_SPECIAL 0x20

void gba_apu_pcm(const s8 *right, const s8 *left, u32 n, u32 rate);
void gba_vstore8(void *p, u8 v);
uint32_t gba_option(uint32_t opt);

u32 MidiKeyToFreq(struct WaveData *wav, u8 key, u8 fineAdjust);

u32 umul3232H32(u32 a, u32 b) { return (u32)(((u64)a * b) >> 32); }

/* Clear64byte's worker (jump table entry 35): zero 64 bytes */
void SoundMainBTM(void *p) { memset(p, 0, 64); }

/* ------------------------------------------------------------- chains */

void RealClearChain(void *x) {
    struct SoundChannel *c = x;
    struct MusicPlayerTrack *t = c->track;
    if (!t) return;
    struct SoundChannel *next = c->nextChannelPointer, *prev = c->prevChannelPointer;
    if (prev)
        prev->nextChannelPointer = next;
    else
        t->chan = next;
    if (next) next->prevChannelPointer = prev;
    c->track = NULL;
}

static void clear_modM(struct MusicPlayerTrack *t) {
    t->modM = 0;
    t->lfoSpeedC = 0;
    t->flags |= t->modT == 0 ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
}

static u8 next_byte(struct MusicPlayerTrack *t) { return *t->cmdPtr++; }

/* ----------------------------------------------------- track commands */

void ply_fine(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    for (struct SoundChannel *c = t->chan; c; c = c->nextChannelPointer) {
        if (c->statusFlags & SOUND_CHANNEL_SF_ON) c->statusFlags |= SOUND_CHANNEL_SF_STOP;
        RealClearChain(c);
    }
    t->flags = 0;
}

void MPlayJumpTableCopy(MPlayFunc *table) {
    for (int i = 0; i < 36; i++) table[i] = (MPlayFunc)gMPlayJumpTableTemplate[i];
}

static void goto_at(struct MusicPlayerTrack *t) {
    u8 *p = t->cmdPtr;
    t->cmdPtr = (u8 *)(uintptr_t)(p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24);
}

void ply_goto(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    goto_at(t);
}

void ply_patt(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    if (t->patternLevel >= 3) {
        ply_fine(mp, t);
        return;
    }
    t->patternStack[t->patternLevel] = t->cmdPtr + 4;
    t->patternLevel++;
    goto_at(t);
}

void ply_pend(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    if (t->patternLevel) {
        t->patternLevel--;
        t->cmdPtr = t->patternStack[t->patternLevel];
    }
}

void ply_rept(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    u8 *p = t->cmdPtr;
    if (*p == 0) {
        t->cmdPtr = p + 1;
        goto_at(t);
        return;
    }
    u8 n = ++t->repN;
    t->cmdPtr = p + 1;
    if (n < *p) {
        goto_at(t);
        return;
    }
    t->repN = 0;
    t->cmdPtr = p + 5;
}

void ply_prio(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    t->priority = next_byte(t);
}

void ply_tempo(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    u32 v = (u32)next_byte(t) << 1;
    mp->tempoD = (u16)v;
    mp->tempoI = (u16)((v * mp->tempoU) >> 8);
}

void ply_keysh(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    t->keyShift = (s8)next_byte(t);
    t->flags |= MPT_FLG_PITCHG;
}

void ply_voice(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    u8 i = next_byte(t);
    memcpy(&t->tone, &mp->tone[i], sizeof(struct ToneData));
}

void ply_vol(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    t->vol = next_byte(t);
    t->flags |= MPT_FLG_VOLCHG;
}

void ply_pan(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    t->pan = (s8)(next_byte(t) - C_V);
    t->flags |= MPT_FLG_VOLCHG;
}

void ply_bend(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    t->bend = (s8)(next_byte(t) - C_V);
    t->flags |= MPT_FLG_PITCHG;
}

void ply_bendr(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    t->bendRange = next_byte(t);
    t->flags |= MPT_FLG_PITCHG;
}

void ply_lfodl(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    t->lfoDelay = next_byte(t);
}

void ply_modt(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    u8 v = next_byte(t);
    if (t->modT != v) {
        t->modT = v;
        t->flags |= MPT_FLG_VOLCHG | MPT_FLG_PITCHG;
    }
}

void ply_tune(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    t->tune = (s8)(next_byte(t) - C_V);
    t->flags |= MPT_FLG_PITCHG;
}

void ply_port(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    u8 reg = next_byte(t);
    u8 val = next_byte(t);
    gba_vstore8((void *)(uintptr_t)(0x04000060u + reg), val);
}

void ply_lfos(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    t->lfoSpeed = next_byte(t);
    if (!t->lfoSpeed) clear_modM(t);
}

void ply_mod(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    t->mod = next_byte(t);
    if (!t->mod) clear_modM(t);
}

void ply_endtie(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    u8 key;
    if (*t->cmdPtr < 0x80) {
        key = *t->cmdPtr++;
        t->key = key;
    } else {
        key = t->key;
    }
    for (struct SoundChannel *c = t->chan; c; c = c->nextChannelPointer) {
        u8 f = c->statusFlags;
        if ((f & (SOUND_CHANNEL_SF_START | SOUND_CHANNEL_SF_ENV)) && !(f & SOUND_CHANNEL_SF_STOP) &&
            c->midiKey == key) {
            c->statusFlags = f | SOUND_CHANNEL_SF_STOP;
            return;
        }
    }
}

/* -------------------------------------------------------------- notes */

static void chn_vol_set(struct SoundChannel *c, struct MusicPlayerTrack *t) {
    s32 rp = (s8)c->rhythmPan;
    s32 r = ((0x80 + rp) * c->velocity * t->volMR) >> 14;
    s32 l = ((0x7F - rp) * c->velocity * t->volML) >> 14;
    c->rightVolume = (u8)(r > 0xFF ? 0xFF : r);
    c->leftVolume = (u8)(l > 0xFF ? 0xFF : l);
}

void ChnVolSetAsm(void) {}

void TrackStop(struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    (void)mp;
    if (!(t->flags & MPT_FLG_EXIST)) return;
    struct SoundChannel *c = t->chan;
    for (; c; c = c->nextChannelPointer) {
        if (c->statusFlags) {
            if (c->type & TONEDATA_TYPE_CGB) SI->CgbOscOff(c->type & TONEDATA_TYPE_CGB);
            c->statusFlags = 0;
        }
        c->track = NULL;
    }
    t->chan = NULL;
}

void ply_note(u32 idx, struct MusicPlayerInfo *mp, struct MusicPlayerTrack *t) {
    struct SoundInfo *si = SI;
    t->gateTime = gClockTable[idx];
    u8 *p = t->cmdPtr;
    if (p[0] < 0x80) {
        t->key = *p++;
        if (p[0] < 0x80) {
            t->velocity = *p++;
            if (p[0] < 0x80) t->gateTime += *p++;
        }
        t->cmdPtr = p;
    }
    s32 rhythm_pan = 0;
    struct ToneData *tone = &t->tone, *td;
    u8 key = t->key;
    if (tone->type & (TONEDATA_TYPE_RHY | TONEDATA_TYPE_SPL)) {
        u8 k = key;
        if (tone->type & TONEDATA_TYPE_SPL) {
            const u8 *split; /* a key-split voice keeps its table where attack.. sits */
            memcpy(&split, &tone->attack, sizeof split);
            k = split[key];
        }
        td = (struct ToneData *)tone->wav + k;
        if (td->type & (TONEDATA_TYPE_SPL | TONEDATA_TYPE_RHY)) return;
        if (tone->type & TONEDATA_TYPE_RHY) {
            if (td->pan_sweep & 0x80) rhythm_pan = (s32)(td->pan_sweep - TONEDATA_P_S_PAN) << 1;
            key = td->key;
        }
    } else {
        td = tone;
    }
    u32 prio = (u32)mp->priority + t->priority;
    if (prio > 0xFF) prio = 0xFF;
    u32 cgb = td->type & TONEDATA_TYPE_CGB;
    struct SoundChannel *c;
    if (cgb) {
        if (!si->cgbChans) return;
        c = (struct SoundChannel *)&si->cgbChans[cgb - 1];
        if ((c->statusFlags & SOUND_CHANNEL_SF_ON) && !(c->statusFlags & SOUND_CHANNEL_SF_STOP)) {
            if (c->priority > prio) return;
            if (c->priority == prio && (uintptr_t)c->track < (uintptr_t)t) return;
        }
    } else {
        u32 best_prio = prio;
        struct MusicPlayerTrack *best_track = t;
        int stopping = 0;
        c = NULL;
        struct SoundChannel *ch = si->chans;
        for (int n = si->maxChans; n > 0; n--, ch++) {
            u8 f = ch->statusFlags;
            if (!(f & SOUND_CHANNEL_SF_ON)) {
                c = ch;
                goto take;
            }
            if (f & SOUND_CHANNEL_SF_STOP) {
                if (!stopping) {
                    stopping = 1;
                    best_prio = ch->priority;
                    best_track = ch->track;
                    c = ch;
                    continue;
                }
            } else if (stopping) {
                continue;
            }
            if (ch->priority < best_prio) {
                best_prio = ch->priority;
                best_track = ch->track;
                c = ch;
            } else if (ch->priority == best_prio) {
                if ((uintptr_t)ch->track > (uintptr_t)best_track) {
                    best_track = ch->track;
                    c = ch;
                } else if (ch->track == best_track) {
                    c = ch;
                }
            }
        }
        if (!c) return;
    }
take:
    ClearChain(c);
    c->prevChannelPointer = NULL;
    c->nextChannelPointer = t->chan;
    if (t->chan) t->chan->prevChannelPointer = c;
    t->chan = c;
    c->track = t;
    t->lfoDelayC = t->lfoDelay;
    if (t->lfoDelay) clear_modM(t);
    TrkVolPitSet(mp, t);
    c->gateTime = t->gateTime;
    c->midiKey = t->key;
    c->velocity = t->velocity;
    c->priority = (u8)prio;
    c->key = key;
    c->rhythmPan = (u8)rhythm_pan;
    c->type = td->type;
    c->wav = td->wav;
    c->attack = td->attack;
    c->decay = td->decay;
    c->sustain = td->sustain;
    c->release = td->release;
    c->pseudoEchoVolume = t->pseudoEchoVolume;
    c->pseudoEchoLength = t->pseudoEchoLength;
    chn_vol_set(c, t);
    s32 k = (s32)c->key + (s8)t->keyM;
    if (k < 0) k = 0;
    u32 freq;
    if (cgb) {
        struct CgbChannel *g = (struct CgbChannel *)c;
        g->length = td->length;
        u8 ps = td->pan_sweep;
        if ((ps & 0x80) || !(ps & 0x70)) ps = 8;
        g->sweep = ps;
        freq = si->MidiKeyToCgbFreq((u8)cgb, (u8)k, t->pitM);
    } else {
        c->count = t->unk_3C;
        freq = MidiKeyToFreq(td->wav, (u8)k, t->pitM);
    }
    c->frequency = freq;
    c->statusFlags = SOUND_CHANNEL_SF_START;
    t->flags &= 0xF0;
}

/* ---------------------------------------------------------- sequencer */

void MPlayMain(struct MusicPlayerInfo *mp) {
    if (mp->ident != ID_NUMBER) return;
    mp->ident++;
    if (mp->MPlayMainNext) mp->MPlayMainNext(mp->musicPlayerNext);
    struct SoundInfo *si = SI;
    if ((s32)mp->status < 0) goto done;
    FadeOutBody(mp);
    if ((s32)mp->status < 0) goto done;

    u16 tc = (u16)(mp->tempoC + mp->tempoI);
    for (;;) {
        mp->tempoC = tc;
        if (tc < 150) break;
        u32 active = 0;
        struct MusicPlayerTrack *t = mp->tracks;
        for (u32 n = mp->trackCount, bit = 1; n > 0; n--, t++, bit <<= 1) {
            if (!(t->flags & MPT_FLG_EXIST)) continue;
            active |= bit;
            for (struct SoundChannel *c = t->chan; c;) {
                struct SoundChannel *next;
                if (c->statusFlags & SOUND_CHANNEL_SF_ON) {
                    if (c->gateTime && --c->gateTime == 0) c->statusFlags |= SOUND_CHANNEL_SF_STOP;
                } else {
                    ClearChain(c);
                }
                next = c->nextChannelPointer;
                c = next;
            }
            if (t->flags & MPT_FLG_START) {
                Clear64byte(t);
                t->flags = MPT_FLG_EXIST;
                t->bendRange = 2;
                t->volX = 0x40;
                t->lfoSpeed = 0x16;
                t->tone.type = 1;
            }
            int stopped = 0;
            while (t->wait == 0) {
                u8 cmd = *t->cmdPtr;
                if (cmd < 0x80) {
                    cmd = t->runningStatus;
                } else {
                    t->cmdPtr++;
                    if (cmd >= 0xBD) t->runningStatus = cmd;
                }
                if (cmd >= 0xCF) {
                    si->plynote(cmd - 0xCF, mp, t);
                } else if (cmd > 0xB0) {
                    mp->cmd = cmd - 0xB1;
                    si->MPlayJumpTable[mp->cmd](mp, t);
                    if (t->flags == 0) {
                        stopped = 1;
                        break;
                    }
                } else {
                    t->wait = gClockTable[cmd - 0x80];
                }
            }
            if (stopped) continue;
            t->wait--;
            if (t->lfoSpeed && t->mod) {
                if (t->lfoDelayC) {
                    t->lfoDelayC--;
                } else {
                    t->lfoSpeedC += t->lfoSpeed;
                    u8 s = t->lfoSpeedC;
                    s32 v = (u8)(s - 0x40) < 0x80 ? 0x80 - (s32)s : (s8)s;
                    v = (t->mod * v) >> 6;
                    if ((u8)t->modM != (u8)v) {
                        t->modM = (s8)v;
                        t->flags |= t->modT == 0 ? MPT_FLG_PITCHG : MPT_FLG_VOLCHG;
                    }
                }
            }
        }
        mp->clock++;
        if (!active) {
            mp->status = MUSICPLAYER_STATUS_PAUSE;
            goto done;
        }
        mp->status = active;
        tc = (u16)(mp->tempoC - 150);
    }

    {
        struct MusicPlayerTrack *t = mp->tracks;
        for (u32 n = mp->trackCount; n > 0; n--, t++) {
            if (!(t->flags & MPT_FLG_EXIST) || !(t->flags & (MPT_FLG_VOLCHG | MPT_FLG_PITCHG))) continue;
            TrkVolPitSet(mp, t);
            for (struct SoundChannel *c = t->chan; c;) {
                struct SoundChannel *next = c->nextChannelPointer;
                if (!(c->statusFlags & SOUND_CHANNEL_SF_ON)) {
                    ClearChain(c);
                    c = next;
                    continue;
                }
                u32 cgb = c->type & TONEDATA_TYPE_CGB;
                if (t->flags & MPT_FLG_VOLCHG) {
                    chn_vol_set(c, t);
                    if (cgb) ((struct CgbChannel *)c)->modify |= CGB_CHANNEL_MO_VOL;
                }
                if (t->flags & MPT_FLG_PITCHG) {
                    s32 k = (s32)c->key + (s8)t->keyM;
                    if (k < 0) k = 0;
                    if (cgb) {
                        ((struct CgbChannel *)c)->frequency = si->MidiKeyToCgbFreq((u8)cgb, (u8)k, t->pitM);
                        ((struct CgbChannel *)c)->modify |= CGB_CHANNEL_MO_PIT;
                    } else {
                        c->frequency = MidiKeyToFreq(c->wav, (u8)k, t->pitM);
                    }
                }
                c = next;
            }
            t->flags &= 0xF0;
        }
    }
done:
    mp->ident = ID_NUMBER;
}

/* --------------------------------------------------------------- mixer */

void m4aSoundVSync(void) {
    struct SoundInfo *si = SI;
    if (si->ident - ID_NUMBER > 1) return;
    s32 c = (s32)si->pcmDmaCounter - 1;
    si->pcmDmaCounter = (u8)c;
    if (c > 0) return;
    si->pcmDmaCounter = si->pcmDmaPeriod;
}

/* The host's volume for whoever owns a channel: the BGM player's tracks
 * follow NP_OPT_BGM_VOLUME, everything else (the SE players, cries)
 * NP_OPT_SE_VOLUME. 256 leaves the cartridge's mix as it is. */
static u32 owner_volume(const struct MusicPlayerTrack *t) {
    const struct MusicPlayerInfo *bgm = gMPlayTable[0].info;
    int is_bgm = !t || (t >= bgm->tracks && t < bgm->tracks + bgm->trackCount);
    u32 v = gba_option(is_bgm ? NP_OPT_BGM_VOLUME : NP_OPT_SE_VOLUME);
    return v > 256 ? 256 : v;
}

/* the same for PSG channel `ch` (0..3), for the APU */
u32 gba_m4a_psg_volume(int ch) {
    struct SoundInfo *si = SI;
    return owner_volume(si && si->cgbChans ? si->cgbChans[ch].track : NULL);
}

/* DPCM: blocks of 64 samples in 33 bytes, a raw first sample and then
 * deltas from gDeltaEncodingTable, low nibble of byte 1, then high and low
 * nibbles of bytes 2..32 (SoundMainRAM_Unk2). One block stays decoded. */
static s8 s_dpcm[64];
static const struct WaveData *s_dpcm_wav;
static u32 s_dpcm_block = ~0u;

static s32 dpcm_sample(const struct WaveData *w, u32 i) {
    u32 b = i >> 6;
    if (w != s_dpcm_wav || b != s_dpcm_block) {
        const u8 *src = (const u8 *)w->data + b * 33;
        s8 s = (s8)src[0];
        s_dpcm[0] = s;
        s = (s8)(s + gDeltaEncodingTable[src[1] & 15]);
        s_dpcm[1] = s;
        for (int k = 2; k < 64; k += 2) {
            u8 v = src[1 + k / 2];
            s = (s8)(s + gDeltaEncodingTable[v >> 4]);
            s_dpcm[k] = s;
            s = (s8)(s + gDeltaEncodingTable[v & 15]);
            s_dpcm[k + 1] = s;
        }
        s_dpcm_wav = w;
        s_dpcm_block = b;
    }
    return s_dpcm[i & 63];
}

/* sample `i` counted in playing order: reversed voices play from the end,
 * compressed ones (WaveData.type != 0) are DPCM; past either end is 0 */
static s32 special_sample(const struct WaveData *w, u32 i, int rev) {
    if (i >= w->size) return 0;
    if (rev) i = w->size - 1 - i;
    return w->type ? dpcm_sample(w, i) : ((const s8 *)w->data)[i];
}

/* SoundMainRAM_Unk1: compressed and/or reversed voices. The channel's
 * position is a sample index in playing order (in currentPointer, as the
 * ARM code keeps it), interpolated like the plain path; only forward
 * voices loop. */
static void mix_special(struct SoundInfo *si, struct SoundChannel *c, const struct WaveData *w, s8 *buf, s32 n,
                        s32 vr, s32 vl) {
    int rev = (c->type & TONEDATA_TYPE_REV) != 0;
    if (!(c->statusFlags & SOUND_CHANNEL_SF_SPECIAL)) {
        c->statusFlags |= SOUND_CHANNEL_SF_SPECIAL;
        c->currentPointer = (s8 *)(uintptr_t)(u32)(c->currentPointer - (const s8 *)w->data);
    }
    u32 pos = (u32)(uintptr_t)c->currentPointer;
    s32 count = (s32)c->count;
    int loop = !rev && (c->statusFlags & SOUND_CHANNEL_SF_LOOP);
    u32 loop_len = w->size - w->loopStart;
    u32 step = (c->type & TONEDATA_TYPE_FIX) ? 0x800000u : (u32)si->divFreq * c->frequency;
    u32 fw = c->fw;
    s8 *r = buf, *l = buf + PCM_DMA_BUF_SIZE;
    s32 s0 = special_sample(w, pos, rev), d = special_sample(w, pos + 1, rev) - s0;
    for (s32 i = 0; i < n; i++) {
        s32 s = s0 + (s32)(((s64)(s32)fw * d) >> 23);
        r[i] = (s8)(r[i] + ((s * vr) >> 8));
        l[i] = (s8)(l[i] + ((s * vl) >> 8));
        fw += step;
        u32 adv = fw >> 23;
        if (!adv) continue;
        fw &= 0x7FFFFF;
        count -= (s32)adv;
        pos += adv;
        if (count <= 0) {
            if (!loop || loop_len == 0) {
                c->statusFlags = 0;
                return;
            }
            while (count <= 0) count += (s32)loop_len;
            pos = w->size - (u32)count;
        }
        s0 = special_sample(w, pos, rev);
        d = special_sample(w, pos + 1, rev) - s0;
    }
    c->fw = fw;
    c->count = (u32)count;
    c->currentPointer = (s8 *)(uintptr_t)pos;
}

/* mixes channel `c` into `n` samples at buf (right) / buf + PCM_DMA_BUF_SIZE (left) */
static void mix_channel(struct SoundInfo *si, struct SoundChannel *c, s8 *buf, s32 n) {
    struct WaveData *w = c->wav;
    u8 f = c->statusFlags;
    u32 env;
    if (f & SOUND_CHANNEL_SF_START) {
        if (f & SOUND_CHANNEL_SF_STOP) {
            c->statusFlags = 0;
            return;
        }
        f = SOUND_CHANNEL_SF_ENV_ATTACK;
        c->currentPointer = (s8 *)w->data + c->count;
        c->count = w->size - c->count;
        env = 0;
        c->envelopeVolume = 0;
        c->fw = 0;
        if (w->status & 0xC000) f |= SOUND_CHANNEL_SF_LOOP; /* WaveData flags: bit 14 = loop */
        c->statusFlags = f;
        goto attack;
    }
    env = c->envelopeVolume;
    if (f & SOUND_CHANNEL_SF_IEC) {
        if (--c->pseudoEchoLength == 0) {
            c->statusFlags = 0;
            return;
        }
        goto store;
    }
    if (f & SOUND_CHANNEL_SF_STOP) {
        env = (env * c->release) >> 8;
        if (env > c->pseudoEchoVolume) goto store;
    echo:
        env = c->pseudoEchoVolume;
        if (env == 0) {
            c->statusFlags = 0;
            return;
        }
        f |= SOUND_CHANNEL_SF_IEC;
        c->statusFlags = f;
        goto store;
    }
    if ((f & SOUND_CHANNEL_SF_ENV) == SOUND_CHANNEL_SF_ENV_DECAY) {
        env = (env * c->decay) >> 8;
        if (env > c->sustain) goto store;
        env = c->sustain;
        if (env == 0) goto echo;
        f--;
        c->statusFlags = f;
        goto store;
    }
    if ((f & SOUND_CHANNEL_SF_ENV) != SOUND_CHANNEL_SF_ENV_ATTACK) goto store;
attack:
    env += c->attack;
    if (env >= 0xFF) {
        env = 0xFF;
        f--;
        c->statusFlags = f;
    }
store:
    c->envelopeVolume = (u8)env;
    {
        u32 e = ((si->masterVolume + 1) * env) >> 4;
        c->envelopeVolumeRight = (u8)((c->rightVolume * e) >> 8);
        c->envelopeVolumeLeft = (u8)((c->leftVolume * e) >> 8);
    }
    s32 vr = c->envelopeVolumeRight, vl = c->envelopeVolumeLeft;
    const s8 *loop_start = NULL;
    s32 loop_len = 0;
    if (f & SOUND_CHANNEL_SF_LOOP) {
        loop_start = (const s8 *)w->data + w->loopStart;
        loop_len = (s32)(w->size - w->loopStart);
    }
    {
        u32 vol = owner_volume(c->track);
        vr = (s32)((u32)vr * vol >> 8);
        vl = (s32)((u32)vl * vol >> 8);
    }
    if (c->type & (TONEDATA_TYPE_CMP | TONEDATA_TYPE_REV)) {
        mix_special(si, c, w, buf, n, vr, vl);
        return;
    }
    s32 count = (s32)c->count;
    const s8 *p = c->currentPointer;
    s8 *r = buf, *l = buf + PCM_DMA_BUF_SIZE;
    if (c->type & TONEDATA_TYPE_FIX) {
        for (s32 i = 0; i < n; i++) {
            s32 s = *p++;
            r[i] = (s8)(r[i] + ((s * vr) >> 8));
            l[i] = (s8)(l[i] + ((s * vl) >> 8));
            if (--count == 0) {
                if (loop_len) {
                    p = loop_start;
                    count = loop_len;
                } else {
                    c->statusFlags = 0;
                    return;
                }
            }
        }
        c->count = (u32)count;
        c->currentPointer = (s8 *)p;
        return;
    }
    u32 step = (u32)si->divFreq * c->frequency;
    u32 fw = c->fw;
    s32 s0 = p[0], d = p[1] - p[0];
    for (s32 i = 0; i < n; i++) {
        s32 s = s0 + (s32)(((s64)(s32)fw * d) >> 23);
        r[i] = (s8)(r[i] + ((s * vr) >> 8));
        l[i] = (s8)(l[i] + ((s * vl) >> 8));
        fw += step;
        u32 adv = fw >> 23;
        if (!adv) continue;
        fw &= 0x7FFFFF;
        count -= (s32)adv;
        if (count <= 0) {
            if (!loop_len) {
                c->statusFlags = 0;
                return;
            }
            s32 over = -count;
            do {
                count += loop_len;
                if (count > 0) break;
                over -= loop_len;
            } while (1);
            p = loop_start + over;
        } else {
            p += adv;
        }
        s0 = p[0];
        d = p[1] - p[0];
    }
    c->fw = fw;
    c->count = (u32)count;
    c->currentPointer = (s8 *)p;
}

void SoundMain(void) {
    struct SoundInfo *si = SI;
    if (si->ident != ID_NUMBER) return;
    si->ident++;
    if (si->MPlayMainHead) si->MPlayMainHead(si->musicPlayerHead);
    si->CgbSound();
    s32 n = si->pcmSamplesPerVBlank;
    s8 *buf = si->pcmBuffer;
    u32 counter = si->pcmDmaCounter;
    if (counter > 1) buf += n * (s32)(si->pcmDmaPeriod - (counter - 1));
    if (si->reverb) {
        s8 *other = counter == 2 ? si->pcmBuffer : buf + n;
        for (s32 i = 0; i < n; i++) {
            s32 v = buf[i + PCM_DMA_BUF_SIZE] + buf[i] + other[i + PCM_DMA_BUF_SIZE] + other[i];
            v = (v * si->reverb) >> 9;
            if (v & 0x80) v++;
            buf[i + PCM_DMA_BUF_SIZE] = (s8)v;
            buf[i] = (s8)v;
        }
    } else {
        memset(buf, 0, (size_t)n);
        memset(buf + PCM_DMA_BUF_SIZE, 0, (size_t)n);
    }
    struct SoundChannel *c = si->chans;
    for (int k = si->maxChans; k > 0; k--, c++)
        if (c->statusFlags & SOUND_CHANNEL_SF_ON) mix_channel(si, c, buf, n);
    gba_apu_pcm(buf, buf + PCM_DMA_BUF_SIZE, (u32)n, (u32)si->pcmFreq);
    si->ident = ID_NUMBER;
}
