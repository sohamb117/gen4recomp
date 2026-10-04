/*
 * One station of the WM model's Diamond/Pearl unit test: D's own ARM9 WM
 * library (arm9/asm/WM_*.s, recompiled by armrec for the D module) driving
 * pc_wm.c built with PC_GAME_DP, along the path D's local-wireless layer
 * takes (arm9/asm/unk_0202C198.s: parent SetParentParameter -> StartParent
 * -> StartMP; child StartScan per channel -> EndScan -> StartConnectEx ->
 * StartMP; both SetMPDataToPortEx paced by the send callback; Disconnect).
 * Games/platinum/pc/tests/wm/run_wm_test.mjs instantiates it once per
 * station and carries datagrams between them with loss, duplication and
 * reordering; the exports are the Platinum station's.
 *
 * The recompiled functions take armrec's calling convention, four words in
 * and r0:r1 out, with arguments past the fourth on the guest stack
 * (call_stk below), exactly as D's own code calls them.
 *
 * On top of the Platinum scenarios it holds pc_wm.c to the three points
 * where D's protocol differs from 4.2's (pc_wm.c header, notes 1-3): the
 * command buffers start full of stale words, so a model that read a fourth
 * INITIALIZE word or a WMMPTmpParam after D's 0x30-byte START_MP would see
 * garbage (LISTEN_ONLY: StartParent/StartConnect refused); StartMP's mpFreq
 * must land in mp_minFreq/mp_freq as D's WMSP_SetMPParameterCore puts it;
 * and nothing may be written past D's 0x7C0-byte WMStatus.
 */
#include <nitro/os.h>
#include <nitro/wm/common/wm.h>
#include "armrec_rt.h"
#include "pc_np_options.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "pc_wm.h"

#define EXPORT(name) __attribute__((export_name(#name)))

/* The recompiled library (armrec's uniform signature). */
#define REC(name) uint64_t name(uint32_t, uint32_t, uint32_t, uint32_t)
REC(WM_Initialize);
REC(WM_SetIndCallback);
REC(WM_SetPortCallback);
REC(WM_SetParentParameter);
REC(WM_StartParent);
REC(WM_StartMP);
REC(WM_GetMPReceiveBufferSize);
REC(WM_GetMPSendBufferSize);
REC(WM_SetMPDataToPortEx);
REC(WM_StartScan);
REC(WM_EndScan);
REC(WM_StartConnectEx);
REC(WM_Disconnect);
REC(WM_GetAllowedChannel);
REC(WM_GetDispersionScanPeriod);
REC(WMi_GetStatusAddress);
void armrec_init_WM_system(void);
void armrec_init_WM_standard(void);
void armrec_init_WM_mp(void);
void armrec_init_WM_sync(void);
void armrec_init_WM_etc(void);

/* The record offsets D's game reads (unk_0202C198.s) are the 4.2 headers'. */
_Static_assert(offsetof(WMStartParentCallback, state) == 0x8 && offsetof(WMStartParentCallback, aid) == 0x10
                   && offsetof(WMStartParentCallback, ssid) == 0x14, "StartParent record");
_Static_assert(offsetof(WMStartConnectCallback, aid) == 0xA, "StartConnect record");
_Static_assert(offsetof(WMStartScanCallback, gameInfoLength) == 0x36 && offsetof(WMStartScanCallback, gameInfo) == 0x38,
               "StartScan record");
_Static_assert(offsetof(WMStartMPCallback, state) == 0x4, "StartMP record");
_Static_assert(offsetof(WMPortRecvCallback, aid) == 0x12 && sizeof(WMPortRecvCallback) == 0x44, "PortRecv record");

#define U(p) ((uint32_t)(uintptr_t)(p))
#define RC(x) ((WMErrCode)(uint32_t)(x))

