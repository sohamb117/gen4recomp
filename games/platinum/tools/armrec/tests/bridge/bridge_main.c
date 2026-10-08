/*
 * The bridge test: C calling recompiled code (directly, through function
 * pointers, variadically, with aggregates), recompiled code calling C
 * (directly, through pointers C hands it, through .words), and data both
 * ways. Every value is checked; the run prints one line per failure and a
 * summary, and exits non-zero on any failure.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "armrec_rt.h"

struct Big { int a, b, c; };
struct Small { uint16_t x, y; };
struct S12 { int a, b, c; };

/* The assembly (bridge_test.s), under the prototypes C gives it. */
int asm_mix6(int, int, int, int, int, int);
int asm_thumb_mul(int, int);
uint64_t asm_ret64(uint32_t, uint32_t, uint32_t, uint64_t);
int asm_callback(int (*)(int, int, int, int, int), int);
int asm_call_c(int);
int asm_sum_var(int, ...);
struct Big asm_make_big(int, int);
struct Small asm_make_small(int, int);
int asm_struct_stack(int, int, struct S12);
int8_t asm_id_a(int);
float asm_id_b(uint32_t);
double asm_id_c(double);
int asm_call_word(int);
int asm_read_cdata(void);
int asm_call_var(void);
int asm_call_structs(int *);
int asm_call_u8(void);
int asm_ldm_self(const int *);
uint32_t asm_thumb_lit(void);
extern int asm_table[];
extern uint32_t asm_cptrs[];

/* bridge_c2.c */
int c_target(int);
extern int c_data[];
int c_cb5(int, int, int, int, int);

void armrec_init_all(void);

static int c_mul2(int a, int b) { return a * b; }
static int static_cb5(int a, int b, int c, int d, int e) { return a * e + b * c * d; }

/* An asm data label in a static initializer, and an asm function's address
 * in a static table next to a C function's. */
static int *const third = &asm_table[2];
static int (*const tab[2])(int, int) = { asm_thumb_mul, c_mul2 };

static int failures, checks;

#define CHECK(what, got, want)                                              \
    do {                                                                    \
        unsigned long long g_ = (unsigned long long)(got);                  \
        unsigned long long w_ = (unsigned long long)(want);                 \
        checks++;                                                           \
        if (g_ != w_) {                                                     \
            failures++;                                                     \
            printf("FAIL %s: got 0x%llX want 0x%llX\n", what, g_, w_);      \
        }                                                                   \
    } while (0)

