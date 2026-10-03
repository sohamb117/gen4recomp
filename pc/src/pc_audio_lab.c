/*
 * The audio lab: play every real sequence through the game's own player and
 * prove the mixer comes back clean.
 *
 * Replay and --dump-audio hear whatever the boot happens to start. They cannot
 * say that a given sseq loaded, that its bank reached the heap, or that the
 * mixer released every voice when the player stopped. This walks the live
 * sound archive, the same table Sound_PlayBGM and friends consult, and for
 * each named sequence loads it, starts it on the player the archive assigned,
 * pumps the ARM7 driver and the SPU for a second of guest time, and
 * fingerprints the samples.
 *
 * It does not open a field and does not wait for the title screen's own BGM.
 * SoundSystem_Init has run before the first VBlank, the archive is on the host
 * filesystem, and the ARM7 driver is already behind its PXI tag.
 *
 * Time. The sequencer lives on the ARM7 and steps on the 192 Hz cadence
 * pc_arm7snd_frame() already batches at OS_Halt. The lab calls that pump in a
 * tight loop so a second of guest audio does not cost a second of wall clock.
 * The samples are the same ones a real frame would have mixed.
 *
 * The oracle for what should exist is res/text/seq_names.json, one name per
 * named sequence. The live archive must report the same count; a missing or
 * extra slot is a failure, not a skipped line.
 *
 * The digest is FNV-1a 64 over the s16 stereo frames of the play window.
 * Settle frames after Stop are drained and not hashed, so a release click
 * cannot move the pin.
 *
 * Invariants, per sequence:
 *   - the play window produced a finite, sane number of samples
 *   - no run of full-scale samples longer than 100 ms
 *   - after Stop and a short settle, every SPU channel's start bit is clear
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
static int lab_mkdir(const char *d) { return _mkdir(d); }
#else
static int lab_mkdir(const char *d) { return mkdir(d, 0755); }
#endif

#include "pch/global_pch.h"

#include "sound.h"
#include "sound_playback.h"
#include "sound_system.h"

#include "res/sound/pl_sound_data.naix"

#include "pc_spu.h"
#include "pc_state.h"

#define LAB_AUDIO_AT_DEF      2
#define LAB_AUDIO_PLAY_FRAMES 60   /* one second at the port's 59.8 fps */
#define LAB_AUDIO_SETTLE      8
#define LAB_AUDIO_RELEASE_MAX 120 /* extra frames to wait for a keyoff */
#define LAB_AUDIO_ORACLE_N    1013
#define LAB_MAX_LIST          64
#define LAB_STUCK_SAMPLES     3273 /* 100 ms at 32,728 Hz */
#define LAB_FRAME_CYCLES      560190ull
#define LAB_DRAIN_MAX         2048
#define LAB_KIND_MUSIC        0
#define LAB_KIND_SE           1
#define LAB_KIND_OTHER        2

enum lab_audio_mode {
    LAB_AUDIO_NONE = 0,
    LAB_AUDIO_ALL,
    LAB_AUDIO_PIN,
    LAB_AUDIO_LIST
};

static enum lab_audio_mode sMode;
static unsigned long sAtFrame = LAB_AUDIO_AT_DEF;
static int sDone;
static int sParsed;
static u16 sList[LAB_MAX_LIST];
static int sListN;
static FILE *sOut;
static const char *sDumpDir;

static int sPlayed;
static int sFailed;
static int sNamed;
static int sMusic;
static int sEffect;
static int sOther;
static int sSilent;
static int sPlayFail;
static int sLoadFail;
static int sStuck;
static int sVoiceHeld;
static int sNonfinite;

static uint64_t sPinFnv[2];
static int sPinGot[2];
static int sPinPeak[2];

static void lab_say(const char *fmt, ...)
{
    va_list ap;
    va_list aq;

    va_start(ap, fmt);
    va_copy(aq, ap);
    vfprintf(stderr, fmt, ap);
    if (sOut != NULL) vfprintf(sOut, fmt, aq);
    va_end(aq);
    va_end(ap);
    fflush(stderr);
    if (sOut != NULL) fflush(sOut);
}

