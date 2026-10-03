/*
 * The touch panel, with no pen on it.
 *
 * TP requests are another ARM7 SPI conversation; the SDK's tp.c sends a
 * command over PXI and spins in TP_WaitBusy for the sample to come back.
 * With no ARM7 the whole request/wait surface is replaced by the state
 * it would report on an untouched console: samples arrive instantly,
 * touch == 0, coordinates zero and marked valid. When host input exists
 * (a window with a mouse), pc_tp_set() is the one place it feeds.
 *
 * Auto-sampling (the ARM7 sampling into a ring buffer on a VCount
 * schedule) is accepted and remembered but nothing fills the buffers:
 * readers see the same untouched state. TP_GetCalibratedResult reads
 * whatever the last sample was, which is the same answer.
 */
#include <nitro/os.h>
#include <nitro/spi/ARM9/tp.h>
#include <nitro/spi/common/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TPData sCurrent; /* zeroed: x=0, y=0, touch=0 (up), validity=0 (ok) */

/*
 * The firmware's touch calibration, which the ARM7 boot leaves in
 * OS_GetSystemWork()->nvramUserInfo on hardware and nothing left here:
 * TP_GetUserInfo returns TRUE unconditionally (a console always has
 * firmware), so a zeroed page handed the game an all-zero calibration,
 * TP_SetCalibrateParam computed inverse dot sizes of zero, and every raw
 * sample calibrated to pixel (0,0), measured as a touch that never
 * landed on a button. Two calibration points at 16 raw counts per pixel
 * (point 1: pixel (16,16) = raw (0x100,0x100); point 2: pixel (240,176)
 * = raw (0xF00,0xB00)) give TP_CalcCalibrateParam a clean linear map.
 * The rest of the user info (name, language, colour) stays zeroed until
 * something is measured to read it.
 */
void pc_tp_boot_userinfo(void)
{
    NVRAMConfig *info = (NVRAMConfig *)(OS_GetSystemWork()->nvramUserInfo);

    info->ncd.tp.raw_x1 = 0x100;
    info->ncd.tp.raw_y1 = 0x100;
    info->ncd.tp.dx1 = 16;
    info->ncd.tp.dy1 = 16;
    info->ncd.tp.raw_x2 = 0xF00;
    info->ncd.tp.raw_y2 = 0xB00;
    info->ncd.tp.dx2 = 240;
    info->ncd.tp.dy2 = 176;
}

/*
 * The input layer speaks SCREEN pixels; the panel reports RAW ADC counts,
 * and the game runs its own TP_GetCalibratedPoint over what it samples
 * (src/system.c: TP_RequestRawSampling → TP_GetCalibratedPoint), a
 * screen coordinate stored as raw lands wherever the calibration sends
 * it, which was measured as "the YES button never presses". The map from
 * raw to screen lives in the SDK object's own static calibration state,
 * so the inverse here is a scan over the guest's own transform: the raw
 * value whose calibrated result is nearest the requested pixel, one axis
 * at a time (the transform is separable). pokediamond's input layer
 * settled this shape; the scan is 4096 forward transforms per axis per
 * touch event, which is nothing.
 */
static u16 raw_for_pixel(u16 px, int axis)
{
    TPData raw, disp;
    u16 best = 0;
    int best_err = 0x7FFFFFFF;
    u32 r;

    memset(&raw, 0, sizeof raw);
    raw.touch = 1;
    for (r = 0; r < 4096; r += 4) {
        int got, err;

        raw.x = (u16)r;
        raw.y = (u16)r;
        TP_GetCalibratedPoint(&disp, &raw);
        got = axis == 0 ? disp.x : disp.y;
        err = got - (int)px;
        if (err < 0) err = -err;
        if (err < best_err) {
            best_err = err;
            best = (u16)r;
            if (err == 0) break;
        }
    }
    return best;
}

/* The input layer's write side: screen pixels in, raw counts stored. */
void pc_tp_set(u16 x, u16 y, int touching)
{
    if (touching) {
        sCurrent.x = raw_for_pixel(x, 0);
        sCurrent.y = raw_for_pixel(y, 1);
    } else {
        sCurrent.x = 0;
        sCurrent.y = 0;
    }
    sCurrent.touch = (u16)(touching ? 1 : 0);
    sCurrent.validity = 0;

    if (getenv("PC_TP_DEBUG") != NULL) {
        TPData chk;

        TP_GetCalibratedPoint(&chk, &sCurrent);
        fprintf(stderr,
                "pc_tp: set pixel (%u,%u) touch=%d -> raw (%u,%u) -> "
                "recalibrated (%u,%u) touch=%u validity=%u\n",
                (unsigned)x, (unsigned)y, touching,
                (unsigned)sCurrent.x, (unsigned)sCurrent.y,
                (unsigned)chk.x, (unsigned)chk.y,
                (unsigned)chk.touch, (unsigned)chk.validity);
    }
}

void TP_Init(void)
{
}

void TP_SetCallback(TPRecvCallback callback)
{
    (void)callback;
}

void TP_RequestSamplingAsync(void)
{
}

u32 TP_RequestSampling(void)
{
    return 0;
}

u32 TP_WaitRawResult(TPData *result)
{
    *result = sCurrent;
    return 0;
}

u32 TP_WaitCalibratedResult(TPData *result)
{
    TP_GetCalibratedPoint(result, &sCurrent);
    return 0;
}

u32 TP_GetCalibratedResult(TPData *result)
{
    TP_GetCalibratedPoint(result, &sCurrent);
    return 0;
}

/*
 * Whether the game is asking for the pen at all, which is a thing this port
 * can see and a frontend outside the process cannot. The SDK's auto-sampling
 * is started when a screen wants touch input (the game's InitializeTouchPad)
 * and stopped when it stops wanting it (DisableTouchPad), so the state
 * between those two calls is the game saying so in its own words. The window
 * layer reads it to decide which screen deserves the space.
 */
static int sSampling;

int pc_tp_sampling(void)
{
    return sSampling;
}

void TP_RequestAutoSamplingStartAsync(u16 vcount, u16 frequence,
                                      TPData samplingBufs[], u16 bufSize)
{
    sSampling = 1;
    (void)vcount;
    (void)frequence;
    (void)bufSize;
    /* Keep the ring pointed at truth: fill slot 0 with the current
     * (untouched) sample so a reader that indexes the buffer directly
     * sees a valid pen-up entry rather than stack garbage. */
    if (samplingBufs) {
        samplingBufs[0] = sCurrent;
    }
}

/* The field's input path uses auto-sampling once the Poketch (or any
 * other screen) starts the pad. The SDK walk reads a ring the ARM7
 * would fill; there is no ARM7 here, so the latest sample is sCurrent. */
void TP_GetLatestRawPointInAuto(TPData *result)
{
    *result = sCurrent;
}

void TP_RequestAutoSamplingStopAsync(void)
{
    sSampling = 0;
}

void TP_RequestSetStabilityAsync(u8 retry, u16 range)
{
    (void)retry;
    (void)range;
}

void TP_WaitBusy(TPRequestCommandFlag command_flgs)
{
    (void)command_flgs;
}

void TP_WaitAllBusy(void)
{
}

u32 TP_CheckBusy(TPRequestCommandFlag command_flgs)
{
    (void)command_flgs;
    return 0;
}

u32 TP_CheckError(TPRequestCommandFlag command)
{
    (void)command;
    return 0;
}