static uint64_t call_stk(armrec_fn fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t s0, uint32_t s1,
                         uint32_t s2)
{
    uint64_t r;

    armrec_sp -= 16;
    *(uint32_t *)(uintptr_t)(armrec_sp + 0) = s0;
    *(uint32_t *)(uintptr_t)(armrec_sp + 4) = s1;
    *(uint32_t *)(uintptr_t)(armrec_sp + 8) = s2;
    r = fn(a0, a1, a2, a3);
    armrec_sp += 16;
    return r;
}

#define TEST_GGID 0x00000333u
#define TEST_TGID 0x1234
#define TEST_PORT 14
#define TEST_MP_FREQ 2 /* not the default 1: D's ARM7 must store it */
#define PARENT_SIZE 192
#define CHILD_SIZE 38
#define MAX_ENTRY 4

enum {
    PH_INIT = 0,
    PH_READY = 1,
    PH_SEARCH = 2,
    PH_LINKED = 3,
    PH_DONE = 4,
    PH_LOST = 5,
};

static u8 sWmBuf[WM_SYSTEM_BUF_SIZE] ATTRIBUTE_ALIGN(32);
static WMParentParam sPP ATTRIBUTE_ALIGN(32);
static u16 sUserGI[16] ATTRIBUTE_ALIGN(32);
static u8 sRecvBuf[4096] ATTRIBUTE_ALIGN(32);
static u8 sSendBuf[1024] ATTRIBUTE_ALIGN(32);
static WMBssDesc sBss ATTRIBUTE_ALIGN(32);
static WMScanParam sScan ATTRIBUTE_ALIGN(32);
static u16 sMsg[2][256] ATTRIBUTE_ALIGN(32);

static struct {
    int role; /* 0 parent, 1 child */
    int phase;
    int target;
    int errors;
    u32 frame;
    u16 aid;
    u16 children;
    int sending;
    int msg_sel;
    u32 tx;
    u32 rx_next[16];
    u32 rx;
    u32 dups_or_gaps;
    u16 reason;
    int scan_channel;
    int disconnect_sent;
} S;

static void fail(const char *what, int code)
{
    fprintf(stderr, "test_wm_dp[%s]: %s (%d) at frame %u\n", S.role ? "child" : "parent", what, code, (unsigned)S.frame);
    S.errors++;
}

static WMStatus *status(void)
{
    return (WMStatus *)(uintptr_t)(uint32_t)WMi_GetStatusAddress(0, 0, 0, 0);
}

/* ---- receiving ---- */

static void port_cb(void *arg)
{
    WMPortRecvCallback *cb = arg;
    const u16 *d = cb->data;
    u32 seq;
    int i;

    if (cb->errcode != WM_ERRCODE_SUCCESS) {
        fail("port callback error", cb->errcode);
        return;
    }
    if (cb->state != WM_STATECODE_PORT_RECV) {
        return; /* PORT_INIT, CONNECTED, DISCONNECTED: generated by the ARM9 */
    }
    if (cb->length < 8 || d[0] != 0xC0DE || d[1] != cb->aid) {
        fail("bad message header", cb->length);
        return;
    }
    seq = d[2] | ((u32)d[3] << 16);
    if (S.role == 1 && S.rx == 0 && S.dups_or_gaps == 0) {
        S.rx_next[cb->aid] = seq;
    }
    if (seq != S.rx_next[cb->aid]) {
        S.dups_or_gaps++;
        fail("out-of-sequence message", (int)seq - (int)S.rx_next[cb->aid]);
        return;
    }
    for (i = 4; i < cb->length / 2; i++) {
        if (d[i] != (u16)(seq * 7 + i)) {
            fail("corrupt payload", i);
            return;
        }
    }
    S.rx_next[cb->aid]++;
    S.rx++;
}

/* ---- sending ---- */

static void send_cb(void *arg)
{
    WMPortSendCallback *cb = arg;

    S.sending = 0;
    if (cb->errcode != WM_ERRCODE_SUCCESS) {
        fail("send callback error", cb->errcode);
    }
}

