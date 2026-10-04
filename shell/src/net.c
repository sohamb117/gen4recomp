/* Local wireless over UDP; the design is in net.h. */
#include "net.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET np_sock;
#define NP_BAD_SOCK INVALID_SOCKET
#define np_closesock closesocket
#define np_would_block() (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAECONNRESET)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int np_sock;
#define NP_BAD_SOCK (-1)
#define np_closesock close
#define np_would_block() (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNREFUSED || errno == EINTR)
#endif

#define NET_MAGIC 0x314E504Eu /* "NPN1" */
#define NET_HEADER 12
#define NET_MAX_PAYLOAD 1460
#define NET_MAX_PEERS 32
#define NET_HELLO_MS 1000
#define NET_PEER_TIMEOUT_MS 10000

enum { NET_DATA = 0, NET_HELLO = 1, NET_HELLO_ACK = 2 };

typedef struct {
    uint32_t id; /* 0 = a manual address whose station has not answered yet */
    struct sockaddr_in addr;
    uint64_t last_rx;
    int manual;
} net_peer;

struct np_net {
    np_sock sock;
    uint32_t self;
    uint16_t port, base_port;
    int lan;
    int drop;
    uint32_t rng;
    uint64_t next_hello;
    net_peer peer[NET_MAX_PEERS];
    int npeers;
    void (*log)(void *user, const char *line);
    void *log_user;
};

static void net_log(np_net *n, const char *fmt, ...)
{
    char line[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n->log) {
        n->log(n->log_user, line);
    } else {
        fprintf(stderr, "net: %s\n", line);
    }
}

