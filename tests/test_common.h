#ifndef OOPS_TEST_COMMON_H
#define OOPS_TEST_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The host test harness. A failed assertion ends its test, not the run: it hands
 * control back to `RUN_TEST` through `longjmp`, the failure is printed and kept, and
 * the next test starts. The summary lists every failure with its suite, test and line,
 * and the exit status is non-zero. The test itself stops, because an
 * `ASSERT_TRUE(p != NULL)` is followed by code that dereferences `p`.
 *
 * A test that faults or hangs still ends the process: there is no signal handler and
 * no timeout. Tests share process state (a heap, a display, a GL context), so a test
 * that stops partway can leave failures after it; read the first one first.
 */

extern int g_tests_run;
extern int g_tests_passed;
extern int g_tests_failed;

/*
 * Where a failed assertion lands, and whether anything is there to catch it. Both live
 * in `tests/test_runner.c` beside the counters. `g_test_abort_ready` is zero outside a
 * test, and an assertion that fails there has nowhere to unwind to and exits.
 */
extern jmp_buf g_test_abort;
extern int g_test_abort_ready;

void oops_test_suite_begin(const char *suite);
void oops_test_begin(const char *test);
/* Prints the failure, keeps it for the summary, and unwinds to `RUN_TEST`. Never
 * returns. */
void oops_test_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4), noreturn));
/* Prints every failure and the totals, and returns the process's exit status. */
int oops_test_report(void);

#define TEST_SUITE_BEGIN(name) oops_test_suite_begin(name)

#define TEST_SUITE_END()                                                               \
    do {                                                                               \
    } while (0)

#define RUN_TEST(fn)                                                                   \
    do {                                                                               \
        g_tests_run++;                                                                 \
        oops_test_begin(#fn);                                                          \
        printf("  - %-45s ... ", #fn);                                                 \
        fflush(stdout);                                                                \
        if (setjmp(g_test_abort) == 0) {                                               \
            g_test_abort_ready = 1;                                                    \
            fn();                                                                      \
            g_test_abort_ready = 0;                                                    \
            g_tests_passed++;                                                          \
            printf("\033[32mPASS\033[0m\n");                                           \
        } else {                                                                       \
            g_test_abort_ready = 0;                                                    \
        }                                                                              \
    } while (0)

#define ASSERT_TRUE(cond)                                                              \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            oops_test_fail(__FILE__, __LINE__, "condition '%s' failed", #cond);        \
        }                                                                              \
    } while (0)

#define ASSERT_EQ(a, b)                                                                \
    do {                                                                               \
        int64_t _va = (int64_t)(a);                                                    \
        int64_t _vb = (int64_t)(b);                                                    \
        if (_va != _vb) {                                                              \
            oops_test_fail(__FILE__, __LINE__, "expected %lld == %lld",                \
                           (long long)_va, (long long)_vb);                            \
        }                                                                              \
    } while (0)

#define ASSERT_NE(a, b)                                                                \
    do {                                                                               \
        int64_t _va = (int64_t)(a);                                                    \
        int64_t _vb = (int64_t)(b);                                                    \
        if (_va == _vb) {                                                              \
            oops_test_fail(__FILE__, __LINE__, "expected %lld != %lld",                \
                           (long long)_va, (long long)_vb);                            \
        }                                                                              \
    } while (0)

#define ASSERT_STR_EQ(a, b)                                                            \
    do {                                                                               \
        const char *_sa = (const char *)(a);                                           \
        const char *_sb = (const char *)(b);                                           \
        if (!_sa || !_sb || strcmp(_sa, _sb) != 0) {                                   \
            oops_test_fail(__FILE__, __LINE__, "expected '%s' == '%s'",                \
                           _sa ? _sa : "(null)", _sb ? _sb : "(null)");                \
        }                                                                              \
    } while (0)

#define ASSERT_FLOAT_NEAR(a, b, eps)                                                   \
    do {                                                                               \
        float _fa = (float)(a);                                                        \
        float _fb = (float)(b);                                                        \
        float _diff = (_fa > _fb) ? (_fa - _fb) : (_fb - _fa);                         \
        if (_diff > (float)(eps)) {                                                    \
            oops_test_fail(__FILE__, __LINE__, "expected %f ~= %f within %f, diff %f", \
                           (double)_fa, (double)_fb, (double)(eps), (double)_diff);    \
        }                                                                              \
    } while (0)

#endif /* OOPS_TEST_COMMON_H */
