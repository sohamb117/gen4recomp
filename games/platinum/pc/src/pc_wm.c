/*
 * The ARM7 wireless manager, behind PXI tag 10, with the radio replaced by
 * datagrams.
 *
 * The game links the stock NitroSDK ARM9 WM library (libraries/wm): it
 * validates every call, writes the request into a command buffer and sends
 * the buffer's address over PXI_FIFO_TAG_WM, and it decodes the callback
 * records the ARM7 writes back into the shared fifo7to9 buffer. On a console
 * the ARM7 runs the WMSP request thread (libraries/wm_arm7, req_*.c) on top
 * of the WL radio firmware. This file is that ARM7 half. It follows the
 * WMSP sources record for record: the same state transitions in the shared
 * WMStatus, the same callback shapes and order, the same size bookkeeping
 * (WMSP_SetParentMaxSize and friends, which the ARM9's WM_StartMP precheck
 * reads back). The WL layer underneath is what changes: beacons, joins and
 * MP data travel as datagrams through the host's np_host_net_* imports.
 *
 * Shape of the model.
 *
 *  - The responder copies each request and acknowledges the command buffer
 *    at once, but never answers synchronously: a reply inside
 *    PXI_SendWordByFifo would run the ARM9 callback before WMi_SendCommand
 *    has even returned OPERATING, and a callback that issues the next WM
 *    call would overwrite fifo7to9 while the ARM9 is still reading it. The
 *    requests run from pc_wm_step(), the port's per-VBlank point (OS_Halt),
 *    which is where a FIFO interrupt would have landed. Requests that block
 *    the ARM7's request thread on hardware (a scan for maxChannelTime, a
 *    join, a channel measurement) block this queue the same way, so the
 *    ordering the game sees (StartScan's result before the EndScan queued
 *    behind it) is the console's.
 *
 *  - Station identity: np_host_net_self() is the station id; the MAC is
 *    00:09:BF plus its low 24 bits and is also written where the firmware
 *    puts it (OS_GetMacAddress), because the game compares its own MAC with
 *    BSSIDs it scans. With networking off (id 0) the firmware MAC is left as
 *    the port has always had it and the model answers as a DS with nobody in
 *    range.
 *
 *  - Parents broadcast a beacon every beaconPeriod (game info snapshotted
 *    at StartParent / SetGameInfo / SetEntry, as WL does); scanning children
 *    keep a table of parents heard recently and a scan on a channel reports
 *    one of them. StartConnect sends a join request, the parent assigns the
 *    lowest free AID, and both sides raise the WMSP CONNECTED indications.
 *
 *  - MP data: no frame barrier. On hardware the game's own protocol already
 *    tolerates MP cycles that carry nothing (MP_IND with NO_DATA, retries):
 *    CommSys clocks itself on the port-send callbacks (Unk_02100A1C/1D in
 *    communication_system.c) and on CommSys_CheckRecvLimit, which stops the
 *    parent after three sends a child has not answered, and the battle,
 *    trade and union-room state machines meet at sync tokens
 *    (CommTiming_IsSyncState), never at frame numbers. What it cannot
 *    tolerate is loss or reordering inside a port's stream (ports 8-15 are
 *    sequenced on hardware) or a failed send (sub_020353B0 asserts). So each
 *    link (parent<->child) is a reliable ordered stream over datagrams:
 *    frame-tagged bundles carrying sequence numbers, a cumulative ack and the
 *    sender's oldest held sequence, retransmitted until acknowledged. The
 *    port-send callback reports success as soon as the message is inside a
 *    small window of unacknowledged data and is deferred (never failed) when
 *    the receiver stops draining, which pipelines like MP over latency and
 *    still bounds the queue. A receiver only takes data while its own MP is
 *    running, as a child that has not started MP is never polled. A link
 *    with no datagram for the MP lifetime (WM_SetLifeTime; 4 s default) is
 *    torn down with WM_DISCONNECT_REASON_MP_LIFETIME, which is what a
 *    console reports when its partner walks out of range.
 *
 *  - NP_STAT_LINK_ACTIVE is 1 while the radio is in a link state with
 *    networking on (and for two seconds after, so the union room's
 *    parent/scan alternation does not flap the host's speed lock).
 *
 * The protocol is this port's own (version byte below); libntr's
 * sim7/wmsp_request.c (MIT, cybervisi0n/libntr) was read as a second
 * opinion on the WMSP request mapping, no code was taken from it.
 *
 * Diamond/Pearl (PC_GAME_DP). The same model answers D/P's NitroSDK 3.2-era
 * WM library (arm9/asm/WM_{system,standard,mp,sync,etc,ds,dcf}.s, recompiled
 * by armrec) and stands where D's own ARM7 WMSP runs (arm7/asm/WM_sp.s and
 * the WMSP_* functions in arm7/asm/ext.s, wram2.s). Measured field by field
 * against those, with the 4.2 offsets from clang's record layout for wasm32:
 *
 *   Identical. PXI tag 10 and the FIFO word (the 26-bit address of the
 *   command buffer one way, of fifo7to9 the other, error bit clear; D's
 *   WMSP_ReturnResult2Wm9). wm_callback_control at 0x027FFF96 bit 0 and
 *   wm_rssi_pool at 0x027FFF98. The firmware MAC at 0x027FFCF4 (D's
 *   OS_GetMacAddress) and the allowed-channel word at 0x027FFCFA (D's
 *   WM_GetAllowedChannel). Every API id the D ARM9 sends (0-16, 17-19 DCF,
 *   20 WEPKEY, 24 SET_GAMEINFO, 25 SET_BEACON_IND, 29 SET_LIFETIME, 30
 *   MEASURE_CHANNEL, 33 SET_ENTRY, 38 START_SCAN_EX, 39 WEPKEY_EX) and
 *   0x80-0x82; WmReceiveFifo dispatches apiid < 44 (WM_NUM_OF_CALLBACK) as
 *   4.2 does. The system buffer split (WMArm9Buf 0x200 at +0, WMArm7Buf
 *   0x300 at +0x200, WMStatus 0x800 at +0x500, fifo9to7 +0xD00, fifo7to9
 *   +0xE00) and WMArm9Buf itself (CallbackTable 0x18, indCallback 0xC8,
 *   portCallbackTable 0xCC, portCallbackArgument 0x10C,
 *   connectedAidBitmap 0x14C, myAid 0x150). WMArm7Buf: status 0, fifo7to9
 *   8, connectPInfo 0x10 (D's WMSP_GetLinkLevel reads its platform byte at
 *   0x53), requestBuf 0xD0, 32 entries of 16 bytes. WMStatus from 0 to
 *   0x7C0, every offset either side uses: state 0, mp_flag 0xC, the size
 *   words 0x30-0x3E, VCounts/intervals 0x40-0x46 and their ticks 0x48/0x50,
 *   mp_minFreq/freq/maxFreq 0x58-0x5C, the MP flags 0x5E-0x6A, mp_recvBufSel
 *   0x70, mp_recvBufSize 0x72, mp_recvBuf 0x74/0x78, mp_sendBuf 0x7C,
 *   mp_sendBufSize 0x80, mp_readyBitmap 0x86, the mode words 0x92-0x9C,
 *   mp_pingFlag/Counter 0x9E/0xA0, linkLevel 0xBC, minRssi 0xBE,
 *   beaconIndicateFlag 0xC2, wepKeyId 0xC4, pwrMgtMode 0xC6, miscFlags 0xC8,
 *   valarm_queuedFlag 0xCE, MacAddress 0xE0, mode 0xE6, pparam 0xE8
 *   (maxEntry 0xF8), child_bitmap 0x182, aid 0x188, wepMode 0x196,
 *   wep_flag 0x198, wepKey 0x19C, rate 0x1EC, preamble 0x1EE,
 *   enableChannel 0x1F4, allowedChannel 0x1F6, portSeqNo 0x1F8 (0x100
 *   bytes), sendQueueMutex 0x71C, sendQueueInUse 0x734, mp_lastRecvTick
 *   0x738, mp_lifeTimeTick 0x7B8. WMParentParam (64 bytes; D's
 *   WMSP_CopyParentParam and WmCheckParentParameter) and WMGameInfo.
 *   WMBssDesc with otherElementCount (gameInfoLength 0x3C, gameInfo 0x40;
 *   D's WM_GetOtherElements). WM_SIZE_MP_DATA_MAX 512 (WM_SetMPDataToPortEx
 *   refuses > 0x200). WMMpRecvHeader with errBitmap (10-byte head) and
 *   WMMpRecvData (12), WMMpRecvBuf (54): D's WM_GetMPReceiveBufferSize
 *   computes ((maxRecv + 12) * maxEntry + 0x29) & ~31 and (maxRecv + 0x51)
 *   & ~31, and WM_ReadMPData walks count 4 / length 6 / data 0xA. The
 *   request blocks for START_SCAN (16 bytes), START_SCAN_EX (60, with
 *   ssidMatchLength), START_CONNECT (40; powerSave 0x20, authMode 0x26),
 *   MEASURE_CHANNEL (10), and the word lists of SET_P_PARAM, START_PARENT
 *   (powerSave), DISCONNECT (aid bitmap), SET_MP_DATA (data, size, dest,
 *   port, prio, callback, arg), SET_GAMEINFO, SET_LIFETIME, SET_ENTRY,
 *   SET_BEACON_IND, SET_WEPKEY(_EX). The callback records as the ARM9 and
 *   the game read them: WMCallback, WMStartParentCallback (state 8, mac 0xA,
 *   aid 0x10, reason 0x12, ssid 0x14, sizes 0x2C/0x2E), WMStartConnect
 *   (state 8, aid 0xA, reason 0xC, mac 0x10, sizes 0x16/0x18),
 *   WMStartScanCallback (state 8, gameInfoLength 0x36, gameInfo 0x38),
 *   WMStartMPCallback (state 4, recvBuf 8), WMPortSendCallback (callback
 *   0x1C), WMPortRecvCallback (68 bytes: port 6, recvBuf 8, aid 0x12, mac
 *   0x14, seqNo 0x1A, arg 0x1C, myAid 0x20, connectedAidBitmap 0x22, ssid
 *   0x24, reason 0x3C, sizes 0x40/0x42; the ARM9 synthesizes the same
 *   record for CONNECTED/DISCONNECTED), WMDisconnectCallback.
 *
 *   Different, and handled under PC_GAME_DP:
 *    1. INITIALIZE/ENABLE carry three words (WM7, status, fifo7to9; D's
 *       WM_Initialize/WM_Enable and WMSP_Initialize/WMSP_Enable). 4.2 adds
 *       miscFlags as req[4]; on D that word is whatever an earlier command
 *       left in the reused command buffer (a SET_GAMEINFO leaves its tgid
 *       there), and an odd one would read as WM_MISC_FLAG_LISTEN_ONLY and
 *       refuse StartParent/StartConnect. D has no misc flags: 0.
 *    2. START_MP is 0x30 bytes: {apiid, rsv, recvBuf, recvBufSize/2,
 *       sendBuf, sendBufSize, WMMPParam param} with no WMMPTmpParam at 0x30.
 *       D's ARM9 fills `param` (mask 0x0003 from WM_StartMP with
 *       minFrequency = frequency = mpFreq at 0x18/0x1A; 0x1E03 or 0x1E07
 *       from WM_StartMPEx, defaultRetryCount at 0x2A and the three mode
 *       bytes at 0x2C-0x2E), and D's WMSP_StartMP hands it to
 *       WMSP_SetMPParameterCore: the values land in the persistent
 *       mp_minFreq/mp_freq/... words, not in a per-MP copy.
 *    3. WMStatus ends at 0x7C0 (WM_ReadStatus copies 0x7C0 bytes). 4.2
 *       appends mp_current_{minFreq, freq, maxFreq, minPollBmpMode,
 *       singlePacketMode, defaultRetryCount, ignoreFatalErrorMode} and two
 *       reserved bytes at 0x7C0-0x7CF, inside the 0x800 status buffer, so
 *       nothing reads them on D and the model does not write them.
 *
 *   Different, and unreachable from D (left as they are): 4.2's
 *   WMSP_Set{Parent,Child}MaxSize clamp to 0x200 where D's do not, and
 *   4.2's CopyParentParam caps a multiboot childMaxSize at 8; D's ARM9
 *   refuses parent/child sizes over 0x200 (KS included) and the game never
 *   multiboots. SET_MP_PARAMETER, SET_BEACON_PERIOD, SET_PS_MODE and the RX
 *   test modes have no D ARM9 caller.
 */
#include <nitro/os.h>
#include <nitro/pxi.h>
#include <nitro/spi/common/config.h>
#include <nitro/wm.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_wm.h"

#include "pc_np_options.h"

#if defined(__wasm__)
#include <np_guest_abi.h>
#endif

extern void pc_pxi_set_responder(int tag, void (*fn)(u32 data));
extern void pc_pxi_reply(int tag, u32 data);

/* WL labels the WMSP sources use (nitro_wl/ARM7/WlCmdLabel.h is an ARM7
 * header and does not compile against the ARM9 ones). */
