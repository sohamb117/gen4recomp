/* The wasm host layer's shared pieces; see pc/wasm/include/pc_wasm.h. */
#include <pc_wasm.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

np_frame_desc pc_wasm_frame = {
    .magic = NP_FRAME_MAGIC,
    .version = NP_GUEST_ABI_VERSION,
};

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