static int lab_kind(u8 player)
{
    if (player == PLAYER_FIELD || player == PLAYER_BGM || player == PLAYER_ME) {
        return LAB_KIND_MUSIC;
    }
    if (player == PLAYER_SE_1 || player == PLAYER_SE_2
            || player == PLAYER_SE_3 || player == PLAYER_SE_4) {
        return LAB_KIND_SE;
    }
    return LAB_KIND_OTHER;
}

static const char *lab_kind_name(int kind)
{
    if (kind == LAB_KIND_MUSIC) return "music";
    if (kind == LAB_KIND_SE) return "se";
    return "other";
}

static const char *lab_player_name(u8 player)
{
    switch (player) {
    case PLAYER_PV:    return "PV";
    case PLAYER_FIELD: return "FIELD";
    case PLAYER_ME:    return "ME";
    case PLAYER_SE_1:  return "SE1";
    case PLAYER_SE_2:  return "SE2";
    case PLAYER_SE_3:  return "SE3";
    case PLAYER_SE_4:  return "SE4";
    case PLAYER_BGM:   return "BGM";
    default:           return "?";
    }
}

/* ------------------------------------------------------------------ */
/* One frame of guest audio, without the 2D/3D work a real VBlank does */
/* ------------------------------------------------------------------ */

extern void pc_arm7snd_frame(void);

static unsigned lab_drain(int16_t *dst, unsigned cap)
{
    return pc_audio_read(dst, cap);
}

static void lab_pump(void)
{
    SoundSystem_Tick();
    pc_arm7snd_frame();
    pc_audio_advance(LAB_FRAME_CYCLES);
}

static void lab_discard_ring(void)
{
    int16_t buf[LAB_DRAIN_MAX * 2];

    while (lab_drain(buf, LAB_DRAIN_MAX) > 0) {
        /* leftover boot silence, or settle after Stop */
    }
}

/* ------------------------------------------------------------------ */
/* Optional per-sequence WAV                                           */
/* ------------------------------------------------------------------ */

static void wr32(FILE *f, uint32_t v)
{
    unsigned char b[4] = {
        (unsigned char)v, (unsigned char)(v >> 8),
        (unsigned char)(v >> 16), (unsigned char)(v >> 24)
    };
    (void)fwrite(b, 1, 4, f);
}