static uint64_t net_now_ms(void)
{
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static uint32_t net_mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint32_t np_net_random_id(void)
{
    uint32_t seed = (uint32_t)time(NULL) ^ (uint32_t)net_now_ms() ^ (uint32_t)(uintptr_t)&seed;
#if defined(_WIN32)
    seed ^= (uint32_t)GetCurrentProcessId() << 8;
#else
    seed ^= (uint32_t)getpid() << 8;
#endif
    for (;;) {
        uint32_t id = net_mix(seed++) & 0xffffffu;
        if (id != 0) {
            return id;
        }
    }
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void net_header(np_net *n, uint8_t *p, int type)
{
    put32(p, NET_MAGIC);
    p[4] = (uint8_t)type;
    p[5] = 1; /* version */
    p[6] = (uint8_t)n->port;
    p[7] = (uint8_t)(n->port >> 8);
    put32(p + 8, n->self);
}

static int net_sendto(np_net *n, const struct sockaddr_in *to, const uint8_t *p, size_t len)
{
    if (sendto(n->sock, (const char *)p, (int)len, 0, (const struct sockaddr *)to, sizeof *to) < 0) {
        return np_would_block() ? 0 : -1;
    }
    return 0;
}

static void net_send_control(np_net *n, const struct sockaddr_in *to, int type)
{
    uint8_t p[NET_HEADER];

    net_header(n, p, type);
    (void)net_sendto(n, to, p, sizeof p);
}

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static net_peer *net_find(np_net *n, uint32_t id)
{
    int i;

    for (i = 0; i < n->npeers; i++) {
        if (n->peer[i].id == id) {
            return &n->peer[i];
        }
    }
    return NULL;
}

/* A datagram from station `id` at `from`: remember where it lives. */
static void net_learn(np_net *n, uint32_t id, const struct sockaddr_in *from)
{
    net_peer *p = net_find(n, id);
    int i;

    if (p == NULL) {
        for (i = 0; i < n->npeers; i++) {
            if (n->peer[i].id == 0 && same_addr(&n->peer[i].addr, from)) {
                p = &n->peer[i];
                break;
            }
        }
    }
    if (p == NULL) {
        if (n->npeers == NET_MAX_PEERS) {
            return;
        }
        p = &n->peer[n->npeers++];
        memset(p, 0, sizeof *p);
    }
    if (p->id != id) {
        char addr[32];

        inet_ntop(AF_INET, &from->sin_addr, addr, sizeof addr);
        net_log(n, "station %06x at %s:%u", (unsigned)id, addr, (unsigned)ntohs(from->sin_port));
    }
    p->id = id;
    p->addr = *from;
    p->last_rx = net_now_ms();
}

np_net *np_net_open(const np_net_config *cfg, char *err, size_t errlen)
{
    np_net *n;
    int i, yes = 1;

#if defined(_WIN32)
    {
        WSADATA wsa;

        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            snprintf(err, errlen, "WSAStartup failed");
            return NULL;
        }
    }
#endif
    n = calloc(1, sizeof *n);
    if (n == NULL) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    n->self = cfg->station_id ? (cfg->station_id & 0xffffffu) : np_net_random_id();
    if (n->self == 0) {
        n->self = np_net_random_id();
    }
    n->base_port = cfg->port ? cfg->port : NP_NET_DEFAULT_PORT;
    n->lan = cfg->lan_discovery;
    n->drop = cfg->drop_percent < 0 ? 0 : cfg->drop_percent > 100 ? 100 : cfg->drop_percent;
    n->rng = net_mix(n->self ^ 0x9e3779b9u);
    n->log = cfg->log;
    n->log_user = cfg->log_user;

    n->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (n->sock == NP_BAD_SOCK) {
        snprintf(err, errlen, "cannot create a UDP socket");
        free(n);
        return NULL;
    }
    (void)setsockopt(n->sock, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes);
#if defined(_WIN32)
    {
        u_long nb = 1;
        ioctlsocket(n->sock, FIONBIO, &nb);
    }
#else
    fcntl(n->sock, F_SETFL, fcntl(n->sock, F_GETFL, 0) | O_NONBLOCK);
#endif
    for (i = 0; i < NP_NET_PORT_RANGE; i++) {
        struct sockaddr_in a;

        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((uint16_t)(n->base_port + i));
        if (bind(n->sock, (struct sockaddr *)&a, sizeof a) == 0) {
            n->port = (uint16_t)(n->base_port + i);
            break;
        }
    }
    if (n->port == 0) {
        snprintf(err, errlen, "UDP ports %u-%u are all in use", (unsigned)n->base_port,
                 (unsigned)(n->base_port + NP_NET_PORT_RANGE - 1));
        np_closesock(n->sock);
        free(n);
        return NULL;
    }
    net_log(n, "local wireless on UDP port %u, station %06x", (unsigned)n->port, (unsigned)n->self);
    return n;
}

void np_net_close(np_net *n)
{
    if (n == NULL) {
        return;
    }
    np_closesock(n->sock);
    free(n);
#if defined(_WIN32)
    WSACleanup();
#endif
}

int np_net_add_peer(np_net *n, const char *hostport, char *err, size_t errlen)
{
    char host[256];
    const char *colon = strrchr(hostport, ':');
    unsigned port = n->base_port;
    struct addrinfo hints, *res = NULL;
    net_peer *p;
    size_t len;

    if (colon != NULL) {
        char *end;
        unsigned long v = strtoul(colon + 1, &end, 10);

        if (*end != '\0' || v == 0 || v > 65535) {
            snprintf(err, errlen, "bad port in %s", hostport);
            return -1;
        }
        port = (unsigned)v;
        len = (size_t)(colon - hostport);
    } else {
        len = strlen(hostport);
    }
    if (len == 0 || len >= sizeof host) {
        snprintf(err, errlen, "bad address %s", hostport);
        return -1;
    }
    memcpy(host, hostport, len);
    host[len] = '\0';
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
        snprintf(err, errlen, "cannot resolve %s", host);
        return -1;
    }
    if (n->npeers == NET_MAX_PEERS) {
        freeaddrinfo(res);
        snprintf(err, errlen, "too many peers");
        return -1;
    }
    p = &n->peer[n->npeers++];
    memset(p, 0, sizeof *p);
    memcpy(&p->addr, res->ai_addr, sizeof p->addr);
    p->addr.sin_port = htons((uint16_t)port);
    p->manual = 1;
    freeaddrinfo(res);
    n->next_hello = 0; /* say hello at the next poll */
    return 0;
}

