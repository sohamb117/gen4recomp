/*
 * The link cable: SIO multi-player mode between two consoles, over the
 * host's datagram transport (np_host_net_*: LAN, relay, loopback).
 *
 * Multi-player mode is a synchronous bus. The parent (master, SI low)
 * starts a transfer by setting SIOCNT's start bit; at that instant every
 * console's SIOMLT_SEND goes to every console's SIOMULTI0-3 (0xFFFF for an
 * absent player) and each raises its serial interrupt if enabled. Games
 * build their protocol on that: Gen 3's link.c exchanges a block of nine
 * words per frame, each word answered by the handler that loads the next,
 * with a checksum over what all players received. So the model keeps the
 * bus exact: both consoles see the same words at the same emulated time.
 *
 * Time is the link line count since the cable was plugged (each side counts
 * its own scanlines from its plug, both plugs at a frame boundary). The
 * child never runs ahead of the parent: before it enters line X it needs
 * the parent's word that everything before X is done (a PROG at each of the
 * parent's frame ends, or an XFER stamped later), and it takes the parent's
 * transfers stamped X there, raising its interrupt at the same line the
 * parent does. The parent never waits, except at a transfer, for the
 * child's SIOMLT_SEND at that line: it sends XFER(k, X, its word) and spins
 * for REPLY(k, the child's word), resending XFER until it comes. All
 * effects between the consoles happen at transfers, at fixed emulated
 * times, so a linked session repeats exactly whatever the host's timing.
 *
 * Plugging: with networking on, an unplugged console says HELLO to every
 * station each half second; of two that hear each other the lower station
 * id is the parent and sends PLUG (resent until PLUG_ACK). A partner silent
 * for ~10 s unplugs the cable (SD low, the error bit on a pending
 * transfer). PC_GBA_LINK_WAIT=1 (tests): the first frame end waits for the
 * partner, so both plug at the same line of the same frame.
 *
 * Two players (SIOMULTI2/3 read 0xFFFF): the Gen 3 trade and battle rooms.
 */
#include <stdlib.h>
#include <string.h>

#include "gba_port.h"
#include "np_guest_abi.h"

#define LINK_MAGIC 0x4C414247u /* 'GBAL' */
#define LINK_VERSION 1
#define POLLS_RESEND 20000u    /* empty polls between resends (~tens of ms) */
#define POLLS_TIMEOUT 20000000u /* empty polls before the partner counts as gone (seconds) */

enum { M_HELLO = 1, M_PLUG, M_PLUG_ACK, M_PROG, M_XFER, M_REPLY };

/* SIOCNT bits (multi-player mode) */
#define SIO_SI 0x0004
#define SIO_SD 0x0008
#define SIO_ID 0x0030
#define SIO_ERROR 0x0040
#define SIO_START 0x0080
#define SIO_MODE_MASK 0x3000
#define SIO_MODE_MULTI 0x2000
#define SIO_IRQ 0x4000

#define R_SIOMULTI0 0x120
#define R_SIOMLT_SEND 0x12A

typedef struct {
    uint32_t magic;
    uint8_t type, version;
    uint16_t value;
    uint32_t index;   /* XFER/REPLY: transfer number k; PROG: transfers so far */
    uint32_t session; /* the parent's plug */
    uint64_t time;    /* link lines */
} link_msg;

static struct {
    int plugged, parent, acked, wait_done;
    uint32_t peer, session;
    uint64_t lines;  /* scanlines since boot */
    uint64_t origin; /* lines at the plug */
    uint32_t xfers;  /* parent: transfers started; child: transfers taken */
    /* child */
    int have_pending;
    uint32_t pend_index;
    uint64_t pend_time;
    uint16_t pend_value, last_reply;
    int64_t prog_time; /* latest PROG: everything before it is done */
    uint32_t prog_xfers;
} L;

static uint32_t self_id(void) { return np_host_net_self(); }

static void send_msg(uint32_t to, int type, uint32_t index, uint64_t time, uint16_t value) {
    link_msg m = {LINK_MAGIC, (uint8_t)type, LINK_VERSION, value, index, L.session, time};
    np_host_net_send(to, &m, sizeof m);
}

/* One datagram from the transport: 1 a link message in *m, 0 none waiting,
 * -1 something else (not ours, or another station once plugged). */
static int recv_msg(link_msg *m, uint32_t *from) {
    uint8_t buf[64];
    int32_t n = np_host_net_recv(from, buf, sizeof buf);
    if (n <= 0) return 0;
    if (n != (int32_t)sizeof *m) return -1;
    memcpy(m, buf, sizeof *m);
    if (m->magic != LINK_MAGIC || m->version != LINK_VERSION) return -1;
    if (L.plugged && *from != L.peer) return -1;
    return 1;
}

