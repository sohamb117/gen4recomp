/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NP_TESTS_TESTUTIL_H
#define NP_TESTS_TESTUTIL_H

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                          \
    do {                                                                            \
        long long _a = (long long)(a), _b = (long long)(b);                        \
        if (_a != _b) {                                                             \
            fprintf(stderr, "%s:%d: %s == %s failed: %lld vs %lld\n", __FILE__,    \
                    __LINE__, #a, #b, _a, _b);                                      \
            g_failures++;                                                           \
        }                                                                           \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                           \
    do {                                                                             \
        const char *_a = (a), *_b = (b);                                             \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                                     \
            fprintf(stderr, "%s:%d: %s == %s failed: \"%s\" vs \"%s\"\n", __FILE__, \
                    __LINE__, #a, #b, _a ? _a : "(null)", _b ? _b : "(null)");       \
            g_failures++;                                                            \
        }                                                                            \
    } while (0)

#define TEST_RESULT()                                                   \
    (g_failures ? (fprintf(stderr, "%d check(s) failed\n", g_failures), 1) \
                : (printf("all checks passed\n"), 0))

#endif