int main(void) {
    volatile int vi = 0;
    int (*volatile p6)(int, int, int, int, int, int) = asm_mix6;
    int (*volatile pv)(int, ...) = asm_sum_var;
    int (*volatile pc)(int) = c_target;
    struct Big big;
    struct Small sm;
    struct S12 s12 = { 3, 4, 5 };
    float f;
    uint32_t fbits;
    int out3[3] = { 0, 0, 0 };
    static const int pair[2] = { 0x5, 0x30 };

    if (armrec_mem_init() != 0) {
        printf("FAIL armrec_mem_init: %s\n", armrec_mem_strerror());
        return 1;
    }
    armrec_bind_externs();
    armrec_init_all();

    /* C -> recompiled, direct */
    CHECK("asm_mix6 direct", asm_mix6(1, 2, 3, 4, 5, 6), 0x654321);
    CHECK("asm_thumb_mul direct", asm_thumb_mul(6, 7), 42);
    CHECK("asm_ret64", asm_ret64(5, 0, 0, 0x1FFFFFFFFull), 0x200000004ull);
    CHECK("asm_sum_var direct", asm_sum_var(6, 1, 2, 3, 4, 5, 6), 21);
    big = asm_make_big(4, 5);
    CHECK("asm_make_big a", big.a, 4);
    CHECK("asm_make_big b", big.b, 5);
    CHECK("asm_make_big c", big.c, 9);
    sm = asm_make_small(0x1234, 0x5678);
    CHECK("asm_make_small x", sm.x, 0x1234);
    CHECK("asm_make_small y", sm.y, 0x5678);
    CHECK("asm_struct_stack", asm_struct_stack(1, 2, s12), 0x54321);
    CHECK("asm_id_a int8_t", asm_id_a(0x1FF80), (uint64_t)(int64_t)-128);
    CHECK("asm_ldm_self (ldmia r0!, {r0, r1})", asm_ldm_self(pair), 0x35);
    f = asm_id_b(0x40490FDBu);
    memcpy(&fbits, &f, 4);
    CHECK("asm_id_b float", fbits, 0x40490FDBu);
    CHECK("asm_id_c double", asm_id_c(2.5) == 2.5, 1);

    /* C -> recompiled, through pointers (guest addresses) */
    CHECK("asm_mix6 indirect", p6(1, 2, 3, 4, 5, 6), 0x654321);
    CHECK("asm_sum_var indirect", pv(3, 10, 20, 30), 60);
    CHECK("tab[0] is the Thumb address", (uint32_t)(uintptr_t)tab[0], 0x02000021u);
    CHECK("tab[0] call", tab[vi](3, 4), 12);
    vi = 1;
    CHECK("tab[1] call (C)", tab[vi](3, 5), 15);

    /* C -> C through a pointer (native) */
    CHECK("C pointer", pc(2), 7);

    /* recompiled -> C */
    CHECK("asm_call_c", asm_call_c(9), 8873056);
    CHECK("asm_callback(c_cb5)", asm_callback(c_cb5, 1), 54322);
    CHECK("asm_callback(static_cb5)", asm_callback(static_cb5, 2), 73);
    CHECK("asm_call_word (.word c_target)", asm_call_word(5), 16);
    CHECK("asm_read_cdata (.word c_data)", asm_read_cdata(), 200);
    CHECK("asm_call_var (variadic C)", asm_call_var(), 210);
    CHECK("asm_call_structs small", asm_call_structs(out3), 0x40003);
    CHECK("asm_call_structs big a", out3[0], 7);
    CHECK("asm_call_structs big b", out3[1], 9);
    CHECK("asm_call_structs big c", out3[2], 63);
    CHECK("asm_call_u8 (signed char)", asm_call_u8(), 995);

    /* data */
    CHECK("asm_table[1]", asm_table[1], 0x22222222);
    CHECK("static &asm_table[2]", *third, 0x33333333);
    CHECK("asm_table address", (uint32_t)(uintptr_t)asm_table, 0x02001000u);
    CHECK("asm .word c_target is the table index",
          asm_cptrs[0], (uint32_t)(uintptr_t)c_target);
    CHECK("asm .word c_data is the address",
          asm_cptrs[1], (uint32_t)(uintptr_t)c_data);
    CHECK("call through asm .word", ((int (*)(int))(uintptr_t)asm_cptrs[0])(7), 22);

    /* The address of a Thumb function, by every route: asm literal pool,
     * asm .word in data, C &F (the static tab[0] above). */
    CHECK("asm literal =asm_thumb_mul is the Thumb address", asm_thumb_lit(), 0x02000021u);
    CHECK("asm .word asm_thumb_mul is the Thumb address", asm_cptrs[2], 0x02000021u);
    CHECK("asm literal == C &asm_thumb_mul",
          asm_thumb_lit(), (uint32_t)(uintptr_t)&asm_thumb_mul);
    CHECK("call through the asm literal",
          ((int (*)(int, int))(uintptr_t)asm_thumb_lit())(6, 9), 54);

    /* runtime entry points for C function pointers */
    CHECK("armrec_call_code(c_target)",
          (uint32_t)armrec_call_code((uint32_t)(uintptr_t)c_target, 4, 0, 0, 0), 13);
    /* Word five of a dispatched call is the word at armrec_sp. */
    ARM_ST32(armrec_sp, 2);
    CHECK("armrec_dispatch(c_cb5)",
          (uint32_t)armrec_dispatch((uint32_t)(uintptr_t)c_cb5, 1, 1, 1, 1), 21111);
    CHECK("armrec_resolve_code(c_target)",
          (uintptr_t)armrec_resolve_code((uint32_t)(uintptr_t)c_target),
          (uintptr_t)c_target);
    CHECK("armrec_sp restored", armrec_sp, ARM_STACK_TOP);

    printf("bridge test: %d/%d checks passed\n", checks - failures, checks);
    return failures != 0;
}