enum {
    WMI_PMG_CONT_ACT = 0,
    WMI_PMG_PS = 1,
    WMI_MODE_PARENT = 1,
    WMI_MODE_CHILD = 2,
    WMI_RATE_2M = 2,
    WMI_PREAMBLE_SHORT = 1,
    WMI_RSN_DEAUTH_LEAVING = 3,
    WMI_ENABLE_CHANNEL = 0x7ffe,      /* channels 1-14, what a US console reports */
    WMI_ENABLE_CHANNEL_MASK = 0x1fff, /* WMSP_ENABLE_CHANNEL_MASK */
    WMI_ENABLE_CHANNEL_PERIOD = 5,    /* WMSP_ENABLE_CHANNEL_PERIOD */
    WMI_DEFAULT_LIFE_TIME = 40,       /* WMSP_DEFAULT_{CAM,MP}_LIFE_TIME, 100 ms units */
};

/* ------------------------------------------------------------------ */
/* Model state                                                        */

#define WMI_REQ_MAX 64
#define WMI_REQ_WORDS 32 /* 128 bytes: the largest request is ~64 */
#define WMI_BSS_MAX 16
#define WMI_LINKS (WM_NUM_MAX_CHILD + 1) /* [0] = the parent (child side), [aid] = child */
#define WMI_LINK_QUEUE 32
#define WMI_WINDOW 8      /* unacked messages a send callback may run ahead of */
#define WMI_RTO 3         /* frames before an unacked message is resent */
#define WMI_PEND_MAX 64
#define WMI_PKT_MAX 1400  /* datagram budget, under a LAN MTU */
#define WMI_BSS_TTL 120   /* frames a parent stays in the scan table after its last beacon */
#define WMI_JOIN_TIMEOUT 180 /* frames: WMSP_JOIN_TIMEOUT + AUTHENTICATE + ASSOCIATE, 2 s each */
#define WMI_JOIN_RETRY 10
#define WMI_ACTIVE_HOLD 120

typedef struct {
    u16 seq;
    u16 port;
    u16 len;
    u32 last_tx; /* 0 = never sent */
    u16 data[WM_SIZE_MP_DATA_MAX / 2];
} wmi_msg;

typedef struct {
    u8 used;
    u8 mac[WM_SIZE_MACADDR];
    u32 peer;
    u32 last_rx;
    u16 tx_seq;   /* next sequence to assign */
    u16 rx_next;  /* next sequence expected from the peer */
    int qhead, qcount;
    wmi_msg q[WMI_LINK_QUEUE]; /* unacked, oldest first */
} wmi_link;

typedef struct {
    u8 used;
    u8 mac[WM_SIZE_MACADDR];
    u32 peer;
    u16 channel;
    u16 period;
    u16 gi_len;
    u32 seen;
    u16 gi[WM_SIZE_GAMEINFO / 2];
} wmi_bss;

typedef struct {
    WMPortSendCallback cb;
    u16 links;
    u16 seq[WMI_LINKS];
} wmi_pend;

static struct {
    WMArm7Buf *wm7;
    WMStatus *st;
    u32 *fifo;
    u32 frame;
    int in_step;

    u32 req[WMI_REQ_MAX][WMI_REQ_WORDS];
    int req_head, req_count;

    u32 self; /* station id, 0 = networking off */
    u8 mac[WM_SIZE_MACADDR];

    /* parent */
    u16 gi[WM_SIZE_GAMEINFO / 2]; /* beacon game info snapshot */
    u16 gi_len;
    u32 next_beacon;
    u16 beacon_period; /* ms */

    /* scan / join / measure: requests that hold the request thread */
    struct {
        u8 active, ex;
        u32 done;
        u16 channels; /* bit n = channel n */
        u16 buf_size;
        u8 bssid[WM_SIZE_MACADDR];
        WMBssDesc *buf;
        u16 ssid_len;
        u8 ssid[WM_SIZE_SSID];
    } scan;
    u32 scan_rr;
    struct {
        u8 active;
        u32 deadline, next_tx, nonce, peer;
        u8 bssid[WM_SIZE_MACADDR];
        u8 ssid[WM_SIZE_CHILD_SSID];
        u16 tgid;
    } join;
    struct {
        u8 active;
        u32 done;
        u16 channel;
    } measure;

    wmi_bss bss[WMI_BSS_MAX];
    wmi_link link[WMI_LINKS];
    wmi_pend pend[WMI_PEND_MAX];
    int pend_head, pend_count;

    u32 lifetime; /* frames, 0 = never */
    u32 active_until;
    int trace;
} W;

