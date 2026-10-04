/*
 * Local wireless over UDP: the host side of the np_host net callbacks
 * (np_core.h, contract v2). The core's ARM7 wireless model hands it opaque
 * datagrams addressed to station ids; this file finds the other stations and
 * carries the datagrams. Plain BSD sockets (winsock on Windows), one
 * non-blocking socket, no threads: everything happens inside the calls,
 * which the shell makes from its frame loop.
 *
 * Discovery: every second a HELLO goes to the LAN broadcast address and to
 * 127.0.0.1 on each port of the search range (so two instances on one machine
 * find each other), and to every manually added peer (join by IP:port for
 * VPNs and the internet). A HELLO or its ACK teaches the receiver the
 * sender's station id and address. Peers silent for 10 s are forgotten.
 *
 * Relay mode (internet play without port forwarding): with relay set, no LAN
 * discovery happens; every second a JOIN carrying the room PIN goes to the
 * relay (server/relay), which answers with the room's station ids and
 * forwards datagrams between the stations of one room.
 *
 * Datagram loss is the guest's problem by contract (its protocol resends);
 * drop_percent injects loss on purpose for tests.
 */
#ifndef NP_SHELL_NET_H
#define NP_SHELL_NET_H

#include <stddef.h>
#include <stdint.h>

#define NP_NET_DEFAULT_PORT 2009
#define NP_NET_PORT_RANGE 4 /* bind tries port..port+3; HELLOs cover the same range */

typedef struct np_net np_net;

typedef struct np_net_config {
    uint16_t port;          /* first UDP port to try, 0 = NP_NET_DEFAULT_PORT */
    uint32_t station_id;    /* 24-bit station id, persistent per install; 0 = random */
    int lan_discovery;      /* broadcast HELLOs on the LAN */
    int drop_percent;       /* 0..100: drop this share of outgoing datagrams (testing) */
    const char *relay;      /* "host:port" of a relay (server/relay), NULL = LAN mode */
    const char *pin;        /* relay room PIN, 1..32 bytes */
    void (*log)(void *user, const char *line);
    void *log_user;
} np_net_config;

/* Opens the socket (first free port in the range). NULL with a message in
 * err on failure. */
np_net *np_net_open(const np_net_config *cfg, char *err, size_t errlen);
void np_net_close(np_net *n);

/* Adds a peer by "host:port" or "host" (default port); resolved now. */
int np_net_add_peer(np_net *n, const char *hostport, char *err, size_t errlen);

uint32_t np_net_self(const np_net *n);
uint16_t np_net_port(const np_net *n);
int np_net_peer_count(const np_net *n);

/* Discovery, expiry and draining the socket (control packets answer even
 * while the guest is not reading); call once per frame. */
void np_net_poll(np_net *n);

/* The np_host callbacks' semantics: send returns 0 or -1 (peer
 * NP_NET_BROADCAST = every known station); recv returns the length, 0 when
 * nothing waits, -1 on error. */
int np_net_send(np_net *n, uint32_t peer, const void *buf, uint32_t len);
int np_net_recv(np_net *n, uint32_t *peer, void *buf, uint32_t cap);

/* A random 24-bit nonzero id, for a first run's persistent station id. */
uint32_t np_net_random_id(void);

#endif /* NP_SHELL_NET_H */
