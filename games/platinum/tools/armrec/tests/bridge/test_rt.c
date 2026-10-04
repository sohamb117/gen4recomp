/* What the bridge test's module needs from the host layer it does not link:
 * pc_wasm.h's fatal (armrec_rt.c and armrec_bridge_wasm.c report through it). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void pc_wasm_fatal(const char *msg) {
    printf("FATAL %s\n", msg);
    exit(2);
}

void pc_wasm_fatalf(const char *fmt, ...) {
    va_list ap;
    printf("FATAL ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    exit(2);
}