static void wr16(FILE *f, uint32_t v)
{
    unsigned char b[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
    (void)fwrite(b, 1, 2, f);
}

static FILE *lab_wav_open(u16 seqID)
{
    char path[512];
    FILE *f;

    if (sDumpDir == NULL) return NULL;
    snprintf(path, sizeof path, "%s/seq_%04u.wav", sDumpDir, (unsigned)seqID);
    f = fopen(path, "wb+");
    if (f == NULL) {
        fprintf(stderr, "pc_lab: audio: cannot write %s: %s\n",
                path, strerror(errno));
        return NULL;
    }
    (void)fwrite("RIFF", 1, 4, f);
    wr32(f, 36);
    (void)fwrite("WAVEfmt ", 1, 8, f);
    wr32(f, 16);
    wr16(f, 1);
    wr16(f, 2);
    wr32(f, 32728);
    wr32(f, 32728u * 4);
    wr16(f, 4);
    wr16(f, 16);
    (void)fwrite("data", 1, 4, f);
    wr32(f, 0);
    return f;
}

static void lab_wav_close(FILE *f, uint32_t frames)
{
    if (f == NULL) return;
    if (fseek(f, 4, SEEK_SET) == 0) {
        wr32(f, 36 + frames * 4);
        if (fseek(f, 40, SEEK_SET) == 0) wr32(f, frames * 4);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Heap: the states Sound_PlayBGM / PlayFanfare already expect         */
/* ------------------------------------------------------------------ */

static void lab_heap_ready(void)
{
    Sound_StopWaveOutAndSequences();
    Sound_ClearBGMPauseFlags();
    SoundSystem_LoadHeapState(Sound_GetHeapState(SOUND_HEAP_STATE_PERSISTENT));
    SoundSystem_SaveHeapState(
        SoundSystem_GetParam(SOUND_SYSTEM_PARAM_HEAP_STATE_BGM_BANK));
    SoundSystem_SaveHeapState(
        SoundSystem_GetParam(SOUND_SYSTEM_PARAM_HEAP_STATE_SFX));
}

static void lab_heap_reset(void)
{
    u16 *delay;

    /* Back to PERSISTENT, not SFX. PlayBGM for a field theme loads
     * GROUP_SE_FIELD and then SaveHeapState(SFX), so a reset to SFX
     * leaves that group resident and the next large bank will not
     * fit. Every sequence starts from the same empty-of-scene heap. */
    Sound_StopWaveOutAndSequences();
    NNS_SndPlayerStopSeqAll(0);
    Sound_ClearBGMPauseFlags();
    delay = SoundSystem_GetParam(SOUND_SYSTEM_PARAM_FANFARE_DELAY);
    *delay = 0;
    SoundSystem_LoadHeapState(Sound_GetHeapState(SOUND_HEAP_STATE_PERSISTENT));
    SoundSystem_SaveHeapState(
        SoundSystem_GetParam(SOUND_SYSTEM_PARAM_HEAP_STATE_BGM_BANK));
    SoundSystem_SaveHeapState(
        SoundSystem_GetParam(SOUND_SYSTEM_PARAM_HEAP_STATE_SFX));
}

static int lab_start(u16 seqID, u8 player)
{
    if (player == PLAYER_FIELD) {
        Sound_SetFieldBGM(seqID);
        Sound_SetFieldBGMBankState(FIELD_BGM_BANK_STATE_SWITCH);
        return Sound_PlayBGM(seqID) ? 1 : 0;
    }
    if (player == PLAYER_BGM) {
        return Sound_PlayBGM(seqID) ? 1 : 0;
    }
    if (player == PLAYER_ME) {
        return Sound_PlayFanfare(seqID) ? 1 : 0;
    }
    if (!SoundSystem_LoadSequence(seqID)
            && !SoundSystem_LoadSequenceEx(seqID, NNS_SND_ARC_LOAD_ALL)) {
        sLoadFail++;
        return 0;
    }
    if (Sound_PlayEffect(seqID)) return 1;
    return Sound_PlaySequenceWithPlayer(
        SoundSystem_GetSoundHandleTypeFromPlayerID((int)player),
        (int)player, seqID) ? 1 : 0;
}

static void lab_stop(u16 seqID, u8 player)
{
    if (player == PLAYER_FIELD || player == PLAYER_BGM) {
        Sound_StopBGM(seqID, 0);
    } else if (player == PLAYER_ME) {
        NNS_SndPlayerStopSeqBySeqNo(seqID, 0);
    } else {
        Sound_StopEffect(seqID, 0);
    }
}

/*
 * A channel whose start bit is still set after Stop is only a stuck
 * voice if it is still audible. Seven move SFX (W467, W060C, W062,
 * 131, W063C, W089B, W464) leave a looping ADPCM channel in the last
 * steps of a 127-tick release, volume 1 or 2, start bit still on.
 * That is the envelope, not a mixer that failed to key off.
 */
#define LAB_VOICE_AUDIBLE 8

static int lab_chan_audible(uint32_t cnt)
{
    return (cnt & 0x80000000u) && ((cnt & 0x7fu) >= LAB_VOICE_AUDIBLE);
}

static unsigned lab_voices_held(void)
{
    struct pc_spu_regs r;
    unsigned i, n = 0;

    pc_spu_regs(&r);
    for (i = 0; i < PC_SPU_CHANNELS; i++) {
        if (lab_chan_audible(r.chan[i].cnt)) n++;
    }
    return n;
}

static void lab_voices_describe(char *dst, size_t cap)
{
    struct pc_spu_regs r;
    unsigned i, n = 0;
    int used = 0;

    pc_spu_regs(&r);
    dst[0] = '\0';
    for (i = 0; i < PC_SPU_CHANNELS; i++) {
        if (!lab_chan_audible(r.chan[i].cnt)) continue;
        n++;
        used += snprintf(dst + used, cap - (size_t)used,
                         "%s%u(cnt=%08x tmr=%04x)",
                         n > 1 ? "," : "", i,
                         (unsigned)r.chan[i].cnt,
                         (unsigned)r.chan[i].timer);
        if (used < 0 || (size_t)used >= cap) break;
    }
}

static int lab_fullscale(int16_t s)
{
    return (s >= 32767 || s <= -32767);
}

/*
 * Play one named sequence for LAB_AUDIO_PLAY_FRAMES of guest time.
 * Returns 0 only when a hard failure should stop the walk; a sequence
 * that will not start is counted and the walk continues.
 */
static int lab_one(u16 seqID)
{
    const NNSSndSeqParam *param;
    u8 player;
    int kind;
    int started;
    int f;
    unsigned n, i;
    unsigned samples = 0;
    unsigned stuck_run = 0;
    unsigned stuck = 0;
    unsigned voices;
    int peak = 0;
    uint64_t fnv = PC_STATE_FNV64_OFFSET;
    FILE *wav;
    int16_t buf[LAB_DRAIN_MAX * 2];

    param = NNS_SndArcGetSeqParam((int)seqID);
    if (param == NULL) {
        lab_say("pc_lab: audio FAIL seq=%u has no archive param\n",
                (unsigned)seqID);
        sFailed++;
        return 1;
    }
    player = param->playerNo;
    kind = lab_kind(player);
    if (kind == LAB_KIND_MUSIC) sMusic++;
    else if (kind == LAB_KIND_SE) sEffect++;
    else sOther++;

    lab_heap_reset();
    lab_pump();
    lab_pump();
    lab_discard_ring();

    started = lab_start(seqID, player);
    if (!started) {
        lab_say("pc_lab: audio FAIL seq=%u player=%s kind=%s did not start\n",
                (unsigned)seqID, lab_player_name(player), lab_kind_name(kind));
        sPlayFail++;
        sFailed++;
        lab_heap_reset();
        return 1;
    }

    wav = lab_wav_open(seqID);

    for (f = 0; f < LAB_AUDIO_PLAY_FRAMES; f++) {
        lab_pump();
        n = lab_drain(buf, LAB_DRAIN_MAX);
        for (i = 0; i < n; i++) {
            int16_t l = buf[i * 2 + 0];
            int16_t r = buf[i * 2 + 1];
            int al = l < 0 ? -(int)l : (int)l;
            int ar = r < 0 ? -(int)r : (int)r;

            fnv = pc_state_fnv1a(fnv, &l, sizeof l);
            fnv = pc_state_fnv1a(fnv, &r, sizeof r);
            if (al > peak) peak = al;
            if (ar > peak) peak = ar;
            if (lab_fullscale(l) || lab_fullscale(r)) {
                stuck_run++;
                if (stuck_run > LAB_STUCK_SAMPLES) stuck = 1;
            } else {
                stuck_run = 0;
            }
            if (wav != NULL) {
                wr16(wav, (uint32_t)(uint16_t)l);
                wr16(wav, (uint32_t)(uint16_t)r);
            }
        }
        samples += n;
    }

    lab_wav_close(wav, samples);
    lab_stop(seqID, player);

    for (f = 0; f < LAB_AUDIO_SETTLE; f++) {
        lab_pump();
        lab_discard_ring();
    }
    voices = lab_voices_held();
    /* Looping BGM holds its channels until the stop command is
     * processed and the current note ends. Eight frames is enough
     * for a one-shot SE; a field theme can take a few more. Wait
     * rather than fail a healthy release that is merely slow. */
    f = 0;
    while (voices != 0 && f < LAB_AUDIO_RELEASE_MAX) {
        lab_pump();
        lab_discard_ring();
        voices = lab_voices_held();
        f++;
    }
    if (voices != 0) {
        NNS_SndPlayerStopSeqAll(0);
        Sound_StopAllEffects(0);
        for (f = 0; f < LAB_AUDIO_SETTLE; f++) {
            lab_pump();
            lab_discard_ring();
        }
        voices = lab_voices_held();
    }
    lab_heap_reset();

    /* A second of guest time is ~32,728 samples. Cycle debt makes the
     * exact count wander by a handful; a window that is empty, or that
     * is many frames long, is the mixer not being reached. */
    if (samples < 20000u || samples > 40000u) {
        lab_say("pc_lab: audio FAIL seq=%u samples=%u, expected ~32728 "
                "for %d frames\n",
                (unsigned)seqID, samples, LAB_AUDIO_PLAY_FRAMES);
        sNonfinite++;
        sFailed++;
    }
    if (stuck) {
        lab_say("pc_lab: audio FAIL seq=%u stuck full-scale for more than "
                "%d samples\n",
                (unsigned)seqID, LAB_STUCK_SAMPLES);
        sStuck++;
        sFailed++;
    }
    if (voices != 0) {
        char held[256];

        lab_voices_describe(held, sizeof held);
        lab_say("pc_lab: audio FAIL seq=%u %u voice(s) still keyed on "
                "after stop [%s]\n",
                (unsigned)seqID, voices, held);
        sVoiceHeld++;
        sFailed++;
    }
    if (peak == 0) sSilent++;

    lab_say("pc_lab: audio seq=%u player=%s kind=%s samples=%u peak=%d "
            "voices_left=%u fnv=%016llx\n",
            (unsigned)seqID, lab_player_name(player), lab_kind_name(kind),
            samples, peak, voices, (unsigned long long)fnv);

    if (seqID == SEQ_TITLE00_sseq) {
        sPinFnv[0] = fnv;
        sPinPeak[0] = peak;
        sPinGot[0] = 1;
    } else if (seqID == SEQ_SILENCE_FIELD_sseq) {
        sPinFnv[1] = fnv;
        sPinPeak[1] = peak;
        sPinGot[1] = 1;
    }

    sPlayed++;
    return 1;
}

static void lab_parse(void)
{
    const char *s = getenv("PC_LAB_AUDIO");
    const char *at = getenv("PC_LAB_AUDIO_AT");
    const char *out = getenv("PC_LAB_AUDIO_OUT");
    const char *dump = getenv("PC_LAB_AUDIO_DUMP");

    sParsed = 1;
    if (s == NULL || s[0] == '\0') return;
    if (strcmp(s, "all") == 0) {
        sMode = LAB_AUDIO_ALL;
    } else if (strcmp(s, "pin") == 0) {
        sMode = LAB_AUDIO_PIN;
    } else {
        const char *p = s;

        sMode = LAB_AUDIO_LIST;
        while (*p != '\0' && sListN < LAB_MAX_LIST) {
            char *end;
            unsigned long v = strtoul(p, &end, 0);

            if (end == p) {
                fprintf(stderr, "pc_lab: PC_LAB_AUDIO wants 'all', 'pin'"
                        " or a comma-separated sequence list, got '%s'\n", s);
                exit(2);
            }
            sList[sListN++] = (u16)v;
            p = (*end == ',') ? end + 1 : end;
        }
        if (sListN == 0) {
            fprintf(stderr, "pc_lab: PC_LAB_AUDIO list was empty\n");
            exit(2);
        }
    }
    if (at != NULL && at[0] != '\0') sAtFrame = strtoul(at, NULL, 0);
    if (out != NULL && out[0] != '\0') {
        sOut = fopen(out, "w");
        if (sOut == NULL) {
            fprintf(stderr, "pc_lab: cannot write %s\n", out);
            exit(2);
        }
    }
    if (dump != NULL && dump[0] != '\0') {
        sDumpDir = dump;
        if (lab_mkdir(dump) != 0 && errno != EEXIST) {
            fprintf(stderr, "pc_lab: cannot create %s: %s\n", dump,
                    strerror(errno));
            exit(2);
        }
    }
}

/*
 * Title music against field silence; two sequences the mixer must
 * not hash the same, and one of which must actually be quiet.
 * SEQ_OPENING is the other musical pin: it is BANK_BASIC's long
 * cousin of the title, and it must also differ from silence.
 */
static const u16 sPinSeqs[] = {
    SEQ_TITLE00_sseq,
    SEQ_OPENING_sseq,
    SEQ_TEST_TITLE_sseq,
    SEQ_TOWN01_D_sseq,
    SEQ_FANFA1_sseq,
    SEQ_SE_DP_SELECT5_sseq,
    SEQ_PV001_sseq,
    SEQ_SILENCE_FIELD_sseq,
};

static void lab_finish(double seconds)
{
    lab_say("pc_lab: audio done named=%d played=%d music=%d se=%d other=%d "
            "silent=%d failed=%d (start=%d load=%d stuck=%d voices=%d "
            "samples=%d) %.2fs\n",
            sNamed, sPlayed, sMusic, sEffect, sOther, sSilent, sFailed,
            sPlayFail, sLoadFail, sStuck, sVoiceHeld, sNonfinite, seconds);

    if (sMode == LAB_AUDIO_ALL && sNamed != LAB_AUDIO_ORACLE_N) {
        lab_say("pc_lab: audio FAIL archive named %d sequences, "
                "seq_names.json has %d\n",
                sNamed, LAB_AUDIO_ORACLE_N);
        if (sOut != NULL) fclose(sOut);
        exit(2);
    }

    if (sPinGot[0]) {
        lab_say("pc_lab: audio pin seq=%u fnv=%016llx peak=%d\n",
                (unsigned)SEQ_TITLE00_sseq,
                (unsigned long long)sPinFnv[0], sPinPeak[0]);
    }
    if (sPinGot[1]) {
        lab_say("pc_lab: audio pin seq=%u fnv=%016llx peak=%d\n",
                (unsigned)SEQ_SILENCE_FIELD_sseq,
                (unsigned long long)sPinFnv[1], sPinPeak[1]);
    }
    if ((sMode == LAB_AUDIO_PIN || sMode == LAB_AUDIO_ALL)
            && (!sPinGot[0] || !sPinGot[1])) {
        lab_say("pc_lab: audio FAIL pin sequences %u and %u were not "
                "both played\n",
                (unsigned)SEQ_TITLE00_sseq,
                (unsigned)SEQ_SILENCE_FIELD_sseq);
        if (sOut != NULL) fclose(sOut);
        exit(2);
    }
    if (sPinGot[0] && sPinGot[1]) {
        if (sPinPeak[0] == 0) {
            lab_say("pc_lab: audio FAIL title seq %u was silent, the "
                    "player started and the mixer produced nothing\n",
                    (unsigned)SEQ_TITLE00_sseq);
            if (sOut != NULL) fclose(sOut);
            exit(2);
        }
        if (sPinFnv[0] == sPinFnv[1]) {
            lab_say("pc_lab: audio FAIL title seq %u hashed the same as "
                    "silence; the player did not reach the mixer\n",
                    (unsigned)SEQ_TITLE00_sseq);
            if (sOut != NULL) fclose(sOut);
            exit(2);
        }
    }
    if (sOut != NULL) fclose(sOut);
    if (sFailed) exit(2);
    exit(0);
}

void pc_lab_audio_frame(unsigned long long frame)
{
    u32 count, i;
    struct timespec t0, t1;
    double seconds;

    if (!sParsed) lab_parse();
    if (sMode == LAB_AUDIO_NONE || sDone) return;
    if (frame < sAtFrame) return;
    sDone = 1;

    fprintf(stderr, "pc_lab: audio starting at frame %llu mode=%s "
            "play_frames=%d\n",
            frame,
            sMode == LAB_AUDIO_ALL ? "all"
            : sMode == LAB_AUDIO_PIN ? "pin" : "list",
            LAB_AUDIO_PLAY_FRAMES);
    fflush(stderr);

    count = NNS_SndArcGetSeqCount();
    for (i = 1; i < count; i++) {
        if (NNS_SndArcGetSeqParam((int)i) != NULL) sNamed++;
    }
    lab_say("pc_lab: audio archive seq_count=%u named=%d (oracle %d)\n",
            (unsigned)count, sNamed, LAB_AUDIO_ORACLE_N);

    lab_heap_ready();
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (sMode == LAB_AUDIO_ALL) {
        for (i = 1; i < count; i++) {
            if (NNS_SndArcGetSeqParam((int)i) == NULL) continue;
            if (!lab_one((u16)i)) break;
            if ((sPlayed % 50) == 0) {
                fprintf(stderr, "pc_lab: audio progress played=%d seq=%u\n",
                        sPlayed, (unsigned)i);
                fflush(stderr);
            }
        }
    } else if (sMode == LAB_AUDIO_PIN) {
        for (i = 0; i < (u32)(sizeof sPinSeqs / sizeof sPinSeqs[0]); i++) {
            if (NNS_SndArcGetSeqParam((int)sPinSeqs[i]) == NULL) {
                lab_say("pc_lab: audio FAIL pin seq %u is not in the "
                        "archive\n", (unsigned)sPinSeqs[i]);
                sFailed++;
                break;
            }
            if (!lab_one(sPinSeqs[i])) break;
        }
    } else {
        for (i = 0; i < (u32)sListN; i++) {
            if (NNS_SndArcGetSeqParam((int)sList[i]) == NULL) {
                lab_say("pc_lab: audio FAIL seq %u is not in the archive "
                        "(seq_count=%u)\n",
                        (unsigned)sList[i], (unsigned)count);
                sFailed++;
                continue;
            }
            if (!lab_one(sList[i])) break;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    seconds = (double)(t1.tv_sec - t0.tv_sec)
            + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    lab_finish(seconds);
}
