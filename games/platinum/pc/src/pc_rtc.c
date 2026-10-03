/*
 * The real-time clock, the device behind PXI tag 5, after pokediamond's
 * pc_rtc model.
 *
 * On hardware the RTC hangs off the ARM7. The ARM9's rtc library sends a
 * command word over PXI tag 5 (`cmd << 8`), the ARM7 reads or writes the
 * raw BCD image at OS_GetSystemWork()->real_time_clock, and replies
 * `0x8000 | (cmd << 8) | result`; RtcCommonCallback decodes the work area.
 * This file is that ARM7 half, registered as pc_pxi.c's tag-5 responder.
 *
 * Determinism comes first, exactly as diamond settled it: the game seeds
 * RNGs from this clock, so reading the host clock would make every run
 * draw different randomness and unbisectable frames. The reported time is
 * a fixed epoch advanced by the port's own frame counter (one VBlank =
 * 1/60 s), one clock in the port, the RTC a view of it. PC_RTC=
 * "YYYY-MM-DD HH:MM:SS" overrides the epoch; nothing reads the host time,
 * except the wasm guest, which asks the runtime (np_host_rtc_now) and keeps
 * this clock whenever the runtime declines; see host_secs().
 *
 * Why it advances instead of being frozen: frozen is also deterministic
 * and wrong twice, the day/night cycle and berry timers would wait
 * forever (silent hang), and it would disagree with OS_GetTick.
 *
 * What it implements: the reads the library issues (DATETIME/DATE/TIME,
 * STATUS1/STATUS2) and the writes as clock adoption; a write command
 * means "the values the ARM9 just placed in the work area are now the
 * time", which re-bases the epoch so subsequent reads continue from
 * there. Alarms, pulse, adjust and free read/write as zeros; the
 * INTERRUPT push (0x30) is never generated, no alarm fires here.
 */

#include <nitro/os.h>
#include <nitro/pxi.h>
#include <nitro/rtc/common/fifo.h>

#include <stdio.h>
#include <stdlib.h>

#if defined(__wasm__)
#include <pc_wasm.h>
#endif

extern u32 pc_os_vblank_count;
extern void pc_pxi_set_responder(int tag, void (*fn)(u32 data));
extern void pc_pxi_reply(int tag, u32 data);

/* Epoch as calendar fields; default is Platinum's US release morning.
 * Advanced by whole days/seconds derived from the frame counter. */
static int ep_year = 9, ep_month = 3, ep_day = 22;   /* year is 2000+n */
static int ep_hour = 10, ep_min = 0, ep_sec = 0;
static u32 ep_base_frames;      /* frame count the epoch is anchored at */