static void try_send(void)
{
    u16 *m;
    u16 size = S.role == 0 ? PARENT_SIZE : CHILD_SIZE;
    WMErrCode err;
    int i;

    if (S.sending || S.phase != PH_LINKED || (int)S.tx >= S.target) {
        return;
    }
    if (S.role == 0 && S.children == 0) {
        return;
    }
    m = sMsg[S.msg_sel];
    m[0] = 0xC0DE;
    m[1] = S.aid;
    m[2] = (u16)S.tx;
    m[3] = (u16)(S.tx >> 16);
    for (i = 4; i < size / 2; i++) {
        m[i] = (u16)(S.tx * 7 + i);
    }
    err = RC(call_stk(WM_SetMPDataToPortEx, U(send_cb), 0, U(m), size, 0xFFFF, TEST_PORT, WM_PRIORITY_NORMAL));
    if (err != WM_ERRCODE_OPERATING) {
        fail("WM_SetMPDataToPortEx", err);
        return;
    }
    S.sending = 1;
    S.msg_sel ^= 1;
    S.tx++;
}

/* ---- MP ---- */

static void check_dp_status_after_mp_start(void)
{
    const WMStatus *st = status();
    const u8 *tail = (const u8 *)st + 0x7C0;
    int i;

    /* D's WMSP_StartMP: SetMPParameterCore(req->param), mask 0x0003. */
    if (st->mp_minFreq != TEST_MP_FREQ || st->mp_freq != TEST_MP_FREQ || st->mp_maxFreq != WM_DEFAULT_MP_FREQ_LIMIT) {
        fail("StartMP mpFreq not in mp_minFreq/mp_freq", st->mp_freq);
    }
    if (st->miscFlags != 0) {
        fail("misc flags set from a stale INITIALIZE word", (int)st->miscFlags);
    }
    for (i = 0; i < 0x40; i++) { /* to the end of the 0x800 status buffer */
        if (tail[i] != 0) {
            fail("write past D's 0x7C0-byte WMStatus", 0x7C0 + i);
            break;
        }
    }
}

static void mp_cb(void *arg)
{
    WMStartMPCallback *cb = arg;

    if (cb->errcode != WM_ERRCODE_SUCCESS) {
        fail("StartMP callback", cb->errcode);
        return;
    }
    if (cb->state == WM_STATECODE_MP_START) {
        check_dp_status_after_mp_start();
        if (S.role == 1) {
            S.phase = PH_LINKED;
        }
    }
}

static void start_mp(void)
{
    WMErrCode err = RC(call_stk(WM_StartMP, U(mp_cb), U(sRecvBuf), (u32)WM_GetMPReceiveBufferSize(0, 0, 0, 0),
                                U(sSendBuf), (u32)WM_GetMPSendBufferSize(0, 0, 0, 0), TEST_MP_FREQ, 0));

    if (err != WM_ERRCODE_OPERATING) {
        fail("WM_StartMP", err);
    }
}

/* ---- parent ---- */

static void parent_cb(void *arg)
{
    WMStartParentCallback *cb = arg;

    if (cb->errcode != WM_ERRCODE_SUCCESS) {
        fail("StartParent callback", cb->errcode);
        return;
    }
    switch (cb->state) {
    case WM_STATECODE_PARENT_START:
        start_mp();
        break;
    case WM_STATECODE_CONNECTED:
        if (cb->ssid[0] != 'T' || memcmp(&cb->ssid[1], "DP", 3) != 0) {
            fail("child ssid not carried", cb->ssid[0]);
        }
        S.children |= (u16)(1u << cb->aid);
        S.rx_next[cb->aid] = 0;
        S.phase = PH_LINKED;
        break;
    case WM_STATECODE_DISCONNECTED:
        S.children &= (u16) ~(1u << cb->aid);
        S.reason = cb->reason;
        S.phase = S.children ? PH_LINKED : PH_LOST;
        break;
    default:
        break;
    }
}

