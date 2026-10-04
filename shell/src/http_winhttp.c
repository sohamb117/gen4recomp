/*
 * np_http_get on WinHTTP (http.h). WinHTTP follows redirects itself (but
 * never from https to http by default) and validates certificates against
 * the Windows store.
 */
#include "http.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <winhttp.h>

static void win_error(char *err, size_t errn, const char *what)
{
    snprintf(err, errn, "%s failed (error %lu)", what, (unsigned long)GetLastError());
}

static int widen(const char *s, wchar_t *out, int cap)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) > 0 ? 0 : -1;
}

int np_http_get(const np_http_request *r, char *err, size_t errn)
{
    wchar_t url[2048], agent[128], host[256], path[2048];
    if (widen(r->url, url, 2048) || widen(r->user_agent, agent, 128)) {
        snprintf(err, errn, "bad URL");
        return -1;
    }
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[1024];
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        win_error(err, errn, "WinHttpCrackUrl");
        return -1;
    }
    if (wcslen(path) + wcslen(extra) + 1 > 2048) {
        snprintf(err, errn, "URL too long");
        return -1;
    }
    wcscat(path, extra); /* the query string */
    int status = -1;
    HINTERNET session = NULL, conn = NULL, req = NULL;
    session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        win_error(err, errn, "WinHttpOpen");
        goto done;
    }
    WinHttpSetTimeouts(session, 20000, 20000, 60000, 60000);
    conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) {
        win_error(err, errn, "WinHttpConnect");
        goto done;
    }
    req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        win_error(err, errn, "WinHttpOpenRequest");
        goto done;
    }
    wchar_t accept[200];
    const wchar_t *headers = WINHTTP_NO_ADDITIONAL_HEADERS;
    if (r->accept) {
        wchar_t value[160];
        if (widen(r->accept, value, 160) == 0) {
            swprintf(accept, 200, L"Accept: %ls", value);
            headers = accept;
        }
    }
    if (!WinHttpSendRequest(req, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        win_error(err, errn, "the request");
        goto done;
    }
    DWORD code = 0, size = sizeof code;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &code, &size, WINHTTP_NO_HEADER_INDEX)) {
        win_error(err, errn, "reading the status");
        goto done;
    }
    char buf[16384];
    for (;;) {
        if (r->cancel && *r->cancel) {
            snprintf(err, errn, "cancelled");
            goto done;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, buf, sizeof buf, &got)) {
            win_error(err, errn, "reading the response");
            goto done;
        }
        if (!got)
            break;
        if (r->sink(r->user, buf, got)) {
            snprintf(err, errn, "cancelled");
            goto done;
        }
    }
    status = (int)code;
done:
    if (req)
        WinHttpCloseHandle(req);
    if (conn)
        WinHttpCloseHandle(conn);
    if (session)
        WinHttpCloseHandle(session);
    return status;
}