static uint64_t link_time(void) { return L.lines - L.origin; }

/* SI and SD live in the register itself, because games also read SIOCNT
 * through plain pointers (pokeruby's CheckMasterOrSlave): SD high while the
 * cable is plugged, SI high on the child. Unplugged they stay as the game
 * wrote them (low), as before the cable existed. */
static void apply_terminals(void) {
    uint16_t v = IO16(R_SIOCNT) & (uint16_t)~(SIO_SI | SIO_SD);
    if (L.plugged) v |= (uint16_t)(SIO_SD | (L.parent ? 0 : SIO_SI));
    IO16(R_SIOCNT) = v;
}

static void unplug(const char *why) {
    if (L.plugged) gba_log("link: cable unplugged (%s)", why);
    L.plugged = 0;
    L.have_pending = 0;
    apply_terminals();
}

static void plug(uint32_t peer, int parent, uint32_t session) {
    L.plugged = 1;
    L.parent = parent;
    L.acked = 0;
    L.peer = peer;
    L.session = session;
    L.origin = L.lines;
    L.xfers = 0;
    L.have_pending = 0;
    L.prog_time = -1;
    L.prog_xfers = 0;
    apply_terminals();
    gba_log("link: cable plugged, %s of station %06x (frame %llu)", parent ? "parent" : "child", peer,
            (unsigned long long)gba_frames);
}

/* Messages that need no time: plugging, duplicates. Returns 1 if consumed. */
static int handle_common(const link_msg *m, uint32_t from) {
    uint32_t self = self_id();
    switch (m->type) {
    case M_HELLO:
        if (!L.plugged && self < from) {
            plug(from, 1, self << 8 ^ (uint32_t)gba_frames);
            send_msg(from, M_PLUG, 0, 0, 0);
        }
        return 1;
    case M_PLUG:
        if (!L.plugged && self > from) plug(from, 0, m->session);
        if (L.plugged && !L.parent && m->session == L.session) send_msg(from, M_PLUG_ACK, 0, 0, 0);
        return 1;
    case M_PLUG_ACK:
        if (L.plugged && L.parent && m->session == L.session) L.acked = 1;
        return 1;
    default:
        return 0;
    }
}

/* Child: a message about the parent's progress or a transfer. */
static void child_take(const link_msg *m) {
    if (m->session != L.session) return;
    if (m->type == M_PROG) {
        if ((int64_t)m->time > L.prog_time) {
            L.prog_time = (int64_t)m->time;
            L.prog_xfers = m->index;
        }
    } else if (m->type == M_XFER) {
        if (m->index == L.xfers && L.xfers) send_msg(L.peer, M_REPLY, m->index, m->time, L.last_reply);
        else if (m->index == L.xfers + 1) {
            L.have_pending = 1;
            L.pend_index = m->index;
            L.pend_time = m->time;
            L.pend_value = m->value;
        }
    }
}

static int multi_mode(void) {
    return !(IO16(R_RCNT) & 0x8000) && (IO16(R_SIOCNT) & SIO_MODE_MASK) == SIO_MODE_MULTI;
}

/* The bus words land in every console's SIOMULTI0-3. */
static void complete(uint16_t parent_word, uint16_t child_word, int error) {
    IO16(R_SIOMULTI0) = parent_word;
    IO16(R_SIOMULTI0 + 2) = child_word;
    IO16(R_SIOMULTI0 + 4) = 0xFFFF;
    IO16(R_SIOMULTI0 + 6) = 0xFFFF;
    uint16_t cnt = IO16(R_SIOCNT) & (uint16_t)~(SIO_START | SIO_ID | SIO_ERROR);
    cnt |= (uint16_t)((L.parent ? 0 : 1) << 4);
    if (error) cnt |= SIO_ERROR;
    IO16(R_SIOCNT) = cnt;
    if (cnt & SIO_IRQ) gba_raise_irq(IRQ_SERIAL);
}

/* Child: the parent's transfer at this line. */
static void child_transfer(void) {
    uint16_t mine = multi_mode() ? IO16(R_SIOMLT_SEND) : 0xFFFF;
    L.xfers = L.pend_index;
    L.last_reply = mine;
    L.have_pending = 0;
    send_msg(L.peer, M_REPLY, L.pend_index, L.pend_time, mine);
    if (multi_mode()) complete(L.pend_value, mine, 0);
}

