/*
 * The cartridge's Seiko S-3511 real-time clock, as the siirtc API the
 * decomps call (siirtc.c is not compiled). The clock is the host's
 * np_host_rtc_now(), or, when the host keeps it deterministic, 2004-01-01
 * 10:00:00 advancing with frames (60 per second). Setting the clock keeps
 * the difference, as the chip would.
 */
#include <string.h>

#include "global.h"
#include "siirtc.h"
#include "gba_port.h"
#include "np_guest_abi.h"

#define DETERMINISTIC_BASE 126266400LL /* 2004-01-01 10:00:00, seconds since 2000-01-01 */

static int64_t s_offset;
static uint8_t s_status = SIIRTCINFO_24HOUR;

int64_t gba_rtc_seconds(void) {
    int64_t t = np_host_rtc_now();
    if (t < 0) t = DETERMINISTIC_BASE + (int64_t)(gba_frames / 60);
    return t + s_offset;
}

static u8 bcd(u32 v) { return (u8)((v / 10) << 4 | (v % 10)); }
static u32 unbcd(u8 v) { return (v >> 4) * 10 + (v & 15); }

/* days since 2000-01-01 <-> civil date (proleptic Gregorian) */
static void civil(int64_t days, u32 *y, u32 *m, u32 *d) {
    int64_t z = days + 10957 + 719468; /* to days since 0000-03-01 */
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (uint32_t)(yoe + era * 400 + (*m <= 2));
}

static int64_t days_from_civil(u32 y, u32 m, u32 d) {
    y -= m <= 2;
    int64_t era = y / 400;
    uint32_t yoe = y - (uint32_t)era * 400;
    uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468 - 10957;
}

static void fill(struct SiiRtcInfo *rtc) {
    int64_t t = gba_rtc_seconds();
    int64_t days = t / 86400, secs = t % 86400;
    if (secs < 0) secs += 86400, days--;
    u32 y, m, d;
    civil(days, &y, &m, &d);
    rtc->year = bcd((y - 2000) % 100);
    rtc->month = bcd(m);
    rtc->day = bcd(d);
    rtc->dayOfWeek = (u8)(((days % 7) + 7 + 6) % 7); /* 2000-01-01 was a Saturday */
    rtc->hour = bcd((u32)(secs / 3600));
    rtc->minute = bcd((u32)(secs / 60 % 60));
    rtc->second = bcd((u32)(secs % 60));
}

void SiiRtcUnprotect(void) {}
void SiiRtcProtect(void) {}
u8 SiiRtcProbe(void) { return 1; }

bool8 SiiRtcReset(void) {
    s_status = SIIRTCINFO_24HOUR;
    s_offset = -gba_rtc_seconds() + s_offset; /* back to 2000-01-01 00:00:00 */
    return TRUE;
}

bool8 SiiRtcGetStatus(struct SiiRtcInfo *rtc) {
    rtc->status = s_status;
    return TRUE;
}

bool8 SiiRtcSetStatus(struct SiiRtcInfo *rtc) {
    s_status = (rtc->status & ~SIIRTCINFO_POWER) | SIIRTCINFO_24HOUR;
    return TRUE;
}

bool8 SiiRtcGetDateTime(struct SiiRtcInfo *rtc) {
    fill(rtc);
    return TRUE;
}

bool8 SiiRtcGetTime(struct SiiRtcInfo *rtc) {
    struct SiiRtcInfo t;
    fill(&t);
    rtc->hour = t.hour;
    rtc->minute = t.minute;
    rtc->second = t.second;
    return TRUE;
}

static void set_from(u32 y, u32 m, u32 d, u32 h, u32 mi, u32 s) {
    int64_t want = days_from_civil(y, m, d) * 86400 + h * 3600 + mi * 60 + s;
    s_offset += want - gba_rtc_seconds();
}

bool8 SiiRtcSetDateTime(struct SiiRtcInfo *rtc) {
    set_from(2000 + unbcd(rtc->year), unbcd(rtc->month), unbcd(rtc->day), unbcd(rtc->hour & 0x3F),
             unbcd(rtc->minute), unbcd(rtc->second));
    return TRUE;
}

bool8 SiiRtcSetTime(struct SiiRtcInfo *rtc) {
    struct SiiRtcInfo now;
    fill(&now);
    set_from(2000 + unbcd(now.year), unbcd(now.month), unbcd(now.day), unbcd(rtc->hour & 0x3F),
             unbcd(rtc->minute), unbcd(rtc->second));
    return TRUE;
}

bool8 SiiRtcSetAlarm(struct SiiRtcInfo *rtc) {
    (void)rtc;
    return TRUE;
}