static void wmi_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void wmi_trace(const char *fmt, ...)
{
    va_list ap;

    if (!W.trace) {
        return;
    }
    va_start(ap, fmt);
    fprintf(stderr, "pc_wm: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static u32 wmi_ms_to_frames(u32 ms)
{
    u32 f = (ms * 60u + 999u) / 1000u;
    return f ? f : 1;
}

static int wmi_seq_lt(u16 a, u16 b)
{
    return (s16)(a - b) < 0;
}

/* ------------------------------------------------------------------ */
/* The host transport                                                 */

#if defined(__wasm__)
static u32 wmi_net_self(void) { return np_host_net_self(); }
static void wmi_net_send(u32 peer, const void *buf, u32 len) { (void)np_host_net_send(peer, buf, len); }
static int wmi_net_recv(u32 *peer, void *buf, u32 cap) { return np_host_net_recv(peer, buf, cap); }
#else
/* Hosts without the contract-v2 imports have no partner in range. */
static u32 wmi_net_self(void) { return 0; }
static void wmi_net_send(u32 peer, const void *buf, u32 len) { (void)peer; (void)buf; (void)len; }
static int wmi_net_recv(u32 *peer, void *buf, u32 cap) { (void)peer; (void)buf; (void)cap; return 0; }
#endif

#ifndef NP_NET_BROADCAST
#define NP_NET_BROADCAST 0xFFFFFFFFu
#endif

/* ------------------------------------------------------------------ */
/* Callback records to the ARM9                                       */

/* Every record goes through fifo7to9, the buffer WMSP_GetBuffer4Callback2Wm9
 * returns, and the ARM9's WmReceiveFifo runs synchronously from here. A
 * callback that issues another WM call only queues it, so the buffer is
 * never rewritten while the ARM9 still reads it. */
static void *wmi_cb_begin(void)
{
    memset(W.fifo, 0, WM_FIFO_BUF_SIZE);
    return W.fifo;
}

static void wmi_cb_send(void)
{
    OS_GetSystemWork()->wm_callback_control |= WM_EXCEPTION_CB_MASK;
    pc_pxi_reply(PXI_FIFO_TAG_WM, (u32)(uintptr_t)W.fifo);
}

static void wmi_cb_simple(u16 apiid, u16 errcode)
{
    WMCallback *cb = wmi_cb_begin();

    cb->apiid = apiid;
    cb->errcode = errcode;
    wmi_cb_send();
}

static void wmi_cb_parent(u16 state, u16 aid, const u8 *mac, u16 reason, const u8 *ssid)
{
    WMStartParentCallback *cb = wmi_cb_begin();

    cb->apiid = WM_APIID_START_PARENT;
    cb->errcode = WM_ERRCODE_SUCCESS;
    cb->state = state;
    cb->aid = aid;
    cb->reason = reason;
    if (mac != NULL) {
        memcpy(cb->macAddress, mac, WM_SIZE_MACADDR);
    }
    if (ssid != NULL) {
        memcpy(cb->ssid, ssid, WM_SIZE_CHILD_SSID);
    }
    cb->parentSize = W.st->mp_parentSize;
    cb->childSize = W.st->mp_childSize;
    wmi_cb_send();
}

static void wmi_cb_connect(u16 errcode, u16 state, u16 aid, const u8 *mac, u16 reason)
{
    WMStartConnectCallback *cb = wmi_cb_begin();

    cb->apiid = WM_APIID_START_CONNECT;
    cb->errcode = errcode;
    cb->state = state;
    cb->aid = aid;
    cb->reason = reason;
    if (mac != NULL) {
        memcpy(cb->macAddress, mac, WM_SIZE_MACADDR);
    }
    cb->parentSize = W.st->mp_parentSize;
    cb->childSize = W.st->mp_childSize;
    wmi_cb_send();
}

static void wmi_cb_port_send(const WMPortSendCallback *rec)
{
    WMPortSendCallback *cb = wmi_cb_begin();

    *cb = *rec;
    wmi_cb_send();
}

/* ------------------------------------------------------------------ */
/* WMStatus bookkeeping, as wmsp_system.c does it                     */

static void wmi_reset_size_vars(void)
{
    WMStatus *st = W.st;

    st->mp_sendSize = 0;
    st->mp_recvSize = 0;
    st->mp_parentSize = 0;
    st->mp_childSize = 0;
    st->mp_maxSendSize = 0;
    st->mp_maxRecvSize = 0;
    st->mp_parentMaxSize = 0;
    st->mp_childMaxSize = 0;
}

static void wmi_set_parent_max_size(u16 size)
{
    WMStatus *st = W.st;

    if (size > 0x200) {
        size = 0x200;
    }
    st->mp_parentSize = size;
    st->mp_parentMaxSize = size;
    if (st->aid == 0) {
        st->mp_maxSendSize = (u16)(size + WM_HEADER_PARENT_MAX_SIZE);
        st->mp_sendSize = st->mp_maxSendSize;
    } else {
        st->mp_maxRecvSize = (u16)(size + WM_HEADER_PARENT_MAX_SIZE);
        st->mp_recvSize = st->mp_maxRecvSize;
    }
}

static void wmi_set_child_max_size(u16 size)
{
    WMStatus *st = W.st;

    if (size > 0x200) {
        size = 0x200;
    }
    st->mp_childMaxSize = size;
    st->mp_childSize = size;
    if (st->aid == 0) {
        st->mp_maxRecvSize = (u16)(size + WM_HEADER_CHILD_MAX_SIZE);
        st->mp_recvSize = st->mp_maxRecvSize;
    } else {
        st->mp_maxSendSize = (u16)(size + WM_HEADER_CHILD_MAX_SIZE);
        st->mp_sendSize = st->mp_maxSendSize;
    }
}

/* WMSP_GetAllowedChannel, verbatim in behaviour. */
static u16 wmi_allowed_channel(u16 bits)
{
    u16 temp = (u16)(bits & WMI_ENABLE_CHANNEL_MASK);
    s32 min, max, center, i;

    if (temp == 0) {
        return 0;
    }
    for (min = 0; min < 16; min++) {
        if (temp & (1 << min)) {
            break;
        }
    }
    for (max = 15; max; max--) {
        if (temp & (1 << max)) {
            break;
        }
    }
    if ((max - min) < WMI_ENABLE_CHANNEL_PERIOD) {
        return (u16)(1 << min);
    }
    center = (max + min) / 2;
    for (i = 0; i < (max - min); i++) {
        center = center + ((((i % 2) * 2) - 1) * i);
        if (temp & (1 << center)) {
            break;
        }
    }
    if (((max - center) < WMI_ENABLE_CHANNEL_PERIOD) || ((center - min) < WMI_ENABLE_CHANNEL_PERIOD)) {
        return (u16)((1 << min) | (1 << max));
    }
    return (u16)((1 << max) | (1 << center) | (1 << min));
}

/* WMSPi_CommonInit: what Initialize and Enable reset. */
static void wmi_common_init(u32 misc_flags)
{
    WMStatus *st = W.st;

    st->mp_flag = FALSE;
    st->child_bitmap = 0;
    st->mp_readyBitmap = 0;
    st->ks_flag = FALSE;
    st->dcf_flag = FALSE;
    st->VSyncFlag = FALSE;
    st->valarm_queuedFlag = FALSE;
    st->beaconIndicateFlag = 0;
    st->mp_minFreq = 1;
    st->mp_freq = 1;
    st->mp_maxFreq = WM_DEFAULT_MP_FREQ_LIMIT;
    st->mp_defaultRetryCount = 0;
    st->mp_minPollBmpMode = FALSE;
    st->mp_singlePacketMode = FALSE;
    st->mp_ignoreFatalErrorMode = FALSE;
    st->mp_ignoreSizePrecheckMode = FALSE;
#if !defined(PC_GAME_DP) /* past the end of D's 0x7C0-byte WMStatus (header note 3) */
    st->mp_current_minFreq = st->mp_minFreq;
    st->mp_current_freq = st->mp_freq;
    st->mp_current_maxFreq = st->mp_maxFreq;
    st->mp_current_defaultRetryCount = st->mp_defaultRetryCount;
    st->mp_current_minPollBmpMode = st->mp_minPollBmpMode;
    st->mp_current_singlePacketMode = st->mp_singlePacketMode;
    st->mp_current_ignoreFatalErrorMode = st->mp_ignoreFatalErrorMode;
#endif
    st->wep_flag = FALSE;
    st->wepMode = 0;
    memset(st->wepKey, 0, sizeof st->wepKey);
    wmi_reset_size_vars();
    st->mp_parentVCount = WM_VALARM_COUNT_PARENT_MP;
    st->mp_childVCount = WM_VALARM_COUNT_CHILD_MP;
    st->mp_parentInterval = WM_DEFAULT_MP_PARENT_INTERVAL;
    st->mp_childInterval = WM_DEFAULT_MP_CHILD_INTERVAL;
    st->pwrMgtMode = 0;
    st->preamble = WMI_PREAMBLE_SHORT;
    st->miscFlags = misc_flags;
    {
        int i;

        for (i = 0; i < 32; i++) {
            W.wm7->requestBuf[i * 4] = WM_API_REQUEST_ACCEPTED;
        }
    }
    {
        u16 *p = &st->portSeqNo[0][0];
        size_t i;

        for (i = 0; i < sizeof st->portSeqNo / sizeof(u16); i++) {
            p[i] = 1;
        }
    }
    st->state = WM_STATE_STOP;
    W.lifetime = wmi_ms_to_frames(WMI_DEFAULT_LIFE_TIME * 100);
}

/* WMSPi_CommonWlIdle: the radio comes up, reports its channels and MAC. */
static void wmi_wl_idle(void)
{
    WMStatus *st = W.st;

    st->enableChannel = WMI_ENABLE_CHANNEL;
    st->allowedChannel = wmi_allowed_channel((u16)(WMI_ENABLE_CHANNEL >> 1));
    st->rate = WMI_RATE_2M;
    st->preamble = WMI_PREAMBLE_SHORT;
    memcpy(st->MacAddress, W.mac, WM_SIZE_MACADDR);
}

/* WMSP_CopyParentParam into the beacon snapshot. */
static void wmi_snapshot_gameinfo(void)
{
    WMParentParam *pp = &W.st->pparam;
    WMGameInfo *gi = (WMGameInfo *)W.gi;
    u16 ulen = pp->userGameInfoLength;

    if (ulen > WM_SIZE_USER_GAMEINFO) {
        ulen = WM_SIZE_USER_GAMEINFO;
    }
    memset(W.gi, 0, sizeof W.gi);
    gi->ggid = pp->ggid;
    gi->tgid = pp->tgid;
    gi->attribute = (u8)((pp->entryFlag ? WM_ATTR_FLAG_ENTRY : 0) | (pp->multiBootFlag ? WM_ATTR_FLAG_MB : 0)
                         | (pp->KS_Flag ? WM_ATTR_FLAG_KS : 0));
    gi->userGameInfoLength = (u8)ulen;
    gi->magicNumber = WM_GAMEINFO_MAGIC_NUMBER;
    gi->ver = WM_GAMEINFO_VERSION_NUMBER;
    gi->platform = WM_GAMEINFO_PLATFORM_ID_NITRO;
    gi->parentMaxSize = pp->parentMaxSize;
    gi->childMaxSize = (pp->multiBootFlag && pp->childMaxSize >= 8) ? 8 : pp->childMaxSize;
    if (ulen != 0 && pp->userGameInfo != NULL) {
        memcpy(gi->userGameInfo, pp->userGameInfo, (ulen + 1u) & ~1u);
    }
    W.gi_len = (u16)(WM_SIZE_SYSTEM_GAMEINFO + ulen);
}

static int wmi_is_parent(void)
{
    return W.st->state == WM_STATE_PARENT || W.st->state == WM_STATE_MP_PARENT;
}

static int wmi_is_child(void)
{
    return W.st->state == WM_STATE_CHILD || W.st->state == WM_STATE_MP_CHILD;
}

/* ------------------------------------------------------------------ */
/* Datagrams                                                          */

#define WMI_MAGIC 0x4D57504Eu /* "NPWM" */
#define WMI_VERSION 1

enum {
    WMI_PKT_BEACON = 1,
    WMI_PKT_JOIN_REQ = 2,
    WMI_PKT_JOIN_ACK = 3,
    WMI_PKT_DEAUTH = 4,
    WMI_PKT_DATA = 5,
};

enum { WMI_JOIN_OK = 0, WMI_JOIN_NO_ENTRY = 1 };

typedef struct {
    u8 *p, *end;
    int bad;
} wmi_buf;

static void put8(wmi_buf *b, u32 v)
{
    if (b->p + 1 > b->end) { b->bad = 1; return; }
    *b->p++ = (u8)v;
}
static void put16(wmi_buf *b, u32 v) { put8(b, v); put8(b, v >> 8); }
static void put32(wmi_buf *b, u32 v) { put16(b, v); put16(b, v >> 16); }
static void putn(wmi_buf *b, const void *src, u32 n)
{
    if (b->p + n > b->end) { b->bad = 1; return; }
    memcpy(b->p, src, n);
    b->p += n;
}
static u32 get8(wmi_buf *b)
{
    if (b->p + 1 > b->end) { b->bad = 1; return 0; }
    return *b->p++;
}
static u32 get16(wmi_buf *b) { u32 lo = get8(b); return lo | (get8(b) << 8); }
static u32 get32(wmi_buf *b) { u32 lo = get16(b); return lo | (get16(b) << 16); }
static void getn(wmi_buf *b, void *dst, u32 n)
{
    if (b->p + n > b->end) { b->bad = 1; memset(dst, 0, n); return; }
    memcpy(dst, b->p, n);
    b->p += n;
}

static u8 sPkt[WMI_PKT_MAX + 64];

static wmi_buf wmi_pkt_begin(u32 type)
{
    wmi_buf b = { sPkt, sPkt + WMI_PKT_MAX, 0 };

    put32(&b, WMI_MAGIC);
    put8(&b, WMI_VERSION);
    put8(&b, type);
    putn(&b, W.mac, WM_SIZE_MACADDR);
    put32(&b, W.frame); /* the sender's frame: bundles are frame-tagged */
    return b;
}

static void wmi_pkt_send(u32 peer, wmi_buf *b)
{
    if (!b->bad && W.self != 0) {
        wmi_net_send(peer, sPkt, (u32)(b->p - sPkt));
    }
}

static void wmi_send_beacon(void)
{
    wmi_buf b = wmi_pkt_begin(WMI_PKT_BEACON);

    put16(&b, W.st->pparam.channel);
    put16(&b, W.beacon_period);
    put16(&b, W.gi_len);
    putn(&b, W.gi, W.gi_len);
    wmi_pkt_send(NP_NET_BROADCAST, &b);
}

static void wmi_send_deauth(u32 peer, const u8 *dst, u16 aid, u16 reason)
{
    int i;

    /* Three copies: nobody acknowledges a deauthentication, and the MP
     * lifetime catches the case where all three are lost. */
    for (i = 0; i < 3; i++) {
        wmi_buf b = wmi_pkt_begin(WMI_PKT_DEAUTH);

        putn(&b, dst, WM_SIZE_MACADDR);
        put16(&b, aid);
        put16(&b, reason);
        wmi_pkt_send(peer, &b);
    }
}

/* ------------------------------------------------------------------ */
/* Links                                                              */

static void wmi_link_open(int i, u32 peer, const u8 *mac)
{
    wmi_link *l = &W.link[i];

    memset(l, 0, sizeof *l);
    l->used = 1;
    l->peer = peer;
    memcpy(l->mac, mac, WM_SIZE_MACADDR);
    l->last_rx = W.frame;
}

static void wmi_link_clear_queue(wmi_link *l)
{
    l->qhead = 0;
    l->qcount = 0;
}

static void wmi_link_close(int i)
{
    W.link[i].used = 0;
    wmi_link_clear_queue(&W.link[i]);
}

/* The oldest sequence a link still holds, or the next one if none. */
static u16 wmi_link_first(const wmi_link *l)
{
    return l->qcount ? l->q[l->qhead].seq : l->tx_seq;
}

/* Send callbacks waiting for window room, oldest first. WMSP_CleanSendQueue
 * reports cleaned entries as SUCCESS with restBitmap 0, so a link that went
 * away releases its waiters the same way. */
static void wmi_pend_run(void)
{
    while (W.pend_count > 0) {
        wmi_pend *p = &W.pend[W.pend_head];
        int i;

        for (i = 0; i < WMI_LINKS; i++) {
            wmi_link *l = &W.link[i];

            if (!(p->links & (1u << i)) || !l->used) {
                continue;
            }
            if ((s16)(p->seq[i] - wmi_link_first(l)) >= WMI_WINDOW) {
                return;
            }
        }
        W.pend_head = (W.pend_head + 1) % WMI_PEND_MAX;
        W.pend_count--;
        wmi_cb_port_send(&p->cb);
    }
}

static void wmi_send_data(int i)
{
    wmi_link *l = &W.link[i];
    wmi_buf b = wmi_pkt_begin(WMI_PKT_DATA);
    u8 *countp;
    int n, count = 0;

    putn(&b, l->mac, WM_SIZE_MACADDR);
    put16(&b, (u16)(W.st->aid == 0 ? i : W.st->aid));
    put16(&b, l->rx_next);
    put16(&b, wmi_link_first(l));
    countp = b.p;
    put8(&b, 0);
    for (n = 0; n < l->qcount; n++) {
        wmi_msg *m = &l->q[(l->qhead + n) % WMI_LINK_QUEUE];

        if (m->last_tx != 0 && W.frame - m->last_tx < WMI_RTO) {
            continue;
        }
        if (b.p + 6 + m->len > b.end) {
            break;
        }
        put16(&b, m->seq);
        put16(&b, m->port);
        put16(&b, m->len);
        putn(&b, m->data, m->len);
        m->last_tx = W.frame ? W.frame : 1;
        count++;
    }
    *countp = (u8)count;
    wmi_pkt_send(l->peer, &b);
}

/* Deliver one message to the ARM9 as WMSP_ParsePortPacket would: the record
 * points into the game's own MP receive buffer, laid out the way the radio
 * leaves it (WMMpRecvHeader on a parent, WMMpRecvBuf on a child). */
static void wmi_deliver(u16 aid, u16 port, const u16 *data, u16 len)
{
    WMStatus *st = W.st;
    static u32 sSpare[(sizeof(WMMpRecvBuf) + WM_SIZE_MP_DATA_MAX + 64) / 4] ATTRIBUTE_ALIGN(32);
    WMMpRecvBuf *recvBuf;
    u16 *payload;
    u16 header = (u16)((port << WM_HEADER_PORT_SHIFT) | ((len / 2) & WM_HEADER_LENGTH_MASK)
                       | (port >= WM_NUM_OF_SEQ_PORT ? WM_HEADER_SEQ_FLAG : 0));
    u32 need;

    if (st->aid == 0) {
        need = sizeof(WMMpRecvHeader) + len;
    } else {
        need = sizeof(WMMpRecvBuf) + len;
    }
    recvBuf = st->mp_recvBuf[st->mp_recvBufSel];
    if (recvBuf == NULL || need > st->mp_recvBufSize) {
        recvBuf = (WMMpRecvBuf *)sSpare;
    }
    if (st->aid == 0) {
        WMMpRecvHeader *h = (WMMpRecvHeader *)recvBuf;
        WMMpRecvData *d = h->data;

        memset(h, 0, sizeof *h);
        h->bitmap = (u16)(1u << aid);
        h->count = 1;
        h->length = (u16)(offsetof(WMMpRecvData, cdata) + len);
        d->length = len;
        d->aid = aid;
        d->wmHeader = header;
        payload = d->cdata;
    } else {
        memset(recvBuf, 0, sizeof *recvBuf);
        recvBuf->length = len;
        recvBuf->wmHeader = header;
        recvBuf->bitmap = (u16)(1u << st->aid);
        memcpy(recvBuf->destAdrs, W.mac, WM_SIZE_MACADDR);
        memcpy(recvBuf->srcAdrs, W.link[0].mac, WM_SIZE_MACADDR);
        payload = recvBuf->data;
    }
    memcpy(payload, data, len);

    {
        WMPortRecvCallback *cb = wmi_cb_begin();

        cb->apiid = WM_APIID_PORT_RECV;
        cb->errcode = WM_ERRCODE_SUCCESS;
        cb->state = WM_STATECODE_PORT_RECV;
        cb->port = port;
        cb->recvBuf = recvBuf;
        cb->data = payload;
        cb->length = len;
        cb->aid = aid;
        cb->myAid = st->aid;
        cb->seqNo = 0xffff;
        memcpy(cb->macAddress, W.link[st->aid == 0 ? aid : 0].mac, WM_SIZE_MACADDR);
        cb->maxSendDataSize = (st->aid == 0) ? st->mp_parentSize : st->mp_childSize;
        cb->maxRecvDataSize = (st->aid == 0) ? st->mp_childSize : st->mp_parentSize;
        wmi_cb_send();
    }
}

/* ------------------------------------------------------------------ */
/* Connection teardown                                                */

/* A child leaves this parent: by its own deauth (reason from the packet),
 * by the lifetime, or by this side's Disconnect/EndParent/Reset (myself). */
static void wmi_parent_drop_child(int aid, u16 reason, int myself, int notify_peer)
{
    WMStatus *st = W.st;
    wmi_link *l = &W.link[aid];
    u8 mac[WM_SIZE_MACADDR];

    if (!l->used) {
        return;
    }
    memcpy(mac, l->mac, sizeof mac);
    if (notify_peer) {
        wmi_send_deauth(l->peer, mac, (u16)aid, WMI_RSN_DEAUTH_LEAVING);
    }
    wmi_link_close(aid);
    st->child_bitmap &= (u16)~(1u << aid);
    st->mp_readyBitmap &= (u16)~(1u << aid);
    memset(st->childMacAddress[aid - 1], 0, WM_SIZE_MACADDR);
    wmi_trace("parent: aid %d left (reason %#x%s)", aid, reason, myself ? ", myself" : "");
    if (myself) {
        wmi_cb_parent(WM_STATECODE_DISCONNECTED_FROM_MYSELF, (u16)aid, mac, WM_DISCONNECT_REASON_FROM_MYSELF, NULL);
    } else {
        wmi_cb_parent(WM_STATECODE_DISCONNECTED, (u16)aid, mac, reason, NULL);
    }
    wmi_pend_run();
}

/* The parent is lost to this child (deauth or lifetime), the indicate path
 * of WMSP_DisconnectCore: MP stops and the radio drops to IDLE. */
static void wmi_child_lost(u16 reason)
{
    WMStatus *st = W.st;
    u8 mac[WM_SIZE_MACADDR];

    memcpy(mac, st->parentMacAddress, sizeof mac);
    wmi_link_close(0);
    st->mp_flag = FALSE;
    st->child_bitmap = 0;
    st->mp_readyBitmap = 0;
    st->ks_flag = FALSE;
    st->dcf_flag = FALSE;
    st->VSyncFlag = FALSE;
    st->beaconIndicateFlag = 0;
    st->state = WM_STATE_IDLE;
    st->wep_flag = FALSE;
    st->wepMode = 0;
    wmi_reset_size_vars();
    wmi_trace("child: parent lost (reason %#x)", reason);
    wmi_cb_connect(WM_ERRCODE_SUCCESS, WM_STATECODE_DISCONNECTED, st->aid, mac, reason);
    wmi_pend_run();
}

/* ------------------------------------------------------------------ */
/* Requests                                                           */

static void wmi_req_initialize(const u32 *req, int full)
{
    W.wm7 = (WMArm7Buf *)(uintptr_t)req[1];
    W.st = (WMStatus *)(uintptr_t)req[2];
    W.wm7->status = W.st;
    W.fifo = (u32 *)(uintptr_t)req[3];
    W.wm7->fifo7to9 = W.fifo;
    memset(W.link, 0, sizeof W.link);
    W.pend_count = 0;
#if defined(PC_GAME_DP)
    wmi_common_init(0); /* three words, no miscFlags (header note 1) */
#else
    wmi_common_init(req[4]);
#endif
    if (full) {
        wmi_wl_idle();
        W.st->state = WM_STATE_IDLE;
    }
    wmi_cb_simple(full ? WM_APIID_INITIALIZE : WM_APIID_ENABLE, WM_ERRCODE_SUCCESS);
}

static void wmi_req_reset(void)
{
    WMStatus *st = W.st;
    u16 child_bitmap = 0;
    int was_parent = 0, i;

    if (st->mp_flag) {
        st->mp_flag = FALSE;
        if (st->state == WM_STATE_MP_CHILD) {
            st->state = WM_STATE_CHILD;
        } else if (st->state == WM_STATE_MP_PARENT) {
            st->state = WM_STATE_PARENT;
        }
    }
    if (st->state == WM_STATE_PARENT || st->state == WM_STATE_CHILD) {
        child_bitmap = st->child_bitmap;
        was_parent = st->state == WM_STATE_PARENT;
    }
    st->child_bitmap = 0;
    st->mp_readyBitmap = 0;
    st->ks_flag = FALSE;
    st->dcf_flag = FALSE;
    st->VSyncFlag = FALSE;
    st->beaconIndicateFlag = 0;
    if (was_parent) {
        st->pparam.entryFlag = FALSE;
    }
    for (i = 0; i < WMI_LINKS; i++) {
        if (child_bitmap & (1u << i)) {
            wmi_link *l = &W.link[i];
            u8 mac[WM_SIZE_MACADDR];

            memcpy(mac, i == 0 ? st->parentMacAddress : st->childMacAddress[i - 1], sizeof mac);
            if (l->used) {
                wmi_send_deauth(l->peer, l->mac, (u16)(was_parent ? i : st->aid), WMI_RSN_DEAUTH_LEAVING);
            }
            if (was_parent) {
                wmi_cb_parent(WM_STATECODE_DISCONNECTED_FROM_MYSELF, (u16)i, mac, WM_DISCONNECT_REASON_FROM_MYSELF, NULL);
            } else {
                wmi_cb_connect(WM_ERRCODE_SUCCESS, WM_STATECODE_DISCONNECTED_FROM_MYSELF, st->aid, mac,
                               WM_DISCONNECT_REASON_FROM_MYSELF);
            }
        }
        wmi_link_close(i);
    }
    memset(st->childMacAddress, 0, sizeof st->childMacAddress);
    st->preamble = WMI_PREAMBLE_SHORT;
    st->state = WM_STATE_IDLE;
    st->wep_flag = FALSE;
    wmi_reset_size_vars();
    wmi_pend_run();
    wmi_cb_simple(WM_APIID_RESET, WM_ERRCODE_SUCCESS);
}

static void wmi_req_end(void)
{
    if (W.st->state != WM_STATE_IDLE) {
        wmi_cb_simple(WM_APIID_END, WM_ERRCODE_ILLEGAL_STATE);
        return;
    }
    W.st->state = WM_STATE_READY;
    wmi_cb_simple(WM_APIID_END, WM_ERRCODE_SUCCESS);
}

static void wmi_req_power(u16 apiid)
{
    WMStatus *st = W.st;

    switch (apiid) {
    case WM_APIID_POWER_ON:
        if (st->state != WM_STATE_STOP) {
            break;
        }
        wmi_wl_idle();
        st->state = WM_STATE_IDLE;
        wmi_cb_simple(apiid, WM_ERRCODE_SUCCESS);
        return;
    case WM_APIID_POWER_OFF:
        if (st->state != WM_STATE_IDLE) {
            break;
        }
        st->state = WM_STATE_STOP;
        wmi_cb_simple(apiid, WM_ERRCODE_SUCCESS);
        return;
    case WM_APIID_DISABLE:
        if (st->state != WM_STATE_STOP) {
            break;
        }
        st->state = WM_STATE_READY;
        wmi_cb_simple(apiid, WM_ERRCODE_SUCCESS);
        return;
    }
    wmi_cb_simple(apiid, WM_ERRCODE_ILLEGAL_STATE);
}

static void wmi_req_set_parent_param(const u32 *req)
{
    WMStatus *st = W.st;

    memcpy(&st->pparam, (const void *)(uintptr_t)req[1], WM_PARENT_PARAM_SIZE);
    if (!((1u << st->pparam.channel) & st->enableChannel)) {
        wmi_cb_simple(WM_APIID_SET_P_PARAM, WM_ERRCODE_INVALID_PARAM);
        return;
    }
    W.beacon_period = st->pparam.beaconPeriod;
    wmi_cb_simple(WM_APIID_SET_P_PARAM, WM_ERRCODE_SUCCESS);
}

static void wmi_req_start_parent(const u32 *req)
{
    WMStatus *st = W.st;
    WMParentParam *pp = &st->pparam;
    WMStartParentCallback *cb;

    if (st->state != WM_STATE_IDLE || (st->miscFlags & WM_MISC_FLAG_LISTEN_ONLY)) {
        cb = wmi_cb_begin();
        cb->apiid = WM_APIID_START_PARENT;
        cb->errcode = WM_ERRCODE_ILLEGAL_STATE;
        cb->state = WM_STATECODE_PARENT_START;
        wmi_cb_send();
        return;
    }
    if (!(st->allowedChannel & ((1u << pp->channel) >> 1))) {
        cb = wmi_cb_begin();
        cb->apiid = WM_APIID_START_PARENT;
        cb->errcode = WM_ERRCODE_INVALID_PARAM;
        cb->state = WM_STATECODE_PARENT_START;
        wmi_cb_send();
        return;
    }
    st->mode = WMI_MODE_PARENT;
    st->aid = 0;
    st->child_bitmap = 0;
    st->mp_readyBitmap = 0;
    st->preamble = WMI_PREAMBLE_SHORT;
    st->pwrMgtMode = (u16)(req[1] ? WMI_PMG_PS : WMI_PMG_CONT_ACT);
    wmi_snapshot_gameinfo();
    wmi_set_parent_max_size((u16)(pp->parentMaxSize + (pp->KS_Flag ? WM_SIZE_KS_PARENT_DATA + WM_SIZE_MP_PARENT_PADDING : 0)));
    wmi_set_child_max_size((u16)(pp->childMaxSize + (pp->KS_Flag ? WM_SIZE_KS_CHILD_DATA + WM_SIZE_MP_CHILD_PADDING : 0)));
    W.beacon_period = pp->beaconPeriod ? pp->beaconPeriod : WM_DEFAULT_BEACON_PERIOD;
    W.next_beacon = W.frame + 1;
    memset(W.link, 0, sizeof W.link);

    cb = wmi_cb_begin();
    st->state = WM_STATE_PARENT;
    cb->apiid = WM_APIID_START_PARENT;
    cb->errcode = WM_ERRCODE_SUCCESS;
    cb->state = WM_STATECODE_PARENT_START;
    cb->parentSize = st->mp_parentSize;
    cb->childSize = st->mp_childSize;
    wmi_cb_send();
    st->beaconIndicateFlag = 1;
    wmi_trace("parent: started on channel %d, ggid %#lx tgid %#x", pp->channel, (unsigned long)pp->ggid, pp->tgid);
}

static void wmi_req_end_parent(void)
{
    WMStatus *st = W.st;
    int i;

    if (st->state != WM_STATE_PARENT) {
        wmi_cb_simple(WM_APIID_END_PARENT, WM_ERRCODE_ILLEGAL_STATE);
        return;
    }
    st->pparam.entryFlag = FALSE;
    for (i = 1; i < WMI_LINKS; i++) {
        if (st->child_bitmap & (1u << i)) {
            wmi_parent_drop_child(i, WM_DISCONNECT_REASON_FROM_MYSELF, 1, 1);
        }
    }
    st->beaconIndicateFlag = 0;
    st->state = WM_STATE_IDLE;
    st->wep_flag = FALSE;
    st->wepMode = 0;
    memset(st->wepKey, 0, sizeof st->wepKey);
    wmi_reset_size_vars();
    wmi_cb_simple(WM_APIID_END_PARENT, WM_ERRCODE_SUCCESS);
}

/* Shared front half of StartScan/StartScanEx: the state checks and the
 * CLASS1 -> SCAN transition. Returns 0 if a failure record was sent. */
static int wmi_scan_begin(u16 apiid, u16 channels, WMBssDesc *buf)
{
    WMStatus *st = W.st;
    u16 errcode = WM_ERRCODE_SUCCESS;

    if (st->state != WM_STATE_IDLE && st->state != WM_STATE_CLASS1 && st->state != WM_STATE_SCAN) {
        errcode = WM_ERRCODE_ILLEGAL_STATE;
    } else if (channels == 0 || buf == NULL) {
        errcode = WM_ERRCODE_INVALID_PARAM;
    }
    if (errcode != WM_ERRCODE_SUCCESS) {
        WMStartScanCallback *cb = wmi_cb_begin();

        cb->apiid = apiid;
        cb->errcode = errcode;
        cb->state = WM_STATECODE_PARENT_NOT_FOUND;
        wmi_cb_send();
        return 0;
    }
    st->mode = WMI_MODE_CHILD;
    if (st->state == WM_STATE_IDLE) {
        st->pwrMgtMode = WMI_PMG_PS;
    }
    st->state = WM_STATE_SCAN;
    st->pInfoBuf = buf;
    return 1;
}

static void wmi_req_start_scan(const WMStartScanReq *req)
{
    u16 channel = req->channel;

    if (channel == 0 || !((1u << channel) & W.st->enableChannel)) {
        WMStartScanCallback *cb = wmi_cb_begin();

        cb->apiid = WM_APIID_START_SCAN;
        cb->errcode = W.st->state == WM_STATE_IDLE || W.st->state == WM_STATE_CLASS1 || W.st->state == WM_STATE_SCAN
                          ? WM_ERRCODE_INVALID_PARAM
                          : WM_ERRCODE_ILLEGAL_STATE;
        cb->state = WM_STATECODE_PARENT_NOT_FOUND;
        wmi_cb_send();
        return;
    }
    if (!wmi_scan_begin(WM_APIID_START_SCAN, (u16)(1u << channel), req->scanBuf)) {
        return;
    }
    W.st->scan_channel = channel;
    W.scan.active = 1;
    W.scan.ex = 0;
    W.scan.channels = (u16)(1u << channel);
    W.scan.buf = req->scanBuf;
    W.scan.buf_size = WM_BSS_DESC_SIZE;
    memcpy(W.scan.bssid, req->bssid, WM_SIZE_MACADDR);
    W.scan.ssid_len = 0;
    W.scan.done = W.frame + wmi_ms_to_frames(req->maxChannelTime);
}

static void wmi_req_start_scan_ex(const WMStartScanExReq *req)
{
    u16 channels = (u16)((req->channelList << 1) & W.st->enableChannel);

    if (req->scanBufSize < 64) {
        channels = 0;
    }
    if (!wmi_scan_begin(WM_APIID_START_SCAN_EX, channels, req->scanBuf)) {
        return;
    }
    W.st->scan_channel = req->channelList;
    W.scan.active = 1;
    W.scan.ex = 1;
    W.scan.channels = channels;
    W.scan.buf = req->scanBuf;
    W.scan.buf_size = req->scanBufSize;
    memcpy(W.scan.bssid, req->bssid, WM_SIZE_MACADDR);
    W.scan.ssid_len = req->ssidLength;
    memcpy(W.scan.ssid, req->ssid, WM_SIZE_SSID);
    {
        int n = 0, c;

        for (c = 1; c < 15; c++) {
            n += (channels >> c) & 1;
        }
        W.scan.done = W.frame + wmi_ms_to_frames((u32)req->maxChannelTime * (u32)(n ? n : 1));
    }
}

static int wmi_bss_matches(const wmi_bss *e)
{
    static const u8 any[WM_SIZE_MACADDR] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    if (!e->used || W.frame - e->seen > (u32)wmi_ms_to_frames(e->period) + WMI_BSS_TTL) {
        return 0;
    }
    if (!(W.scan.channels & (1u << e->channel))) {
        return 0;
    }
    if (memcmp(W.scan.bssid, any, WM_SIZE_MACADDR) != 0 && memcmp(W.scan.bssid, e->mac, WM_SIZE_MACADDR) != 0) {
        return 0;
    }
    return 1;
}

/* The WlBssDesc a scan leaves in the ARM9's buffer for one parent. */
static u16 wmi_fill_bss(WMBssDesc *d, const wmi_bss *e)
{
    const WMGameInfo *gi = (const WMGameInfo *)e->gi;
    u16 bytes = (u16)(offsetof(WMBssDesc, gameInfo) + e->gi_len);

    bytes = (u16)((bytes + 1) & ~1);
    memset(d, 0, bytes);
    d->length = (u16)(bytes / 2);
    d->rssi = 0xff;
    memcpy(d->bssid, e->mac, WM_SIZE_MACADDR);
    d->ssidLength = WM_SIZE_SSID;
    d->ssid[0] = (u8)gi->ggid;
    d->ssid[1] = (u8)(gi->ggid >> 8);
    d->ssid[2] = (u8)(gi->ggid >> 16);
    d->ssid[3] = (u8)(gi->ggid >> 24);
    d->ssid[4] = (u8)gi->tgid;
    d->ssid[5] = (u8)(gi->tgid >> 8);
    d->capaInfo = 0x0021; /* ESS | short preamble */
    d->rateSet.basic = 0x0003;
    d->rateSet.support = 0x0003;
    d->beaconPeriod = e->period;
    d->dtimPeriod = 2;
    d->channel = e->channel;
    d->gameInfoLength = e->gi_len;
    memcpy(&d->gameInfo, e->gi, e->gi_len);
    return bytes;
}

static void wmi_scan_finish(void)
{
    int i, k, found = -1;

    W.scan.active = 0;
    if (!W.scan.ex) {
        WMStartScanCallback *cb;

        /* One parent per scan, round-robin over the ones in range, so a
         * table of several is reported over successive scans the way
         * beacons arriving at different times would be. */
        for (k = 0; k < WMI_BSS_MAX; k++) {
            i = (int)((W.scan_rr + 1 + k) % WMI_BSS_MAX);
            if (wmi_bss_matches(&W.bss[i])) {
                found = i;
                break;
            }
        }
        cb = wmi_cb_begin();
        cb->apiid = WM_APIID_START_SCAN;
        cb->errcode = WM_ERRCODE_SUCCESS;
        if (found < 0) {
            cb->state = WM_STATECODE_PARENT_NOT_FOUND;
            cb->channel = W.st->scan_channel;
            cb->linkLevel = WM_LINK_LEVEL_0;
        } else {
            wmi_bss *e = &W.bss[found];

            W.scan_rr = (u32)found;
            wmi_fill_bss(W.scan.buf, e);
            cb->state = WM_STATECODE_PARENT_FOUND;
            cb->channel = e->channel;
            cb->linkLevel = WM_LINK_LEVEL_3;
            cb->ssidLength = W.scan.buf->ssidLength;
            memcpy(cb->macAddress, e->mac, WM_SIZE_MACADDR);
            memcpy(cb->ssid, W.scan.buf->ssid, WM_SIZE_SSID);
            cb->gameInfoLength = e->gi_len;
            memcpy(&cb->gameInfo, e->gi, e->gi_len);
        }
        wmi_cb_send();
        return;
    }
    {
        WMStartScanExCallback *cb;
        u8 *dst = (u8 *)W.scan.buf;
        u32 used = 0;
        int count = 0;

        memset(W.scan.buf, 0, W.scan.buf_size);
        cb = wmi_cb_begin();
        for (i = 0; i < WMI_BSS_MAX && count < WM_SCAN_EX_PARENT_MAX; i++) {
            wmi_bss *e = &W.bss[i];
            u32 bytes;

            if (!wmi_bss_matches(e)) {
                continue;
            }
            bytes = (u32)((offsetof(WMBssDesc, gameInfo) + e->gi_len + 3) & ~3u);
            if (used + bytes > W.scan.buf_size) {
                break;
            }
            wmi_fill_bss((WMBssDesc *)(dst + used), e);
            if (W.scan.ssid_len > 0 && W.scan.ssid_len <= WM_SIZE_SSID) {
                WMBssDesc *d = (WMBssDesc *)(dst + used);
                int j, valid = 0;

                for (j = 0; j < d->ssidLength; j++) {
                    valid |= d->ssid[j] != 0;
                }
                if (!valid) {
                    d->ssidLength = W.scan.ssid_len;
                    memcpy(d->ssid, W.scan.ssid, WM_SIZE_SSID);
                }
            }
            cb->bssDesc[count] = (WMBssDesc *)(dst + used);
            cb->linkLevel[count] = WM_LINK_LEVEL_3;
            count++;
            used += bytes;
        }
        cb->apiid = WM_APIID_START_SCAN_EX;
        cb->errcode = WM_ERRCODE_SUCCESS;
        cb->state = count ? WM_STATECODE_PARENT_FOUND : WM_STATECODE_PARENT_NOT_FOUND;
        cb->bssDescCount = (u16)count;
        cb->channelList = (u16)(W.scan.channels >> 1);
        wmi_cb_send();
    }
}

static void wmi_req_end_scan(void)
{
    WMStatus *st = W.st;

    if (st->state != WM_STATE_SCAN) {
        wmi_cb_simple(WM_APIID_END_SCAN, WM_ERRCODE_ILLEGAL_STATE);
        return;
    }
    st->state = WM_STATE_IDLE;
    st->preamble = WMI_PREAMBLE_SHORT;
    wmi_cb_simple(WM_APIID_END_SCAN, WM_ERRCODE_SUCCESS);
}

static wmi_bss *wmi_bss_find(const u8 *mac)
{
    int i;

    for (i = 0; i < WMI_BSS_MAX; i++) {
        if (W.bss[i].used && memcmp(W.bss[i].mac, mac, WM_SIZE_MACADDR) == 0) {
            return &W.bss[i];
        }
    }
    return NULL;
}

static void wmi_join_send(void)
{
    wmi_buf b = wmi_pkt_begin(WMI_PKT_JOIN_REQ);

    putn(&b, W.join.bssid, WM_SIZE_MACADDR);
    put16(&b, W.join.tgid);
    put32(&b, W.join.nonce);
    putn(&b, W.join.ssid, WM_SIZE_CHILD_SSID);
    wmi_pkt_send(W.join.peer, &b);
    W.join.next_tx = W.frame + WMI_JOIN_RETRY;
}

static void wmi_req_start_connect(const WMStartConnectReq *req)
{
    WMStatus *st = W.st;
    WMBssDesc *p = &W.wm7->connectPInfo;
    wmi_bss *e;

    if (st->state != WM_STATE_IDLE || (st->miscFlags & WM_MISC_FLAG_LISTEN_ONLY)) {
        wmi_cb_connect(WM_ERRCODE_ILLEGAL_STATE, WM_STATECODE_CONNECT_START, 0, NULL, 0);
        return;
    }
    memcpy(p, req->pInfo, WM_BSS_DESC_SIZE);
    if (p->gameInfoLength >= 16 && !(p->gameInfo.attribute & WM_ATTR_FLAG_ENTRY)) {
        wmi_cb_connect(WM_ERRCODE_NO_ENTRY, WM_STATECODE_CONNECT_START, 0, NULL, 0);
        return;
    }
    if (!((1u << p->channel) & st->enableChannel) || !(((1u << p->channel) >> 1) & WMI_ENABLE_CHANNEL_MASK)) {
        wmi_cb_connect(WM_ERRCODE_INVALID_PARAM, WM_STATECODE_CONNECT_START, 0, NULL, 0);
        return;
    }
    wmi_cb_connect(WM_ERRCODE_SUCCESS, WM_STATECODE_CONNECT_START, 0, NULL, 0);

    st->mode = WMI_MODE_CHILD;
    st->state = WM_STATE_CLASS1;
    st->pwrMgtMode = (u16)(req->powerSave ? WMI_PMG_PS : WMI_PMG_CONT_ACT);
    memset(&W.join, 0, sizeof W.join);
    W.join.active = 1;
    W.join.deadline = W.frame + WMI_JOIN_TIMEOUT;
    W.join.nonce = (W.self << 8) ^ W.frame ^ 0x5a5a1234u;
    memcpy(W.join.bssid, p->bssid, WM_SIZE_MACADDR);
    memcpy(W.join.ssid, req->ssid, WM_SIZE_CHILD_SSID);
    W.join.tgid = p->gameInfo.tgid;
    e = wmi_bss_find(p->bssid);
    if (e != NULL) {
        W.join.peer = e->peer;
        wmi_join_send();
    }
    /* No beacon from that BSSID in the table: the join simply times out,
     * which is what MlmeJoin does for a parent that is gone. */
}

static void wmi_join_fail(u16 errcode)
{
    W.join.active = 0;
    wmi_trace("child: join failed (%d)", errcode);
    wmi_cb_connect(errcode, errcode == WM_ERRCODE_FAILED ? 0 : WM_STATECODE_CONNECT_START, 0, NULL, 0);
}

static void wmi_join_done(u16 aid, u32 peer)
{
    WMStatus *st = W.st;
    WMBssDesc *p = &W.wm7->connectPInfo;

    W.join.active = 0;
    memcpy(st->parentMacAddress, W.join.bssid, WM_SIZE_MACADDR);
    st->aid = aid;
    st->curr_tgid = p->gameInfo.tgid;
    {
        int j;

        for (j = 0; j < WM_NUM_OF_SEQ_PORT; j++) {
            st->portSeqNo[0][j] = 1;
        }
    }
    st->linkLevel = WM_LINK_LEVEL_3;
    st->child_bitmap = 0x0001;
    st->mp_readyBitmap = 0x0001;
    st->state = WM_STATE_CHILD;
    wmi_set_parent_max_size((u16)(p->gameInfo.parentMaxSize
                                  + ((p->gameInfo.attribute & WM_ATTR_FLAG_KS) ? WM_SIZE_KS_PARENT_DATA + WM_SIZE_MP_PARENT_PADDING : 0)));
    wmi_set_child_max_size((u16)(p->gameInfo.childMaxSize
                                 + ((p->gameInfo.attribute & WM_ATTR_FLAG_KS) ? WM_SIZE_KS_CHILD_DATA + WM_SIZE_MP_CHILD_PADDING : 0)));
    st->beaconIndicateFlag = 1;
    wmi_link_open(0, peer, W.join.bssid);
    wmi_trace("child: joined as aid %d", aid);
    wmi_cb_connect(WM_ERRCODE_SUCCESS, WM_STATECODE_CONNECTED, aid, st->parentMacAddress, 0);
}

static void wmi_req_disconnect(const u32 *req)
{
    WMStatus *st = W.st;
    u16 try_bmp = (u16)req[1];
    u16 res_bmp = 0;
    WMDisconnectCallback *cb;
    int i;

    if (wmi_is_parent()) {
        for (i = 1; i < WMI_LINKS; i++) {
            if (st->child_bitmap & try_bmp & (1u << i)) {
                res_bmp |= (u16)(1u << i);
                wmi_parent_drop_child(i, WM_DISCONNECT_REASON_FROM_MYSELF, 1, 1);
            }
        }
    } else if (wmi_is_child() && st->child_bitmap != 0) {
        u8 mac[WM_SIZE_MACADDR];

        memcpy(mac, st->parentMacAddress, sizeof mac);
        if (W.link[0].used) {
            wmi_send_deauth(W.link[0].peer, mac, st->aid, WMI_RSN_DEAUTH_LEAVING);
        }
        wmi_link_close(0);
        st->mp_flag = FALSE;
        st->child_bitmap = 0;
        st->mp_readyBitmap = 0;
        st->ks_flag = FALSE;
        st->dcf_flag = FALSE;
        st->VSyncFlag = FALSE;
        st->beaconIndicateFlag = 0;
        st->state = WM_STATE_IDLE;
        st->wep_flag = FALSE;
        st->wepMode = 0;
        memset(st->wepKey, 0, sizeof st->wepKey);
        wmi_reset_size_vars();
        res_bmp = 0x0001;
        wmi_cb_connect(WM_ERRCODE_SUCCESS, WM_STATECODE_DISCONNECTED_FROM_MYSELF, st->aid, mac,
                       WM_DISCONNECT_REASON_FROM_MYSELF);
        wmi_pend_run();
    } else {
        cb = wmi_cb_begin();
        cb->apiid = WM_APIID_DISCONNECT;
        cb->errcode = WM_ERRCODE_ILLEGAL_STATE;
        cb->tryBitmap = try_bmp;
        wmi_cb_send();
        return;
    }
    cb = wmi_cb_begin();
    cb->apiid = WM_APIID_DISCONNECT;
    cb->errcode = WM_ERRCODE_SUCCESS;
    cb->tryBitmap = try_bmp;
    cb->disconnectedBitmap = res_bmp;
    wmi_cb_send();
}

#if defined(PC_GAME_DP)
/* D's START_MP request (header note 2): WMStartMPReq up to `param`, which
 * is the whole per-call parameter; there is no tmpParam. */
typedef struct {
    u16 apiid;
    u16 rsv1;
    u32 *recvBuf;
    u32 recvBufSize;
    u32 *sendBuf;
    u32 sendBufSize;
    WMMPParam param;
} wmi_dp_start_mp_req;

_Static_assert(sizeof(wmi_dp_start_mp_req) == 0x30, "D WMi_StartMP sends 0x30 bytes");
_Static_assert(offsetof(wmi_dp_start_mp_req, sendBufSize) == offsetof(WMStartMPReq, sendBufSize)
                   && offsetof(wmi_dp_start_mp_req, param) == offsetof(WMStartMPReq, param),
               "the words before param are 4.2's");
_Static_assert(offsetof(WMStatus, mp_lifeTimeTick) + sizeof(OSTick) == 0x7C0,
               "D's WMStatus is 4.2's without the mp_current_* tail");

/* What D's WMSP_SetMPParameterCore does with the bits D's ARM9 can put in
 * a START_MP request: 0x0003 (WM_StartMP) and 0x1E03 | 0x0004
 * (WM_StartMPEx). It runs before WMSP_StartMP's state check. */
static void wmi_dp_start_mp_param(const WMMPParam *p)
{
    WMStatus *st = W.st;
    u32 mask = p->mask;

    if (mask & WM_MP_PARAM_MIN_FREQUENCY) {
        st->mp_minFreq = p->minFrequency ? p->minFrequency : 16;
    }
    if (mask & WM_MP_PARAM_FREQUENCY) {
        st->mp_freq = p->frequency ? p->frequency : 16;
        if (st->mp_count > (s16)st->mp_freq) {
            st->mp_count = (s16)st->mp_freq;
        }
    }
    if (mask & WM_MP_PARAM_MAX_FREQUENCY) {
        st->mp_maxFreq = p->maxFrequency ? p->maxFrequency : 16;
        if (st->mp_count > (s16)st->mp_maxFreq) {
            st->mp_count = (s16)st->mp_maxFreq;
        }
    }
    if (mask & WM_MP_PARAM_DEFAULT_RETRY_COUNT) {
        st->mp_defaultRetryCount = p->defaultRetryCount;
    }
    if (mask & WM_MP_PARAM_MIN_POLL_BMP_MODE) {
        st->mp_minPollBmpMode = p->minPollBmpMode;
    }
    if (mask & WM_MP_PARAM_SINGLE_PACKET_MODE) {
        st->mp_singlePacketMode = p->singlePacketMode;
    }
    if (mask & WM_MP_PARAM_IGNORE_FATAL_ERROR_MODE) {
        st->mp_ignoreFatalErrorMode = p->ignoreFatalErrorMode;
    }
}
#endif

static void wmi_req_start_mp(const WMStartMPReq *req)
{
    WMStatus *st = W.st;
    WMStartMPCallback *cb;
#if defined(PC_GAME_DP)
    wmi_dp_start_mp_param(&((const wmi_dp_start_mp_req *)req)->param);
#else
    u32 mask = req->tmpParam.mask;
    u16 v1, v2, v3;
#endif
    if (st->state != WM_STATE_CHILD && st->state != WM_STATE_PARENT) {
        cb = wmi_cb_begin();
        cb->apiid = WM_APIID_START_MP;
        cb->errcode = WM_ERRCODE_ILLEGAL_STATE;
        cb->state = WM_STATECODE_MP_START;
        wmi_cb_send();
        return;
    }
#if !defined(PC_GAME_DP)
    /* HandleMask */
    v1 = (mask & WM_MP_TMP_PARAM_MAX_FREQUENCY) ? req->tmpParam.maxFrequency : st->mp_maxFreq;
    v1 = v1 ? v1 : 16;
    v2 = (mask & WM_MP_TMP_PARAM_MIN_FREQUENCY) ? req->tmpParam.minFrequency : st->mp_minFreq;
    v2 = v2 ? v2 : 16;
    v3 = (mask & WM_MP_TMP_PARAM_FREQUENCY) ? req->tmpParam.frequency : st->mp_freq;
    v3 = v3 ? v3 : 16;
    st->mp_current_maxFreq = v1;
    st->mp_current_minFreq = v2 > v1 ? v1 : v2;
    st->mp_current_freq = v3 > v1 ? v1 : v3;
    st->mp_current_defaultRetryCount = (mask & WM_MP_TMP_PARAM_DEFAULT_RETRY_COUNT) ? req->tmpParam.defaultRetryCount : st->mp_defaultRetryCount;
    st->mp_current_minPollBmpMode = (mask & WM_MP_TMP_PARAM_MIN_POLL_BMP_MODE) ? req->tmpParam.minPollBmpMode : st->mp_minPollBmpMode;
    st->mp_current_singlePacketMode = (mask & WM_MP_TMP_PARAM_SINGLE_PACKET_MODE) ? req->tmpParam.singlePacketMode : st->mp_singlePacketMode;
    st->mp_current_ignoreFatalErrorMode = (mask & WM_MP_TMP_PARAM_IGNORE_FATAL_ERROR_MODE) ? req->tmpParam.ignoreFatalErrorMode : st->mp_ignoreFatalErrorMode;
#endif

    st->mp_flag = FALSE;
    st->mp_waitAckFlag = FALSE;
    st->mp_vsyncOrderedFlag = FALSE;
    st->mp_vsyncFlag = TRUE;
    st->mp_newFrameFlag = FALSE;
    st->sendQueueInUse = FALSE;
    st->mp_setDataFlag = FALSE;
    st->mp_sentDataFlag = FALSE;
    st->mp_bufferEmptyFlag = FALSE;
    st->mp_isPolledFlag = FALSE;
    st->mp_resumeFlag = FALSE;
    st->mp_recvBuf[0] = (WMMpRecvBuf *)req->recvBuf;
    st->mp_recvBufSize = (u16)req->recvBufSize;
    st->mp_recvBuf[1] = (WMMpRecvBuf *)((u8 *)req->recvBuf + req->recvBufSize);
    st->mp_recvBufSel = 0;
    st->mp_sendBuf = req->sendBuf;
    st->mp_sendBufSize = (u16)req->sendBufSize;
    st->mp_count = 0;
    st->mp_limitCount = 0;
    st->mp_prevPollBitmap = 0;
    st->mp_prevWmHeader = 0;
    st->state = st->state == WM_STATE_CHILD ? WM_STATE_MP_CHILD : WM_STATE_MP_PARENT;

    cb = wmi_cb_begin();
    cb->apiid = WM_APIID_START_MP;
    cb->errcode = WM_ERRCODE_SUCCESS;
    cb->state = WM_STATECODE_MP_START;
    wmi_cb_send();
    st->mp_flag = TRUE;
}

static void wmi_req_end_mp(void)
{
    WMStatus *st = W.st;
    int i;

    if (st->state != WM_STATE_MP_PARENT && st->state != WM_STATE_MP_CHILD) {
        wmi_cb_simple(WM_APIID_END_MP, WM_ERRCODE_ILLEGAL_STATE);
        return;
    }
    st->mp_flag = FALSE;
    st->state = st->state == WM_STATE_MP_CHILD ? WM_STATE_CHILD : WM_STATE_PARENT;
    st->mp_setDataFlag = FALSE;
    /* WM_CLEAN_SEND_QUEUE: queued sends are dropped and their callbacks
     * report SUCCESS; the receiver skips the gap via the "first" field. */
    for (i = 0; i < WMI_LINKS; i++) {
        wmi_link_clear_queue(&W.link[i]);
    }
    wmi_pend_run();
    wmi_cb_simple(WM_APIID_END_MP, WM_ERRCODE_SUCCESS);
}

static void wmi_req_set_mp_data(const u32 *req)
{
    WMStatus *st = W.st;
    const u16 *data = (const u16 *)(uintptr_t)req[1];
    u16 size = (u16)req[2];
    u16 dest = (u16)req[3];
    u16 port = (u16)req[4];
    u16 errcode;
    wmi_pend p;
    int i;

    memset(&p, 0, sizeof p);
    if (st->aid != 0) {
        dest = 0x0001;
    }
    p.cb.apiid = WM_APIID_PORT_SEND;
    p.cb.state = WM_STATECODE_PORT_SEND;
    p.cb.port = port;
    p.cb.destBitmap = dest;
    p.cb.size = size;
    p.cb.data = data;
    p.cb.callback = (WMCallbackFunc)(uintptr_t)req[6];
    p.cb.arg = (void *)(uintptr_t)req[7];
    p.cb.seqNo = 0xffff;
    p.cb.maxSendDataSize = (st->aid == 0) ? st->mp_parentSize : st->mp_childSize;
    p.cb.maxRecvDataSize = (st->aid == 0) ? st->mp_childSize : st->mp_parentSize;

    if (!st->mp_flag) {
        errcode = WM_ERRCODE_ILLEGAL_STATE;
    } else if ((dest & st->child_bitmap) == 0) {
        errcode = WM_ERRCODE_SUCCESS;
    } else {
        u16 targets = (u16)(dest & st->child_bitmap);
        u16 len = (u16)((size + 1) & ~1);

        if (len > WM_SIZE_MP_DATA_MAX) {
            len = WM_SIZE_MP_DATA_MAX;
        }
        for (i = 0; i < WMI_LINKS; i++) {
            if ((targets & (1u << i)) && W.link[i].used && W.link[i].qcount >= WMI_LINK_QUEUE) {
                break;
            }
        }
        if (i < WMI_LINKS || W.pend_count >= WMI_PEND_MAX) {
            p.cb.errcode = WM_ERRCODE_SEND_QUEUE_FULL;
            p.cb.restBitmap = targets;
            wmi_cb_port_send(&p.cb);
            return;
        }
        for (i = 0; i < WMI_LINKS; i++) {
            wmi_link *l = &W.link[i];
            wmi_msg *m;

            if (!(targets & (1u << i)) || !l->used) {
                continue;
            }
            m = &l->q[(l->qhead + l->qcount) % WMI_LINK_QUEUE];
            l->qcount++;
            m->seq = l->tx_seq++;
            m->port = port;
            m->len = len;
            m->last_tx = 0;
            memset(m->data, 0, len);
            memcpy(m->data, data, size < len ? size : len);
            p.links |= (u16)(1u << i);
            p.seq[i] = m->seq;
        }
        p.cb.errcode = WM_ERRCODE_SUCCESS;
        p.cb.sentBitmap = targets;
        W.pend[(W.pend_head + W.pend_count) % WMI_PEND_MAX] = p;
        W.pend_count++;
        wmi_pend_run();
        return;
    }
    p.cb.errcode = errcode;
    wmi_cb_port_send(&p.cb);
}

static void wmi_req_measure_channel(const WMMeasureChannelReq *req)
{
    WMStatus *st = W.st;

    if (st->state != WM_STATE_IDLE) {
        wmi_cb_simple(WM_APIID_MEASURE_CHANNEL, WM_ERRCODE_ILLEGAL_STATE);
        return;
    }
    st->mode = WMI_MODE_CHILD;
    st->state = WM_STATE_CLASS1;
    st->pwrMgtMode = WMI_PMG_PS;
    W.measure.active = 1;
    W.measure.channel = req->channel;
    W.measure.done = W.frame + wmi_ms_to_frames(req->measureTime);
}

static void wmi_measure_finish(void)
{
    WMMeasureChannelCallback *cb;
    int i, busy = 0;

    W.measure.active = 0;
    W.st->state = WM_STATE_IDLE;
    /* Every parent heard on the channel adds to its CCA busy ratio, so
     * hosts spread over channels as consoles do; an empty channel is 0. */
    for (i = 0; i < WMI_BSS_MAX; i++) {
        if (W.bss[i].used && W.bss[i].channel == W.measure.channel && W.frame - W.bss[i].seen < WMI_BSS_TTL) {
            busy += 10;
        }
    }
    cb = wmi_cb_begin();
    cb->apiid = WM_APIID_MEASURE_CHANNEL;
    cb->errcode = WM_ERRCODE_SUCCESS;
    cb->channel = W.measure.channel;
    cb->ccaBusyRatio = (u16)(busy > 100 ? 100 : busy);
    wmi_cb_send();
}

static void wmi_req_set_game_info(const u32 *req)
{
    WMParentParam *pp = &W.st->pparam;
    u8 attr = (u8)req[5];

    pp->userGameInfo = (u16 *)(uintptr_t)req[1];
    pp->userGameInfoLength = (u16)req[2];
    pp->ggid = req[3];
    pp->tgid = (u16)req[4];
    pp->entryFlag = (u16)((attr & WM_ATTR_FLAG_ENTRY) ? 1 : 0);
    pp->multiBootFlag = (u16)((attr & WM_ATTR_FLAG_MB) ? 1 : 0);
    pp->KS_Flag = (u16)((attr & WM_ATTR_FLAG_KS) ? 1 : 0);
    pp->CS_Flag = (u16)((attr & WM_ATTR_FLAG_CS) ? 1 : 0);
    wmi_snapshot_gameinfo();
    wmi_cb_simple(WM_APIID_SET_GAMEINFO, WM_ERRCODE_SUCCESS);
}

static void wmi_req_set_mp_parameter(const WMSetMPParameterReq *req)
{
    WMStatus *st = W.st;
    WMSetMPParameterCallback *cb = wmi_cb_begin();
    const WMMPParam *p = &req->param;

    cb->apiid = WM_APIID_SET_MP_PARAMETER;
    cb->errcode = WM_ERRCODE_SUCCESS;
    cb->mask = p->mask;
    cb->oldParam.minFrequency = st->mp_minFreq;
    cb->oldParam.frequency = st->mp_freq;
    cb->oldParam.maxFrequency = st->mp_maxFreq;
    cb->oldParam.parentSize = st->mp_parentSize;
    cb->oldParam.childSize = st->mp_childSize;
    cb->oldParam.parentInterval = st->mp_parentInterval;
    cb->oldParam.childInterval = st->mp_childInterval;
    cb->oldParam.parentVCount = st->mp_parentVCount;
    cb->oldParam.childVCount = st->mp_childVCount;
    cb->oldParam.defaultRetryCount = st->mp_defaultRetryCount;
    cb->oldParam.minPollBmpMode = (u8)st->mp_minPollBmpMode;
    cb->oldParam.singlePacketMode = (u8)st->mp_singlePacketMode;
    cb->oldParam.ignoreFatalErrorMode = (u8)st->mp_ignoreFatalErrorMode;
    cb->oldParam.ignoreSizePrecheckMode = (u8)st->mp_ignoreSizePrecheckMode;
    if (p->mask & WM_MP_PARAM_MIN_FREQUENCY) st->mp_minFreq = p->minFrequency;
    if (p->mask & WM_MP_PARAM_FREQUENCY) st->mp_freq = p->frequency;
    if (p->mask & WM_MP_PARAM_MAX_FREQUENCY) st->mp_maxFreq = p->maxFrequency;
    if ((p->mask & WM_MP_PARAM_PARENT_SIZE) && p->parentSize <= st->mp_parentMaxSize) st->mp_parentSize = p->parentSize;
    if ((p->mask & WM_MP_PARAM_CHILD_SIZE) && p->childSize <= st->mp_childMaxSize) st->mp_childSize = p->childSize;
    if (p->mask & WM_MP_PARAM_PARENT_INTERVAL) st->mp_parentInterval = p->parentInterval;
    if (p->mask & WM_MP_PARAM_CHILD_INTERVAL) st->mp_childInterval = p->childInterval;
    if (p->mask & WM_MP_PARAM_PARENT_VCOUNT) st->mp_parentVCount = p->parentVCount;
    if (p->mask & WM_MP_PARAM_CHILD_VCOUNT) st->mp_childVCount = p->childVCount;
    if (p->mask & WM_MP_PARAM_DEFAULT_RETRY_COUNT) st->mp_defaultRetryCount = p->defaultRetryCount;
    if (!st->mp_flag) {
        if (p->mask & WM_MP_PARAM_MIN_POLL_BMP_MODE) st->mp_minPollBmpMode = p->minPollBmpMode;
        if (p->mask & WM_MP_PARAM_SINGLE_PACKET_MODE) st->mp_singlePacketMode = p->singlePacketMode;
        if (p->mask & WM_MP_PARAM_IGNORE_SIZE_PRECHECK_MODE) st->mp_ignoreSizePrecheckMode = p->ignoreSizePrecheckMode;
    }
    if (p->mask & WM_MP_PARAM_IGNORE_FATAL_ERROR_MODE) st->mp_ignoreFatalErrorMode = p->ignoreFatalErrorMode;
    wmi_cb_send();
}

static void wmi_run_request(const u32 *req)
{
    WMStatus *st = W.st;
    u16 apiid = (u16)(*(const u16 *)req & ~WM_API_REQUEST_ACCEPTED);

    if (apiid != WM_APIID_INITIALIZE && apiid != WM_APIID_ENABLE && (st == NULL || W.fifo == NULL)) {
        return; /* nothing to answer through before the buffers are known */
    }
    if (st != NULL) {
        st->apiBusy = TRUE;
        st->BusyApiid = apiid;
    }
    switch (apiid) {
    case WM_APIID_INITIALIZE:
        wmi_req_initialize(req, 1);
        break;
    case WM_APIID_ENABLE:
        wmi_req_initialize(req, 0);
        break;
    case WM_APIID_RESET:
        wmi_req_reset();
        break;
    case WM_APIID_END:
        wmi_req_end();
        break;
    case WM_APIID_DISABLE:
    case WM_APIID_POWER_ON:
    case WM_APIID_POWER_OFF:
        wmi_req_power(apiid);
        break;
    case WM_APIID_SET_P_PARAM:
        wmi_req_set_parent_param(req);
        break;
    case WM_APIID_START_PARENT:
        wmi_req_start_parent(req);
        break;
    case WM_APIID_END_PARENT:
        wmi_req_end_parent();
        break;
    case WM_APIID_START_SCAN:
        wmi_req_start_scan((const WMStartScanReq *)req);
        break;
    case WM_APIID_START_SCAN_EX:
        wmi_req_start_scan_ex((const WMStartScanExReq *)req);
        break;
    case WM_APIID_END_SCAN:
        wmi_req_end_scan();
        break;
    case WM_APIID_START_CONNECT:
        wmi_req_start_connect((const WMStartConnectReq *)req);
        break;
    case WM_APIID_DISCONNECT:
        wmi_req_disconnect(req);
        break;
    case WM_APIID_START_MP:
        wmi_req_start_mp((const WMStartMPReq *)req);
        break;
    case WM_APIID_SET_MP_DATA:
        wmi_req_set_mp_data(req);
        break;
    case WM_APIID_END_MP:
        wmi_req_end_mp();
        break;
    case WM_APIID_SET_GAMEINFO:
        wmi_req_set_game_info(req);
        break;
    case WM_APIID_SET_ENTRY:
        st->pparam.entryFlag = (u16)req[1];
        wmi_snapshot_gameinfo();
        wmi_cb_simple(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_SET_BEACON_PERIOD:
        W.beacon_period = (u16)req[1];
        wmi_cb_simple(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_SET_LIFETIME: {
        u16 cam = (u16)req[2], mp = (u16)req[4];

        if (mp != 0xffff) {
            W.lifetime = wmi_ms_to_frames((u32)(mp ? mp : 1) * 100u);
        } else if (cam != 0xffff && cam != 0) {
            W.lifetime = wmi_ms_to_frames((u32)cam * 100u);
        } else {
            W.lifetime = 0;
        }
        wmi_cb_simple(apiid, WM_ERRCODE_SUCCESS);
        break;
    }
    case WM_APIID_SET_PS_MODE:
        st->pwrMgtMode = (u16)(req[1] ? WMI_PMG_PS : WMI_PMG_CONT_ACT);
        wmi_cb_simple(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_SET_WEPKEY:
    case WM_APIID_SET_WEPKEY_EX:
        st->wepMode = (u16)req[1];
        st->wep_flag = req[1] != 0;
        wmi_cb_simple(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_SET_BEACON_IND:
        st->beaconIndicateFlag = (u16)req[1];
        wmi_cb_simple(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_MEASURE_CHANNEL:
        wmi_req_measure_channel((const WMMeasureChannelReq *)req);
        break;
    case WM_APIID_SET_MP_PARAMETER:
        wmi_req_set_mp_parameter((const WMSetMPParameterReq *)req);
        break;
    case WM_APIID_INIT_W_COUNTER:
        wmi_cb_simple(apiid, WM_ERRCODE_SUCCESS);
        break;
    case WM_APIID_GET_W_COUNTER: {
        WMGetWirelessCounterCallback *cb = wmi_cb_begin();

        cb->apiid = apiid;
        cb->errcode = WM_ERRCODE_SUCCESS;
        wmi_cb_send();
        break;
    }
    case WM_APIID_START_KS:
    case WM_APIID_END_KS:
    case WM_APIID_GET_KEYSET:
        /* WmspRequestFuncDummy on the ARM7: no reply at all. */
        break;
    case WM_APIID_START_DCF:
    case WM_APIID_SET_DCF_DATA:
    case WM_APIID_END_DCF:
    case WM_APIID_START_TESTMODE:
    case WM_APIID_STOP_TESTMODE:
    case WM_APIID_START_TESTRXMODE:
    case WM_APIID_STOP_TESTRXMODE:
        /* Infrastructure DCF and RF test modes have no peer on this side;
         * refuse them the way a radio in the wrong state does. */
        wmi_cb_simple(apiid, WM_ERRCODE_ILLEGAL_STATE);
        break;
    default:
        /* Internal ARM7 requests (VAlarm, auto-deauth, MP kicks) never come
         * from the ARM9. */
        break;
    }
    if (st != NULL && W.st != NULL) {
        W.st->apiBusy = FALSE;
    }
}

/* ------------------------------------------------------------------ */
/* Incoming datagrams                                                 */

static void wmi_rx_beacon(u32 peer, const u8 *src, wmi_buf *b)
{
    wmi_bss *e = wmi_bss_find(src);
    u16 channel = (u16)get16(b);
    u16 period = (u16)get16(b);
    u16 gi_len = (u16)get16(b);
    u16 gi[WM_SIZE_GAMEINFO / 2];
    int i;

    if (gi_len > WM_SIZE_GAMEINFO) {
        return;
    }
    getn(b, gi, gi_len);
    if (b->bad) {
        return;
    }
    if (e == NULL) {
        u32 oldest = 0;
        int victim = 0;

        for (i = 0; i < WMI_BSS_MAX; i++) {
            if (!W.bss[i].used) {
                victim = i;
                break;
            }
            if (W.frame - W.bss[i].seen >= oldest) {
                oldest = W.frame - W.bss[i].seen;
                victim = i;
            }
        }
        e = &W.bss[victim];
        memset(e, 0, sizeof *e);
        e->used = 1;
        memcpy(e->mac, src, WM_SIZE_MACADDR);
        wmi_trace("heard parent %02x:%02x:%02x on channel %d", src[3], src[4], src[5], channel);
    }
    e->peer = peer;
    e->channel = channel;
    e->period = period ? period : WM_DEFAULT_BEACON_PERIOD;
    e->gi_len = gi_len;
    memcpy(e->gi, gi, gi_len);
    e->seen = W.frame;
}

static void wmi_send_join_ack(u32 peer, const u8 *dst, u32 nonce, u16 status, u16 aid)
{
    wmi_buf b = wmi_pkt_begin(WMI_PKT_JOIN_ACK);

    putn(&b, dst, WM_SIZE_MACADDR);
    put32(&b, nonce);
    put16(&b, status);
    put16(&b, aid);
    wmi_pkt_send(peer, &b);
}

static void wmi_rx_join_req(u32 peer, const u8 *src, wmi_buf *b)
{
    WMStatus *st = W.st;
    u8 dst[WM_SIZE_MACADDR], ssid[WM_SIZE_CHILD_SSID];
    u16 tgid;
    u32 nonce;
    int i, aid = 0, count = 0;

    getn(b, dst, sizeof dst);
    tgid = (u16)get16(b);
    nonce = get32(b);
    getn(b, ssid, sizeof ssid);
    if (b->bad || memcmp(dst, W.mac, WM_SIZE_MACADDR) != 0 || !wmi_is_parent()) {
        return;
    }
    if (tgid != st->pparam.tgid) {
        return; /* a different session's SSID: MlmeJoin never matches it */
    }
    for (i = 1; i < WMI_LINKS; i++) {
        if (W.link[i].used) {
            count++;
            if (memcmp(W.link[i].mac, src, WM_SIZE_MACADDR) == 0) {
                /* A retransmitted request whose ack was lost. */
                wmi_send_join_ack(peer, src, nonce, WMI_JOIN_OK, (u16)i);
                return;
            }
        }
    }
    if (!st->pparam.entryFlag || count >= st->pparam.maxEntry) {
        wmi_send_join_ack(peer, src, nonce, WMI_JOIN_NO_ENTRY, 0);
        return;
    }
    for (i = 1; i < WMI_LINKS; i++) {
        if (!W.link[i].used) {
            aid = i;
            break;
        }
    }
    if (aid == 0) {
        wmi_send_join_ack(peer, src, nonce, WMI_JOIN_NO_ENTRY, 0);
        return;
    }
    wmi_link_open(aid, peer, src);
    st->child_bitmap |= (u16)(1u << aid);
    memcpy(st->childMacAddress[aid - 1], src, WM_SIZE_MACADDR);
    st->linkLevel = WM_LINK_LEVEL_3;
    wmi_send_join_ack(peer, src, nonce, WMI_JOIN_OK, (u16)aid);
    wmi_trace("parent: %02x:%02x:%02x joined as aid %d", src[3], src[4], src[5], aid);
    wmi_cb_parent(WM_STATECODE_CONNECTED, (u16)aid, src, 0, ssid);
}

static void wmi_rx_join_ack(u32 peer, const u8 *src, wmi_buf *b)
{
    u8 dst[WM_SIZE_MACADDR];
    u32 nonce;
    u16 status, aid;

    getn(b, dst, sizeof dst);
    nonce = get32(b);
    status = (u16)get16(b);
    aid = (u16)get16(b);
    if (b->bad || !W.join.active || nonce != W.join.nonce || memcmp(dst, W.mac, WM_SIZE_MACADDR) != 0
        || memcmp(src, W.join.bssid, WM_SIZE_MACADDR) != 0) {
        return;
    }
    if (status != WMI_JOIN_OK || aid == 0 || aid > WM_NUM_MAX_CHILD) {
        wmi_join_fail(WM_ERRCODE_OVER_MAX_ENTRY);
        return;
    }
    wmi_join_done(aid, peer);
}

static void wmi_rx_deauth(const u8 *src, wmi_buf *b)
{
    WMStatus *st = W.st;
    u8 dst[WM_SIZE_MACADDR];
    u16 aid, reason;

    getn(b, dst, sizeof dst);
    aid = (u16)get16(b);
    reason = (u16)get16(b);
    if (b->bad || memcmp(dst, W.mac, WM_SIZE_MACADDR) != 0) {
        return;
    }
    if (wmi_is_parent()) {
        if (aid >= 1 && aid < WMI_LINKS && W.link[aid].used && memcmp(W.link[aid].mac, src, WM_SIZE_MACADDR) == 0) {
            wmi_parent_drop_child(aid, reason, 0, 0);
        }
    } else if (wmi_is_child() && W.link[0].used && memcmp(W.link[0].mac, src, WM_SIZE_MACADDR) == 0) {
        (void)st;
        wmi_child_lost(reason);
    }
}

static void wmi_rx_data(const u8 *src, wmi_buf *b)
{
    WMStatus *st = W.st;
    u8 dst[WM_SIZE_MACADDR];
    u16 aid, ack, first;
    u32 count, n;
    wmi_link *l;
    int li;

    getn(b, dst, sizeof dst);
    aid = (u16)get16(b);
    ack = (u16)get16(b);
    first = (u16)get16(b);
    count = get8(b);
    if (b->bad || memcmp(dst, W.mac, WM_SIZE_MACADDR) != 0) {
        return;
    }
    if (wmi_is_parent()) {
        li = aid;
    } else if (wmi_is_child() && aid == st->aid) {
        li = 0;
    } else {
        return;
    }
    if (li >= WMI_LINKS) {
        return;
    }
    l = &W.link[li];
    if (!l->used || memcmp(l->mac, src, WM_SIZE_MACADDR) != 0) {
        return;
    }
    l->last_rx = W.frame;

    /* Acknowledged messages leave the queue. */
    while (l->qcount > 0 && wmi_seq_lt(l->q[l->qhead].seq, ack)) {
        l->qhead = (l->qhead + 1) % WMI_LINK_QUEUE;
        l->qcount--;
    }
    /* The sender dropped everything before `first` (EndMP cleans its
     * queue): skip the gap instead of waiting for it forever. */
    if (wmi_seq_lt(l->rx_next, first)) {
        l->rx_next = first;
    }
    for (n = 0; n < count; n++) {
        static u16 sData[WM_SIZE_MP_DATA_MAX / 2];
        u16 seq = (u16)get16(b);
        u16 port = (u16)get16(b);
        u16 len = (u16)get16(b);

        if (len > WM_SIZE_MP_DATA_MAX) {
            return;
        }
        getn(b, sData, len);
        if (b->bad) {
            return;
        }
        if (seq != l->rx_next || !st->mp_flag || port >= WM_NUM_OF_PORT) {
            continue; /* old, ahead of a gap, or MP not running here: the sender resends */
        }
        l->rx_next++;
        if (len > 0) {
            wmi_deliver(li == 0 ? 0 : (u16)li, port, sData, len);
            if (!l->used || W.st == NULL) {
                return; /* the callback tore the link down */
            }
        }
    }
    wmi_pend_run();
}

static void wmi_net_pump(void)
{
    static u8 sRx[2048];
    int guard;

    for (guard = 0; guard < 512; guard++) {
        u32 peer = 0;
        int n = wmi_net_recv(&peer, sRx, sizeof sRx);
        wmi_buf b;
        u8 src[WM_SIZE_MACADDR];
        u32 type;

        if (n <= 0) {
            break;
        }
        b.p = sRx;
        b.end = sRx + n;
        b.bad = 0;
        if (get32(&b) != WMI_MAGIC || get8(&b) != WMI_VERSION) {
            continue;
        }
        type = get8(&b);
        getn(&b, src, sizeof src);
        (void)get32(&b); /* sender frame */
        if (b.bad || memcmp(src, W.mac, WM_SIZE_MACADDR) == 0 || W.st == NULL) {
            continue;
        }
        switch (type) {
        case WMI_PKT_BEACON:
            wmi_rx_beacon(peer, src, &b);
            break;
        case WMI_PKT_JOIN_REQ:
            wmi_rx_join_req(peer, src, &b);
            break;
        case WMI_PKT_JOIN_ACK:
            wmi_rx_join_ack(peer, src, &b);
            break;
        case WMI_PKT_DEAUTH:
            wmi_rx_deauth(src, &b);
            break;
        case WMI_PKT_DATA:
            wmi_rx_data(src, &b);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Timers                                                             */

static void wmi_timers(void)
{
    WMStatus *st = W.st;
    int i;

    if (W.scan.active && (int)(W.frame - W.scan.done) >= 0) {
        wmi_scan_finish();
    }
    if (W.measure.active && (int)(W.frame - W.measure.done) >= 0) {
        wmi_measure_finish();
    }
    if (W.join.active) {
        if ((int)(W.frame - W.join.deadline) >= 0) {
            wmi_join_fail(WM_ERRCODE_FAILED);
        } else if (W.join.peer != 0 && (int)(W.frame - W.join.next_tx) >= 0) {
            wmi_join_send();
        } else if (W.join.peer == 0) {
            wmi_bss *e = wmi_bss_find(W.join.bssid);

            if (e != NULL) {
                W.join.peer = e->peer;
                wmi_join_send();
            }
        }
    }
    if (wmi_is_parent() && W.beacon_period != 0 && (int)(W.frame - W.next_beacon) >= 0) {
        W.next_beacon = W.frame + wmi_ms_to_frames(W.beacon_period);
        wmi_send_beacon();
        if (st->beaconIndicateFlag) {
            wmi_cb_parent(WM_STATECODE_BEACON_SENT, 0, NULL, 0, NULL);
        }
    }
    if (W.lifetime != 0) {
        for (i = 0; i < WMI_LINKS; i++) {
            if (W.link[i].used && W.frame - W.link[i].last_rx > W.lifetime) {
                if (i == 0 && wmi_is_child()) {
                    wmi_child_lost(WM_DISCONNECT_REASON_MP_LIFETIME);
                } else if (i != 0 && wmi_is_parent()) {
                    wmi_parent_drop_child(i, WM_DISCONNECT_REASON_MP_LIFETIME, 0, 1);
                }
            }
        }
    }
}

/* The MP cycle's own indications, once per frame while MP runs: a parent
 * hears MPEND_IND after polling its children, a child MP_IND when polled.
 * They carry no port data here (that went out as PORT_RECV records). */
static void wmi_mp_indications(void)
{
    WMStatus *st = W.st;
    WMStartMPCallback *cb;
    WMMpRecvBuf *buf;

    if (!st->mp_flag || st->child_bitmap == 0) {
        return;
    }
    buf = st->mp_recvBuf[st->mp_recvBufSel];
    if (buf == NULL) {
        return;
    }
    if (st->aid == 0) {
        WMMpRecvHeader *h = (WMMpRecvHeader *)buf;

        memset(h, 0, sizeof(WMMpRecvHeader) - sizeof(WMMpRecvData));
        h->length = (u16)offsetof(WMMpRecvData, cdata);
    } else {
        memset(buf, 0, offsetof(WMMpRecvBuf, data));
    }
    cb = wmi_cb_begin();
    cb->apiid = WM_APIID_START_MP;
    cb->errcode = WM_ERRCODE_SUCCESS;
    cb->state = st->aid == 0 ? WM_STATECODE_MPEND_IND : WM_STATECODE_MP_IND;
    cb->recvBuf = buf;
    wmi_cb_send();
}

/* ------------------------------------------------------------------ */
/* Entry points                                                       */

static void wmi_pxi_recv(u32 data)
{
    u16 *cmd = (u16 *)(uintptr_t)data;

    if (W.req_count >= WMI_REQ_MAX) {
        fprintf(stderr, "pc_wm: request queue full, dropping api %u\n", (unsigned)(*cmd & 0x7fff));
        *cmd |= WM_API_REQUEST_ACCEPTED;
        return;
    }
    memcpy(W.req[(W.req_head + W.req_count) % WMI_REQ_MAX], cmd, WMI_REQ_WORDS * 4);
    W.req_count++;
    /* The ARM9 reuses a command buffer once this bit is back; the request
     * is copied, so it may have it at once. */
    *cmd |= WM_API_REQUEST_ACCEPTED;
}

static int wmi_busy(void)
{
    return W.scan.active || W.join.active || W.measure.active;
}

static void wmi_run_requests(void)
{
    int guard;

    for (guard = 0; guard < 256 && W.req_count > 0 && !wmi_busy(); guard++) {
        u32 req[WMI_REQ_WORDS];

        memcpy(req, W.req[W.req_head], sizeof req);
        W.req_head = (W.req_head + 1) % WMI_REQ_MAX;
        W.req_count--;
        wmi_run_request(req);
    }
}

void pc_wm_step(void)
{
    WMStatus *st;
    int i, active;

    if (W.in_step) {
        return;
    }
    W.in_step = 1;
    W.frame++;

    wmi_run_requests();
    if (W.st != NULL && W.fifo != NULL) {
        wmi_net_pump();
        wmi_timers();
        wmi_run_requests(); /* what the network and timers released */
        st = W.st;
        if (st != NULL && st->state != WM_STATE_READY) {
            wmi_mp_indications();
            for (i = 0; i < WMI_LINKS; i++) {
                if (W.link[i].used) {
                    wmi_send_data(i);
                }
            }
        }
    }

    st = W.st;
    active = 0;
    if (W.self != 0 && st != NULL && (W.join.active || (st->state >= WM_STATE_SCAN && st->state <= WM_STATE_MP_CHILD))) {
        W.active_until = W.frame + WMI_ACTIVE_HOLD;
    }
    if (W.active_until != 0 && (int)(W.active_until - W.frame) > 0) {
        active = 1;
    }
    pc_np_stat.link_active = (unsigned)active; /* NP_STAT_LINK_ACTIVE, published by pc_view.c */
    W.in_step = 0;
}

int pc_wm_init(void)
{
    u8 *nvram = (u8 *)OS_GetSystemWork()->nvramUserInfo + ((sizeof(NVRAMConfig) + 3) & ~3u);
    const char *trace = getenv("PC_WM_TRACE");
    u16 allowed;

    memset(&W, 0, sizeof W);
    W.trace = trace != NULL && trace[0] != '\0' && trace[0] != '0';
    W.self = wmi_net_self();
    if (W.self != 0) {
        W.mac[0] = 0x00;
        W.mac[1] = 0x09;
        W.mac[2] = 0xbf; /* Nintendo's OUI */
        W.mac[3] = (u8)(W.self >> 16);
        W.mac[4] = (u8)(W.self >> 8);
        W.mac[5] = (u8)W.self;
        memcpy(nvram, W.mac, WM_SIZE_MACADDR);
        fprintf(stderr, "pc_wm: local wireless on, station %06lx, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                (unsigned long)(W.self & 0xffffff), W.mac[0], W.mac[1], W.mac[2], W.mac[3], W.mac[4], W.mac[5]);
    } else {
        memcpy(W.mac, nvram, WM_SIZE_MACADDR);
    }
    /* The firmware's allowed-channel word right after the MAC, which the ARM9
     * reads with WM_PRECALC_ALLOWEDCHANNEL (WM_GetAllowedChannel). Every
     * console has it; the port left it zero, which the game reads as "no
     * channel allowed" and answers with a comm error. */
    allowed = wmi_allowed_channel((u16)(WMI_ENABLE_CHANNEL >> 1));
    nvram[6] = (u8)allowed;
    nvram[7] = (u8)(allowed >> 8);
    pc_pxi_set_responder(PXI_FIFO_TAG_WM, wmi_pxi_recv);
    return 0;
}
