/* The second translation unit that includes windows.h, pc_win_ipc.h says
 * why. POSIX builds compile the empty tail. */

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "pc_win_ipc.h"

static char ipc_err[256];

const char *pcw_ipc_error(void) { return ipc_err; }

static void ipc_fail(const char *what) {
    snprintf(ipc_err, sizeof ipc_err, "%s failed, error %lu", what,
             (unsigned long)GetLastError());
}

void *pcw_shm_create(const char *name, unsigned size, void **handle_out) {
    SECURITY_ATTRIBUTES sa;
    char local[160];
    HANDLE h;
    void *view;

    *handle_out = NULL;

    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = name == NULL;   /* only the --view shape is inherited */

    if (name != NULL) {
        snprintf(local, sizeof local, "Local\\%s", name);
    }
    h = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                           0, size, name != NULL ? local : NULL);
    if (h == NULL) {
        ipc_fail("CreateFileMapping");
        return NULL;
    }
    /* A second publisher to one name would be two ports fighting over one
     * seqlock; POSIX said no with O_EXCL and this says it the Windows way. */
    if (name != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        snprintf(ipc_err, sizeof ipc_err,
                 "%s already exists; something is already publishing to it",
                 local);
        CloseHandle(h);
        return NULL;
    }

    view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (view == NULL) {
        ipc_fail("MapViewOfFile");
        CloseHandle(h);
        return NULL;
    }
    *handle_out = (void *)h;
    return view;
}

void pcw_shm_close(void *view, void *handle) {
    if (view != NULL) UnmapViewOfFile(view);
    if (handle != NULL) CloseHandle((HANDLE)handle);
}

unsigned long pcw_ipc_handle_value(void *handle) {
    return (unsigned long)(UINT_PTR)handle;
}

/*
 * One argument, quoted the way every Windows CRT re-splits it: wrapped when
 * it holds a space or a quote, with interior quotes and the backslashes
 * before them escaped. Anything this port actually passes is a flag, a
 * number or a device name, but a rule half-done is a rule that breaks on
 * the first audio device with a quote in its name.
 */
static int quote_arg(const char *a, char *out, int cap) {
    int n = 0, i, j, bs;
    int need = strchr(a, ' ') != NULL || strchr(a, '\t') != NULL ||
               strchr(a, '"') != NULL || a[0] == '\0';

#define PUT(c) do { if (n >= cap) return -1; out[n++] = (c); } while (0)
    if (!need) {
        for (i = 0; a[i] != '\0'; i++) PUT(a[i]);
        return n;
    }
    PUT('"');
    for (i = 0; a[i] != '\0'; i++) {
        bs = 0;
        while (a[i] == '\\') { bs++; i++; }
        if (a[i] == '"') {
            for (j = 0; j < 2 * bs + 1; j++) PUT('\\');
            PUT('"');
        } else if (a[i] == '\0') {
            for (j = 0; j < 2 * bs; j++) PUT('\\');
            break;
        } else {
            for (j = 0; j < bs; j++) PUT('\\');
            PUT(a[i]);
        }
    }
    PUT('"');
    return n;
#undef PUT
}

void *pcw_spawn(char *const argv[]) {
    static char cmdline[2048];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    int n = 0, i, k;

    for (i = 0; argv[i] != NULL; i++) {
        if (i > 0) {
            if (n + 1 >= (int)sizeof cmdline) { ipc_fail("command line"); return NULL; }
            cmdline[n++] = ' ';
        }
        k = quote_arg(argv[i], cmdline + n, (int)sizeof cmdline - n - 1);
        if (k < 0) {
            snprintf(ipc_err, sizeof ipc_err, "command line too long");
            return NULL;
        }
        n += k;
    }
    cmdline[n] = '\0';

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);

    if (!CreateProcessA(argv[0], cmdline, NULL, NULL, TRUE /* inherit */,
                        0, NULL, NULL, &si, &pi)) {
        ipc_fail("CreateProcess");
        return NULL;
    }
    CloseHandle(pi.hThread);
    return (void *)pi.hProcess;
}

int pcw_process_gone(void *proc) {
    if (proc == NULL) return 0;
    if (WaitForSingleObject((HANDLE)proc, 0) != WAIT_OBJECT_0) return 0;
    CloseHandle((HANDLE)proc);
    return 1;
}

void *pcw_self_process(void) {
    HANDLE dup = NULL;

    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
                         GetCurrentProcess(), &dup,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                         TRUE /* inheritable */, 0)) {
        ipc_fail("DuplicateHandle");
        return NULL;
    }
    return (void *)dup;
}

unsigned pcw_self_pid(void) {
    return (unsigned)GetCurrentProcessId();
}

int pcw_pid_alive(unsigned pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    DWORD w;

    /* No handle means gone, or means a process this one may not open, and
     * the difference does not matter here: a viewer the port started is
     * always openable by it. */
    if (h == NULL) return 0;
    w = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return w == WAIT_TIMEOUT;   /* still running */
}

/* A fixed-address anonymous mapping, the GBA-slot window's Windows
 * spelling. Same contract as armrec_rt.c's region mapping: the address or
 * nothing. */
void *pcw_valloc_fixed(void *want, unsigned len) {
    return VirtualAlloc(want, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

int pcw_module_dir(char *out, unsigned cap) {
    DWORD n = GetModuleFileNameA(NULL, out, cap);
    char *slash;

    if (n == 0 || n >= cap) return 0;
    for (slash = out + n; slash > out; slash--) {
        if (slash[-1] == '\\' || slash[-1] == '/') break;
    }
    if (slash == out) return 0;
    *slash = '\0';
    return 1;
}

#else

/* ISO C dislikes an empty translation unit. */
typedef int pc_win_ipc_is_windows_only;

#endif
