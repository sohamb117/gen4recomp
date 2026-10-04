/* The bridge test's second C TU: the C functions recompiled code calls
 * (B names), reads the address of (X names), and is handed pointers to. */
#include <stdarg.h>
#include <stdint.h>

struct Big { int a, b, c; };
struct Small { uint16_t x, y; };

/* Six words: a, b, c (two), d, e. */
int c_helper6(int a, short b, uint64_t c, int d, int e) {
    return a * 1000000 + b * 100000 + (int)(c >> 32) * 10000 + (int)c * 1000 + d * 10 + e;
}

int c_target(int x) { return x * 3 + 1; }

int c_data[3] = { 100, 200, 300 };

int c_varsum(int n, ...) {
    va_list ap;
    int s = 0;
    va_start(ap, n);
    while (n-- > 0) s += va_arg(ap, int);
    va_end(ap);
    return s;
}

struct Big c_make_big(int a, int b) {
    struct Big r = { a, b, a * b };
    return r;
}

struct Small c_make_small(int x, int y) {
    struct Small r = { (uint16_t)x, (uint16_t)y };
    return r;
}

signed char c_ret_s8(void) { return -5; }

/* Five words: called back by asm_callback through a C function pointer. */
int c_cb5(int a, int b, int c, int d, int e) {
    return a + 10 * b + 100 * c + 1000 * d + 10000 * e;
}
