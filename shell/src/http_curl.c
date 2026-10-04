/*
 * np_http_get on libcurl (http.h), linked from the macOS SDK. The system
 * libcurl uses the OS trust store, so certificates need no bundling.
 */
#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

typedef struct transfer {
    const np_http_request *r;
    int aborted;
} transfer;

static size_t on_data(char *data, size_t size, size_t n, void *user)
{
    transfer *t = user;
    size_t len = size * n;
    if (t->r->sink(t->r->user, data, len)) {
        t->aborted = 1;
        return 0; /* curl stops with CURLE_WRITE_ERROR */
    }
    return len;
}

static int on_progress(void *user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal, (void)dlnow, (void)ultotal, (void)ulnow;
    transfer *t = user;
    return t->r->cancel && *t->r->cancel ? 1 : 0;
}

int np_http_get(const np_http_request *r, char *err, size_t errn)
{
    static int initialized;
    if (!initialized) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT)) {
            snprintf(err, errn, "libcurl could not start");
            return -1;
        }
        initialized = 1; /* process lifetime: cleanup would race other threads */
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(err, errn, "libcurl could not start");
        return -1;
    }
    transfer t = {r, 0};
    struct curl_slist *headers = NULL;
    char accept[160];
    if (r->accept) {
        snprintf(accept, sizeof accept, "Accept: %s", r->accept);
        headers = curl_slist_append(headers, accept);
    }
    char errbuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_URL, r->url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, r->user_agent);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L); /* give up after 60 s without data */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); /* thread-safe timeouts */
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    CURLcode rc = curl_easy_perform(curl);
    long status = -1;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    else if (rc == CURLE_ABORTED_BY_CALLBACK || t.aborted)
        snprintf(err, errn, "cancelled");
    else
        snprintf(err, errn, "%s", errbuf[0] ? errbuf : curl_easy_strerror(rc));
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return (int)status;
}
