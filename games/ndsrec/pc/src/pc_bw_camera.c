/*
 * NP_OPT_CAMERA_ZOOM / NP_OPT_CAMERA_TILT on Black/White: Platinum's and
 * HeartGold's field camera options (pc/src/pc_np_field.c, pc_hg_field.c) on
 * B/W's field.
 *
 * B/W's 3D camera is the GFL library's GFL_G3D_CAMERA, a 0x44-byte block:
 *
 *   +0x00 the projection type, +0x04.. its parameters (fovy sin / cos,
 *         aspect), +0x14 near, +0x18 far (fx32)
 *   +0x20 the position, +0x2C the up vector, +0x38 the target (VecFx32)
 *
 * GFL_G3D_CAMERA_Switching (Black sub_02048AD0, White sub_02048AE8) loads
 * one into NNS_G3dGlb: the projection from +0x00.., then the look-at from
 * +0x20/+0x2C/+0x38. The field map keeps its camera at FIELDMAP+0xA4 (the
 * one field_camera.c's FIELD_CAMERA drives, FIELD_CAMERA+0x0C; FIELDMAP is
 * GAMESYS+0x14). The field overlay's seven Switching calls go to
 * PcBw_CameraSwitch (pc/patches/<VER>/arm9/overlays/21/asm/*.s.patch).
 *
 * While an option is off its default and the camera is the field map's,
 * the view loaded is a copy seen from farther away (zoom: the distance to
 * the target x zoom / 256) and from a different pitch (tilt, 1/16 degree,
 * + toward the horizon, kept 5..85 degrees above the target), around the
 * same target and heading; the far plane follows the distance as on
 * Platinum. The game's camera is never written, so every scripted camera
 * move keeps working on the real values and the default options load the
 * game's own camera untouched. Other cameras (battle, menus, the field's
 * second camera) pass straight through.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "armrec_rt.h"
#include "pc_np_options.h"

#if defined(PC_BW_VER_WHITE)
#define BW_GAMESYS_PTR 0x02146268u
#define BW_CAMERA_SWITCHING sub_02048AE8
#else
#define BW_GAMESYS_PTR 0x02146248u
#define BW_CAMERA_SWITCHING sub_02048AD0
#endif

extern uint64_t BW_CAMERA_SWITCHING(uint32_t cam, uint32_t unused1, uint32_t unused2, uint32_t unused3);

#define GAMESYS_FIELDMAP 0x14u
#define FIELDMAP_CAMERA 0xA4u

#define CAM_SIZE 0x44
#define CAM_NEAR 0x14
#define CAM_FAR 0x18
#define CAM_POS 0x20
#define CAM_TARGET 0x38

#define FX_ONE 4096.0
#define DEG (3.14159265358979323846 / 180.0)

static int bw_ram(uint32_t a)
{
    return a >= 0x02000000u && a < 0x02400000u && (a & 3) == 0;
}

static uint32_t rd32(uint32_t a)
{
    return *(const volatile uint32_t *)(uintptr_t)a;
}

static uint32_t field_camera(void)
{
    const uint32_t gamesys = rd32(BW_GAMESYS_PTR);
    uint32_t fieldmap;

    if (!bw_ram(gamesys)) return 0;
    fieldmap = rd32(gamesys + GAMESYS_FIELDMAP);
    if (!bw_ram(fieldmap)) return 0;
    return rd32(fieldmap + FIELDMAP_CAMERA);
}

static int32_t get(const uint8_t *cam, int off)
{
    int32_t v;

    memcpy(&v, cam + off, 4);
    return v;
}

static void put(uint8_t *cam, int off, int32_t v)
{
    memcpy(cam + off, &v, 4);
}

static int32_t to_fx(double units)
{
    if (units > 524287.0) units = 524287.0;
    if (units < -524287.0) units = -524287.0;
    return (int32_t)lround(units * FX_ONE);
}

/* The copy Switching loads instead of the field camera; guest-addressable
 * (the guest's memory is the module's). */
static uint8_t sCam[CAM_SIZE] __attribute__((aligned(4)));

uint32_t PcBw_CameraSwitch(uint32_t cam)
{
    const unsigned zoom = pc_np_opt.camera_zoom;
    const int tilt = pc_np_opt.camera_tilt;
    double ox, oy, oz, horiz, dist, elev, head, nearClip, farClip;
    int i;

    if ((zoom == 256 && tilt == 0) || !bw_ram(cam) || cam != field_camera()) {
        ARMREC_CALL(BW_CAMERA_SWITCHING, cam, 0, 0, 0);
        return 0;
    }
    memcpy(sCam, (const void *)(uintptr_t)cam, CAM_SIZE);

    ox = (get(sCam, CAM_POS) - get(sCam, CAM_TARGET)) / FX_ONE;
    oy = (get(sCam, CAM_POS + 4) - get(sCam, CAM_TARGET + 4)) / FX_ONE;
    oz = (get(sCam, CAM_POS + 8) - get(sCam, CAM_TARGET + 8)) / FX_ONE;
    horiz = sqrt(ox * ox + oz * oz);
    dist = sqrt(horiz * horiz + oy * oy) * zoom / 256.0;
    elev = atan2(oy, horiz) - tilt / 16.0 * DEG;
    if (elev < 5 * DEG) elev = 5 * DEG;
    if (elev > 85 * DEG) elev = 85 * DEG;
    head = horiz > 0 ? atan2(ox, oz) : 0;
    for (i = 0; i < 3; i++) {
        const double d = i == 0 ? dist * cos(elev) * sin(head)
                       : i == 1 ? dist * sin(elev)
                                : dist * cos(elev) * cos(head);

        put(sCam, CAM_POS + 4 * i, get(sCam, CAM_TARGET + 4 * i) + to_fx(d));
    }

    /* The clip planes as Platinum moves them (pc_np_field.c). */
    nearClip = get(sCam, CAM_NEAR) / FX_ONE;
    farClip = get(sCam, CAM_FAR) / FX_ONE;
    if (zoom < 256) nearClip = nearClip * zoom / 256.0;
    if (zoom > 256) farClip = farClip * zoom / 256.0;
    if (tilt > 0) farClip += farClip * tilt / (15.0 * 16.0); /* +100% per 15 degrees */
    if (farClip > 30000) farClip = 30000;
    if (nearClip * farClip > 250000) nearClip = 250000 / farClip; /* 2 near far fits in fx32 */
    put(sCam, CAM_NEAR, to_fx(nearClip));
    put(sCam, CAM_FAR, to_fx(farClip));

    ARMREC_CALL(BW_CAMERA_SWITCHING, (uint32_t)(uintptr_t)sCam, 0, 0, 0);
    return 0;
}
