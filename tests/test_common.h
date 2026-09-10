#ifndef OOPS_TEST_COMMON_H
#define OOPS_TEST_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int g_tests_run;
extern int g_tests_passed;
extern int g_tests_failed;

#define TEST_SUITE_BEGIN(name) printf("\n=== [SUITE: %s] ===\n", name)

#define TEST_SUITE_END()                                                       \
  do {                                                                         \
  } while (0)

#define RUN_TEST(fn)                                                           \
  do {                                                                         \
    g_tests_run++;                                                             \
    printf("  - %-45s ... ", #fn);                                             \
    fflush(stdout);                                                            \
    fn();                                                                      \
    g_tests_passed++;                                                          \
    printf("\033[32mPASS\033[0m\n");                                           \
  } while (0)

#define ASSERT_TRUE(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      g_tests_failed++;                                                        \
      printf("\033[31mFAIL\033[0m (%s:%d: condition '%s' failed)\n", __FILE__, \
             __LINE__, #cond);                                                 \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#define ASSERT_EQ(a, b)                                                        \
  do {                                                                         \
    int64_t _va = (int64_t)(a);                                                \
    int64_t _vb = (int64_t)(b);                                                \
    if (_va != _vb) {                                                          \
      g_tests_failed++;                                                        \
      printf("\033[31mFAIL\033[0m (%s:%d: expected %lld == %lld)\n", __FILE__, \
             __LINE__, (long long)_va, (long long)_vb);                        \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#define ASSERT_NE(a, b)                                                        \
  do {                                                                         \
    int64_t _va = (int64_t)(a);                                                \
    int64_t _vb = (int64_t)(b);                                                \
    if (_va == _vb) {                                                          \
      g_tests_failed++;                                                        \
      printf("\033[31mFAIL\033[0m (%s:%d: expected %lld != %lld)\n", __FILE__, \
             __LINE__, (long long)_va, (long long)_vb);                        \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#define ASSERT_STR_EQ(a, b)                                                    \
  do {                                                                         \
    const char *_sa = (const char *)(a);                                       \
    const char *_sb = (const char *)(b);                                       \
    if (!_sa || !_sb || strcmp(_sa, _sb) != 0) {                               \
      g_tests_failed++;                                                        \
      printf("\033[31mFAIL\033[0m (%s:%d: expected '%s' == '%s')\n", __FILE__, \
             __LINE__, _sa ? _sa : "(null)", _sb ? _sb : "(null)");            \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#endif /* OOPS_TEST_COMMON_H */