static const u8 kDaysInMonth[13] = {
    0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static int days_in_month(int y, int m)
{
    /* 2000..2099: every year divisible by 4 is a leap year. */
    if (m == 2 && (y % 4) == 0) return 29;
    return kDaysInMonth[m];
}

/* Day of week, 0 = Sunday, for 20yy-mm-dd (Sakamoto). */
static int weekday(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int yy = 2000 + y;

    if (m < 3) yy -= 1;
    return (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + d) % 7;
}

static u32 hex2bcd(int v) { return (u32)((v / 10) * 16 + (v % 10)); }
static int bcd2hex(u32 v) { return (int)((v >> 4) * 10 + (v & 0xF)); }

#if defined(__wasm__)
/*
 * The wasm guest's clock: the runtime's local wall time when it offers one
 * (np_host_rtc_now() >= 0), the deterministic frame clock otherwise. A
 * player of the wasm build expects the DS behaviour, berries and day/night
 * following the real clock even across sessions, and the runtime is the one
 * place that knows whether it wants that or a reproducible run (it returns
 * -1 for the latter). PC_RTC still wins: an explicit epoch is a request for
 * the deterministic clock.
 *
 * A game write (adopt_raw) is kept as an offset from the host clock, so a
 * clock the player sets in the game keeps running from where they set it.
 */
static int sRtcEnvOverride;
static long long sHostOffset;   /* seconds added to np_host_rtc_now() */

/* 2000-01-01 00:00:00 + 100 years: the chip's BCD year has two digits. */
#define RTC_HOST_LIMIT 3155760000LL

static long long fields_to_secs(int y, int mo, int d, int h, int mi, int s)
{
    long long days = 0;
    int i;

    for (i = 0; i < y; i++) {
        days += (i % 4) == 0 ? 366 : 365;
    }
    for (i = 1; i < mo; i++) {
        days += days_in_month(y, i);
    }
    days += d - 1;
    return ((days * 24 + h) * 60 + mi) * 60 + s;
}

static void secs_to_fields(long long t, int *y, int *mo, int *d,
                           int *h, int *mi, int *s)
{
    long long days = t / 86400;
    int rem = (int)(t % 86400);

    *h = rem / 3600;
    *mi = (rem / 60) % 60;
    *s = rem % 60;
    *y = 0;
    for (;;) {
        int ylen = (*y % 4) == 0 ? 366 : 365;

        if (days < ylen) break;
        days -= ylen;
        (*y)++;
    }
    *mo = 1;
    while (days >= days_in_month(*y, *mo)) {
        days -= days_in_month(*y, *mo);
        (*mo)++;
    }
    *d = (int)days + 1;
}

/* The host's time in seconds since 2000, game offset applied; -1 when the
 * deterministic clock is to be used. */
static long long host_secs(void)
{
    long long t;

    if (sRtcEnvOverride) return -1;
    t = (long long)np_host_rtc_now();
    if (t < 0) return -1;
    t += sHostOffset;
    if (t < 0 || t >= RTC_HOST_LIMIT) return -1;
    return t;
}
#endif

/* The current time: epoch fields plus elapsed whole seconds. */
static void now(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    u32 elapsed = (pc_os_vblank_count - ep_base_frames) / 60u;

#if defined(__wasm__)
    {
        long long t = host_secs();

        if (t >= 0) {
            secs_to_fields(t, y, mo, d, h, mi, s);
            return;
        }
    }
#endif
    *y = ep_year; *mo = ep_month; *d = ep_day;
    *h = ep_hour; *mi = ep_min;
    *s = ep_sec + (int)(elapsed % 60u);
    elapsed /= 60u;
    *mi += (int)(elapsed % 60u);
    elapsed /= 60u;
    *h += (int)(elapsed % 24u);
    elapsed /= 24u;

    if (*s >= 60) { *s -= 60; (*mi)++; }
    if (*mi >= 60) { *mi -= 60; (*h)++; }
    if (*h >= 24) { *h -= 24; elapsed++; }
    while (elapsed > 0) {
        int dim = days_in_month(*y, *mo);
        u32 step = (u32)(dim - *d) + 1u;

        if (elapsed < step) {
            *d += (int)elapsed;
            break;
        }
        elapsed -= step;
        *d = 1;
        if (++*mo > 12) { *mo = 1; (*y)++; }
    }
}

/* The raw image the library decodes, written where the ARM7 writes it. */
static void write_raw(int date_half, int time_half)
{
    volatile u32 *raw = (volatile u32 *)OS_GetSystemWork()->real_time_clock;
    int y, mo, d, h, mi, s;

    now(&y, &mo, &d, &h, &mi, &s);
    if (date_half) {
        raw[0] = (hex2bcd(y))
               | (hex2bcd(mo) << 8)
               | (hex2bcd(d) << 16)
               | ((u32)weekday(y, mo, d) << 24);
    }
    if (time_half) {
        /* 24-hour format: the afternoon bit mirrors hour >= 12, which is
         * what the hardware reports in 24h mode and what RtcBCD2HEX's
         * caller masks off. */
        raw[1] = (hex2bcd(h) | (h >= 12 ? 0x40u : 0u))
               | (hex2bcd(mi) << 8)
               | (hex2bcd(s) << 16);
    }
}

/* A write command: the ARM9 placed raw values in the work area; adopt them
 * as the new clock base, anchored at the current frame. */
static void adopt_raw(int date_half, int time_half)
{
    const volatile u32 *raw =
        (const volatile u32 *)OS_GetSystemWork()->real_time_clock;
    int y, mo, d, h, mi, s;

    now(&y, &mo, &d, &h, &mi, &s);
    if (date_half) {
        u32 w = raw[0];

        y = bcd2hex(w & 0xFF);
        mo = bcd2hex((w >> 8) & 0x1F);
        d = bcd2hex((w >> 16) & 0x3F);
    }
    if (time_half) {
        u32 w = raw[1];

        h = bcd2hex(w & 0x3F);
        mi = bcd2hex((w >> 8) & 0x7F);
        s = bcd2hex((w >> 16) & 0x7F);
    }
    ep_year = y; ep_month = mo; ep_day = d;
    ep_hour = h; ep_min = mi; ep_sec = s;
    ep_base_frames = pc_os_vblank_count;
#if defined(__wasm__)
    if (!sRtcEnvOverride) {
        long long t = (long long)np_host_rtc_now();

        if (t >= 0) {
            sHostOffset = fields_to_secs(y, mo, d, h, mi, s) - t;
        }
    }
#endif
}

static void rtc_respond(u32 data)
{
    u32 cmd = (data & RTC_PXI_COMMAND_MASK) >> RTC_PXI_COMMAND_SHIFT;
    u32 result = RTC_PXI_RESULT_SUCCESS;
    volatile u32 *raw = (volatile u32 *)OS_GetSystemWork()->real_time_clock;

    switch (cmd) {
    case RTC_PXI_COMMAND_RESET:
    case RTC_PXI_COMMAND_SET_HOUR_FORMAT:
        break;
    case RTC_PXI_COMMAND_READ_DATETIME: write_raw(1, 1); break;
    case RTC_PXI_COMMAND_READ_DATE:     write_raw(1, 0); break;
    case RTC_PXI_COMMAND_READ_TIME:     write_raw(0, 1); break;
    case RTC_PXI_COMMAND_WRITE_DATETIME: adopt_raw(1, 1); break;
    case RTC_PXI_COMMAND_WRITE_DATE:     adopt_raw(1, 0); break;
    case RTC_PXI_COMMAND_WRITE_TIME:     adopt_raw(0, 1); break;
    case RTC_PXI_COMMAND_READ_STATUS1:
    case RTC_PXI_COMMAND_READ_STATUS2:
    case RTC_PXI_COMMAND_READ_PULSE:
    case RTC_PXI_COMMAND_READ_ALARM1:
    case RTC_PXI_COMMAND_READ_ALARM2:
    case RTC_PXI_COMMAND_READ_ADJUST:
    case RTC_PXI_COMMAND_READ_FREE:
        /* A powered, healthy, alarm-less chip: all zeros. */
        raw[0] = 0;
        raw[1] = 0;
        break;
    case RTC_PXI_COMMAND_WRITE_STATUS1:
    case RTC_PXI_COMMAND_WRITE_STATUS2:
    case RTC_PXI_COMMAND_WRITE_PULSE:
    case RTC_PXI_COMMAND_WRITE_ALARM1:
    case RTC_PXI_COMMAND_WRITE_ALARM2:
    case RTC_PXI_COMMAND_WRITE_ADJUST:
    case RTC_PXI_COMMAND_WRITE_FREE:
        /* Accepted and forgotten: no alarm ever fires here, and the
         * status bits a write could set are ones nothing reads back. */
        break;
    default:
        result = RTC_PXI_RESULT_INVALID_COMMAND;
        break;
    }

    pc_pxi_reply(PXI_FIFO_TAG_RTC,
                 RTC_PXI_RESULT_BIT_MASK
                 | ((cmd << RTC_PXI_COMMAND_SHIFT) & RTC_PXI_COMMAND_MASK)
                 | result);
}

/* Called from pc_main.c before NitroMain. PC_RTC="YYYY-MM-DD HH:MM:SS"
 * overrides the fixed default epoch; the format is strict on purpose. */
int pc_rtc_init(void)
{
    const char *env = getenv("PC_RTC");

    if (env != NULL) {
        int y, mo, d, h, mi, s;

        if (sscanf(env, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6
            || y < 2000 || y > 2099 || mo < 1 || mo > 12
            || d < 1 || d > days_in_month(y - 2000, mo)
            || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) {
            fprintf(stderr, "pc-rtc: PC_RTC wants \"YYYY-MM-DD HH:MM:SS\" "
                            "(2000-2099), got \"%s\"\n", env);
            return -1;
        }
        ep_year = y - 2000; ep_month = mo; ep_day = d;
        ep_hour = h; ep_min = mi; ep_sec = s;
#if defined(__wasm__)
        sRtcEnvOverride = 1;
#endif
    }
    ep_base_frames = 0;
    pc_pxi_set_responder(PXI_FIFO_TAG_RTC, rtc_respond);
    return 0;
}