void gba_link_line(void) {
    L.lines++;
    if (!L.plugged || L.parent) return;
    uint64_t t = link_time();
    uint32_t idle = 0;
    for (;;) {
        if (L.have_pending && L.pend_index == L.xfers + 1) {
            if (L.pend_time < t) {
                gba_log("link: transfer %u for line %llu taken late at %llu", L.pend_index,
                        (unsigned long long)L.pend_time, (unsigned long long)t);
                child_transfer();
                continue;
            }
            if (L.pend_time == t) {
                child_transfer();
                idle = 0;
                continue;
            }
            return; /* the next transfer is later: this line is clear */
        }
        if (L.prog_time > (int64_t)t && L.prog_xfers <= L.xfers) return;
        link_msg m;
        uint32_t from;
        int r = recv_msg(&m, &from);
        if (r > 0) {
            if (!handle_common(&m, from)) child_take(&m);
            idle = 0;
        } else if (r == 0 && ++idle >= POLLS_TIMEOUT) {
            unplug("the parent went quiet");
            return;
        }
    }
}

/* Parent: SIOCNT's start bit in multi-player mode. */
static void parent_transfer(void) {
    uint16_t mine = IO16(R_SIOMLT_SEND);
    if (!L.plugged) {
        complete(mine, 0xFFFF, 0);
        return;
    }
    uint32_t k = ++L.xfers;
    uint64_t t = link_time();
    send_msg(L.peer, M_XFER, k, t, mine);
    uint32_t idle = 0;
    for (;;) {
        link_msg m;
        uint32_t from;
        int r = recv_msg(&m, &from);
        if (r > 0) {
            if (m.type == M_REPLY && m.session == L.session && m.index == k) {
                complete(mine, m.value, 0);
                return;
            }
            handle_common(&m, from);
            idle = 0;
        } else if (r == 0) {
            if (++idle % POLLS_RESEND == 0) send_msg(L.peer, M_XFER, k, t, mine);
            if (idle >= POLLS_TIMEOUT) {
                unplug("the child went quiet");
                complete(mine, 0xFFFF, 1);
                return;
            }
        }
    }
}

void gba_sio_write_cnt(uint16_t v) {
    uint16_t old = IO16(R_SIOCNT);
    /* SI, SD, ID and busy belong to the hardware; error is cleared by writing */
    uint16_t keep = old & (SIO_SI | SIO_SD | SIO_ID | SIO_START);
    IO16(R_SIOCNT) = (uint16_t)((v & ~(SIO_SI | SIO_SD | SIO_ID | SIO_ERROR | SIO_START)) | keep);
    if ((v & SIO_START) && (v & SIO_MODE_MASK) == SIO_MODE_MULTI && !(IO16(R_RCNT) & 0x8000) &&
        (!L.plugged || L.parent)) {
        IO16(R_SIOCNT) |= SIO_START;
        parent_transfer();
    }
}

/* Frame end (before the host's vblank): plugging, the parent's PROG. */
void gba_link_frame(uint32_t *status) {
    if (!self_id()) return;
    static int wait = -1;
    if (wait < 0) {
        const char *w = getenv("PC_GBA_LINK_WAIT");
        wait = w && w[0] == '1';
    }
    uint32_t idle = 0;
    for (;;) {
        int waiting = wait && !L.wait_done;
        if (!L.plugged && (gba_frames % 30 == 0 || waiting) && idle % POLLS_RESEND == 0)
            send_msg(0xFFFFFFFFu, M_HELLO, 0, 0, 0);
        if (L.plugged && L.parent && !L.acked && (gba_frames % 30 == 0 || waiting) && idle % POLLS_RESEND == 0)
            send_msg(L.peer, M_PLUG, 0, 0, 0);
        link_msg m;
        uint32_t from;
        int r = recv_msg(&m, &from);
        if (r > 0) {
            if (!handle_common(&m, from) && L.plugged && !L.parent) child_take(&m);
            idle = 0;
        } else if (r == 0) {
            /* PC_GBA_LINK_WAIT: the first frame end holds until plugged
             * (the child) or acknowledged (the parent) */
            int done = !waiting || (L.plugged && (!L.parent || L.acked));
            if (done) break;
            if (++idle >= POLLS_TIMEOUT * 3) {
                gba_log("link: PC_GBA_LINK_WAIT: no partner");
                break;
            }
        }
    }
    L.wait_done = 1;
    apply_terminals(); /* again after a soft reset cleared the registers */
    if (L.plugged && L.parent) send_msg(L.peer, M_PROG, L.xfers, link_time(), 0);
    status[NP_STAT_LINK_ACTIVE] =
        L.plugged && multi_mode() && (IO16(R_SIOCNT) & SIO_IRQ) && (IO16(R_IE) & IRQ_SERIAL) ? 1 : 0;
}