static void set_pparam_cb(void *arg)
{
    WMCallback *cb = arg;

    if (cb->errcode != WM_ERRCODE_SUCCESS) {
        fail("SetParentParameter callback", cb->errcode);
        return;
    }
    if (RC(WM_StartParent(U(parent_cb), 0, 0, 0)) != WM_ERRCODE_OPERATING) {
        fail("WM_StartParent", 0);
    }
}

static void start_parent(void)
{
    memset(&sPP, 0, sizeof sPP);
    memcpy(sUserGI, "HELLO-UNION-ROOM", 16);
    sPP.userGameInfo = sUserGI;
    sPP.userGameInfoLength = 16;
    sPP.ggid = TEST_GGID;
    sPP.tgid = TEST_TGID;
    sPP.entryFlag = 1;
    sPP.maxEntry = MAX_ENTRY;
    sPP.beaconPeriod = 200;
    sPP.channel = 7;
    sPP.parentMaxSize = PARENT_SIZE;
    sPP.childMaxSize = CHILD_SIZE;
    if (RC(WM_SetParentParameter(U(set_pparam_cb), U(&sPP), 0, 0)) != WM_ERRCODE_OPERATING) {
        fail("WM_SetParentParameter", 0);
    }
    S.phase = PH_SEARCH;
}

/* ---- child ---- */

static void start_scan(void);

static void connect_cb(void *arg)
{
    WMStartConnectCallback *cb = arg;

    if (cb->errcode != WM_ERRCODE_SUCCESS) {
        fail("StartConnect callback", cb->errcode);
        return;
    }
    switch (cb->state) {
    case WM_STATECODE_CONNECTED:
        S.aid = cb->aid;
        start_mp();
        break;
    case WM_STATECODE_DISCONNECTED:
        S.reason = cb->reason;
        S.phase = PH_LOST;
        break;
    default:
        break;
    }
}

static void end_scan_cb(void *arg)
{
    static u8 ssid[WM_SIZE_CHILD_SSID];
    WMCallback *cb = arg;

    if (cb->errcode != WM_ERRCODE_SUCCESS) {
        fail("EndScan callback", cb->errcode);
        return;
    }
    ssid[0] = 'T';
    memcpy(&ssid[1], "DP", 3);
    if (RC(call_stk(WM_StartConnectEx, U(connect_cb), U(&sBss), U(ssid), 1, WM_AUTHMODE_OPEN_SYSTEM, 0, 0))
        != WM_ERRCODE_OPERATING) {
        fail("WM_StartConnectEx", 0);
    }
}

static void scan_cb(void *arg)
{
    WMStartScanCallback *cb = arg;

    if (cb->errcode != WM_ERRCODE_SUCCESS) {
        fail("StartScan callback", cb->errcode);
        return;
    }
    if (cb->state == WM_STATECODE_PARENT_FOUND && cb->gameInfoLength >= 16 && cb->gameInfo.ggid == TEST_GGID
        && (cb->gameInfo.attribute & WM_ATTR_FLAG_ENTRY)) {
        if (memcmp(cb->gameInfo.userGameInfo, "HELLO-UNION-ROOM", 16) != 0 || sBss.gameInfo.ggid != TEST_GGID) {
            fail("game info not carried", 0);
        }
        if (RC(WM_EndScan(U(end_scan_cb), 0, 0, 0)) != WM_ERRCODE_OPERATING) {
            fail("WM_EndScan", 0);
        }
        return;
    }
    start_scan();
}