uint32_t np_net_self(const np_net *n) { return n->self; }
uint16_t np_net_port(const np_net *n) { return n->port; }

int np_net_peer_count(const np_net *n)
{
    int i, c = 0;

    for (i = 0; i < n->npeers; i++) {
        c += n->peer[i].id != 0;
    }
    return c;
}

void np_net_poll(np_net *n)
{
    uint64_t now = net_now_ms();
    int i;

    if (now < n->next_hello) {
        return;
    }
    n->next_hello = now + NET_HELLO_MS;
    for (i = 0; i < NP_NET_PORT_RANGE; i++) {
        struct sockaddr_in a;
        uint16_t port = (uint16_t)(n->base_port + i);

        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if (port != n->port) {
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            net_send_control(n, &a, NET_HELLO);
        }
        if (n->lan) {
            a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            net_send_control(n, &a, NET_HELLO);
        }
    }
    for (i = 0; i < n->npeers; i++) {
        net_peer *p = &n->peer[i];

        if (p->manual) {
            net_send_control(n, &p->addr, NET_HELLO);
            if (p->id != 0 && now - p->last_rx > NET_PEER_TIMEOUT_MS) {
                net_log(n, "station %06x went quiet", (unsigned)p->id);
                p->id = 0;
            }
        } else if (now - p->last_rx > NET_PEER_TIMEOUT_MS) {
            net_log(n, "station %06x went quiet", (unsigned)p->id);
            n->peer[i] = n->peer[--n->npeers];
            i--;
        }
    }
}

static int net_drop(np_net *n)
{
    if (n->drop == 0) {
        return 0;
    }
    n->rng = n->rng * 1664525u + 1013904223u;
    return (int)((n->rng >> 8) % 100u) < n->drop;
}

int np_net_send(np_net *n, uint32_t peer, const void *buf, uint32_t len)
{
    uint8_t p[NET_HEADER + NET_MAX_PAYLOAD];
    int i, rc = 0;

    if (len > NET_MAX_PAYLOAD) {
        return -1;
    }
    net_header(n, p, NET_DATA);
    memcpy(p + NET_HEADER, buf, len);
    for (i = 0; i < n->npeers; i++) {
        net_peer *q = &n->peer[i];

        if (q->id == 0 || (peer != 0xFFFFFFFFu && q->id != peer)) {
            continue;
        }
        if (net_drop(n)) {
            continue;
        }
        if (net_sendto(n, &q->addr, p, NET_HEADER + len) != 0) {
            rc = -1;
        }
    }
    return rc;
}

int np_net_recv(np_net *n, uint32_t *peer, void *buf, uint32_t cap)
{
    uint8_t p[NET_HEADER + NET_MAX_PAYLOAD + 64];

    for (;;) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof from;
        int got = (int)recvfrom(n->sock, (char *)p, sizeof p, 0, (struct sockaddr *)&from, &fromlen);
        uint32_t id;

        if (got < 0) {
            return np_would_block() ? 0 : -1;
        }
        if (got < NET_HEADER || get32(p) != NET_MAGIC || p[5] != 1) {
            continue;
        }
        id = get32(p + 8) & 0xffffffu;
        if (id == 0 || id == n->self) {
            continue; /* our own broadcast, looped back */
        }
        net_learn(n, id, &from);
        switch (p[4]) {
        case NET_HELLO:
            net_send_control(n, &from, NET_HELLO_ACK);
            continue;
        case NET_HELLO_ACK:
            continue;
        case NET_DATA:
            if ((uint32_t)(got - NET_HEADER) > cap) {
                return -1;
            }
            memcpy(buf, p + NET_HEADER, (size_t)(got - NET_HEADER));
            *peer = id;
            return got - NET_HEADER;
        default:
            continue;
        }
    }
}
