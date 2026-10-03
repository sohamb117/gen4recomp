/* The wasm host layer's shared pieces; see pc/wasm/include/pc_wasm.h. */
#include <pc_wasm.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

np_frame_desc pc_wasm_frame = {
    .magic = NP_FRAME_MAGIC,
    .version = NP_GUEST_ABI_VERSION,
};

/* What NitroSDK's mb_fileinfo.c takes the address of in place of
 * `_start_AutoloadDoneCallback` on wasm, where a symbol cannot be data in
 * one object and a function in another; see pc/Makefile.wasm. */
unsigned int pc_wasm_mb_autoload_anchor[1];

void pc_wasm_fatal(const char *msg)
{
    size_t n = strlen(msg);
    fprintf(stderr, "pokeplatinum: fatal: %s\n", msg);
    fflush(stderr);
    np_host_trap(msg, (uint32_t)n);
}

void pc_wasm_fatalf(const char *fmt, ...)
{
    static char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pc_wasm_fatal(buf);
}

/* The port's fatal paths (gx_fatal in pc/hw, pc_mi, pc_os_lite, ...) print
 * their reason to stderr and call abort(). wasi-libc's abort is a bare
 * `unreachable`, which reaches the runtime as an anonymous trap; this one
 * (linked ahead of libc, so libc's abort.o is never pulled) hands the
 * runtime a message instead. The reason itself is already on stderr. */
void abort(void)
{
    pc_wasm_fatal("abort() called; the reason is on stderr above");
}