static void start_scan(void)
{
    u16 allowed = (u16)WM_GetAllowedChannel(0, 0, 0, 0);

    if (allowed == 0 || allowed == 0x8000) {
        fail("no allowed channel", allowed);
        return;
    }
    do {
        S.scan_channel = S.scan_channel % 16 + 1;
    } while (!(allowed & (1u << (S.scan_channel - 1))));
    memset(&sScan, 0, sizeof sScan);
    sScan.scanBuf = &sBss;
    sScan.channel = (u16)S.scan_channel;
    sScan.maxChannelTime = (u16)((u16)WM_GetDispersionScanPeriod(0, 0, 0, 0) / 3);
    memset(sScan.bssid, 0xff, sizeof sScan.bssid);
    if (RC(WM_StartScan(U(scan_cb), U(&sScan), 0, 0)) != WM_ERRCODE_OPERATING) {
        fail("WM_StartScan", 0);
    }
    S.phase = PH_SEARCH;
}

static void disconnect_cb(void *arg)
{
    WMCallback *cb = arg;

    if (cb->errcode != WM_ERRCODE_SUCCESS) {
        fail("Disconnect callback", cb->errcode);
        return;
    }
    S.phase = PH_DONE;
}

/* ---- both ---- */

static void ind_cb(void *arg)
{
    WMIndCallback *cb = arg;

    if (cb->errcode != WM_ERRCODE_SUCCESS) {
        fail("indication error", cb->errcode);
    }
}

static void init_cb(void *arg)
{
    WMCallback *cb = arg;

    if (cb->errcode != WM_ERRCODE_SUCCESS) {
        fail("Initialize callback", cb->errcode);
        return;
    }
    if (RC(WM_SetIndCallback(U(ind_cb), 0, 0, 0)) != WM_ERRCODE_SUCCESS
        || RC(WM_SetPortCallback(TEST_PORT, U(port_cb), 0, 0)) != WM_ERRCODE_SUCCESS) {
        fail("callback registration", 0);
        return;
    }
    S.phase = PH_READY;
}

EXPORT(station_init) int station_init(int role, int target)
{
    memset(&S, 0, sizeof S);
    S.role = role;
    S.target = target;
    armrec_init_WM_system();
    armrec_init_WM_standard();
    armrec_init_WM_mp();
    armrec_init_WM_sync();
    armrec_init_WM_etc();
    /* Command buffers as a running game leaves them: every word past the
     * ones a command writes is stale (header comment). */
    memset((void *)(uintptr_t)DP_WM_CMDBUF, 0xFF, 10 * 0x100);
    pc_wm_init();
    if (RC(WM_Initialize(U(sWmBuf), U(init_cb), 2, 0)) != WM_ERRCODE_OPERATING) {
        fail("WM_Initialize", 0);
        return -1;
    }
    return 0;
}

EXPORT(station_frame) void station_frame(void)
{
    S.frame++;
    if (S.phase == PH_READY) {
        if (S.role == 0) {
            start_parent();
        } else {
            start_scan();
        }
    }
    if (S.role == 1 && S.phase == PH_LINKED && (int)S.tx >= S.target && (int)S.rx >= S.target && !S.sending
        && !S.disconnect_sent) {
        S.disconnect_sent = 1;
        if (RC(WM_Disconnect(U(disconnect_cb), 0, 0, 0)) != WM_ERRCODE_OPERATING) {
            fail("WM_Disconnect", 0);
        }
    }
    try_send();
    pc_wm_step(); /* the VBlank: the ARM7 answers */
}

EXPORT(station_phase) int station_phase(void) { return S.phase; }
EXPORT(station_errors) int station_errors(void) { return S.errors; }
EXPORT(station_tx) u32 station_tx(void) { return S.tx; }
EXPORT(station_rx) u32 station_rx(void) { return S.rx; }
EXPORT(station_aid) int station_aid(void) { return S.aid; }
EXPORT(station_reason) int station_reason(void) { return S.reason; }
EXPORT(station_wm_state) int station_wm_state(void)
{
    const WMStatus *st = status();

    return st ? st->state : -1;
}
EXPORT(station_link_active) int station_link_active(void)
{
    return (int)pc_np_stat.link_active;
}
