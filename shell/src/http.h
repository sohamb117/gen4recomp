/*
 * The one network request the shell makes on its own: HTTPS GET for the
 * updater, only when the player asks to check or download. Two small
 * backends use what the OS already ships, so the app bundles no TLS stack:
 *
 *   http_curl.c     macOS: libcurl from the macOS SDK (/usr/lib/libcurl)
 *   http_winhttp.c  Windows: WinHTTP
 *
 * iOS has neither (and updates come from the App Store there), so iOS
 * builds compile no backend and NP_HAVE_HTTP is not defined; the updater is
 * left out of those builds.
 *
 * Calls block; the updater runs them on a worker thread.
 */
#ifndef NP_HTTP_H
#define NP_HTTP_H

#include <stddef.h>

/* Receives the body in order. Return 0 to continue, nonzero to abort. */
typedef int (*np_http_sink)(void *user, const void *data, size_t len);

typedef struct np_http_request {
    const char *url;        /* http:// or https://; redirects are followed */
    const char *accept;     /* Accept header, or NULL */
    const char *user_agent; /* required by GitHub's API */
    np_http_sink sink;
    void *user;
    volatile int *cancel; /* polled while transferring; nonzero aborts */
} np_http_request;

/* Performs the request. Returns the final HTTP status (200 on success; the
 * body of other statuses is delivered too), or -1 with `err` set when there
 * was no response (DNS, TLS, connection, cancel, sink abort). */
int np_http_get(const np_http_request *r, char *err, size_t errn);

#endif
